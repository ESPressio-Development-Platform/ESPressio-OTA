#include <cstdint>

#include "ESPressio_OTA.hpp"

using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;

struct ComponentA {};
struct ComponentB {};
struct ComponentC {};

} // namespace

namespace ESPressio::OTA {

template<>
struct ComponentTypeTraits<ComponentA> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {ComponentTypeId{0x6001U}, "component.a", ComponentKind::Data, ComponentMultiplicity::Multiple, 1U};
    }
};

template<>
struct ComponentTypeTraits<ComponentB> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {ComponentTypeId{0x6002U}, "component.b", ComponentKind::Configuration, ComponentMultiplicity::Single, 1U};
    }
};

template<>
struct ComponentTypeTraits<ComponentC> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {ComponentTypeId{0x6003U}, "component.c", ComponentKind::Filesystem, ComponentMultiplicity::Single, 1U};
    }
};

} // namespace ESPressio::OTA

namespace {

template<typename TComponent>
class Handler final : public ComponentHandler<TComponent, Capacity> {
public:
    ComponentRecoveryInspection InspectRecoveryState(
        const ComponentPreflightContext<Capacity>&) noexcept override {
        return {ComponentRecoveryState::NotPrepared, Result::Success()};
    }
};

void AddRequiredManifestScaffolding(Manifest<Capacity>& manifest) {
    manifest.Identifier.Bytes();
    std::array<std::uint8_t, 16> id{};
    id[15] = 1U;
    manifest.Identifier = id;
    manifest.Release = 1U;
    manifest.RequiredOTAProtocol = OTAProtocolV1.Value();

    ManifestTargetClause<Capacity> clause;
    (void)manifest.TargetClauses.push_back(clause);

    ManifestSignatureDescriptor signature;
    signature.Algorithm = 1U;
    signature.TrustAnchor = 1U;
    signature.TrustPolicy = 1U;
    (void)manifest.SignatureDescriptors.push_back(signature);
}

ManifestComponent<Capacity> Component(
    std::uint32_t identifier,
    std::uint64_t type,
    std::uint16_t schema = 1U) {
    ManifestComponent<Capacity> result;
    result.Identifier = identifier;
    result.TypeId = type;
    result.ParameterSchemaVersion = schema;
    return result;
}

Manifest<Capacity> BuildValidManifest() {
    Manifest<Capacity> manifest;
    AddRequiredManifestScaffolding(manifest);

    (void)manifest.Components.push_back(Component(30U, 0x6001U));
    (void)manifest.Components.push_back(Component(10U, 0x6002U));
    (void)manifest.Components.push_back(Component(20U, 0x6003U));

    ManifestDependency edge;
    edge.Component = 30U;
    edge.DependsOn = 10U;
    (void)manifest.Dependencies.push_back(edge);

    (void)PrepareManifestForSigning(manifest);
    return manifest;
}

} // namespace

int main() {
    Handler<ComponentA> handlerA;
    Handler<ComponentB> handlerB;
    Handler<ComponentC> handlerC;

    ComponentHandlerDirectory<Capacity> directory;
    if (directory.Register(handlerA) != ComponentHandlerDirectoryStatus::Success) return 1;
    if (directory.Register(handlerB) != ComponentHandlerDirectoryStatus::Success) return 2;
    if (directory.Register(handlerC) != ComponentHandlerDirectoryStatus::Success) return 3;

    auto manifest = BuildValidManifest();
    UpdatePlan<Capacity> plan;
    if (BuildUpdatePlan(manifest, directory, plan) != UpdatePlanStatus::HandlerDirectoryNotFrozen) return 4;

    directory.Freeze();
    if (BuildUpdatePlan(manifest, directory, plan) != UpdatePlanStatus::Success) return 5;
    if (!plan.IsReady() || plan.Size() != 3U) return 6;

    const auto* first = plan.Forward(0U);
    const auto* second = plan.Forward(1U);
    const auto* third = plan.Forward(2U);
    if (first == nullptr || second == nullptr || third == nullptr) return 7;
    if (first->Identifier != ComponentIdentifier{10U}) return 8;
    if (second->Identifier != ComponentIdentifier{20U}) return 9;
    if (third->Identifier != ComponentIdentifier{30U}) return 10;
    if (plan.Reverse(0U)->Identifier != ComponentIdentifier{30U}) return 11;
    if (plan.Reverse(2U)->Identifier != ComponentIdentifier{10U}) return 12;

    ComponentHandlerDirectory<Capacity> incomplete;
    if (incomplete.Register(handlerA) != ComponentHandlerDirectoryStatus::Success) return 13;
    if (incomplete.Register(handlerB) != ComponentHandlerDirectoryStatus::Success) return 14;
    incomplete.Freeze();
    UpdatePlan<Capacity> missingPlan;
    if (BuildUpdatePlan(manifest, incomplete, missingPlan) != UpdatePlanStatus::HandlerUnavailable) return 15;

    auto schemaMismatch = manifest;
    schemaMismatch.Components[0].ParameterSchemaVersion = 2U;
    UpdatePlan<Capacity> mismatchPlan;
    if (BuildUpdatePlan(schemaMismatch, directory, mismatchPlan) != UpdatePlanStatus::ParameterSchemaUnsupported) return 16;

    Manifest<Capacity> multiplicity;
    AddRequiredManifestScaffolding(multiplicity);
    (void)multiplicity.Components.push_back(Component(1U, 0x6002U));
    (void)multiplicity.Components.push_back(Component(2U, 0x6002U));
    if (PrepareManifestForSigning(multiplicity) != ManifestStatus::Success) return 17;
    UpdatePlan<Capacity> multiplicityPlan;
    if (BuildUpdatePlan(multiplicity, directory, multiplicityPlan) != UpdatePlanStatus::MultiplicityViolation) return 18;

    auto cycle = manifest;
    cycle.Dependencies.clear();
    ManifestDependency firstEdge;
    firstEdge.Component = 10U;
    firstEdge.DependsOn = 20U;
    ManifestDependency secondEdge;
    secondEdge.Component = 20U;
    secondEdge.DependsOn = 10U;
    (void)cycle.Dependencies.push_back(firstEdge);
    (void)cycle.Dependencies.push_back(secondEdge);
    if (CanonicalizeManifest(cycle) != ManifestStatus::Success) return 19;
    UpdatePlan<Capacity> cyclePlan;
    if (BuildUpdatePlan(cycle, directory, cyclePlan) != UpdatePlanStatus::DependencyCycle) return 20;
    if (cyclePlan.IsReady() || cyclePlan.Size() != 0U) return 21;

    return 0;
}
