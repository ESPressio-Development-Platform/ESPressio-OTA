#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "ESPressio_OTA.hpp"
#include <ESPressio_RuntimeIdentity.hpp>

using namespace ESPressio;
using namespace ESPressio::OTA;

struct CoordinatorTestComponent {};

namespace ESPressio::OTA {
template<>
struct ComponentTypeTraits<::CoordinatorTestComponent> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {
            ComponentTypeId{0x4553504F54414301ULL},
            "ota.test.application",
            ComponentKind::ApplicationFirmware,
            ComponentMultiplicity::Single,
            1U
        };
    }
};
} // namespace ESPressio::OTA

namespace {

using Capacity = ConstrainedV1CapacityProfile;

class CoordinatorTestHandler final : public ComponentHandler<CoordinatorTestComponent, Capacity> {
public:
    ComponentRecoveryInspection InspectRecoveryState(
        const ComponentPreflightContext<Capacity>&) noexcept override {
        return {ComponentRecoveryState::NotPrepared, Result::Success()};
    }
};

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

class FakeArtifactSource final : public IArtifactSource {
public:
    std::array<std::uint8_t, 32> Data{};
    std::size_t Size{0U};
    std::size_t Position{0U};
    std::size_t Chunk{2U};
    std::size_t OpenCalls{0U};
    std::size_t CloseCalls{0U};
    bool PendingOnce{false};
    bool PendingReturned{false};

    Result Open(const ArtifactSourceOpenRequest& request) noexcept override {
        ++OpenCalls;
        Position = 0U;
        PendingReturned = false;
        if (!request.IsValid() || request.Offset != 0U || request.ExpectedLength != Size) {
            return {OutcomeClass::Invalid, {DiagnosticDomain::Source, 1U}};
        }
        return Result::Success();
    }

    StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (PendingOnce && !PendingReturned) {
            PendingReturned = true;
            return StreamReadResult::Pending();
        }
        if (Position == Size) return StreamReadResult::End();
        const auto bytes = std::min({Chunk, Size - Position, capacity});
        for (std::size_t i = 0U; i < bytes; ++i) output[i] = Data[Position + i];
        Position += bytes;
        return StreamReadResult::Data(bytes);
    }

    void Close() noexcept override { ++CloseCalls; }
};

class FakeArtifactStore final : public IArtifactStore {
public:
    std::array<std::uint8_t, 64> Bytes{};
    std::size_t Size{0U};
    std::size_t WriteLimit{1U};
    std::size_t BeginCalls{0U};
    std::size_t FinalizeCalls{0U};
    std::size_t AbortCalls{0U};
    bool PendingWriteOnce{false};
    bool PendingWriteReturned{false};
    bool PendingFinalizeOnce{false};
    bool PendingFinalizeReturned{false};

    Result BeginWrite(const ArtifactStoreOpenRequest& request) noexcept override {
        ++BeginCalls;
        Size = 0U;
        PendingWriteReturned = false;
        PendingFinalizeReturned = false;
        return request.IsValid() ? Result::Success()
                                 : Result{OutcomeClass::Invalid, {DiagnosticDomain::Store, 1U}};
    }

    StreamWriteResult Write(const std::uint8_t* data, std::size_t size) noexcept override {
        if (PendingWriteOnce && !PendingWriteReturned) {
            PendingWriteReturned = true;
            return StreamWriteResult::Pending();
        }
        if (data == nullptr || size == 0U || Size == Bytes.size()) {
            return StreamWriteResult::Failed({OutcomeClass::Failed, {DiagnosticDomain::Store, 2U}});
        }
        const auto accepted = std::min({WriteLimit, size, Bytes.size() - Size});
        for (std::size_t i = 0U; i < accepted; ++i) Bytes[Size + i] = data[i];
        Size += accepted;
        return StreamWriteResult::Accepted(accepted);
    }

    ArtifactStoreFinalizeResult Finalize() noexcept override {
        ++FinalizeCalls;
        if (PendingFinalizeOnce && !PendingFinalizeReturned) {
            PendingFinalizeReturned = true;
            return {ArtifactStoreFinalizeStatus::Pending, {OutcomeClass::Pending, {}}};
        }
        return {ArtifactStoreFinalizeStatus::Stored, Result::Success()};
    }

    void Abort() noexcept override { ++AbortCalls; }

    Result QueryAvailableBytes(std::uint64_t& availableBytes) const noexcept override {
        availableBytes = Bytes.size() - Size;
        return Result::Success();
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
                                     bool requireHealth = true,
                                     bool withArtifact = false) {
    Manifest<Capacity> manifest;
    manifest.Identifier = Id(id);
    manifest.Release = release;
    manifest.SecurityGeneration = security;
    (void)manifest.TargetClauses.push_back(ManifestTargetClause<Capacity>{});
    ManifestComponent<Capacity> component;
    component.Identifier = 1U;
    component.TypeId = 0x4553504F54414301ULL;
    if (withArtifact) {
        const auto artifactId = Id(static_cast<std::uint8_t>(id + 1U));
        (void)component.Artifacts.push_back(artifactId);
        ManifestArtifact<Capacity> artifact;
        artifact.Identifier = artifactId;
        artifact.ExpectedLength = 6U;
        artifact.DigestAlgorithm = Security::DigestAlgorithm::SHA256.Value();
        for (std::size_t i = 0U; i < 32U; ++i) {
            (void)artifact.Digest.push_back(static_cast<std::uint8_t>(i + 1U));
        }
        (void)manifest.Artifacts.push_back(artifact);
    }
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
    ArtifactCheckpointStore<Capacity> Checkpoints{Store};
    ArtifactTransferWorkspace<Capacity> Workspace{};
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
    CoordinatorTestHandler ComponentHandler{};
    ComponentHandlerDirectory<Capacity> Handlers{};
    FakeArtifactSource Source{};
    FakeArtifactStore ArtifactStore{};
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
        if (Handlers.Register(ComponentHandler) != ComponentHandlerDirectoryStatus::Success) return false;
        Handlers.Freeze();
        if (ApplicationReadyChecks.Add(ApplicationReady) != HealthCheckRegistrationStatus::Success) return false;
        if (Health.Add(ApplicationReadyEvaluator) != HealthRegistryStatus::Success) return false;
        return true;
    }

    TestCoordinator MakeCoordinator(std::uint64_t timeout, std::uint64_t reevaluation = 0U) {
        return TestCoordinator{Control, Owners, Boot, Trial, Restart, Clock, Policies, Health, Handlers,
                               Source, ArtifactStore, Checkpoints, Workspace, timeout, reevaluation};
    }
};

[[maybe_unused]] bool AdvanceToStaged(OTAControlStore<Capacity>& control, UpdateTransactionId transaction) {
    return control.AdvanceRecoveryPoint(transaction, RecoveryPoint::ArtifactsAcquired) == OTADurableStatus::Success &&
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
        auto coordinator = fixture.MakeCoordinator(5'000'000'000ULL, 100'000'000ULL);
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
        auto coordinator = fixture.MakeCoordinator(5'000'000'000ULL, 100'000'000ULL);
        if (!coordinator.Initialize()) return 22;

        const auto manifest = CandidateManifest(20U, 20U, 2U);
        if (ValidateManifest(manifest) != ManifestStatus::Success) return 23;
        UpdateTransactionId transaction;
        if (!coordinator.Start({ReleaseIdentifier{20U}, ManifestIdentifier{Id(20U)}, SecurityGeneration{2U}}, transaction)) return 24;
        if (!coordinator.BindVerifiedManifest(manifest)) return 25;
        OTAControlRecord<Capacity> acceptedManifest;
        if (fixture.Control.Load(acceptedManifest) != OTADurableStatus::Success ||
            acceptedManifest.Active.Point != RecoveryPoint::ManifestAccepted ||
            !coordinator.Status().UpdatePlanReady ||
            coordinator.Status().Lifecycle != UpdateLifecycle::CandidateSelected) return 250;
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
        auto coordinator = fixture.MakeCoordinator(1'000'000'000ULL);
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

#elif ESPRESSIO_OTA_COORDINATOR_SCENARIO == 4
    {
        Fixture fixture;
        if (!fixture.InitializeState()) return 70;
        if (fixture.Control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 71;
        fixture.Source.Size = 6U;
        fixture.Source.Chunk = 2U;
        fixture.Source.PendingOnce = true;
        for (std::size_t i = 0U; i < fixture.Source.Size; ++i) {
            fixture.Source.Data[i] = static_cast<std::uint8_t>(0x40U + i);
        }
        fixture.ArtifactStore.WriteLimit = 1U;
        fixture.ArtifactStore.PendingWriteOnce = true;
        fixture.ArtifactStore.PendingFinalizeOnce = true;

        auto coordinator = fixture.MakeCoordinator(5'000'000'000ULL);
        if (!coordinator.Initialize()) return 72;
        const auto manifest = CandidateManifest(40U, 40U, 1U, false, true);
        if (ValidateManifest(manifest) != ManifestStatus::Success) return 73;
        UpdateTransactionId transaction;
        if (!coordinator.Start({ReleaseIdentifier{40U}, ManifestIdentifier{Id(40U)}, SecurityGeneration{1U}}, transaction)) return 74;
        if (!coordinator.BindVerifiedManifest(manifest)) return 75;

        if (coordinator.Advance().Outcome != OutcomeClass::Pending ||
            coordinator.Status().Lifecycle != UpdateLifecycle::Acquiring) return 76;

        bool acquired = false;
        for (std::size_t step = 0U; step < 64U; ++step) {
            const auto result = coordinator.Advance();
            if (result.Outcome != OutcomeClass::Pending && result.Outcome != OutcomeClass::Deferred) return 77;
            OTAControlRecord<Capacity> record;
            if (fixture.Control.Load(record) != OTADurableStatus::Success) return 78;
            if (record.Active.Point == RecoveryPoint::ArtifactsAcquired) {
                acquired = true;
                break;
            }
        }
        if (!acquired || coordinator.Status().Lifecycle != UpdateLifecycle::Verifying) return 79;
        if (fixture.Source.OpenCalls != 1U || fixture.Source.CloseCalls != 1U ||
            fixture.ArtifactStore.BeginCalls != 1U || fixture.ArtifactStore.FinalizeCalls != 2U ||
            fixture.ArtifactStore.AbortCalls != 0U || fixture.ArtifactStore.Size != fixture.Source.Size) return 80;
        for (std::size_t i = 0U; i < fixture.Source.Size; ++i) {
            if (fixture.ArtifactStore.Bytes[i] != fixture.Source.Data[i]) return 81;
        }
        ArtifactCheckpoint<Capacity> checkpoint;
        std::size_t checkpointSlot = Capacity::MaximumArtifactCheckpoints;
        if (fixture.Checkpoints.Find(transaction, ArtifactIdentifier{Id(41U)}, checkpoint, checkpointSlot) !=
            OTADurableStatus::NotFound) return 82;

        const auto frontier = coordinator.Advance();
        if (frontier.Outcome != OutcomeClass::Pending ||
            frontier.Detail.Reason != static_cast<std::uint32_t>(CoordinatorCoreReason::AwaitingExecutionIntegration) ||
            coordinator.Status().Lifecycle != UpdateLifecycle::Verifying) return 83;
        if (!coordinator.Cancel(transaction) || coordinator.Status().Availability != CoordinatorAvailability::Ready) return 84;
    }

#else
#error Unsupported ESPRESSIO_OTA_COORDINATOR_SCENARIO
#endif

    return 0;
}
