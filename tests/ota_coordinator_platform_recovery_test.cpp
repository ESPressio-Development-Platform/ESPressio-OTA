#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTA.hpp"
#include <ESPressio_RuntimeIdentity.hpp>

using namespace ESPressio;
using namespace ESPressio::OTA;

#ifndef ESPRESSIO_OTA_PLATFORM_RECOVERY_SCENARIO
#define ESPRESSIO_OTA_PLATFORM_RECOVERY_SCENARIO 1
#endif

struct PlatformRecoveryComponent {};

namespace ESPressio::OTA {
template<>
struct ComponentTypeTraits<::PlatformRecoveryComponent> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {
            ComponentTypeId{0x4553504F54415052ULL},
            "ota.test.platform-recovery",
            ComponentKind::ApplicationFirmware,
            ComponentMultiplicity::Single,
            1U
        };
    }
};
} // namespace ESPressio::OTA

namespace {
using Capacity = ConstrainedV1CapacityProfile;

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

Timing::QualifiedTime CapturedTime() {
    return {123456U, Timing::TimeReliability::Synchronized};
}

bool InstallIdentity() {
    System::DeviceIdentifier::Storage bytes{};
    bytes[0] = 0x72U;
    const System::DeviceRuntimeIdentity expected{
        System::DeviceIdentifier{bytes}, System::RuntimeIncarnationId{1U}};
    const auto status = System::RuntimeIdentity::Install(expected);
    if (status == System::RuntimeIdentity::InstallationStatus::Success) return true;
    if (status != System::RuntimeIdentity::InstallationStatus::AlreadyInstalled) return false;
    System::DeviceRuntimeIdentity installed{};
    return System::RuntimeIdentity::TryRead(installed) && installed == expected;
}

CommittedBaseline FactoryBaseline() noexcept {
    CommittedBaseline baseline;
    baseline.Generation = UpdateGenerationId{1U};
    baseline.Security = SecurityGeneration{0U};
    return baseline;
}

class MemoryAtomicRecordStore final : public Persistence::IAtomicRecordStore {
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

class InertArtifactSource final : public IArtifactSource {
public:
    Result Open(const ArtifactSourceOpenRequest&) noexcept override { return Result::Success(); }
    StreamReadResult Read(std::uint8_t*, std::size_t) noexcept override { return StreamReadResult::End(); }
    void Close() noexcept override {}
};

class InertArtifactStore final : public IReadableArtifactStore {
public:
    Result BeginWrite(const ArtifactStoreOpenRequest&) noexcept override { return Result::Success(); }
    StreamWriteResult Write(const std::uint8_t*, std::size_t size) noexcept override {
        return size == 0U ? StreamWriteResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Store, 1U}})
                          : StreamWriteResult::Accepted(size);
    }
    ArtifactStoreFinalizeResult Finalize() noexcept override {
        return {ArtifactStoreFinalizeStatus::Stored, Result::Success()};
    }
    void Abort() noexcept override {}
    Result QueryAvailableBytes(std::uint64_t& availableBytes) const noexcept override {
        availableBytes = 1024U * 1024U;
        return Result::Success();
    }
    Result OpenRead(const ArtifactStoreReadRequest&) noexcept override { return Result::Success(); }
    StreamReadResult ReadStored(std::uint8_t*, std::size_t) noexcept override { return StreamReadResult::End(); }
    void CloseRead() noexcept override {}
};

class InertDigest final : public Security::IStreamingDigestVerifier {
public:
    bool Supports(Security::DigestAlgorithmIdentifier) const noexcept override { return true; }
    std::size_t DigestSize(Security::DigestAlgorithmIdentifier) const noexcept override { return 32U; }
    Security::VerificationResult Begin(Security::DigestAlgorithmIdentifier) noexcept override {
        return Security::VerificationResult::Ok();
    }
    Security::VerificationResult Update(Security::ByteView) noexcept override {
        return Security::VerificationResult::Ok();
    }
    Security::VerificationResult VerifyFinal(Security::ByteView) noexcept override {
        return Security::VerificationResult::Ok();
    }
};

struct BootBackend final : Platform::Backend {};
struct TrialBackend final : Platform::Backend {};
struct RestartBackend final : Platform::Backend {};
struct ClockBackend final : Platform::Backend {};

class FaultBootControl final : public Platform::ProviderDeclaration<
    BootBackend, Platform::CapabilitySet<Platform::Capability::BootControl>> {
public:
    Platform::OTA::BootTargetIdentifier Current{1U};
    Platform::OTA::BootTargetIdentifier Committed{1U};
    Platform::OTA::BootTargetIdentifier Next{1U};
    Platform::OTA::Status SelectStatus{Platform::OTA::Status::Success};
    std::size_t SelectCalls{0U};

    Platform::OTA::BootTargetIdentifier CurrentBootTarget() const noexcept { return Current; }
    Platform::OTA::BootTargetIdentifier CommittedBootTarget() const noexcept { return Committed; }
    Platform::OTA::BootTargetIdentifier NextBootTarget() const noexcept { return Next; }
    Platform::OTA::Result SelectNextBootTarget(Platform::OTA::BootTargetIdentifier target) noexcept {
        ++SelectCalls;
        if (SelectStatus == Platform::OTA::Status::Success) Next = target;
        return {SelectStatus, SelectStatus == Platform::OTA::Status::Success ? 0 : -101};
    }
};

class FaultTrialBoot final : public Platform::ProviderDeclaration<
    TrialBackend, Platform::CapabilitySet<Platform::Capability::TrialBoot>> {
public:
    bool Trial{false};
    Platform::OTA::TrialBootState CandidateState{Platform::OTA::TrialBootState::NeverAttempted};
    Platform::OTA::Status InspectStatus{Platform::OTA::Status::Success};
    mutable std::size_t InspectCalls{0U};
    Platform::OTA::Status MarkValidStatus{Platform::OTA::Status::Success};
    Platform::OTA::Status MarkInvalidStatus{Platform::OTA::Status::Success};
    std::size_t MarkValidCalls{0U};
    std::size_t MarkInvalidCalls{0U};

    bool IsCurrentBootTrial() const noexcept { return Trial; }
    Platform::OTA::TrialBootStateResult InspectBootTargetTrialState(
        Platform::OTA::BootTargetIdentifier target) const noexcept {
        ++InspectCalls;
        if (!target) {
            return {Platform::OTA::Status::Invalid, Platform::OTA::TrialBootState::Unknown, -105};
        }
        return {
            InspectStatus,
            InspectStatus == Platform::OTA::Status::Success
                ? CandidateState
                : Platform::OTA::TrialBootState::Unknown,
            InspectStatus == Platform::OTA::Status::Success ? 0 : -106};
    }
    Platform::OTA::Result MarkCurrentBootValid() noexcept {
        ++MarkValidCalls;
        if (MarkValidStatus == Platform::OTA::Status::Success) Trial = false;
        return {MarkValidStatus, MarkValidStatus == Platform::OTA::Status::Success ? 0 : -102};
    }
    Platform::OTA::Result MarkCurrentBootInvalid() noexcept {
        ++MarkInvalidCalls;
        if (MarkInvalidStatus == Platform::OTA::Status::Success) Trial = false;
        return {MarkInvalidStatus, MarkInvalidStatus == Platform::OTA::Status::Success ? 0 : -103};
    }
};

class FaultRestart final : public Platform::ProviderDeclaration<
    RestartBackend, Platform::CapabilitySet<Platform::Capability::SystemRestart>> {
public:
    Platform::OTA::Status Status{Platform::OTA::Status::Success};
    std::size_t Calls{0U};
    Platform::OTA::RestartReason Last{Platform::OTA::RestartReason::Unspecified};

    Platform::OTA::Result Restart(Platform::OTA::RestartReason reason) noexcept {
        ++Calls;
        Last = reason;
        return {Status, Status == Platform::OTA::Status::Success ? 0 : -104};
    }
};

class TestClock final : public Platform::Clock::MonotonicProviderDeclaration<ClockBackend, 1'000'000ULL, 64U> {
public:
    Platform::Clock::Tick Tick{0U};
    Platform::Clock::Tick Now() const noexcept { return Tick; }
};

class AllowActivation final : public IPolicyGate<ActivatePolicyDecisionPoint> {
public:
    PolicyDecision Evaluate(const PolicyContext<ActivatePolicyDecisionPoint>&) noexcept override {
        return PolicyDecision::Allow();
    }
};

class RecoveryHandler final : public ComponentHandler<PlatformRecoveryComponent, Capacity> {
public:
    ComponentRecoveryState Recovery{ComponentRecoveryState::Staged};
    std::size_t ActivateCalls{0U};
    std::size_t CommitCalls{0U};
    std::size_t RollbackCalls{0U};

    ComponentActionResult Activate(const ComponentExecutionContext<Capacity>& context) noexcept override {
        ++ActivateCalls;
        if (!context.IsValid()) return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 1U}});
        Recovery = ComponentRecoveryState::Activated;
        return ComponentActionResult::Complete();
    }
    ComponentActionResult Commit(const ComponentExecutionContext<Capacity>& context) noexcept override {
        ++CommitCalls;
        if (!context.IsValid()) return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 2U}});
        Recovery = ComponentRecoveryState::Committed;
        return ComponentActionResult::Complete();
    }
    ComponentActionResult Rollback(const ComponentExecutionContext<Capacity>& context) noexcept override {
        ++RollbackCalls;
        if (!context.IsValid()) return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 3U}});
        Recovery = ComponentRecoveryState::RolledBack;
        return ComponentActionResult::Complete();
    }
    ComponentRecoveryInspection InspectRecoveryState(const ComponentPreflightContext<Capacity>&) noexcept override {
        return {Recovery, Result::Success()};
    }
};

bool ConfigureTargetProfile(UpdateTargetProfile<Capacity>& profile) {
    if (profile.SetSystemIdentity(System::ProductTypeIdentifier{1U}, System::HardwareFamilyIdentifier{2U},
                                  System::HardwareRevision{1U}, System::ArchitectureIdentifier{3U},
                                  System::SoftwareVariantIdentifier{4U}) != TargetProfileStatus::Success) return false;
    if (profile.SetStorageLayout(Platform::OTA::StorageLayoutIdentifier{5U},
                                 Platform::OTA::StorageLayoutGeneration{1U}) != TargetProfileStatus::Success) return false;
    if (profile.SetPersistenceSchema(Persistence::SchemaIdentifier{6U},
                                     Persistence::SchemaGeneration{1U}) != TargetProfileStatus::Success) return false;
    if (profile.SetOTASupport(OTAProtocolV1, 0U) != TargetProfileStatus::Success) return false;
    if (profile.AddSupportedComponentType(ComponentTypeId{0x4553504F54415052ULL}) != TargetProfileStatus::Success) return false;
    std::array<std::uint8_t, 32> fingerprint{};
    fingerprint[0] = 2U;
    return profile.SetFingerprintAndFreeze(UpdateTargetProfileFingerprint{fingerprint}) == TargetProfileStatus::Success;
}

Manifest<Capacity> CandidateManifest(std::uint8_t id) {
    Manifest<Capacity> manifest;
    manifest.Identifier = Id(id);
    manifest.Release = id;
    manifest.SecurityGeneration = 1U;
    (void)manifest.TargetClauses.push_back(ManifestTargetClause<Capacity>{});
    ManifestComponent<Capacity> component;
    component.Identifier = 1U;
    component.TypeId = 0x4553504F54415052ULL;
    (void)manifest.Components.push_back(component);
    ManifestSignatureDescriptor signature;
    signature.Algorithm = 1U;
    signature.TrustAnchor = 1U;
    signature.TrustPolicy = 1U;
    (void)manifest.SignatureDescriptors.push_back(signature);
    return manifest;
}

using ActivatePolicies = PolicyGateSet<ActivatePolicyDecisionPoint, Capacity::MaximumPolicyProvidersPerDecisionPoint>;
using HealthRegistryType = HealthRegistry<Capacity::MaximumRequiredHealthConditions>;
using TestCoordinator = Coordinator<Capacity, FaultBootControl, FaultTrialBoot, FaultRestart, TestClock>;

struct Fixture final {
    MemoryAtomicRecordStore Store{};
    OTAControlStore<Capacity> Control{Store};
    ArtifactCheckpointStore<Capacity> Checkpoints{Store};
    ArtifactTransferWorkspace<Capacity> Workspace{};
    Primitive::TypeDirectory<9> Directory{};
    OTAStateRuntime<> StateRuntime{};
    OTAStateOwners Owners{};
    FaultBootControl Boot{};
    FaultTrialBoot Trial{};
    FaultRestart Restart{};
    TestClock Clock{};
    ActivatePolicies Policies{};
    AllowActivation Allow{};
    HealthRegistryType Health{};
    RecoveryHandler Handler{};
    ComponentHandlerDirectory<Capacity> Handlers{};
    InertArtifactSource Source{};
    InertArtifactStore ArtifactStore{};
    InertDigest Digest{};
    UpdateTargetProfile<Capacity> TargetProfile{};

    bool InitializeRuntime() {
        if (RegisterOTAStateTypes(Directory) != Primitive::TypeDirectoryRegistrationStatus::Success) return false;
        if (Directory.Initialize() != Primitive::TypeDirectoryInitializationStatus::Success) return false;
        if (!BindOTAStateOwners(StateRuntime, Owners)) return false;
        if (StateRuntime.Initialize(Directory.View(), &CapturedTime) != State::StateRuntimeStatus::Success) return false;
        if (StateRuntime.Start() != State::StateRuntimeStatus::Success) return false;
        if (Policies.Add(Allow) != PolicyGateRegistrationStatus::Success) return false;
        if (Handlers.Register(Handler) != ComponentHandlerDirectoryStatus::Success) return false;
        Handlers.Freeze();
        return ConfigureTargetProfile(TargetProfile);
    }

    TestCoordinator MakeCoordinator() {
        return TestCoordinator{Control, Owners, Boot, Trial, Restart, Clock, Policies, Health, Handlers,
                               Source, ArtifactStore, Checkpoints, Workspace, Digest, 5'000'000'000ULL};
    }

    bool CreateStaged(std::uint8_t id, ActiveTransactionRecord& active) {
        if (Control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return false;
        if (Control.BeginTransaction(ReleaseIdentifier{id}, ManifestIdentifier{Id(id)}, SecurityGeneration{1U}, active) !=
            OTADurableStatus::Success) return false;
        return Control.AdvanceRecoveryPoint(active.Transaction, RecoveryPoint::Staged) == OTADurableStatus::Success;
    }
};

bool LoadActive(Fixture& fixture, OTAControlRecord<Capacity>& record) {
    return fixture.Control.Load(record) == OTADurableStatus::Success && record.HasActiveTransaction;
}

} // namespace

int main() {
    if (!InstallIdentity()) return 1;

#if ESPRESSIO_OTA_PLATFORM_RECOVERY_SCENARIO == 1
    // Activation: BootControl selection failure occurs after ActivationArmed was
    // durably recorded. A fresh Coordinator re-inspects the already-Activated
    // component and can safely retry selection without replaying Activate().
    {
        Fixture fixture;
        if (!fixture.InitializeRuntime()) return 2;
        ActiveTransactionRecord active;
        if (!fixture.CreateStaged(10U, active)) return 3;
        const auto manifest = CandidateManifest(10U);
        if (ValidateManifest(manifest) != ManifestStatus::Success) return 4;
        if (fixture.Control.ArmActivation(active.Transaction, Platform::OTA::BootTargetIdentifier{2U},
                                          Platform::OTA::BootTargetIdentifier{1U}) != OTADurableStatus::Success) return 5;

        fixture.Boot.SelectStatus = Platform::OTA::Status::Failed;
        {
            auto coordinator = fixture.MakeCoordinator();
            if (!coordinator.Initialize() || !coordinator.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 6;
            const auto failed = coordinator.Advance();
            if (failed.Outcome != OutcomeClass::PlatformFailed || fixture.Handler.ActivateCalls != 1U ||
                fixture.Handler.Recovery != ComponentRecoveryState::Activated ||
                fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{1U}) return 7;
        }
        OTAControlRecord<Capacity> record;
        if (!LoadActive(fixture, record) || record.Intent != DurableIntent::ActivationArmed ||
            record.Active.Point != RecoveryPoint::ActivationSelected) return 8;

        fixture.Boot.SelectStatus = Platform::OTA::Status::Success;
        {
            auto recovered = fixture.MakeCoordinator();
            if (!recovered.Initialize() || !recovered.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 9;
            if (recovered.Advance().Outcome != OutcomeClass::Pending ||
                fixture.Handler.ActivateCalls != 1U || fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{2U}) return 10;
        }

        fixture.Restart.Status = Platform::OTA::Status::Failed;
        {
            auto recovered = fixture.MakeCoordinator();
            if (!recovered.Initialize() || !recovered.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 11;
            const auto failed = recovered.Advance();
            if (failed.Outcome != OutcomeClass::PlatformFailed || fixture.Restart.Calls != 1U ||
                fixture.Restart.Last != Platform::OTA::RestartReason::ActivateCandidate) return 12;
        }
        if (!LoadActive(fixture, record) || record.Intent != DurableIntent::ActivationArmed) return 13;

        fixture.Restart.Status = Platform::OTA::Status::Success;
        {
            auto recovered = fixture.MakeCoordinator();
            if (!recovered.Initialize() || !recovered.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 14;
            if (recovered.Advance().Outcome != OutcomeClass::Pending || fixture.Handler.ActivateCalls != 1U) return 15;
        }

        fixture.Boot.Current = Platform::OTA::BootTargetIdentifier{2U};
        fixture.Trial.Trial = true;
        {
            auto trial = fixture.MakeCoordinator();
            if (!trial.Initialize() || !trial.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 16;
            if (trial.Advance().Outcome != OutcomeClass::Pending || trial.Status().Lifecycle != UpdateLifecycle::Trial ||
                fixture.Handler.ActivateCalls != 1U) return 17;
        }
        if (!LoadActive(fixture, record) || record.Active.Point != RecoveryPoint::TrialBootEntered ||
            record.Intent != DurableIntent::None) return 18;
    }

    // A candidate that had entered Trial and was rejected by the bootloader
    // must never be blindly re-armed from recovered ActivationArmed state.
    {
        Fixture fixture;
        if (!fixture.InitializeRuntime()) return 51;
        ActiveTransactionRecord active;
        if (!fixture.CreateStaged(11U, active)) return 52;
        const auto manifest = CandidateManifest(11U);
        if (fixture.Control.ArmActivation(
                active.Transaction,
                Platform::OTA::BootTargetIdentifier{2U},
                Platform::OTA::BootTargetIdentifier{1U}) != OTADurableStatus::Success) return 53;
        fixture.Handler.Recovery = ComponentRecoveryState::Activated;
        fixture.Boot.Current = Platform::OTA::BootTargetIdentifier{1U};
        fixture.Boot.Next = Platform::OTA::BootTargetIdentifier{1U};
        fixture.Trial.CandidateState = Platform::OTA::TrialBootState::Rejected;

        auto coordinator = fixture.MakeCoordinator();
        if (!coordinator.Initialize() ||
            !coordinator.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 54;
        const auto rejected = coordinator.Advance();
        if (rejected.Outcome != OutcomeClass::Failed ||
            rejected.Detail.Reason != static_cast<std::uint32_t>(CoordinatorCoreReason::CandidateTrialRejected) ||
            fixture.Boot.SelectCalls != 0U ||
            fixture.Trial.InspectCalls != 1U) return 55;

        OTAControlRecord<Capacity> record;
        if (!LoadActive(fixture, record) ||
            record.Intent != DurableIntent::RollbackIntent ||
            record.Committed.Generation != UpdateGenerationId{1U}) return 56;

        // Even if next-boot somehow still names the rejected candidate,
        // rollback must normalize it to the previous committed target
        // before durable rollback state can be finalized.
        fixture.Boot.Next = Platform::OTA::BootTargetIdentifier{2U};
        const auto normalize = coordinator.Advance();
        if (normalize.Outcome != OutcomeClass::Pending ||
            fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{1U} ||
            fixture.Boot.SelectCalls != 1U) return 57;
        if (!LoadActive(fixture, record) ||
            record.Intent != DurableIntent::RollbackIntent) return 58;

        if (!coordinator.Advance()) return 59;
        if (fixture.Control.Load(record) != OTADurableStatus::Success ||
            record.HasActiveTransaction ||
            record.Committed.Generation != UpdateGenerationId{1U}) return 60;
    }
#elif ESPRESSIO_OTA_PLATFORM_RECOVERY_SCENARIO == 2
    // Commit: power loss/failure after durable CommitIntent cannot promote the
    // baseline accidentally. Mark-valid failure leaves CommitIntent intact. A
    // reboot after the bootloader was already marked valid is represented by
    // Trial=false with the same durable CommitIntent and remains recoverable.
    {
        Fixture fixture;
        if (!fixture.InitializeRuntime()) return 20;
        ActiveTransactionRecord active;
        if (!fixture.CreateStaged(20U, active)) return 21;
        const auto manifest = CandidateManifest(20U);
        if (fixture.Control.ArmActivation(active.Transaction, Platform::OTA::BootTargetIdentifier{2U},
                                          Platform::OTA::BootTargetIdentifier{1U}) != OTADurableStatus::Success ||
            fixture.Control.MarkTrialEntered(active.Transaction) != OTADurableStatus::Success ||
            fixture.Control.PersistCommitIntent(active.Transaction) != OTADurableStatus::Success) return 22;
        fixture.Handler.Recovery = ComponentRecoveryState::Activated;
        fixture.Boot.Current = Platform::OTA::BootTargetIdentifier{2U};
        fixture.Boot.Next = Platform::OTA::BootTargetIdentifier{2U};
        fixture.Trial.Trial = true;
        fixture.Trial.MarkValidStatus = Platform::OTA::Status::Failed;

        {
            auto coordinator = fixture.MakeCoordinator();
            if (!coordinator.Initialize() || !coordinator.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 23;
            const auto failed = coordinator.Advance();
            if (failed.Outcome != OutcomeClass::PlatformFailed || fixture.Trial.MarkValidCalls != 1U ||
                fixture.Handler.CommitCalls != 0U) return 24;
        }
        OTAControlRecord<Capacity> record;
        if (!LoadActive(fixture, record) || record.Intent != DurableIntent::CommitIntent ||
            record.Committed.Generation != UpdateGenerationId{1U} || record.MinimumAcceptedSecurity != SecurityGeneration{0U}) return 25;

        fixture.Trial.MarkValidStatus = Platform::OTA::Status::Success;
        fixture.Trial.Trial = false;
        {
            auto recovered = fixture.MakeCoordinator();
            if (!recovered.Initialize() || !recovered.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 26;
            if (!recovered.Advance()) return 27;
        }
        if (fixture.Handler.CommitCalls != 1U) return 28;
        if (fixture.Control.Load(record) != OTADurableStatus::Success || record.HasActiveTransaction ||
            record.Committed.Generation != active.CandidateGeneration ||
            record.MinimumAcceptedSecurity != SecurityGeneration{1U}) return 29;
    }
#elif ESPRESSIO_OTA_PLATFORM_RECOVERY_SCENARIO == 3
    // Rollback: each platform failure leaves RollbackIntent durable. Recovery can
    // skip an already-RolledBack component, retry known-good target selection,
    // retry trial invalidation and retry restart without altering the old baseline.
    {
        Fixture fixture;
        if (!fixture.InitializeRuntime()) return 30;
        ActiveTransactionRecord active;
        if (!fixture.CreateStaged(30U, active)) return 31;
        const auto manifest = CandidateManifest(30U);
        if (fixture.Control.ArmActivation(active.Transaction, Platform::OTA::BootTargetIdentifier{2U},
                                          Platform::OTA::BootTargetIdentifier{1U}) != OTADurableStatus::Success ||
            fixture.Control.MarkTrialEntered(active.Transaction) != OTADurableStatus::Success ||
            fixture.Control.PersistRollbackIntent(active.Transaction) != OTADurableStatus::Success) return 32;
        fixture.Handler.Recovery = ComponentRecoveryState::Activated;
        fixture.Boot.Current = Platform::OTA::BootTargetIdentifier{2U};
        fixture.Boot.Next = Platform::OTA::BootTargetIdentifier{2U};
        fixture.Trial.Trial = true;
        fixture.Trial.MarkInvalidStatus = Platform::OTA::Status::Failed;

        // Invalidation of the running Trial candidate is deliberately first.
        // A failure here must leave RollbackIntent durable without selecting a
        // different boot target.
        {
            auto coordinator = fixture.MakeCoordinator();
            if (!coordinator.Initialize() || !coordinator.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 33;
            const auto failed = coordinator.Advance();
            if (failed.Outcome != OutcomeClass::PlatformFailed || fixture.Handler.RollbackCalls != 1U ||
                fixture.Handler.Recovery != ComponentRecoveryState::RolledBack ||
                fixture.Trial.MarkInvalidCalls != 1U ||
                fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{2U}) return 34;
        }
        OTAControlRecord<Capacity> record;
        if (!LoadActive(fixture, record) || record.Intent != DurableIntent::RollbackIntent ||
            record.Committed.Generation != UpdateGenerationId{1U}) return 35;

        fixture.Trial.MarkInvalidStatus = Platform::OTA::Status::Success;
        {
            auto recovered = fixture.MakeCoordinator();
            if (!recovered.Initialize() || !recovered.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 36;
            if (recovered.Advance().Outcome != OutcomeClass::Pending || fixture.Handler.RollbackCalls != 1U ||
                fixture.Trial.Trial || fixture.Trial.MarkInvalidCalls != 2U ||
                fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{2U}) return 37;
        }

        fixture.Boot.SelectStatus = Platform::OTA::Status::Failed;
        {
            auto recovered = fixture.MakeCoordinator();
            if (!recovered.Initialize() || !recovered.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 38;
            const auto failed = recovered.Advance();
            if (failed.Outcome != OutcomeClass::PlatformFailed ||
                fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{2U} ||
                fixture.Boot.SelectCalls != 1U) return 39;
        }
        if (!LoadActive(fixture, record) || record.Intent != DurableIntent::RollbackIntent) return 40;

        fixture.Boot.SelectStatus = Platform::OTA::Status::Success;
        {
            auto recovered = fixture.MakeCoordinator();
            if (!recovered.Initialize() || !recovered.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 41;
            if (recovered.Advance().Outcome != OutcomeClass::Pending ||
                fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{1U} ||
                fixture.Boot.SelectCalls != 2U) return 42;
        }

        fixture.Restart.Status = Platform::OTA::Status::Failed;
        {
            auto recovered = fixture.MakeCoordinator();
            if (!recovered.Initialize() || !recovered.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 43;
            const auto failed = recovered.Advance();
            if (failed.Outcome != OutcomeClass::PlatformFailed || fixture.Restart.Last != Platform::OTA::RestartReason::Rollback) return 44;
        }
        if (!LoadActive(fixture, record) || record.Intent != DurableIntent::RollbackIntent) return 45;

        fixture.Restart.Status = Platform::OTA::Status::Success;
        {
            auto recovered = fixture.MakeCoordinator();
            if (!recovered.Initialize() || !recovered.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 46;
            if (recovered.Advance().Outcome != OutcomeClass::Pending) return 47;
        }

        fixture.Boot.Current = Platform::OTA::BootTargetIdentifier{1U};
        fixture.Boot.Next = Platform::OTA::BootTargetIdentifier{1U};
        {
            auto recovered = fixture.MakeCoordinator();
            if (!recovered.Initialize() || !recovered.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 48;
            if (!recovered.Advance()) return 49;
        }
        if (fixture.Control.Load(record) != OTADurableStatus::Success || record.HasActiveTransaction ||
            record.Committed.Generation != UpdateGenerationId{1U} ||
            record.MinimumAcceptedSecurity != SecurityGeneration{0U}) return 50;
    }
#else
#error "Unknown ESPRESSIO_OTA_PLATFORM_RECOVERY_SCENARIO"
#endif

    return 0;
}
