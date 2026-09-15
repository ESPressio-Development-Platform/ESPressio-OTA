#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTA.hpp"
#include <ESPressio_RuntimeIdentity.hpp>

using namespace ESPressio;
using namespace ESPressio::OTA;

struct CoordinatorSourceSelectionTestComponent {};

namespace ESPressio::OTA {
template<>
struct ComponentTypeTraits<::CoordinatorSourceSelectionTestComponent> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {
            ComponentTypeId{0x4553504F54415301ULL},
            "ota.test.source-selection",
            ComponentKind::ApplicationFirmware,
            ComponentMultiplicity::Single,
            1U
        };
    }
};
} // namespace ESPressio::OTA

namespace {

using Capacity = ConstrainedV1CapacityProfile;

class TestHandler final : public ComponentHandler<CoordinatorSourceSelectionTestComponent, Capacity> {
public:
    ComponentActionResult Prepare(const ComponentPreflightContext<Capacity>& context) noexcept override {
        return context.IsValid() ? ComponentActionResult::Complete()
                                 : ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 1U}});
    }
    ComponentActionResult Stage(const ComponentExecutionContext<Capacity>& context) noexcept override {
        return context.IsValid() ? ComponentActionResult::Complete()
                                 : ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 2U}});
    }
    ComponentActionResult FinalizeStage(const ComponentExecutionContext<Capacity>& context) noexcept override {
        return context.IsValid() ? ComponentActionResult::Complete()
                                 : ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 3U}});
    }
    ComponentActionResult Activate(const ComponentExecutionContext<Capacity>& context) noexcept override {
        return context.IsValid() ? ComponentActionResult::Complete()
                                 : ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 4U}});
    }
    ComponentActionResult Commit(const ComponentExecutionContext<Capacity>& context) noexcept override {
        return context.IsValid() ? ComponentActionResult::Complete()
                                 : ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 5U}});
    }
    ComponentActionResult Rollback(const ComponentExecutionContext<Capacity>& context) noexcept override {
        return context.IsValid() ? ComponentActionResult::Complete()
                                 : ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 6U}});
    }
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
    bytes[0] = 0x72U;
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

bool ConfigureTargetProfile(UpdateTargetProfile<Capacity>& profile) {
    if (profile.SetSystemIdentity(
            System::ProductTypeIdentifier{1U},
            System::HardwareFamilyIdentifier{2U},
            System::HardwareRevision{1U},
            System::ArchitectureIdentifier{3U},
            System::SoftwareVariantIdentifier{4U}) != TargetProfileStatus::Success) return false;
    if (profile.SetStorageLayout(
            Platform::OTA::StorageLayoutIdentifier{5U},
            Platform::OTA::StorageLayoutGeneration{1U}) != TargetProfileStatus::Success) return false;
    if (profile.SetPersistenceSchema(
            Persistence::SchemaIdentifier{6U},
            Persistence::SchemaGeneration{1U}) != TargetProfileStatus::Success) return false;
    if (profile.SetOTASupport(OTAProtocolV1, 0U) != TargetProfileStatus::Success) return false;
    if (profile.AddSupportedComponentType(ComponentTypeId{0x4553504F54415301ULL}) != TargetProfileStatus::Success) return false;
    std::array<std::uint8_t, 32> fingerprint{};
    fingerprint[0] = 1U;
    return profile.SetFingerprintAndFreeze(UpdateTargetProfileFingerprint{fingerprint}) == TargetProfileStatus::Success;
}

class AtomicStore final : public Persistence::IAtomicRecordStore {
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

class ControlledSource final : public IArtifactSource {
public:
    std::array<std::uint8_t, 32> Data{};
    std::size_t Size{0U};
    std::size_t Position{0U};
    std::size_t Chunk{2U};
    std::size_t OpenCalls{0U};
    std::size_t CloseCalls{0U};
    bool FailReads{false};

    Result Open(const ArtifactSourceOpenRequest& request) noexcept override {
        ++OpenCalls;
        Position = 0U;
        if (!request.IsValid() || request.Offset != 0U || request.ExpectedLength != Size) {
            return {OutcomeClass::Invalid, {DiagnosticDomain::Source, 1U}};
        }
        return Result::Success();
    }

    StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (FailReads) {
            return StreamReadResult::Failed(
                {OutcomeClass::Failed, {DiagnosticDomain::Source, 2U}});
        }
        if (Position == Size) return StreamReadResult::End();
        const auto bytes = std::min({Chunk, Size - Position, capacity});
        for (std::size_t i = 0U; i < bytes; ++i) output[i] = Data[Position + i];
        Position += bytes;
        return StreamReadResult::Data(bytes);
    }

    void Close() noexcept override { ++CloseCalls; }
};

class ArtifactStore final : public IReadableArtifactStore {
public:
    std::array<std::uint8_t, 64> Bytes{};
    ArtifactIdentifier StoredId{};
    ArtifactIdentifier WorkingId{};
    std::size_t Size{0U};
    std::size_t ReadPosition{0U};
    std::size_t BeginCalls{0U};
    std::size_t FinalizeCalls{0U};
    std::size_t AbortCalls{0U};

    Result BeginWrite(const ArtifactStoreOpenRequest& request) noexcept override {
        ++BeginCalls;
        Size = 0U;
        WorkingId = request.Identifier;
        return request.IsValid() ? Result::Success()
                                 : Result{OutcomeClass::Invalid, {DiagnosticDomain::Store, 1U}};
    }
    StreamWriteResult Write(const std::uint8_t* data, std::size_t size) noexcept override {
        if (data == nullptr || size == 0U || Size + size > Bytes.size()) {
            return StreamWriteResult::Failed({OutcomeClass::Failed, {DiagnosticDomain::Store, 2U}});
        }
        for (std::size_t i = 0U; i < size; ++i) Bytes[Size + i] = data[i];
        Size += size;
        return StreamWriteResult::Accepted(size);
    }
    ArtifactStoreFinalizeResult Finalize() noexcept override {
        ++FinalizeCalls;
        StoredId = WorkingId;
        return {ArtifactStoreFinalizeStatus::Stored, Result::Success()};
    }
    void Abort() noexcept override { ++AbortCalls; }
    Result QueryAvailableBytes(std::uint64_t& availableBytes) const noexcept override {
        availableBytes = Bytes.size() - Size;
        return Result::Success();
    }
    Result OpenRead(const ArtifactStoreReadRequest& request) noexcept override {
        if (!request.IsValid() || request.Identifier != StoredId || request.ExpectedLength != Size) {
            return {OutcomeClass::VerificationFailed, {DiagnosticDomain::Store, 3U}};
        }
        ReadPosition = 0U;
        return Result::Success();
    }
    StreamReadResult ReadStored(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (output == nullptr || capacity == 0U) {
            return StreamReadResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Store, 4U}});
        }
        if (ReadPosition == Size) return StreamReadResult::End();
        const auto count = std::min(capacity, Size - ReadPosition);
        for (std::size_t i = 0U; i < count; ++i) output[i] = Bytes[ReadPosition + i];
        ReadPosition += count;
        return StreamReadResult::Data(count);
    }
    void CloseRead() noexcept override {}
};

class DigestVerifier final : public Security::IStreamingDigestVerifier {
public:
    bool Supports(Security::DigestAlgorithmIdentifier algorithm) const noexcept override {
        return algorithm == Security::DigestAlgorithm::SHA256;
    }
    std::size_t DigestSize(Security::DigestAlgorithmIdentifier algorithm) const noexcept override {
        return Supports(algorithm) ? 32U : 0U;
    }
    Security::VerificationResult Begin(Security::DigestAlgorithmIdentifier algorithm) noexcept override {
        return Supports(algorithm)
            ? Security::VerificationResult::Ok()
            : Security::VerificationResult{Security::VerificationStatus::UnsupportedAlgorithm, 0};
    }
    Security::VerificationResult Update(Security::ByteView bytes) noexcept override {
        return bytes.IsValid()
            ? Security::VerificationResult::Ok()
            : Security::VerificationResult{Security::VerificationStatus::InvalidArgument, 0};
    }
    Security::VerificationResult VerifyFinal(Security::ByteView expectedDigest) noexcept override {
        return expectedDigest.IsValid() && expectedDigest.Size == 32U
            ? Security::VerificationResult::Ok()
            : Security::VerificationResult{Security::VerificationStatus::InvalidArgument, 0};
    }
};

struct BootBackend final : Platform::Backend {};
struct TrialBackend final : Platform::Backend {};
struct RestartBackend final : Platform::Backend {};
struct ClockBackend final : Platform::Backend {};

class BootControl final : public Platform::ProviderDeclaration<
    BootBackend, Platform::CapabilitySet<Platform::Capability::BootControl>> {
public:
    Platform::OTA::BootTargetIdentifier CurrentBootTarget() const noexcept { return {1U}; }
    Platform::OTA::BootTargetIdentifier CommittedBootTarget() const noexcept { return {1U}; }
    Platform::OTA::BootTargetIdentifier NextBootTarget() const noexcept { return {1U}; }
    Platform::OTA::Result SelectNextBootTarget(Platform::OTA::BootTargetIdentifier) noexcept {
        return {Platform::OTA::Status::Success, 0};
    }
};

class TrialBoot final : public Platform::ProviderDeclaration<
    TrialBackend, Platform::CapabilitySet<Platform::Capability::TrialBoot>> {
public:
    bool IsCurrentBootTrial() const noexcept { return false; }
    Platform::OTA::Result MarkCurrentBootValid() noexcept { return {Platform::OTA::Status::Success, 0}; }
    Platform::OTA::Result MarkCurrentBootInvalid() noexcept { return {Platform::OTA::Status::Success, 0}; }
};

class Restart final : public Platform::ProviderDeclaration<
    RestartBackend, Platform::CapabilitySet<Platform::Capability::SystemRestart>> {
public:
    Platform::OTA::Result Restart(Platform::OTA::RestartReason) noexcept {
        return {Platform::OTA::Status::Success, 0};
    }
};

class Clock final : public Platform::Clock::MonotonicProviderDeclaration<ClockBackend, 1'000'000ULL, 64U> {
public:
    Platform::Clock::Tick Now() const noexcept { return 0U; }
};

class SourceSelector final : public IArtifactSourceSelector {
    IArtifactSource& selected_;
public:
    std::size_t Calls{0U};
    ArtifactSourceSelectionContext Last{};

    explicit SourceSelector(IArtifactSource& selected) noexcept : selected_(selected) {}

    Result Select(const ArtifactSourceSelectionContext& context,
                  ArtifactSourceSelection& selection) noexcept override {
        ++Calls;
        Last = context;
        if (!context.IsValid()) {
            return {OutcomeClass::Invalid, {DiagnosticDomain::Source, 10U}};
        }
        selection.Source = &selected_;
        selection.OffsetRead = false;
        return Result::Success();
    }
};

CommittedBaseline FactoryBaseline() noexcept {
    CommittedBaseline baseline;
    baseline.Generation = UpdateGenerationId{1U};
    baseline.Security = SecurityGeneration{0U};
    return baseline;
}

Manifest<Capacity> CandidateManifest(std::uint8_t id, std::uint64_t release) {
    Manifest<Capacity> manifest;
    manifest.Identifier = Id(id);
    manifest.Release = release;
    manifest.SecurityGeneration = 1U;
    (void)manifest.TargetClauses.push_back(ManifestTargetClause<Capacity>{});

    ManifestComponent<Capacity> component;
    component.Identifier = 1U;
    component.TypeId = 0x4553504F54415301ULL;
    const auto artifactId = Id(static_cast<std::uint8_t>(id + 1U));
    (void)component.Artifacts.push_back(artifactId);
    (void)manifest.Components.push_back(component);

    ManifestArtifact<Capacity> artifact;
    artifact.Identifier = artifactId;
    artifact.ExpectedLength = 6U;
    artifact.DigestAlgorithm = Security::DigestAlgorithm::SHA256.Value();
    for (std::size_t i = 0U; i < 32U; ++i) {
        (void)artifact.Digest.push_back(static_cast<std::uint8_t>(i + 1U));
    }
    (void)manifest.Artifacts.push_back(artifact);

    ManifestSignatureDescriptor signature;
    signature.Algorithm = 1U;
    signature.TrustAnchor = 1U;
    signature.TrustPolicy = 1U;
    (void)manifest.SignatureDescriptors.push_back(signature);
    return manifest;
}

using ActivatePolicies = PolicyGateSet<ActivatePolicyDecisionPoint, Capacity::MaximumPolicyProvidersPerDecisionPoint>;
using HealthRegistryType = HealthRegistry<Capacity::MaximumRequiredHealthConditions>;
using TestCoordinator = Coordinator<Capacity, BootControl, TrialBoot, Restart, Clock>;

struct Fixture final {
    AtomicStore Store{};
    OTAControlStore<Capacity> Control{Store};
    ArtifactCheckpointStore<Capacity> Checkpoints{Store};
    ArtifactTransferWorkspace<Capacity> Workspace{};
    Primitive::TypeDirectory<9> Directory{};
    OTAStateRuntime<> StateRuntime{};
    OTAStateOwners Owners{};
    BootControl Boot{};
    TrialBoot Trial{};
    Restart Restarter{};
    Clock MonotonicClock{};
    ActivatePolicies Policies{};
    HealthRegistryType Health{};
    TestHandler Handler{};
    ComponentHandlerDirectory<Capacity> Handlers{};
    ControlledSource Primary{};
    ArtifactStore Retained{};
    DigestVerifier Digest{};
    UpdateTargetProfile<Capacity> TargetProfile{};

    bool InitializeState() {
        if (RegisterOTAStateTypes(Directory) != Primitive::TypeDirectoryRegistrationStatus::Success) return false;
        if (Directory.Initialize() != Primitive::TypeDirectoryInitializationStatus::Success) return false;
        if (!BindOTAStateOwners(StateRuntime, Owners)) return false;
        if (StateRuntime.Initialize(Directory.View(), &CapturedTime) != State::StateRuntimeStatus::Success) return false;
        if (StateRuntime.Start() != State::StateRuntimeStatus::Success) return false;
        if (Handlers.Register(Handler) != ComponentHandlerDirectoryStatus::Success) return false;
        Handlers.Freeze();
        return ConfigureTargetProfile(TargetProfile);
    }

    TestCoordinator MakeCoordinator() {
        return TestCoordinator{Control, Owners, Boot, Trial, Restarter, MonotonicClock,
                               Policies, Health, Handlers, Primary, Retained,
                               Checkpoints, Workspace, Digest, 5'000'000'000ULL};
    }
};

bool DriveToArtifactsAcquired(TestCoordinator& coordinator,
                              OTAControlStore<Capacity>& control,
                              UpdateTransactionId transaction,
                              std::size_t maximumSteps = 64U) {
    for (std::size_t step = 0U; step < maximumSteps; ++step) {
        const auto result = coordinator.Advance();
        if (result.Outcome != OutcomeClass::Pending && result.Outcome != OutcomeClass::Deferred) return false;
        OTAControlRecord<Capacity> record;
        if (control.Load(record) != OTADurableStatus::Success || !record.HasActiveTransaction ||
            record.Active.Transaction != transaction) return false;
        if (record.Active.Point == RecoveryPoint::ArtifactsAcquired) return true;
    }
    return false;
}

} // namespace

#ifndef ESPRESSIO_OTA_SOURCE_SELECTION_SCENARIO
#define ESPRESSIO_OTA_SOURCE_SELECTION_SCENARIO 1
#endif

int main() {
    if (!InstallIdentity()) return 1;

#if ESPRESSIO_OTA_SOURCE_SELECTION_SCENARIO == 1
    Fixture fixture;
    if (!fixture.InitializeState()) return 2;
    if (fixture.Control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 3;

    fixture.Primary.Size = 6U;
    fixture.Primary.FailReads = true;
    ControlledSource replacement;
    replacement.Size = 6U;
    for (std::size_t i = 0U; i < replacement.Size; ++i) {
        replacement.Data[i] = static_cast<std::uint8_t>(0x80U + i);
    }
    SourceSelector selector{replacement};

    auto coordinator = fixture.MakeCoordinator();
    if (!coordinator.Initialize()) return 4;
    if (!coordinator.ConfigureArtifactSourceSelection(selector, 2U)) return 5;
    const auto manifest = CandidateManifest(70U, 70U);
    if (ValidateManifest(manifest) != ManifestStatus::Success) return 6;
    UpdateTransactionId transaction;
    if (!coordinator.Start({ReleaseIdentifier{70U}, ManifestIdentifier{Id(70U)}, SecurityGeneration{1U}}, transaction)) return 7;
    if (!coordinator.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 8;
    if (!DriveToArtifactsAcquired(coordinator, fixture.Control, transaction)) return 9;

    if (selector.Calls != 1U || selector.Last.Attempt != 2U ||
        !selector.Last.HasPreviousFailure || selector.Last.AcceptedCheckpointPrefix != 0U) return 10;
    if (fixture.Primary.OpenCalls != 1U || fixture.Primary.CloseCalls != 1U ||
        replacement.OpenCalls != 1U || replacement.CloseCalls != 1U) return 11;
    if (fixture.Retained.Size != replacement.Size || fixture.Retained.AbortCalls == 0U) return 12;
    for (std::size_t i = 0U; i < replacement.Size; ++i) {
        if (fixture.Retained.Bytes[i] != replacement.Data[i]) return 13;
    }
    if (!coordinator.Cancel(transaction) || coordinator.Status().HasActiveTransaction) return 14;

#elif ESPRESSIO_OTA_SOURCE_SELECTION_SCENARIO == 2
    Fixture fixture;
    if (!fixture.InitializeState()) return 20;
    if (fixture.Control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 21;

    fixture.Primary.Size = 6U;
    fixture.Primary.FailReads = true;
    SourceSelector selector{fixture.Primary};

    auto coordinator = fixture.MakeCoordinator();
    if (!coordinator.Initialize()) return 22;
    if (!coordinator.ConfigureArtifactSourceSelection(selector, 2U)) return 23;
    const auto manifest = CandidateManifest(80U, 80U);
    if (ValidateManifest(manifest) != ManifestStatus::Success) return 24;
    UpdateTransactionId transaction;
    if (!coordinator.Start({ReleaseIdentifier{80U}, ManifestIdentifier{Id(80U)}, SecurityGeneration{1U}}, transaction)) return 25;
    if (!coordinator.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 26;

    Result terminal{OutcomeClass::Pending, {}};
    bool terminated = false;
    for (std::size_t step = 0U; step < 32U; ++step) {
        terminal = coordinator.Advance();
        if (!coordinator.Status().HasActiveTransaction) {
            terminated = true;
            break;
        }
        if (terminal.Outcome != OutcomeClass::Pending && terminal.Outcome != OutcomeClass::Deferred) return 27;
    }
    if (!terminated) return 28;
    if (terminal.Outcome != OutcomeClass::Unavailable ||
        terminal.Detail.Domain != DiagnosticDomain::OTA ||
        terminal.Detail.Reason != static_cast<std::uint32_t>(CoordinatorCoreReason::ArtifactSourceRetryExhausted)) return 29;
    if (selector.Calls != 1U || selector.Last.Attempt != 2U || fixture.Primary.OpenCalls != 2U) return 30;
    if (coordinator.Status().Availability != CoordinatorAvailability::Ready ||
        coordinator.Status().HasActiveTransaction) return 31;

#else
#error Unsupported ESPRESSIO_OTA_SOURCE_SELECTION_SCENARIO
#endif

    return 0;
}
