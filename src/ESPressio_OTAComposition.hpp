#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "ESPressio_CompositionFramework.hpp"

namespace ESPressio::OTA {

namespace CompositionFramework = ESPressio::System::CompositionFramework;

/** OTA's domain tag for the generic System Composition Framework. */
struct CompositionDomain final : CompositionFramework::Domain {};

using ExclusiveCapability = CompositionFramework::ExclusiveCapability<CompositionDomain>;
using SharedCapability = CompositionFramework::SharedCapability<CompositionDomain>;

template<typename T>
inline constexpr bool IsExclusiveCapabilityV =
    CompositionFramework::IsExclusiveCapabilityForV<CompositionDomain, T>;

template<typename T>
inline constexpr bool IsSharedCapabilityV =
    CompositionFramework::IsSharedCapabilityForV<CompositionDomain, T>;

template<typename T>
inline constexpr bool IsCapabilityV =
    CompositionFramework::IsCapabilityForV<CompositionDomain, T>;

namespace Capability {
struct UpdateCatalog final : SharedCapability {};
struct ManifestSource final : SharedCapability {};
struct ArtifactSource final : SharedCapability {};
struct ArtifactStore final : SharedCapability {};
struct ArtifactDistributor final : SharedCapability {};

template<typename TDecisionPoint>
struct PolicyGate final : SharedCapability {
    using DecisionPoint = TDecisionPoint;
};

template<typename TCondition>
struct HealthCheckFor final : SharedCapability {
    using Condition = TCondition;
};

template<typename TComponent>
struct ComponentHandler final : ExclusiveCapability {
    using Component = TComponent;
};
} // namespace Capability

enum class ArtifactStoreDurability : std::uint8_t {
    Volatile = 0U,
    Durable = 1U
};

template<typename TValue>
using Property = CompositionFramework::Property<CompositionDomain, TValue>;

template<typename T>
inline constexpr bool IsPropertyV = CompositionFramework::IsPropertyForV<CompositionDomain, T>;

namespace PropertyKey {
struct SourceOffsetRead final : Property<bool> {};
struct ArtifactStoreDurability final : Property<OTA::ArtifactStoreDurability> {};
struct StoreMaximumArtifactBytes final : Property<std::uint64_t> {};
struct DistributorMaximumRecipients final : Property<std::size_t> {};
} // namespace PropertyKey

template<typename TProperty, auto TValue>
using PropertyValue = CompositionFramework::PropertyValue<CompositionDomain, TProperty, TValue>;

template<typename... TPropertyValues>
using PropertySet = CompositionFramework::PropertySet<CompositionDomain, TPropertyValues...>;

template<typename TCapability, typename... TPropertyValues>
using CapabilityProfile =
    CompositionFramework::CapabilityProfile<CompositionDomain, TCapability, TPropertyValues...>;

template<typename... TEntries>
using CapabilitySet = CompositionFramework::CapabilitySet<CompositionDomain, TEntries...>;

using Constraint = CompositionFramework::Constraint<CompositionDomain>;

template<typename T>
inline constexpr bool IsConstraintV = CompositionFramework::IsConstraintForV<CompositionDomain, T>;

template<typename TProperty, auto TExpected>
using PropertyEquals =
    CompositionFramework::PropertyEquals<CompositionDomain, TProperty, TExpected>;

template<typename TProperty, auto TMinimum>
using PropertyAtLeast =
    CompositionFramework::PropertyAtLeast<CompositionDomain, TProperty, TMinimum>;

template<typename TProperty, auto TMaximum>
using PropertyAtMost =
    CompositionFramework::PropertyAtMost<CompositionDomain, TProperty, TMaximum>;

template<typename TProperty, auto TMinimumExclusive>
using PropertyGreaterThan =
    CompositionFramework::PropertyGreaterThan<CompositionDomain, TProperty, TMinimumExclusive>;

template<typename TProperty, auto TMaximumExclusive>
using PropertyLessThan =
    CompositionFramework::PropertyLessThan<CompositionDomain, TProperty, TMaximumExclusive>;

template<typename TCapability, typename... TConstraints>
using CapabilityRequirement =
    CompositionFramework::CapabilityRequirement<CompositionDomain, TCapability, TConstraints...>;

template<typename... TEntries>
using RequirementSet = CompositionFramework::RequirementSet<CompositionDomain, TEntries...>;

/**
 * Declares an OTA-domain provider without inventing a second provider/backend identity layer.
 * The concrete provider Type itself is the composition entry.
 */
template<typename TCapabilities, typename TRequirements = RequirementSet<>>
struct ProviderDeclaration
    : CompositionFramework::ProviderDeclaration<CompositionDomain, TCapabilities, TRequirements> {
    using OTACapabilities = TCapabilities;
    using OTARequirements = TRequirements;
};

namespace Detail {
template<typename T, typename = void>
struct IsOTAProvider : std::false_type {};

template<typename T>
struct IsOTAProvider<T, std::void_t<typename T::OTACapabilities, typename T::OTARequirements>>
    : std::bool_constant<CompositionFramework::IsProviderForV<CompositionDomain, T>> {};
} // namespace Detail

template<typename T>
inline constexpr bool IsProviderV = Detail::IsOTAProvider<T>::value;

template<typename... TProviders>
struct Composition : CompositionFramework::Composition<CompositionDomain, TProviders...> {
    static_assert((IsProviderV<TProviders> && ...),
                  "OTA Composition entries must derive from OTA::ProviderDeclaration");
    using Base = CompositionFramework::Composition<CompositionDomain, TProviders...>;

    template<typename TCapability>
    static constexpr std::size_t ProvidersFor = Base::template ProviderCountFor<TCapability>;
};

template<typename... TProviders>
using ProviderList = CompositionFramework::ProviderList<TProviders...>;

template<typename TComposition, typename TRequirements>
struct Require : CompositionFramework::Require<TComposition, TRequirements> {};

template<typename TComposition, typename TRequirements>
using RequireT = typename Require<TComposition, TRequirements>::Type;

} // namespace ESPressio::OTA
