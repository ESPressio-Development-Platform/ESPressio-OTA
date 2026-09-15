#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTA.hpp"

using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;

struct TestComponent {};
struct ExecutionComponentA {};
struct ExecutionComponentB {};

} // namespace

namespace ESPressio::OTA {

template<>
struct ComponentTypeTraits<TestComponent> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {
            ComponentTypeId{0x5101U},
            "test.component",
            ComponentKind::Data,
            ComponentMultiplicity::Single,
            1U
        };
    }
};

template<>
struct ComponentTypeTraits<ExecutionComponentA> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {
            ComponentTypeId{0x5201U},
            "test.execution.a",
            ComponentKind::ApplicationFirmware,
            ComponentMultiplicity::Single,
            1U
        };
    }
};

template<>
struct ComponentTypeTraits<ExecutionComponentB> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {
            ComponentTypeId{0x5202U},
            "test.execution.b",
            ComponentKind::Configuration,
            ComponentMultiplicity::Single,
            1U
        };
    }
};

} // namespace ESPressio::OTA

namespace {

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

class FakeVerifiedReader final : public IVerifiedArtifactReader<Capacity> {
    VerifiedArtifactDescriptor<Capacity> descriptor_{};
public:
    explicit FakeVerifiedReader(std::uint8_t id) noexcept {
        descriptor_.Identifier = ArtifactIdentifier{Id(id)};
        descriptor_.Length = 4U;
        descriptor_.DigestAlgorithm = ESPressio::Security::DigestAlgorithm::SHA256;
        for (std::size_t i = 0U; i < 32U; ++i) {
            (void)descriptor_.Digest.push_back(static_cast<std::uint8_t>(i + id));
        }
    }

    const VerifiedArtifactDescriptor<Capacity>& Descriptor() const noexcept override {
        return descriptor_;
    }

    Result Reset() noexcept override { return Result::Success(); }

    StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (output == nullptr || capacity == 0U) {
            return StreamReadResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 1U}});
        }
        output[0] = 0x5AU;
        return StreamReadResult::Data(1U);
    }
};

class TestHandler final : public ComponentHandler<TestComponent, Capacity> {
public:
    ComponentActionResult Stage(const ComponentExecutionContext<Capacity>&) noexcept override {
        return ComponentActionResult::Pending();
    }

    ComponentActionResult Activate(const ComponentExecutionContext<Capacity>&) noexcept override {
        return ComponentActionResult::RestartRequired();
    }

    ComponentRecoveryInspection InspectRecoveryState(
        const ComponentPreflightContext<Capacity>&) noexcept override {
        return {ComponentRecoveryState::Staged, Result::Success()};
    }
};

class FakeReadableStore final : public IReadableArtifactStore {
public:
    ArtifactIdentifier Identifier{Id(9U)};
    std::array<std::uint8_t, 4> Bytes{{0x11U, 0x22U, 0x33U, 0x44U}};
    std::size_t Position{0U};
    std::size_t OpenCalls{0U};
    std::size_t CloseCalls{0U};

    Result BeginWrite(const ArtifactStoreOpenRequest&) noexcept override {
        return {OutcomeClass::Unsupported, {DiagnosticDomain::Store, 1U}};
    }
    StreamWriteResult Write(const std::uint8_t*, std::size_t) noexcept override {
        return StreamWriteResult::Failed({OutcomeClass::Unsupported, {DiagnosticDomain::Store, 1U}});
    }
    ArtifactStoreFinalizeResult Finalize() noexcept override {
        return {ArtifactStoreFinalizeStatus::Failed, {OutcomeClass::Unsupported, {DiagnosticDomain::Store, 1U}}};
    }
    void Abort() noexcept override {}
    Result QueryAvailableBytes(std::uint64_t& availableBytes) const noexcept override {
        availableBytes = 0U;
        return Result::Success();
    }
    Result OpenRead(const ArtifactStoreReadRequest& request) noexcept override {
        ++OpenCalls;
        if (!request.IsValid() || request.Identifier != Identifier || request.ExpectedLength != Bytes.size()) {
            return {OutcomeClass::Invalid, {DiagnosticDomain::Store, 2U}};
        }
        Position = 0U;
        return Result::Success();
    }
    StreamReadResult ReadStored(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (output == nullptr || capacity == 0U) {
            return StreamReadResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Store, 3U}});
        }
        if (Position == Bytes.size()) return StreamReadResult::End();
        const auto count = std::min<std::size_t>(2U, std::min(capacity, Bytes.size() - Position));
        for (std::size_t i = 0U; i < count; ++i) output[i] = Bytes[Position + i];
        Position += count;
        return StreamReadResult::Data(count);
    }
    void CloseRead() noexcept override { ++CloseCalls; }
};

bool ConfigureProfile(UpdateTargetProfile<Capacity>& profile) {
    if (profile.SetSystemIdentity(
            ESPressio::System::ProductTypeIdentifier{1U},
            ESPressio::System::HardwareFamilyIdentifier{2U},
            ESPressio::System::HardwareRevision{1U},
            ESPressio::System::ArchitectureIdentifier{3U},
            ESPressio::System::SoftwareVariantIdentifier{4U}) != TargetProfileStatus::Success) return false;
    if (profile.SetStorageLayout(
            ESPressio::Platform::OTA::StorageLayoutIdentifier{5U},
            ESPressio::Platform::OTA::StorageLayoutGeneration{1U}) != TargetProfileStatus::Success) return false;
    if (profile.SetPersistenceSchema(
            ESPressio::Persistence::SchemaIdentifier{6U},
            ESPressio::Persistence::SchemaGeneration{1U}) != TargetProfileStatus::Success) return false;
    if (profile.SetOTASupport(OTAProtocolV1, 0U) != TargetProfileStatus::Success) return false;
    if (profile.AddSupportedComponentType(ComponentTypeId{0x5201U}) != TargetProfileStatus::Success) return false;
    if (profile.AddSupportedComponentType(ComponentTypeId{0x5202U}) != TargetProfileStatus::Success) return false;
    std::array<std::uint8_t, 32> fingerprint{};
    fingerprint[0] = 1U;
    return profile.SetFingerprintAndFreeze(UpdateTargetProfileFingerprint{fingerprint}) == TargetProfileStatus::Success;
}

Manifest<Capacity> ExecutionManifest() {
    Manifest<Capacity> manifest;
    manifest.Identifier = Id(7U);
    manifest.Release = 7U;
    (void)manifest.TargetClauses.push_back(ManifestTargetClause<Capacity>{});

    ManifestComponent<Capacity> first;
    first.Identifier = 1U;
    first.TypeId = 0x5201U;
    (void)first.Artifacts.push_back(Id(9U));
    (void)manifest.Components.push_back(first);

    ManifestComponent<Capacity> second;
    second.Identifier = 2U;
    second.TypeId = 0x5202U;
    (void)manifest.Components.push_back(second);

    ManifestArtifact<Capacity> artifact;
    artifact.Identifier = Id(9U);
    artifact.ExpectedLength = 4U;
    artifact.DigestAlgorithm = ESPressio::Security::DigestAlgorithm::SHA256.Value();
    for (std::size_t i = 0U; i < 32U; ++i) (void)artifact.Digest.push_back(static_cast<std::uint8_t>(i + 1U));
    (void)manifest.Artifacts.push_back(artifact);

    ManifestDependency dependency;
    dependency.Component = 2U;
    dependency.DependsOn = 1U;
    (void)manifest.Dependencies.push_back(dependency);

    ManifestSignatureDescriptor signature;
    signature.Algorithm = 1U;
    signature.TrustAnchor = 1U;
    signature.TrustPolicy = 1U;
    (void)manifest.SignatureDescriptors.push_back(signature);
    return manifest;
}

struct ExecutionTrace final {
    std::array<std::uint8_t, 32> Calls{};
    std::size_t Count{0U};
    bool Push(std::uint8_t value) noexcept {
        if (Count == Calls.size()) return false;
        Calls[Count++] = value;
        return true;
    }
};

class ExecutionHandlerA final : public ComponentHandler<ExecutionComponentA, Capacity> {
    ExecutionTrace& trace_;
    bool stageStarted_{false};
    bool readerOpen_{false};
    std::array<std::uint8_t, 4> observed_{};
    std::size_t observedBytes_{0U};
public:
    ComponentRecoveryState Recovery{ComponentRecoveryState::NotPrepared};
    std::size_t PreflightCalls{0U};

    explicit ExecutionHandlerA(ExecutionTrace& trace) noexcept : trace_(trace) {}

    Result Preflight(const ComponentPreflightContext<Capacity>& context) noexcept override {
        ++PreflightCalls;
        return context.IsValid() ? Result::Success()
                                 : Result{OutcomeClass::Invalid, {DiagnosticDomain::Component, 10U}};
    }
    ComponentActionResult Prepare(const ComponentPreflightContext<Capacity>&) noexcept override {
        (void)trace_.Push(1U);
        Recovery = ComponentRecoveryState::PartiallyStaged;
        return ComponentActionResult::Complete();
    }
    ComponentActionResult Stage(const ComponentExecutionContext<Capacity>& context) noexcept override {
        if (!context.IsValid()) return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 11U}});
        auto* reader = context.Artifacts->Find(ArtifactIdentifier{Id(9U)});
        if (reader == nullptr) return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 12U}});
        if (!stageStarted_) {
            (void)trace_.Push(2U);
            const auto reset = reader->Reset();
            if (!reset) return ComponentActionResult::Failed(reset);
            stageStarted_ = true;
            readerOpen_ = true;
        }
        std::array<std::uint8_t, 2> bytes{};
        const auto read = reader->Read(bytes.data(), bytes.size());
        if (read.Status == StreamReadStatus::Data) {
            for (std::size_t i = 0U; i < read.Bytes; ++i) observed_[observedBytes_++] = bytes[i];
            return ComponentActionResult::Pending();
        }
        if (read.Status == StreamReadStatus::End) {
            readerOpen_ = false;
            return ComponentActionResult::Complete();
        }
        if (read.Status == StreamReadStatus::Pending) return ComponentActionResult::Pending();
        return ComponentActionResult::Failed(read.Detail);
    }
    ComponentActionResult FinalizeStage(const ComponentExecutionContext<Capacity>&) noexcept override {
        if (readerOpen_ || observedBytes_ != observed_.size() ||
            observed_[0] != 0x11U || observed_[1] != 0x22U || observed_[2] != 0x33U || observed_[3] != 0x44U) {
            return ComponentActionResult::Failed({OutcomeClass::VerificationFailed, {DiagnosticDomain::Component, 13U}});
        }
        (void)trace_.Push(3U);
        Recovery = ComponentRecoveryState::Staged;
        return ComponentActionResult::Complete();
    }
    ComponentRecoveryInspection InspectRecoveryState(const ComponentPreflightContext<Capacity>&) noexcept override {
        return {Recovery, Result::Success()};
    }
};

class ExecutionHandlerB final : public ComponentHandler<ExecutionComponentB, Capacity> {
    ExecutionTrace& trace_;
public:
    ComponentRecoveryState Recovery{ComponentRecoveryState::NotPrepared};
    std::size_t PreflightCalls{0U};

    explicit ExecutionHandlerB(ExecutionTrace& trace) noexcept : trace_(trace) {}

    Result Preflight(const ComponentPreflightContext<Capacity>& context) noexcept override {
        ++PreflightCalls;
        return context.IsValid() ? Result::Success()
                                 : Result{OutcomeClass::Invalid, {DiagnosticDomain::Component, 20U}};
    }
    ComponentActionResult Prepare(const ComponentPreflightContext<Capacity>&) noexcept override {
        (void)trace_.Push(4U);
        Recovery = ComponentRecoveryState::PartiallyStaged;
        return ComponentActionResult::Complete();
    }
    ComponentActionResult Stage(const ComponentExecutionContext<Capacity>& context) noexcept override {
        if (!context.IsValid() || context.Artifacts->Size() != 0U) {
            return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 21U}});
        }
        (void)trace_.Push(5U);
        return ComponentActionResult::Complete();
    }
    ComponentActionResult FinalizeStage(const ComponentExecutionContext<Capacity>&) noexcept override {
        (void)trace_.Push(6U);
        Recovery = ComponentRecoveryState::Staged;
        return ComponentActionResult::Complete();
    }
    ComponentRecoveryInspection InspectRecoveryState(const ComponentPreflightContext<Capacity>&) noexcept override {
        return {Recovery, Result::Success()};
    }
};

} // namespace

int main() {
    TestHandler handler;
    const auto descriptor = handler.Descriptor();
    if (!descriptor || descriptor.TypeId != ComponentTypeId{0x5101U}) return 1;
    if (descriptor.Kind != ComponentKind::Data || descriptor.Multiplicity != ComponentMultiplicity::Single) return 2;

    ComponentHandlerDirectory<Capacity> directory;
    if (directory.Register(handler) != ComponentHandlerDirectoryStatus::Success) return 3;
    if (directory.Register(handler) != ComponentHandlerDirectoryStatus::DuplicateType) return 4;
    if (directory.Find(ComponentTypeId{0x5101U}) != nullptr) return 5;
    directory.Freeze();
    if (!directory.IsFrozen() || directory.Size() != 1U) return 6;
    if (directory.Find(ComponentTypeId{0x5101U}) != &handler) return 7;
    if (directory.Register(handler) != ComponentHandlerDirectoryStatus::Frozen) return 8;

    FakeVerifiedReader first{1U};
    FakeVerifiedReader second{2U};
    VerifiedComponentArtifacts<Capacity> artifacts;
    if (!artifacts.Add(second) || !artifacts.Add(first)) return 9;
    if (artifacts.Size() != 2U) return 10;
    if (artifacts.Find(ArtifactIdentifier{Id(1U)}) != &first) return 11;
    if (artifacts.Find(ArtifactIdentifier{Id(2U)}) != &second) return 12;
    if (artifacts.Add(first).Outcome != OutcomeClass::Invalid) return 13;

    ComponentPreflightContext<Capacity> preflight;
    if (handler.Preflight(preflight).Outcome != OutcomeClass::Success) return 14;
    if (handler.Prepare(preflight).Status != ComponentActionStatus::Complete) return 15;

    ComponentExecutionContext<Capacity> execution;
    if (handler.Stage(execution).Status != ComponentActionStatus::Pending) return 16;
    if (handler.Activate(execution).Status != ComponentActionStatus::RestartRequired) return 17;
    if (handler.FinalizeStage(execution).Status != ComponentActionStatus::Complete) return 18;
    if (handler.Commit(execution).Status != ComponentActionStatus::Complete) return 19;
    if (handler.Rollback(execution).Status != ComponentActionStatus::Complete) return 20;
    if (handler.Cleanup(execution).Status != ComponentActionStatus::Complete) return 21;

    const auto recovery = handler.InspectRecoveryState(preflight);
    if (recovery.State != ComponentRecoveryState::Staged || recovery.Detail.Outcome != OutcomeClass::Success) return 22;

    FakeReadableStore store;
    RetainedArtifactReadLease lease{store};
    ManifestArtifact<Capacity> retained;
    retained.Identifier = Id(9U);
    retained.ExpectedLength = 4U;
    retained.DigestAlgorithm = ESPressio::Security::DigestAlgorithm::SHA256.Value();
    for (std::size_t i = 0U; i < 32U; ++i) (void)retained.Digest.push_back(static_cast<std::uint8_t>(i + 1U));
    RetainedVerifiedArtifactReader<Capacity> retainedReaderA;
    RetainedVerifiedArtifactReader<Capacity> retainedReaderB;
    if (!retainedReaderA.Configure(retained, lease) || !retainedReaderB.Configure(retained, lease)) return 23;
    if (!retainedReaderA.Reset()) return 24;
    if (retainedReaderB.Reset().Outcome != OutcomeClass::Unavailable) return 25;
    std::array<std::uint8_t, 4> retainedBytes{};
    std::size_t retainedCount = 0U;
    for (;;) {
        std::array<std::uint8_t, 2> chunk{};
        const auto read = retainedReaderA.Read(chunk.data(), chunk.size());
        if (read.Status == StreamReadStatus::Data) {
            for (std::size_t i = 0U; i < read.Bytes; ++i) retainedBytes[retainedCount++] = chunk[i];
            continue;
        }
        if (read.Status != StreamReadStatus::End) return 26;
        break;
    }
    if (retainedCount != 4U || retainedBytes != store.Bytes || store.CloseCalls != 1U) return 27;
    if (!retainedReaderB.Reset()) return 28;
    lease.Close(&retainedReaderB);

    UpdateTargetProfile<Capacity> profile;
    if (!ConfigureProfile(profile)) return 29;
    auto manifest = ExecutionManifest();
    if (ValidateManifest(manifest) != ManifestStatus::Success) return 30;

    ExecutionTrace trace;
    ExecutionHandlerA handlerA{trace};
    ExecutionHandlerB handlerB{trace};
    ComponentHandlerDirectory<Capacity> executionDirectory;
    if (executionDirectory.Register(handlerB) != ComponentHandlerDirectoryStatus::Success ||
        executionDirectory.Register(handlerA) != ComponentHandlerDirectoryStatus::Success) return 31;
    executionDirectory.Freeze();
    UpdatePlan<Capacity> plan;
    if (BuildUpdatePlan(manifest, executionDirectory, plan) != UpdatePlanStatus::Success) return 32;
    if (plan.Size() != 2U || plan.Forward(0U)->Identifier != ComponentIdentifier{1U} ||
        plan.Forward(1U)->Identifier != ComponentIdentifier{2U}) return 33;

    if (!PreflightUpdatePlan(plan, profile, UpdateTransactionId{1U}, UpdateGenerationId{2U})) return 34;
    if (handlerA.PreflightCalls != 1U || handlerB.PreflightCalls != 1U) return 35;

    ComponentStagingSession<Capacity> staging{store};
    if (staging.Begin(manifest, plan, profile, UpdateTransactionId{1U}, UpdateGenerationId{2U}).Outcome != OutcomeClass::Pending) return 36;
    for (std::size_t step = 0U; step < 32U && !staging.IsComplete(); ++step) {
        const auto result = staging.Advance();
        if (result.Outcome != OutcomeClass::Pending && result.Outcome != OutcomeClass::Success) return 37;
    }
    if (!staging.IsComplete()) return 38;
    const std::array<std::uint8_t, 6> expectedTrace{{1U, 2U, 3U, 4U, 5U, 6U}};
    if (trace.Count != expectedTrace.size()) return 39;
    for (std::size_t i = 0U; i < expectedTrace.size(); ++i) if (trace.Calls[i] != expectedTrace[i]) return 40;

    // Recovery resumes at the first incomplete component and never replays a staged prefix.
    trace.Count = 0U;
    handlerA.Recovery = ComponentRecoveryState::Staged;
    handlerB.Recovery = ComponentRecoveryState::NotPrepared;
    ComponentStagingSession<Capacity> recovered{store};
    if (recovered.Begin(manifest, plan, profile, UpdateTransactionId{1U}, UpdateGenerationId{2U}, true).Outcome != OutcomeClass::Pending) return 41;
    if (recovered.ComponentIndex() != 1U || recovered.Phase() != ComponentStagingPhase::Preparing) return 42;
    for (std::size_t step = 0U; step < 8U && !recovered.IsComplete(); ++step) {
        const auto result = recovered.Advance();
        if (result.Outcome != OutcomeClass::Pending && result.Outcome != OutcomeClass::Success) return 43;
    }
    if (!recovered.IsComplete() || trace.Count != 3U ||
        trace.Calls[0] != 4U || trace.Calls[1] != 5U || trace.Calls[2] != 6U) return 44;

    return 0;
}
