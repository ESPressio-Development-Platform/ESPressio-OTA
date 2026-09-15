#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTA.hpp"

using namespace ESPressio;
using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;

struct LifecycleComponentA {};
struct LifecycleComponentB {};

} // namespace

namespace ESPressio::OTA {

template<>
struct ComponentTypeTraits<::LifecycleComponentA> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {ComponentTypeId{0x6301U}, "lifecycle.a", ComponentKind::ApplicationFirmware,
                ComponentMultiplicity::Single, 1U};
    }
};

template<>
struct ComponentTypeTraits<::LifecycleComponentB> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {ComponentTypeId{0x6302U}, "lifecycle.b", ComponentKind::Configuration,
                ComponentMultiplicity::Single, 1U};
    }
};

} // namespace ESPressio::OTA

namespace {

struct Trace final {
    std::array<std::uint8_t, 32> Values{};
    std::size_t Count{0U};
    void Push(std::uint8_t value) noexcept { Values[Count++] = value; }
    void Reset() noexcept { Count = 0U; }
};

class EmptyStore final : public IReadableArtifactStore {
public:
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
    Result OpenRead(const ArtifactStoreReadRequest&) noexcept override {
        return {OutcomeClass::Invalid, {DiagnosticDomain::Store, 2U}};
    }
    StreamReadResult ReadStored(std::uint8_t*, std::size_t) noexcept override {
        return StreamReadResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Store, 3U}});
    }
    void CloseRead() noexcept override {}
};

template<typename TComponent>
class LifecycleHandler final : public ComponentHandler<TComponent, Capacity> {
    Trace& trace_;
    std::uint8_t base_;
public:
    ComponentRecoveryState Recovery{ComponentRecoveryState::Staged};
    bool PendingActivateOnce{false};
    bool ActivatePended{false};
    bool RestartOnActivate{false};

    LifecycleHandler(Trace& trace, std::uint8_t base) noexcept : trace_(trace), base_(base) {}

    ComponentActionResult Activate(const ComponentExecutionContext<Capacity>& context) noexcept override {
        if (!context.IsValid()) return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 1U}});
        if (PendingActivateOnce && !ActivatePended) {
            ActivatePended = true;
            return ComponentActionResult::Pending();
        }
        trace_.Push(static_cast<std::uint8_t>(base_ + 1U));
        Recovery = ComponentRecoveryState::Activated;
        return RestartOnActivate ? ComponentActionResult::RestartRequired() : ComponentActionResult::Complete();
    }

    ComponentActionResult Commit(const ComponentExecutionContext<Capacity>& context) noexcept override {
        if (!context.IsValid()) return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 2U}});
        trace_.Push(static_cast<std::uint8_t>(base_ + 2U));
        Recovery = ComponentRecoveryState::Committed;
        return ComponentActionResult::Complete();
    }

    ComponentActionResult Rollback(const ComponentExecutionContext<Capacity>& context) noexcept override {
        if (!context.IsValid()) return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 3U}});
        trace_.Push(static_cast<std::uint8_t>(base_ + 3U));
        Recovery = ComponentRecoveryState::RolledBack;
        return ComponentActionResult::Complete();
    }

    ComponentActionResult Cleanup(const ComponentExecutionContext<Capacity>& context) noexcept override {
        if (!context.IsValid()) return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 4U}});
        trace_.Push(static_cast<std::uint8_t>(base_ + 4U));
        return ComponentActionResult::Complete();
    }

    ComponentRecoveryInspection InspectRecoveryState(const ComponentPreflightContext<Capacity>&) noexcept override {
        return {Recovery, Result::Success()};
    }
};

bool ConfigureProfile(UpdateTargetProfile<Capacity>& profile) {
    if (profile.SetSystemIdentity(System::ProductTypeIdentifier{1U}, System::HardwareFamilyIdentifier{2U},
            System::HardwareRevision{1U}, System::ArchitectureIdentifier{3U},
            System::SoftwareVariantIdentifier{4U}) != TargetProfileStatus::Success) return false;
    if (profile.SetStorageLayout(Platform::OTA::StorageLayoutIdentifier{5U},
            Platform::OTA::StorageLayoutGeneration{1U}) != TargetProfileStatus::Success) return false;
    if (profile.SetPersistenceSchema(Persistence::SchemaIdentifier{6U},
            Persistence::SchemaGeneration{1U}) != TargetProfileStatus::Success) return false;
    if (profile.SetOTASupport(OTAProtocolV1, 0U) != TargetProfileStatus::Success) return false;
    if (profile.AddSupportedComponentType(ComponentTypeId{0x6301U}) != TargetProfileStatus::Success) return false;
    if (profile.AddSupportedComponentType(ComponentTypeId{0x6302U}) != TargetProfileStatus::Success) return false;
    std::array<std::uint8_t, 32> fingerprint{};
    fingerprint[0] = 1U;
    return profile.SetFingerprintAndFreeze(UpdateTargetProfileFingerprint{fingerprint}) == TargetProfileStatus::Success;
}

Manifest<Capacity> MakeManifest() {
    Manifest<Capacity> manifest;
    auto id = std::array<std::uint8_t, 16>{};
    id[15] = 1U;
    manifest.Identifier = id;
    manifest.Release = 1U;
    (void)manifest.TargetClauses.push_back(ManifestTargetClause<Capacity>{});

    ManifestComponent<Capacity> a;
    a.Identifier = 1U;
    a.TypeId = 0x6301U;
    (void)manifest.Components.push_back(a);

    ManifestComponent<Capacity> b;
    b.Identifier = 2U;
    b.TypeId = 0x6302U;
    (void)manifest.Components.push_back(b);

    ManifestDependency edge;
    edge.Component = 2U;
    edge.DependsOn = 1U;
    (void)manifest.Dependencies.push_back(edge);

    ManifestSignatureDescriptor signature;
    signature.Algorithm = 1U;
    signature.TrustAnchor = 1U;
    signature.TrustPolicy = 1U;
    (void)manifest.SignatureDescriptors.push_back(signature);
    return manifest;
}

bool Drive(ComponentLifecycleExecutionSession<Capacity>& session) {
    for (std::size_t i = 0U; i < 16U && !session.IsComplete(); ++i) {
        const auto result = session.Advance();
        if (result.Outcome != OutcomeClass::Pending && result.Outcome != OutcomeClass::Deferred &&
            result.Outcome != OutcomeClass::Success) return false;
    }
    return session.IsComplete();
}

} // namespace

int main() {
    auto manifest = MakeManifest();
    if (ValidateManifest(manifest) != ManifestStatus::Success) return 1;

    UpdateTargetProfile<Capacity> profile;
    if (!ConfigureProfile(profile)) return 2;

    Trace trace;
    LifecycleHandler<LifecycleComponentA> a{trace, 10U};
    LifecycleHandler<LifecycleComponentB> b{trace, 20U};
    a.PendingActivateOnce = true;
    a.RestartOnActivate = true;

    ComponentHandlerDirectory<Capacity> handlers;
    if (handlers.Register(b) != ComponentHandlerDirectoryStatus::Success ||
        handlers.Register(a) != ComponentHandlerDirectoryStatus::Success) return 3;
    handlers.Freeze();

    UpdatePlan<Capacity> plan;
    if (BuildUpdatePlan(manifest, handlers, plan) != UpdatePlanStatus::Success) return 4;
    EmptyStore store;
    ComponentLifecycleExecutionSession<Capacity> session{store};

    if (session.Begin(manifest, plan, profile, UpdateTransactionId{1U}, UpdateGenerationId{2U},
                      ComponentLifecycleOperation::Activate).Outcome != OutcomeClass::Pending) return 5;
    if (!Drive(session) || !session.RestartRequired()) return 6;
    if (trace.Count != 2U || trace.Values[0] != 11U || trace.Values[1] != 21U) return 7;

    trace.Reset();
    session.Reset();
    if (session.Begin(manifest, plan, profile, UpdateTransactionId{1U}, UpdateGenerationId{2U},
                      ComponentLifecycleOperation::Commit, true).Outcome != OutcomeClass::Pending) return 8;
    if (!Drive(session)) return 9;
    if (trace.Count != 2U || trace.Values[0] != 12U || trace.Values[1] != 22U) return 10;

    trace.Reset();
    session.Reset();
    if (session.Begin(manifest, plan, profile, UpdateTransactionId{1U}, UpdateGenerationId{2U},
                      ComponentLifecycleOperation::Rollback, true).Outcome != OutcomeClass::Pending) return 11;
    if (!Drive(session)) return 12;
    if (trace.Count != 2U || trace.Values[0] != 23U || trace.Values[1] != 13U) return 13;

    trace.Reset();
    session.Reset();
    if (session.Begin(manifest, plan, profile, UpdateTransactionId{1U}, UpdateGenerationId{2U},
                      ComponentLifecycleOperation::Cleanup).Outcome != OutcomeClass::Pending) return 14;
    if (!Drive(session)) return 15;
    if (trace.Count != 2U || trace.Values[0] != 14U || trace.Values[1] != 24U) return 16;

    return 0;
}