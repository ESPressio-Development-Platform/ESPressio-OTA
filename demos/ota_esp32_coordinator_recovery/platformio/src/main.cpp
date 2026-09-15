#include <Arduino.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_timer.h>

#include <ESPressio_OTA.hpp>
#include <ESPressio_RuntimeIdentity.hpp>
#include <ESPressio_Platform_IDFClock.hpp>
#include <ESPressio_Platform_IDFOTA.hpp>
#include <ESPressio_NVSAtomicRecordStore.hpp>

// ESPressio OTA full-Coordinator physical recovery laboratory.
//
// This demo deliberately separates two concerns:
//  1. the inactive application image is prepared by cloning the currently-running
//     valid image through the real ESP-IDF staging provider;
//  2. a zero-Artifact, stateless Component lets the real Coordinator exercise its
//     durable control/activation/trial/commit/rollback state machine without
//     pretending that the clone operation was Artifact acquisition.
//
// The Manifest is a fixed build-time laboratory fixture. It is intentionally
// supplied directly to BindVerifiedManifest() so this harness tests Coordinator
// recovery rather than Manifest-signature verification. It MUST NOT be used as
// evidence that the Security/trust path was exercised.

struct CoordinatorRecoveryLabComponent {};

namespace ESPressio::OTA {
template<>
struct ComponentTypeTraits<::CoordinatorRecoveryLabComponent> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {
            ComponentTypeId{0x4553504F54414C01ULL},
            "ota.lab.recovery.control",
            ComponentKind::PlatformSpecific,
            ComponentMultiplicity::Single,
            1U
        };
    }
};
} // namespace ESPressio::OTA

namespace {

using namespace ESPressio;
using namespace ESPressio::OTA;
using Capacity = ConstrainedV1CapacityProfile;
using BootControl = Platform::IDF::OTABootControl;
using TrialBoot = Platform::IDF::OTATrialBoot;
using Restart = Platform::IDF::OTASystemRestart;
using Clock = Platform::IDF::EspTimerClock;
using ActivatePolicies = PolicyGateSet<
    ActivatePolicyDecisionPoint,
    Capacity::MaximumPolicyProvidersPerDecisionPoint>;
using HealthRegistryType = HealthRegistry<Capacity::MaximumRequiredHealthConditions>;
using CoordinatorType = Coordinator<Capacity, BootControl, TrialBoot, Restart, Clock>;
using DurableStore = Persistence::NVSAtomicRecordStore<
    Capacity::MaximumOTAControlRecordBytes,
    1U>;

constexpr std::size_t CloneChunkBytes = 4096U;
constexpr char DurableNamespace[] = "ota_coord_lab";
constexpr std::uint64_t TrialTimeoutNanoseconds = 60ULL * 1000ULL * 1000ULL * 1000ULL;

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

constexpr std::array<Persistence::AtomicRecordKey, 1U> DurableKeys = [] {
    std::array<Persistence::AtomicRecordKey, 1U> keys{};
    keys[0] = OTAControlStore<Capacity>::RecordKey();
    return keys;
}();

Timing::QualifiedTime CapturedTime() {
    return {
        static_cast<std::uint64_t>(::esp_timer_get_time()) * 1000ULL,
        Timing::TimeReliability::Synchronized
    };
}

bool InstallLabRuntimeIdentity() {
    System::DeviceIdentifier::Storage bytes{};
    bytes[0] = 0x4FU;
    bytes[1] = 0x54U;
    bytes[2] = 0x41U;
    bytes[3] = 0x4CU;
    bytes[4] = 0x41U;
    bytes[5] = 0x42U;
    const System::DeviceRuntimeIdentity expected{
        System::DeviceIdentifier{bytes},
        System::RuntimeIncarnationId{1U}};
    const auto status = System::RuntimeIdentity::Install(expected);
    if (status == System::RuntimeIdentity::InstallationStatus::Success) return true;
    if (status != System::RuntimeIdentity::InstallationStatus::AlreadyInstalled) return false;
    System::DeviceRuntimeIdentity installed{};
    return System::RuntimeIdentity::TryRead(installed) && installed == expected;
}

class StatelessLabHandler final
    : public ComponentHandler<CoordinatorRecoveryLabComponent, Capacity> {
public:
    Result Preflight(const ComponentPreflightContext<Capacity>& context) noexcept override {
        return context.IsValid()
            ? Result::Success()
            : Result{OutcomeClass::Invalid, {DiagnosticDomain::Component, 0x4C01U, 0, {}, 0U}};
    }

    ComponentRecoveryInspection InspectRecoveryState(
        const ComponentPreflightContext<Capacity>& context) noexcept override {
        if (!context.IsValid()) {
            return {
                ComponentRecoveryState::Inconsistent,
                {OutcomeClass::Invalid, {DiagnosticDomain::Component, 0x4C02U, 0, {}, 0U}}
            };
        }
        // The laboratory Component owns no external mutable resource. Its staged
        // condition is therefore permanently satisfied and every post-stage hook
        // is an idempotent no-op. Reporting Staged is truthful across reset/power loss.
        return {ComponentRecoveryState::Staged, Result::Success()};
    }
};

class UnusedArtifactSource final : public IArtifactSource {
public:
    Result Open(const ArtifactSourceOpenRequest&) noexcept override {
        return {OutcomeClass::Unsupported, {DiagnosticDomain::Source, 0x4C10U, 0, {}, 0U}};
    }
    StreamReadResult Read(std::uint8_t*, std::size_t) noexcept override {
        return StreamReadResult::Failed(
            {OutcomeClass::Unsupported, {DiagnosticDomain::Source, 0x4C11U, 0, {}, 0U}});
    }
    void Close() noexcept override {}
};

class UnusedArtifactStore final : public IReadableArtifactStore {
public:
    Result BeginWrite(const ArtifactStoreOpenRequest&) noexcept override {
        return {OutcomeClass::Unsupported, {DiagnosticDomain::Store, 0x4C20U, 0, {}, 0U}};
    }
    StreamWriteResult Write(const std::uint8_t*, std::size_t) noexcept override {
        return StreamWriteResult::Failed(
            {OutcomeClass::Unsupported, {DiagnosticDomain::Store, 0x4C21U, 0, {}, 0U}});
    }
    ArtifactStoreFinalizeResult Finalize() noexcept override {
        return {
            ArtifactStoreFinalizeStatus::Failed,
            {OutcomeClass::Unsupported, {DiagnosticDomain::Store, 0x4C22U, 0, {}, 0U}}
        };
    }
    void Abort() noexcept override {}
    Result QueryAvailableBytes(std::uint64_t& availableBytes) const noexcept override {
        availableBytes = 0U;
        return Result::Success();
    }
    Result OpenRead(const ArtifactStoreReadRequest&) noexcept override {
        return {OutcomeClass::Unsupported, {DiagnosticDomain::Store, 0x4C23U, 0, {}, 0U}};
    }
    StreamReadResult ReadStored(std::uint8_t*, std::size_t) noexcept override {
        return StreamReadResult::Failed(
            {OutcomeClass::Unsupported, {DiagnosticDomain::Store, 0x4C24U, 0, {}, 0U}});
    }
    void CloseRead() noexcept override {}
};

class UnusedDigestVerifier final : public Security::IStreamingDigestVerifier {
public:
    bool Supports(Security::DigestAlgorithmIdentifier) const noexcept override { return false; }
    std::size_t DigestSize(Security::DigestAlgorithmIdentifier) const noexcept override { return 0U; }
    Security::VerificationResult Begin(Security::DigestAlgorithmIdentifier) noexcept override {
        return {Security::VerificationStatus::UnsupportedAlgorithm, 0};
    }
    Security::VerificationResult Update(Security::ByteView) noexcept override {
        return {Security::VerificationStatus::UnsupportedAlgorithm, 0};
    }
    Security::VerificationResult VerifyFinal(Security::ByteView) noexcept override {
        return {Security::VerificationStatus::UnsupportedAlgorithm, 0};
    }
};

class Lab final {
public:
    DurableStore Durable{DurableKeys, DurableNamespace};
    OTAControlStore<Capacity> Control{Durable};
    ArtifactCheckpointStore<Capacity> Checkpoints{Durable};
    ArtifactTransferWorkspace<Capacity> Workspace{};

    Primitive::TypeDirectory<9U> StateDirectory{};
    OTAStateRuntime<> StateRuntime{};
    OTAStateOwners StateOwners{};

    BootControl Boot{};
    TrialBoot Trial{};
    Restart SystemRestart{};
    Clock MonotonicClock{};
    Platform::IDF::OTAApplicationImageStaging ImageStaging{};
    Platform::IDF::OTAStorageLayoutInspection LayoutInspection{};

    ActivatePolicies ActivationPolicies{};
    HealthRegistryType Health{};
    StatelessLabHandler Handler{};
    ComponentHandlerDirectory<Capacity> Handlers{};
    UnusedArtifactSource ArtifactSource{};
    UnusedArtifactStore ArtifactStore{};
    UnusedDigestVerifier Digest{};
    UpdateTargetProfile<Capacity> TargetProfile{};
    Manifest<Capacity> ManifestFixture{};

    CoordinatorType CoordinatorCore{
        Control,
        StateOwners,
        Boot,
        Trial,
        SystemRestart,
        MonotonicClock,
        ActivationPolicies,
        Health,
        Handlers,
        ArtifactSource,
        ArtifactStore,
        Checkpoints,
        Workspace,
        Digest,
        TrialTimeoutNanoseconds,
        0U};

    enum class CloneState : std::uint8_t {
        Idle,
        Writing,
        Complete,
        Failed
    };

    CloneState Clone{CloneState::Idle};
    const esp_partition_t* RunningPartition{nullptr};
    std::uint64_t CloneLength{0U};
    std::uint64_t CloneOffset{0U};
    Platform::OTA::BootTargetIdentifier ClonedTarget{};
    std::array<std::uint8_t, CloneChunkBytes> CloneBuffer{};
    bool Initialized{false};

    static const char* CloneName(CloneState state) noexcept {
        switch (state) {
            case CloneState::Idle: return "idle";
            case CloneState::Writing: return "writing";
            case CloneState::Complete: return "complete";
            case CloneState::Failed: return "failed";
        }
        return "unknown";
    }

    bool ConfigureManifest() {
        ManifestFixture = {};
        ManifestFixture.Identifier = Id(0x51U);
        ManifestFixture.Release = 0x20260915ULL;
        ManifestFixture.SecurityGeneration = 1U;
        if (!ManifestFixture.TargetClauses.push_back(ManifestTargetClause<Capacity>{})) return false;

        ManifestComponent<Capacity> component;
        component.Identifier = 1U;
        component.TypeId = ComponentTypeTraits<CoordinatorRecoveryLabComponent>::Describe().TypeId.Value();
        if (!ManifestFixture.Components.push_back(component)) return false;

        ManifestSignatureDescriptor signature;
        signature.Algorithm = 1U;
        signature.TrustAnchor = 1U;
        signature.TrustPolicy = 1U;
        if (!ManifestFixture.SignatureDescriptors.push_back(signature)) return false;

        return PrepareManifestForSigning(ManifestFixture) == ManifestStatus::Success;
    }

    bool ConfigureTargetProfile() {
        Platform::OTA::StorageLayoutInfo layout{};
        if (!LayoutInspection.InspectStorageLayout(layout)) return false;
        if (TargetProfile.SetSystemIdentity(
                System::ProductTypeIdentifier{0x4F54414C41420001ULL},
                System::HardwareFamilyIdentifier{0x4553503332000001ULL},
                System::HardwareRevision{1U},
                System::ArchitectureIdentifier{0x5854454E53410001ULL},
                System::SoftwareVariantIdentifier{0x4F54414C41420001ULL}) != TargetProfileStatus::Success) return false;
        if (TargetProfile.SetStorageLayout(
                layout.Layout,
                Platform::OTA::StorageLayoutGeneration{1U}) != TargetProfileStatus::Success) return false;
        if (TargetProfile.SetPersistenceSchema(
                Persistence::SchemaIdentifier{0x4553504F54415031ULL},
                Persistence::SchemaGeneration{1U}) != TargetProfileStatus::Success) return false;
        if (TargetProfile.SetOTASupport(OTAProtocolV1, 0U) != TargetProfileStatus::Success) return false;
        if (TargetProfile.AddSupportedComponentType(
                ComponentTypeTraits<CoordinatorRecoveryLabComponent>::Describe().TypeId) !=
            TargetProfileStatus::Success) return false;
        std::array<std::uint8_t, 32U> fingerprint{};
        fingerprint[0] = 0x4FU;
        fingerprint[1] = 0x54U;
        fingerprint[2] = 0x41U;
        fingerprint[3] = 0x31U;
        return TargetProfile.SetFingerprintAndFreeze(UpdateTargetProfileFingerprint{fingerprint}) ==
               TargetProfileStatus::Success;
    }

    bool InitializeState() {
        if (!InstallLabRuntimeIdentity()) return false;
        if (RegisterOTAStateTypes(StateDirectory) !=
            Primitive::TypeDirectoryRegistrationStatus::Success) return false;
        if (StateDirectory.Initialize() !=
            Primitive::TypeDirectoryInitializationStatus::Success) return false;
        if (!BindOTAStateOwners(StateRuntime, StateOwners)) return false;
        if (StateRuntime.Initialize(StateDirectory.View(), &CapturedTime) !=
            State::StateRuntimeStatus::Success) return false;
        return StateRuntime.Start() == State::StateRuntimeStatus::Success;
    }

    bool ProvisionBaseline() {
        CommittedBaseline baseline;
        baseline.Generation = UpdateGenerationId{1U};
        baseline.Security = SecurityGeneration{0U};
        const auto status = Control.ProvisionBaseline(baseline, SecurityGeneration{0U});
        return status == OTADurableStatus::Success || status == OTADurableStatus::AlreadyProvisioned;
    }

    bool Initialize() {
        if (!ConfigureManifest() || !ConfigureTargetProfile()) return false;
        if (Handlers.Register(Handler) != ComponentHandlerDirectoryStatus::Success) return false;
        Handlers.Freeze();
        if (!InitializeState()) return false;
        if (!ProvisionBaseline()) return false;

        const auto initialized = CoordinatorCore.Initialize();
        if (!initialized) {
            PrintResult("Coordinator.Initialize", initialized);
            return false;
        }

        if (CoordinatorCore.Status().HasActiveTransaction) {
            const auto rebound = CoordinatorCore.BindVerifiedManifest(ManifestFixture, TargetProfile);
            if (!rebound) {
                PrintResult("Coordinator.BindVerifiedManifest(recovery)", rebound);
                return false;
            }
        }
        Initialized = true;
        return true;
    }

    static void PrintResult(const char* operation, const Result& result) {
        Serial.printf(
            "%s: outcome=%u domain=%u reason=%lu native=%ld\n",
            operation,
            static_cast<unsigned>(result.Outcome),
            static_cast<unsigned>(result.Detail.Domain),
            static_cast<unsigned long>(result.Detail.Reason),
            static_cast<long>(result.Detail.NativeCode));
    }

    void PrintStatus() {
        const auto status = CoordinatorCore.Status();
        Serial.printf(
            "Coordinator: availability=%u active=%u tx=%llu candidateGen=%llu committedGen=%llu recovery=%u intent=%u lifecycle=%u boundManifest=%u plan=%u\n",
            static_cast<unsigned>(status.Availability),
            status.HasActiveTransaction ? 1U : 0U,
            static_cast<unsigned long long>(status.Transaction.Value()),
            static_cast<unsigned long long>(status.CandidateGeneration.Value()),
            static_cast<unsigned long long>(status.CommittedGeneration.Value()),
            static_cast<unsigned>(status.DurableRecoveryPoint),
            static_cast<unsigned>(status.Intent),
            static_cast<unsigned>(status.Lifecycle),
            status.VerifiedManifestBound ? 1U : 0U,
            status.UpdatePlanReady ? 1U : 0U);

        OTAControlRecord<Capacity> durable{};
        const auto durableStatus = Control.Load(durable);
        Serial.printf("Durable load=%u", static_cast<unsigned>(durableStatus));
        if (durableStatus == OTADurableStatus::Success) {
            Serial.printf(
                " active=%u committed=%llu floor=%llu nextTx=%llu nextGen=%llu intent=%u",
                durable.HasActiveTransaction ? 1U : 0U,
                static_cast<unsigned long long>(durable.Committed.Generation.Value()),
                static_cast<unsigned long long>(durable.MinimumAcceptedSecurity.Value()),
                static_cast<unsigned long long>(durable.NextTransaction.Value()),
                static_cast<unsigned long long>(durable.NextGeneration.Value()),
                static_cast<unsigned>(durable.Intent));
            if (durable.HasActiveTransaction) {
                Serial.printf(
                    " tx=%llu candidate=%llu point=%u candidateBoot=0x%08lx previousBoot=0x%08lx",
                    static_cast<unsigned long long>(durable.Active.Transaction.Value()),
                    static_cast<unsigned long long>(durable.Active.CandidateGeneration.Value()),
                    static_cast<unsigned>(durable.Active.Point),
                    static_cast<unsigned long>(durable.Active.CandidateBootTarget.Value()),
                    static_cast<unsigned long>(durable.Active.PreviousCommittedBootTarget.Value()));
            }
        }
        Serial.println();

        Serial.printf(
            "Platform: running=0x%08lx boot=0x%08lx next=0x%08lx trial=%u inactive=0x%08lx clone=%s %llu/%llu\n",
            static_cast<unsigned long>(Boot.CurrentBootTarget().Value()),
            static_cast<unsigned long>(Boot.CommittedBootTarget().Value()),
            static_cast<unsigned long>(Boot.NextBootTarget().Value()),
            Trial.IsCurrentBootTrial() ? 1U : 0U,
            static_cast<unsigned long>(InactiveTarget().Value()),
            CloneName(Clone),
            static_cast<unsigned long long>(CloneOffset),
            static_cast<unsigned long long>(CloneLength));

        Serial.printf(
            "Memory: heap=%u free=%u minFree=%u sketch=%u freeSketch=%u flash=%u\n",
            ESP.getHeapSize(),
            ESP.getFreeHeap(),
            ESP.getMinFreeHeap(),
            ESP.getSketchSize(),
            ESP.getFreeSketchSpace(),
            ESP.getFlashChipSize());
    }

    Platform::OTA::BootTargetIdentifier InactiveTarget() const noexcept {
        const auto* partition = ::esp_ota_get_next_update_partition(nullptr);
        return partition == nullptr
            ? Platform::OTA::BootTargetIdentifier{}
            : Platform::OTA::BootTargetIdentifier{partition->address};
    }

    bool BeginClone() {
        if (!Initialized || Clone == CloneState::Writing) return false;
        if (CoordinatorCore.Status().HasActiveTransaction || Trial.IsCurrentBootTrial()) {
            Serial.println("clone rejected: Coordinator transaction/trial is active");
            return false;
        }
        RunningPartition = ::esp_ota_get_running_partition();
        CloneLength = ESP.getSketchSize();
        CloneOffset = 0U;
        ClonedTarget = {};
        if (RunningPartition == nullptr || CloneLength == 0U) {
            Clone = CloneState::Failed;
            return false;
        }
        const auto preflight = ImageStaging.PreflightApplicationImage(CloneLength);
        if (!preflight) {
            Serial.printf("clone preflight failed: status=%u native=%ld\n",
                          static_cast<unsigned>(preflight.Code),
                          static_cast<long>(preflight.NativeCode));
            Clone = CloneState::Failed;
            return false;
        }
        const auto begun = ImageStaging.BeginApplicationImage(CloneLength);
        if (!begun) {
            Serial.printf("clone begin failed: status=%u native=%ld\n",
                          static_cast<unsigned>(begun.Code),
                          static_cast<long>(begun.NativeCode));
            Clone = CloneState::Failed;
            return false;
        }
        ClonedTarget = ImageStaging.ApplicationImageTarget();
        Clone = CloneState::Writing;
        Serial.printf("clone started: bytes=%llu target=0x%08lx\n",
                      static_cast<unsigned long long>(CloneLength),
                      static_cast<unsigned long>(ClonedTarget.Value()));
        return true;
    }

    void AdvanceClone() {
        if (Clone != CloneState::Writing) return;
        const auto remaining = CloneLength - CloneOffset;
        const auto bytes = static_cast<std::size_t>(
            std::min<std::uint64_t>(CloneBuffer.size(), remaining));
        if (bytes == 0U) {
            const auto finalized = ImageStaging.FinalizeApplicationImage();
            if (!finalized) {
                Serial.printf("clone finalize failed: status=%u native=%ld\n",
                              static_cast<unsigned>(finalized.Code),
                              static_cast<long>(finalized.NativeCode));
                Clone = CloneState::Failed;
                return;
            }
            ClonedTarget = ImageStaging.ApplicationImageTarget();
            Clone = CloneState::Complete;
            Serial.printf("clone complete: target=0x%08lx\n",
                          static_cast<unsigned long>(ClonedTarget.Value()));
            return;
        }

        if (::esp_partition_read(RunningPartition,
                                 static_cast<std::size_t>(CloneOffset),
                                 CloneBuffer.data(),
                                 bytes) != ESP_OK) {
            (void)ImageStaging.AbortApplicationImage();
            Clone = CloneState::Failed;
            Serial.println("clone failed: source partition read");
            return;
        }
        const auto written = ImageStaging.WriteApplicationImage(CloneBuffer.data(), bytes);
        if (!written || written.Bytes != bytes) {
            (void)ImageStaging.AbortApplicationImage();
            Clone = CloneState::Failed;
            Serial.printf("clone failed: target write status=%u bytes=%u\n",
                          static_cast<unsigned>(written.Code),
                          static_cast<unsigned>(written.Bytes));
            return;
        }
        CloneOffset += bytes;
        if ((CloneOffset % (64U * 1024U)) == 0U || CloneOffset == CloneLength) {
            Serial.printf("clone progress: %llu/%llu\n",
                          static_cast<unsigned long long>(CloneOffset),
                          static_cast<unsigned long long>(CloneLength));
        }
    }

    void StartTransaction() {
        if (!Initialized) return;
        if (Clone != CloneState::Complete) {
            Serial.println("start rejected: run 'clone NOW' and let it complete first");
            return;
        }
        if (CoordinatorCore.Status().HasActiveTransaction) {
            Serial.println("start rejected: transaction already active");
            return;
        }
        UpdateTransactionId transaction{};
        const auto started = CoordinatorCore.Start(
            {ReleaseIdentifier{ManifestFixture.Release},
             ManifestIdentifier{ManifestFixture.Identifier},
             SecurityGeneration{ManifestFixture.SecurityGeneration}},
            transaction);
        PrintResult("Coordinator.Start", started);
        if (!started) return;
        const auto bound = CoordinatorCore.BindVerifiedManifest(ManifestFixture, TargetProfile);
        PrintResult("Coordinator.BindVerifiedManifest", bound);
        PrintStatus();
    }

    void BeginActivation() {
        if (!Initialized) return;
        const auto status = CoordinatorCore.Status();
        if (!status.HasActiveTransaction || status.DurableRecoveryPoint != RecoveryPoint::Staged) {
            Serial.println("activate rejected: transaction is not durably Staged");
            return;
        }
        const auto candidate = InactiveTarget();
        if (!candidate) {
            Serial.println("activate rejected: no inactive OTA application target");
            return;
        }
        Serial.printf("arming candidate target 0x%08lx; durable intent is written before boot mutation\n",
                      static_cast<unsigned long>(candidate.Value()));
        const auto result = CoordinatorCore.BeginActivation(
            {status.Transaction, candidate, true, true, true});
        PrintResult("Coordinator.BeginActivation", result);
        PrintStatus();
    }

    void StepCoordinator() {
        if (!Initialized) return;
        if (!CoordinatorCore.Status().HasActiveTransaction) {
            Serial.println("step: no active transaction");
            return;
        }
        Serial.println("Coordinator.Advance: one bounded step (this step may restart the device)");
        Serial.flush();
        const auto result = CoordinatorCore.Advance();
        PrintResult("Coordinator.Advance", result);
        PrintStatus();
    }

    void CancelTransaction() {
        if (!Initialized) return;
        const auto status = CoordinatorCore.Status();
        if (!status.HasActiveTransaction) {
            Serial.println("cancel: no active transaction");
            return;
        }
        const auto result = CoordinatorCore.Cancel(status.Transaction);
        PrintResult("Coordinator.Cancel", result);
        PrintStatus();
    }

    void AbortClone() {
        if (Clone != CloneState::Writing) {
            Serial.println("abort clone: clone is not active");
            return;
        }
        const auto result = ImageStaging.AbortApplicationImage();
        Clone = result ? CloneState::Idle : CloneState::Failed;
        Serial.printf("abort clone: status=%u native=%ld\n",
                      static_cast<unsigned>(result.Code),
                      static_cast<long>(result.NativeCode));
    }

    void PrintHelp() {
        Serial.println();
        Serial.println("ESPressio OTA Coordinator Recovery Lab");
        Serial.println("Startup is read-only apart from first-time provisioning of the dedicated OTA control record.");
        Serial.println("Mutating commands require the literal suffix ' NOW'.");
        Serial.println("  status");
        Serial.println("  clone NOW      - cooperatively clone running image into inactive OTA slot");
        Serial.println("  abort NOW      - abort an in-progress clone");
        Serial.println("  start NOW      - create durable transaction + bind fixed trusted lab Manifest");
        Serial.println("  step NOW       - perform exactly one Coordinator::Advance() call");
        Serial.println("  activate NOW   - persist ActivationArmed for the inactive application slot");
        Serial.println("  cancel NOW     - pre-activation cancel or post-activation rollback request");
        Serial.println();
        Serial.println("Suggested commit path:");
        Serial.println("  clone NOW -> start NOW -> step NOW until Staged -> activate NOW -> step NOW");
        Serial.println("  one step at a time through boot selection/restart; after reboot use status then step NOW");
        Serial.println("  Trial + step NOW persists CommitIntent; subsequent step completes commit.");
        Serial.println();
        Serial.println("Suggested rollback path:");
        Serial.println("  reach Trial after reboot, then cancel NOW before the health/commit step; use step NOW to rollback.");
        Serial.println();
        Serial.println("For power-loss tests, remove power only between completed commands/printed steps unless specifically");
        Serial.println("testing application-image clone interruption. Do not represent CI compilation as physical evidence.");
    }

    void HandleCommand(String command) {
        command.trim();
        if (command.length() == 0U) return;
        if (command == "help") { PrintHelp(); return; }
        if (command == "status") { PrintStatus(); return; }
        if (command == "clone NOW") { (void)BeginClone(); return; }
        if (command == "abort NOW") { AbortClone(); return; }
        if (command == "start NOW") { StartTransaction(); return; }
        if (command == "step NOW") { StepCoordinator(); return; }
        if (command == "activate NOW") { BeginActivation(); return; }
        if (command == "cancel NOW") { CancelTransaction(); return; }
        Serial.println("unknown command; type 'help'");
    }
};

Lab lab;

} // namespace

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(100U);
    delay(250U);

    Serial.println();
    Serial.println("[OTA LAB] booting Coordinator recovery harness");
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    Serial.println("[OTA LAB] bootloader rollback support: ENABLED");
#else
    Serial.println("[OTA LAB] WARNING: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not enabled.");
    Serial.println("[OTA LAB] Compile validation remains useful, but Trial/rollback physical evidence is NOT valid");
    Serial.println("[OTA LAB] until the device is flashed with a rollback-enabled ESP-IDF bootloader.");
#endif

    if (!lab.Initialize()) {
        Serial.println("[OTA LAB] initialization FAILED; no command mutation will be accepted");
        lab.PrintStatus();
        return;
    }

    Serial.println("[OTA LAB] initialization complete");
    lab.PrintStatus();
    lab.PrintHelp();
}

void loop() {
    lab.AdvanceClone();
    if (Serial.available() > 0) {
        lab.HandleCommand(Serial.readStringUntil('\n'));
    }
    delay(1U);
}
