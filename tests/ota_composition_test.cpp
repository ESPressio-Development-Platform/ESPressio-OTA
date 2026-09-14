#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ESPressio_OTA.hpp"
#include "ESPressio_PlatformCapabilities.hpp"

using namespace ESPressio::OTA;

namespace {

struct ApplicationFirmware {};
struct AcquireDecisionPoint {};
struct ApplicationReadyCondition {};

using OffsetSourceCapabilities = CapabilitySet<
    CapabilityProfile<
        Capability::ArtifactSource,
        PropertyValue<PropertyKey::SourceOffsetRead, true>>>;

using SequentialSourceCapabilities = CapabilitySet<
    CapabilityProfile<
        Capability::ArtifactSource,
        PropertyValue<PropertyKey::SourceOffsetRead, false>>>;

using DurableStoreCapabilities = CapabilitySet<
    CapabilityProfile<
        Capability::ArtifactStore,
        PropertyValue<PropertyKey::ArtifactStoreDurability, ArtifactStoreDurability::Durable>,
        PropertyValue<PropertyKey::StoreMaximumArtifactBytes, std::uint64_t{4U * 1024U * 1024U}>>>;

using DistributorCapabilities = CapabilitySet<
    CapabilityProfile<
        Capability::ArtifactDistributor,
        PropertyValue<PropertyKey::DistributorMaximumRecipients, std::size_t{32U}>>>;

struct OffsetSource final : ProviderDeclaration<OffsetSourceCapabilities> {};
struct SequentialSource final : ProviderDeclaration<SequentialSourceCapabilities> {};
struct DurableStore final : ProviderDeclaration<DurableStoreCapabilities> {};
struct Distributor final : ProviderDeclaration<DistributorCapabilities> {};

using TestComposition = Composition<OffsetSource, SequentialSource, DurableStore, Distributor>;
using EmptyComposition = Composition<>;
using SourceProviders = typename TestComposition::template ProviderListFor<Capability::ArtifactSource>;

static_assert(EmptyComposition::IsValid);
static_assert(EmptyComposition::ProviderCount == 0U);
static_assert(TestComposition::IsValid);
static_assert(TestComposition::template ProvidersFor<Capability::ArtifactSource> == 2U);
static_assert(SourceProviders::Count == 2U);
static_assert(SourceProviders::template Contains<OffsetSource>);
static_assert(SourceProviders::template Contains<SequentialSource>);

static_assert(IsSharedCapabilityV<Capability::ArtifactSource>);
static_assert(IsSharedCapabilityV<Capability::ManifestSource>);
static_assert(IsSharedCapabilityV<Capability::UpdateCatalog>);
static_assert(IsSharedCapabilityV<Capability::ArtifactStore>);
static_assert(IsSharedCapabilityV<Capability::ArtifactDistributor>);
static_assert(IsSharedCapabilityV<Capability::PolicyGate<AcquireDecisionPoint>>);
static_assert(IsSharedCapabilityV<Capability::HealthCheckFor<ApplicationReadyCondition>>);
static_assert(IsExclusiveCapabilityV<Capability::ComponentHandler<ApplicationFirmware>>);

static_assert(TestComposition::template ProviderHasProperty<
    OffsetSource,
    Capability::ArtifactSource,
    PropertyKey::SourceOffsetRead>);
static_assert(TestComposition::template ProviderPropertyValue<
    OffsetSource,
    Capability::ArtifactSource,
    PropertyKey::SourceOffsetRead>);
static_assert(!TestComposition::template ProviderPropertyValue<
    SequentialSource,
    Capability::ArtifactSource,
    PropertyKey::SourceOffsetRead>);
static_assert(TestComposition::template ProviderPropertyValue<
    DurableStore,
    Capability::ArtifactStore,
    PropertyKey::ArtifactStoreDurability> == ArtifactStoreDurability::Durable);
static_assert(TestComposition::template ProviderPropertyValue<
    DurableStore,
    Capability::ArtifactStore,
    PropertyKey::StoreMaximumArtifactBytes> == 4U * 1024U * 1024U);
static_assert(TestComposition::template ProviderPropertyValue<
    Distributor,
    Capability::ArtifactDistributor,
    PropertyKey::DistributorMaximumRecipients> == 32U);

static_assert(!IsCapabilityV<ESPressio::Platform::Capability::Clock>);
static_assert(IsProviderV<OffsetSource>);
static_assert(!std::is_base_of_v<ESPressio::Platform::Backend, OffsetSource>);

using OffsetRequirement = RequirementSet<
    CapabilityRequirement<
        Capability::ArtifactSource,
        PropertyEquals<PropertyKey::SourceOffsetRead, true>>>;
using CheckedComposition = RequireT<TestComposition, OffsetRequirement>;
static_assert(std::is_same_v<CheckedComposition, TestComposition>);

} // namespace

int main() {
    return 0;
}
