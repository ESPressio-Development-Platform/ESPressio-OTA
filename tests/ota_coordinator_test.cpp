#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "ESPressio_OTA.hpp"
#include <ESPressio_RuntimeIdentity.hpp>

using namespace ESPressio;
using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;

Timing::QualifiedTime CapturedTime() {
    return {123456U, Timing::TimeReliability::Synchronized};
}

bool InstallIdentity() {
    System::DeviceIdentifier::Storage bytes{};
    bytes[0] = 0x71U;
    const System::DeviceRuntimeIdentity expected{
        System::DeviceIdentifier{bytes}, System::RuntimeIncarnationId{1U}};
    const auto status = System::RuntimeIdentity::Install(expected);
    if (status == System::RuntimeIdentity::InstallationStatus::Success) return true;
    if (status != System::RuntimeIdentity::InstallationStatus::AlreadyInstalled) return false;
    System::DeviceRuntimeIdentity installed{};
    return System::RuntimeIdentity::TryRead(installed) && installed == expected;
}

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

class FakeAtomicRecordStore final : public Persistence::IAtomicRecordStore {
    struct Entry final {
        Persistence::AtomicRecordKey Key{};
        bool Used{false};
        std::array<std::uint8_t, Capacity::MaximumOTAControlRecordBytes> Bytes{};
        std::size_t Size{0U};
    };
    std::array<Entry, 8> entries_{};

    Entry* Find(const Persistence::AtomicRecordKey& key) noexcept {
        for (auto& entry : entries_) if (entry.Used && entry.Key == key) return &entry;
        return nullptr;
    }
    const Entry* Find(const Persistence::AtomicRecordKey& key) const noexcept {
        for (const auto& entry : entries_) if (entry.Used && entry.Key == key) return &entry;
        return nullptr;
    }
public:
    Persistence::AtomicRecordCapabilities Capabilities() const noexcept override {
        return {true, true, Capacity::MaximumOTAControlRecordBytes, entries_.size()};
    }
    Persistence::AtomicRecordStatus Recover() noexcept override {
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus Read(const Persistence::AtomicRecordKey& key,
                                         std::uint8_t* buffer,
                                         std::size_t capacity,
                                         std::size_t& bytesRead) noexcept override {
        bytesRead = 0U;
        const auto* entry = Find(key);
        if (entry == nullptr) return Persistence::AtomicRecordStatus::NotFound;
        if (buffer == nullptr || capacity < entry->Size) return Persistence::AtomicRecordStatus::BufferTooSmall;
        for (std::size_t i = 0U; i < entry->Size; ++i) buffer[i] = entry->Bytes[i];
        bytesRead = entry->Size;
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus ReplaceAtomically(const Persistence::AtomicRecordKey& key,
                                                       const std::uint8_t* data,
                                                       std::size_t size) noexcept override {
        if (!key || data == nullptr || size > Capacity::MaximumOTAControlRecordBytes) {
            return Persistence::AtomicRecordStatus::InvalidArgument;
        }
        auto* entry = Find(key);
        if (entry == nullptr) {
            for (auto& candidate : entries_) {
                if (!candidate.Used) {
                    entry = &candidate;
                    entry->Used = true;
                    entry->Key = key;
                    break;
                }
            }
        }
        if (entry == nullptr) return Persistence::AtomicRecordStatus::NoSpace;
        for (std::size_t i = 0U; i < size; ++i) entry->Bytes[i] = data[i];
        entry->Size = size;
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus RemoveAfterCommit(const Persistence::AtomicRecordKey& key) noexcept override {
        auto* entry = Find(key);
        if (entry != nullptr) {
            entry->Used = false;
            entry->Size = 0U;
        }
        return Persistence::AtomicRecordStatus::Success;
    }
};

CommittedBaseline FactoryBaseline() noexcept {
    CommittedBaseline baseline;
    baseline.Generation = UpdateGenerationId{1U};
    baseline.Security = SecurityGeneration{0U};
    return baseline;
}

struct BootBackend final : Platform::Backend {};
struct TrialBackend final : Platform::Backend {};
struct RestartBackend final : Platform::Backend {};
struct ClockBackend final : Platform::Backend {};

class FakeBootControl final : public Platform::ProviderDeclaration<
    BootBackend, Platform::CapabilitySet<Platform::Capability::BootControl>> {
public:
    Platform::OTA::BootTargetIdentifier Current{1U};
    Platform::OTA::BootTargetIdentifier Committed{1U};
    Platform::OTA::BootTargetIdentifier Next{1U};
    std::size_t SelectCalls{0U};

    Platform::OTA::BootTargetIdentifier CurrentBootTarget() const noexcept { return Current; }
    Platform::OTA::BootTargetIdentifier CommittedBootTarget() const noexcept { return Committed; }
    Platform::OTA::BootTargetIdentifier NextBootTarget() const noexcept { return Next; }
    Platform::OTA::Result SelectNextBootTarget(Platform::OTA::BootTargetIdentifier target) noexcept {
        ++SelectCalls;
        Next = target;
        return {Platform::OTA::Status::Success, 0};
    }
};

class FakeTrialBoot final : public Platform::ProviderDeclaration<
    TrialBackend, Platform::CapabilitySet<Platform::Capability::TrialBoot>> {
public:
    bool Trial{false};
    std::size_t MarkValidCalls{0U};
    std::size_t MarkInvalidCalls{0U};

    bool IsCurrentBootTrial() const noexcept { return Trial; }
    Platform::OTA::Result MarkCurrentBootValid() noexcept {
        ++MarkValidCalls;
        Trial = false;
        return {Platform::OTA::Status::Success, 0};
    }
    Platform::OTA::Result MarkCurrentBootInvalid() noexcept {
        ++MarkInvalidCalls;
        Trial = false;
        return {Platform::OTA::Status::Success, 0};
    }
};

class FakeRestart final : public Platform::ProviderDeclaration<
    RestartBackend, Platform::CapabilitySet<Platform::Capability::SystemRestart>> {
public:
    std::size_t Calls{0U};
    Platform::OTA::RestartReason Last{Platform::OTA::RestartReason::Unspecified};
    Platform::OTA::Result Restart(Platform::OTA::RestartReason reason) noexcept {
        ++Calls;
        Last = reason;
        return {Platform::OTA::Status::Success, 0};
    }
};

class FakeClock final : public Platform::Clock::MonotonicProviderDeclaration<ClockBackend, 1'000'000ULL, 64U> {
public:
    Platform::Clock::Tick Tick{0U};
    Platform::Clock::Tick Now() const noexcept { return Tick; }
};

class MutableActivatePolicy final : public IPolicyGate<ActivatePolicyDecisionPoint> {
public:
    PolicyVerdict Verdict{PolicyVerdict::Allow};
    PolicyDecision Evaluate(const PolicyContext<ActivatePolicyDecisionPoint>&) noexcept override {
        if (Verdict == PolicyVerdict::Defer) return PolicyDecision::Defer(77U);
        if (Verdict == PolicyVerdict::Reject) return PolicyDecision::Reject(78U);
        return PolicyDecision::Allow();
    }
};

class PassingApplicationReady final : public IHealthCheck<ApplicationReadyHealthCondition> {
public:
    std::size_t Calls{0U};
    HealthDecision Check(const HealthContext<ApplicationReadyHealthCondition>& context) noexcept override {
        ++Calls;
        return context.IsCanonical() ? HealthDecision::Pass() : HealthDecision::Fail(99U);
    }
};

[[maybe_unused]] Manifest<Capacity> CandidateManifest(std::uint8_t id,
                                     std::uint64_t release,
                                     std::uint64_t security,
                                     bool requireHealth = true) {
    Manifest<Capacity> manifest;
    manifest.Identifier = Id(id);
    manifest.Release = release;
    manifest.SecurityGeneration = security;
    (void)manifest.TargetClauses.push_back(ManifestTargetClause<Capacity>{});
    ManifestComponent<Capacity> component;
    component.Identifier = 1U;
    component.TypeId = 0x4553504F54414301ULL;
    (void)manifest.Components.push_back(component);
    if (requireHealth) {
        (void)manifest.RequiredHealthConditions.push_back(ApplicationReadyHealthCondition::TypeId.Value());
    }
    ManifestSignatureDescriptor signature;
    signature.Algorithm = 1U;
    signature.TrustAnchor = 1U;
    signature.TrustPolicy = 1U;
    (void)manifest.SignatureDescriptors.push_back(signature);
    return manifest;
}

using ActivatePolicies = PolicyGateSet<ActivatePolicyDecisionPoint, Capacity::MaximumPolicyProvidersPerDecisionPoint>;
using HealthRegistryType = HealthRegistry<Capacity::MaximumRequiredHealthConditions>;
using TestCoordinator = Coordinator<Capacity, FakeBootControl, FakeTrialBoot, FakeRestart, FakeClock>;

struct Fixture final {
    FakeAtomicRecordStore Store{};
    OTAControlStore<Capacity> Control{Store};
    Primitive::TypeDirectory<9> Directory{};
    OTAStateRuntime<> StateRuntime{};
    OTAStateOwners Owners{};
    FakeBootControl Boot{};
    FakeTrialBoot Trial{};
    FakeRestart Restart{};
    FakeClock Clock{};
    ActivatePolicies Policies{};
    MutableActivatePolicy ActivatePolicy{};
    HealthRegistryType Health{};
    HealthCheckSet<ApplicationReadyHealthCondition, 1U> ApplicationReadyChecks{};
    PassingApplicationReady ApplicationReady{};
    HealthConditionEvaluator<ApplicationReadyHealthCondition, 1U> ApplicationReadyEvaluator{ApplicationReadyChecks};

    bool InitializeState() {
        if (RegisterOTAStateTypes(Directory) != Primitive::TypeDirectoryRegistrationStatus::Success) return false;
        if (Directory.Initialize() != Primitive::TypeDirectoryInitializationStatus::Success) return false;
        if (!BindOTAStateOwners(StateRuntime, Owners)) return false;
        if (StateRuntime.Initialize(Directory.View(), &CapturedTime) != State::StateRuntimeStatus::Success) return false;
        if (StateRuntime.Start() != State::StateRuntimeStatus::Success) return false;
        if (Policies.Add(ActivatePolicy) != PolicyGateRegistrationStatus::Success) return false;
        if (ApplicationReadyChecks.Add(ApplicationReady) != HealthCheckRegistrationStatus::Success) return false;
        if (Health.Add(ApplicationReadyEvaluator) != HealthRegistryStatus::Success) return false;
        return true;
    }
};

[[maybe_unused]] bool AdvanceToStaged(OTAControlStore<Capacity>& control, UpdateTransactionId transaction) {
    return control.AdvanceRecoveryPoint(transaction, RecoveryPoint::ManifestAccepted) == OTADurableStatus::Success &&
           control.AdvanceRecoveryPoint(transaction, RecoveryPoint::ArtifactsAcquired) == OTADurableStatus::Success &&
           control.AdvanceRecoveryPoint(transaction, RecoveryPoint::ArtifactsVerified) == OTADurableStatus::Success &&
           control.AdvanceRecoveryPoint(transaction, RecoveryPoint::StagingStarted) == OTADurableStatus::Success &&
           control.AdvanceRecoveryPoint(transaction, RecoveryPoint::Staged) == OTADurableStatus::Success;
}

} // namespace

#ifndef ESPRESSIO_OTA_COORDINATOR_SCENARIO
#define ESPRESSIO_OTA_COORDINATOR_SCENARIO 1
#endif

int main() {
    if (!InstallIdentity()) return 1;

#if ESPRESSIO_OTA_COORDINATOR_SCENARIO == 1
    {
        Fixture fixture;
        if (!fixture.InitializeState()) return 2;
        if (fixture.Control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 3;
        TestCoordinator coordinator{fixture.Control, fixture.Owners, fixture.Boot, fixture.Trial, fixture.Restart,
                                    fixture.Clock, fixture.Policies, fixture.Health, 5'000'000'000ULL, 100'000'000ULL};
        if (!coordinator.Initialize()) return 4;
        if (coordinator.Status().Availability != CoordinatorAvailability::Ready) return 5;

        UpdateTransactionId first;
        if (!coordinator.Start({ReleaseIdentifier{10U}, ManifestIdentifier{Id(10U)}, SecurityGeneration{1U}}, first) ||
            first != UpdateTransactionId{1U}) return 6;
        if (coordinator.Status().Lifecycle != UpdateLifecycle::Checking) return 7;
        const auto cancelResult = coordinator.Cancel(first);
        if (!cancelResult) {
            std::printf("cancel outcome=%u domain=%u reason=%u native=%d availability=%u lifecycle=%u\n",
                static_cast<unsigned>(cancelResult.Outcome),
                static_cast<unsigned>(cancelResult.Detail.Domain),
                static_cast<unsigned>(cancelResult.Detail.Reason),
                static_cast<int>(cancelResult.Detail.NativeCode),
                static_cast<unsigned>(coordinator.Status().Availability),
                static_cast<unsigned>(coordinator.Status().Lifecycle));
            return 8;
        }
        if (coordinator.Status().Availability != CoordinatorAvailability::Ready || coordinator.Status().HasActiveTransaction) return 9;

        ActiveTransactionRecord replacement;
        if (fixture.Control.BeginTransaction(ReleaseIdentifier{11U}, ManifestIdentifier{Id(11U)}, SecurityGeneration{1U}, replacement) !=
            OTADurableStatus::Success) return 10;
        if (replacement.Transaction != UpdateTransactionId{2U} || replacement.CandidateGeneration != UpdateGenerationId{3U}) return 11;
    }

#elif ESPRESSIO_OTA_COORDINATOR_SCENARIO == 2
    {
        Fixture fixture;
        if (!fixture.InitializeState()) return 20;
        if (fixture.Control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 21;
        TestCoordinator coordinator{fixture.Control, fixture.Owners, fixture.Boot, fixture.Trial, fixture.Restart,
                                    fixture.Clock, fixture.Policies, fixture.Health, 5'000'000'000ULL, 100'000'000ULL};
        if (!coordinator.Initialize()) return 22;

        const auto manifest = CandidateManifest(20U, 20U, 2U);
        if (ValidateManifest(manifest) != ManifestStatus::Success) return 23;
        UpdateTransactionId transaction;
        if (!coordinator.Start({ReleaseIdentifier{20U}, ManifestIdentifier{Id(20U)}, SecurityGeneration{2U}}, transaction)) return 24;
        if (!coordinator.BindVerifiedManifest(manifest)) return 25;
        if (!AdvanceToStaged(fixture.Control, transaction)) return 26;
        if (coordinator.Advance().Outcome != OutcomeClass::Pending || coordinator.Status().Lifecycle != UpdateLifecycle::Staged) return 27;

        fixture.ActivatePolicy.Verdict = PolicyVerdict::Defer;
        CoordinatorActivationRequest activation{transaction, Platform::OTA::BootTargetIdentifier{2U}, true, true, true};
        const auto deferred = coordinator.BeginActivation(activation);
        if (deferred.Outcome != OutcomeClass::Deferred || coordinator.Status().Lifecycle != UpdateLifecycle::ActivationPending) return 28;

        fixture.ActivatePolicy.Verdict = PolicyVerdict::Allow;
        if (coordinator.BeginActivation(activation).Outcome != OutcomeClass::Pending) return 29;
        OTAControlRecord<Capacity> armed;
        if (fixture.Control.Load(armed) != OTADurableStatus::Success || armed.Intent != DurableIntent::ActivationArmed ||
            armed.Active.CandidateBootTarget != Platform::OTA::BootTargetIdentifier{2U} ||
            armed.Active.PreviousCommittedBootTarget != Platform::OTA::BootTargetIdentifier{1U}) return 30;

        if (coordinator.Advance().Outcome != OutcomeClass::Pending || fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{2U}) return 31;
        if (coordinator.Advance().Outcome != OutcomeClass::Pending || fixture.Restart.Calls != 1U ||
            fixture.Restart.Last != Platform::OTA::RestartReason::ActivateCandidate) return 32;

        fixture.Boot.Current = Platform::OTA::BootTargetIdentifier{2U};
        fixture.Boot.Next = Platform::OTA::BootTargetIdentifier{2U};
        fixture.Trial.Trial = true;
        if (coordinator.Advance().Outcome != OutcomeClass::Pending || coordinator.Status().Lifecycle != UpdateLifecycle::Trial) return 33;
        if (coordinator.Advance().Outcome != OutcomeClass::Pending || coordinator.Status().Lifecycle != UpdateLifecycle::Committing) return 34;
        if (fixture.ApplicationReady.Calls != 1U) return 35;
        if (!coordinator.Advance()) return 36;
        if (fixture.Trial.MarkValidCalls != 1U) return 37;
        if (coordinator.Status().Availability != CoordinatorAvailability::Ready || coordinator.Status().HasActiveTransaction) return 38;

        OTAControlRecord<Capacity> committed;
        if (fixture.Control.Load(committed) != OTADurableStatus::Success || committed.HasActiveTransaction ||
            committed.Committed.Generation != UpdateGenerationId{2U} || committed.Committed.Release != ReleaseIdentifier{20U} ||
            committed.MinimumAcceptedSecurity != SecurityGeneration{2U}) return 39;

        State::StateSnapshot<LastUpdateOutcomeState> lastOutcome;
        State::StateSnapshot<CommittedGenerationState> committedState;
        if (!fixture.StateRuntime.TryRead(lastOutcome) || !fixture.StateRuntime.TryRead(committedState)) return 40;
        if (!lastOutcome.Value.Present || lastOutcome.Value.Outcome != TerminalUpdateOutcome::Completed ||
            committedState.Value.Identity.Generation != 2U || committedState.Value.SecurityGeneration != 2U) return 41;
    }

#elif ESPRESSIO_OTA_COORDINATOR_SCENARIO == 3
    {
        Fixture fixture;
        if (!fixture.InitializeState()) return 50;
        if (fixture.Control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 51;
        TestCoordinator coordinator{fixture.Control, fixture.Owners, fixture.Boot, fixture.Trial, fixture.Restart,
                                    fixture.Clock, fixture.Policies, fixture.Health, 1'000'000'000ULL};
        if (!coordinator.Initialize()) return 52;
        const auto manifest = CandidateManifest(30U, 30U, 1U, false);
        UpdateTransactionId transaction;
        if (!coordinator.Start({ReleaseIdentifier{30U}, ManifestIdentifier{Id(30U)}, SecurityGeneration{1U}}, transaction)) return 53;
        if (!coordinator.BindVerifiedManifest(manifest)) return 54;
        if (!AdvanceToStaged(fixture.Control, transaction)) return 55;
        if (coordinator.Advance().Outcome != OutcomeClass::Pending) return 56;
        if (coordinator.BeginActivation({transaction, Platform::OTA::BootTargetIdentifier{2U}, true, true, true}).Outcome != OutcomeClass::Pending) return 57;
        if (coordinator.Advance().Outcome != OutcomeClass::Pending) return 58;
        fixture.Boot.Current = Platform::OTA::BootTargetIdentifier{2U};
        fixture.Boot.Next = Platform::OTA::BootTargetIdentifier{2U};
        fixture.Trial.Trial = true;
        if (coordinator.Advance().Outcome != OutcomeClass::Pending) return 59;

        const auto cancellation = coordinator.Cancel(transaction);
        if (cancellation.Outcome != OutcomeClass::Pending || coordinator.Status().Lifecycle != UpdateLifecycle::RollbackPending) return 60;
        if (coordinator.Advance().Outcome != OutcomeClass::Pending || fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{1U}) return 61;
        if (coordinator.Advance().Outcome != OutcomeClass::Pending || fixture.Trial.MarkInvalidCalls != 1U) return 62;
        fixture.Boot.Current = Platform::OTA::BootTargetIdentifier{1U};
        fixture.Boot.Next = Platform::OTA::BootTargetIdentifier{1U};
        if (!coordinator.Advance()) return 63;
        if (coordinator.Status().Availability != CoordinatorAvailability::Ready) return 64;
        OTAControlRecord<Capacity> rolledBack;
        if (fixture.Control.Load(rolledBack) != OTADurableStatus::Success || rolledBack.HasActiveTransaction ||
            rolledBack.Committed.Generation != UpdateGenerationId{1U} ||
            rolledBack.MinimumAcceptedSecurity != SecurityGeneration{0U}) return 65;
    }

#else
#error Unsupported ESPRESSIO_OTA_COORDINATOR_SCENARIO
#endif

    return 0;
}
