#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTACapacityProfile.hpp"
#include "ESPressio_OTAProfile.hpp"
#include "ESPressio_OTATypes.hpp"
#include "ESPressio_Verification.hpp"

#include "ESPressio_BoundedContainers.hpp"
#include "ESPressio_BoundedDeserializer.hpp"
#include "ESPressio_SerializableBase.hpp"
#include "ESPressio_SerializationMacros.hpp"
#include "ESPressio_SerializationTraits.hpp"

namespace ESPressio::OTA {

inline constexpr std::uint64_t ManifestDomainTag = 0x4553504F54414D31ULL; // "ESPOTAM1"

enum class ManifestStatus : std::uint8_t {
    Success,
    Invalid,
    CapacityUnavailable,
    DuplicateIdentifier,
    DuplicateReference,
    MissingReference,
    DependencyCycle,
    NonCanonical,
    SerializationFailed,
    SignatureCountMismatch,
    NoTrustedSignature
};

enum class TargetMatchMode : std::uint8_t {
    Any = 0U,
    Exact = 1U
};

enum class TargetRevisionMode : std::uint8_t {
    Any = 0U,
    Exact = 1U,
    InclusiveRange = 2U
};

namespace ManifestDetail {

inline constexpr bool NonZero128(const std::array<std::uint8_t, 16>& value) noexcept {
    for (auto byte : value) if (byte != 0U) return true;
    return false;
}

inline constexpr bool Equal128(
    const std::array<std::uint8_t, 16>& left,
    const std::array<std::uint8_t, 16>& right) noexcept {
    for (std::size_t i = 0U; i < left.size(); ++i) if (left[i] != right[i]) return false;
    return true;
}

inline constexpr bool Less128(
    const std::array<std::uint8_t, 16>& left,
    const std::array<std::uint8_t, 16>& right) noexcept {
    for (std::size_t i = 0U; i < left.size(); ++i) {
        if (left[i] < right[i]) return true;
        if (right[i] < left[i]) return false;
    }
    return false;
}

inline constexpr bool MatchModeValid(std::uint8_t mode) noexcept {
    return mode == static_cast<std::uint8_t>(TargetMatchMode::Any) ||
           mode == static_cast<std::uint8_t>(TargetMatchMode::Exact);
}

inline constexpr bool RevisionModeValid(std::uint8_t mode) noexcept {
    return mode == static_cast<std::uint8_t>(TargetRevisionMode::Any) ||
           mode == static_cast<std::uint8_t>(TargetRevisionMode::Exact) ||
           mode == static_cast<std::uint8_t>(TargetRevisionMode::InclusiveRange);
}

template<class TVector, class TLess>
void InsertionSort(TVector& values, TLess less) noexcept {
    for (std::size_t i = 1U; i < values.size(); ++i) {
        auto current = values[i];
        std::size_t position = i;
        while (position != 0U && less(current, values[position - 1U])) {
            values[position] = values[position - 1U];
            --position;
        }
        values[position] = current;
    }
}

template<class TVector, class TEqual>
bool HasAdjacentDuplicate(const TVector& values, TEqual equal) noexcept {
    for (std::size_t i = 1U; i < values.size(); ++i) {
        if (equal(values[i - 1U], values[i])) return true;
    }
    return false;
}

template<class TVector, class TLess>
bool IsStrictlySorted(const TVector& values, TLess less) noexcept {
    for (std::size_t i = 1U; i < values.size(); ++i) {
        if (!less(values[i - 1U], values[i])) return false;
    }
    return true;
}

} // namespace ManifestDetail

template<typename TCapacityProfile>
struct ManifestTargetClause final
    : Serializable::Serializable<ManifestTargetClause<TCapacityProfile>> {
    static_assert(TCapacityProfile::IsValid, "ManifestTargetClause requires a valid OTA capacity profile");

    std::uint8_t ProductTypeMode{static_cast<std::uint8_t>(TargetMatchMode::Any)};
    std::uint64_t ProductType{0U};
    std::uint8_t HardwareFamilyMode{static_cast<std::uint8_t>(TargetMatchMode::Any)};
    std::uint64_t HardwareFamily{0U};
    std::uint8_t HardwareRevisionMode{static_cast<std::uint8_t>(TargetRevisionMode::Any)};
    std::uint32_t HardwareRevisionMinimum{0U};
    std::uint32_t HardwareRevisionMaximum{0U};
    std::uint8_t ArchitectureMode{static_cast<std::uint8_t>(TargetMatchMode::Any)};
    std::uint64_t Architecture{0U};
    std::uint8_t SoftwareVariantMode{static_cast<std::uint8_t>(TargetMatchMode::Any)};
    std::uint64_t SoftwareVariant{0U};

    std::uint8_t CurrentStorageLayoutMode{static_cast<std::uint8_t>(TargetMatchMode::Any)};
    std::uint64_t CurrentStorageLayout{0U};
    std::uint32_t CurrentStorageLayoutGeneration{0U};
    std::uint8_t TargetStorageLayoutMode{static_cast<std::uint8_t>(TargetMatchMode::Any)};
    std::uint64_t TargetStorageLayout{0U};
    std::uint32_t TargetStorageLayoutGeneration{0U};

    std::uint8_t CurrentPersistenceSchemaMode{static_cast<std::uint8_t>(TargetMatchMode::Any)};
    std::uint64_t CurrentPersistenceSchema{0U};
    std::uint32_t CurrentPersistenceSchemaGeneration{0U};
    std::uint8_t TargetPersistenceSchemaMode{static_cast<std::uint8_t>(TargetMatchMode::Any)};
    std::uint64_t TargetPersistenceSchema{0U};
    std::uint32_t TargetPersistenceSchemaGeneration{0U};

    std::uint16_t MinimumProfileSchema{0U};
    std::uint16_t MinimumOTAProtocol{0U};
    std::uint64_t RequiredOTAFeatures{0U};
    Serializable::BoundedVector<std::uint64_t, TCapacityProfile::MaximumSupportedComponentTypes>
        RequiredComponentTypes{};

    ESPRESSIO_SERIALIZABLE_TYPE(ManifestTargetClause<TCapacityProfile>)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("productTypeMode", ProductTypeMode),
        ESPRESSIO_PROPERTY_REQUIRED("productType", ProductType),
        ESPRESSIO_PROPERTY_REQUIRED("hardwareFamilyMode", HardwareFamilyMode),
        ESPRESSIO_PROPERTY_REQUIRED("hardwareFamily", HardwareFamily),
        ESPRESSIO_PROPERTY_REQUIRED("hardwareRevisionMode", HardwareRevisionMode),
        ESPRESSIO_PROPERTY_REQUIRED("hardwareRevisionMinimum", HardwareRevisionMinimum),
        ESPRESSIO_PROPERTY_REQUIRED("hardwareRevisionMaximum", HardwareRevisionMaximum),
        ESPRESSIO_PROPERTY_REQUIRED("architectureMode", ArchitectureMode),
        ESPRESSIO_PROPERTY_REQUIRED("architecture", Architecture),
        ESPRESSIO_PROPERTY_REQUIRED("softwareVariantMode", SoftwareVariantMode),
        ESPRESSIO_PROPERTY_REQUIRED("softwareVariant", SoftwareVariant),
        ESPRESSIO_PROPERTY_REQUIRED("currentStorageLayoutMode", CurrentStorageLayoutMode),
        ESPRESSIO_PROPERTY_REQUIRED("currentStorageLayout", CurrentStorageLayout),
        ESPRESSIO_PROPERTY_REQUIRED("currentStorageLayoutGeneration", CurrentStorageLayoutGeneration),
        ESPRESSIO_PROPERTY_REQUIRED("targetStorageLayoutMode", TargetStorageLayoutMode),
        ESPRESSIO_PROPERTY_REQUIRED("targetStorageLayout", TargetStorageLayout),
        ESPRESSIO_PROPERTY_REQUIRED("targetStorageLayoutGeneration", TargetStorageLayoutGeneration),
        ESPRESSIO_PROPERTY_REQUIRED("currentPersistenceSchemaMode", CurrentPersistenceSchemaMode),
        ESPRESSIO_PROPERTY_REQUIRED("currentPersistenceSchema", CurrentPersistenceSchema),
        ESPRESSIO_PROPERTY_REQUIRED("currentPersistenceSchemaGeneration", CurrentPersistenceSchemaGeneration),
        ESPRESSIO_PROPERTY_REQUIRED("targetPersistenceSchemaMode", TargetPersistenceSchemaMode),
        ESPRESSIO_PROPERTY_REQUIRED("targetPersistenceSchema", TargetPersistenceSchema),
        ESPRESSIO_PROPERTY_REQUIRED("targetPersistenceSchemaGeneration", TargetPersistenceSchemaGeneration),
        ESPRESSIO_PROPERTY_REQUIRED("minimumProfileSchema", MinimumProfileSchema),
        ESPRESSIO_PROPERTY_REQUIRED("minimumOTAProtocol", MinimumOTAProtocol),
        ESPRESSIO_PROPERTY_REQUIRED("requiredOTAFeatures", RequiredOTAFeatures),
        ESPRESSIO_PROPERTY_REQUIRED("requiredComponentTypes", RequiredComponentTypes))
};

template<typename TCapacityProfile>
struct ManifestComponent final
    : Serializable::Serializable<ManifestComponent<TCapacityProfile>> {
    std::uint32_t Identifier{0U};
    std::uint64_t TypeId{0U};
    std::uint16_t ParameterSchemaVersion{1U};
    Serializable::BoundedBytes<TCapacityProfile::MaximumComponentParameterBytes> Parameters{};
    Serializable::BoundedVector<std::array<std::uint8_t, 16>, TCapacityProfile::MaximumArtifactsPerComponent>
        Artifacts{};

    ESPRESSIO_SERIALIZABLE_TYPE(ManifestComponent<TCapacityProfile>)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("identifier", Identifier),
        ESPRESSIO_PROPERTY_REQUIRED("typeId", TypeId),
        ESPRESSIO_PROPERTY_REQUIRED("parameterSchemaVersion", ParameterSchemaVersion),
        ESPRESSIO_PROPERTY_REQUIRED("parameters", Parameters),
        ESPRESSIO_PROPERTY_REQUIRED("artifacts", Artifacts))
};

template<typename TCapacityProfile>
struct ManifestArtifact final
    : Serializable::Serializable<ManifestArtifact<TCapacityProfile>> {
    std::array<std::uint8_t, 16> Identifier{};
    std::uint64_t ExpectedLength{0U};
    std::uint16_t DigestAlgorithm{0U};
    Serializable::BoundedBytes<TCapacityProfile::MaximumDigestBytes> Digest{};

    ESPRESSIO_SERIALIZABLE_TYPE(ManifestArtifact<TCapacityProfile>)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("identifier", Identifier),
        ESPRESSIO_PROPERTY_REQUIRED("expectedLength", ExpectedLength),
        ESPRESSIO_PROPERTY_REQUIRED("digestAlgorithm", DigestAlgorithm),
        ESPRESSIO_PROPERTY_REQUIRED("digest", Digest))
};

struct ManifestDependency final
    : Serializable::Serializable<ManifestDependency> {
    std::uint32_t Component{0U};
    std::uint32_t DependsOn{0U};

    ESPRESSIO_SERIALIZABLE_TYPE(ManifestDependency)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("component", Component),
        ESPRESSIO_PROPERTY_REQUIRED("dependsOn", DependsOn))
};

struct ManifestSignatureDescriptor final
    : Serializable::Serializable<ManifestSignatureDescriptor> {
    std::uint16_t Algorithm{0U};
    std::uint32_t TrustAnchor{0U};
    std::uint32_t TrustPolicy{0U};

    ESPRESSIO_SERIALIZABLE_TYPE(ManifestSignatureDescriptor)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("algorithm", Algorithm),
        ESPRESSIO_PROPERTY_REQUIRED("trustAnchor", TrustAnchor),
        ESPRESSIO_PROPERTY_REQUIRED("trustPolicy", TrustPolicy))
};

template<typename TCapacityProfile>
struct Manifest final
    : Serializable::Serializable<Manifest<TCapacityProfile>> {
    static_assert(TCapacityProfile::IsValid, "Manifest requires a valid OTA capacity profile");

    std::uint64_t DomainTag{ManifestDomainTag};
    std::uint16_t SchemaVersion{ManifestSchemaV1.Value()};
    std::array<std::uint8_t, 16> Identifier{};
    std::uint64_t Release{0U};
    std::uint32_t ReleaseChannel{0U};
    std::uint64_t SecurityGeneration{0U};
    std::uint16_t RequiredOTAProtocol{OTAProtocolV1.Value()};
    std::uint64_t RequiredOTAFeatures{0U};

    Serializable::BoundedVector<ManifestTargetClause<TCapacityProfile>, TCapacityProfile::MaximumTargetClauses>
        TargetClauses{};
    Serializable::BoundedVector<ManifestComponent<TCapacityProfile>, TCapacityProfile::MaximumComponents>
        Components{};
    Serializable::BoundedVector<ManifestArtifact<TCapacityProfile>, TCapacityProfile::MaximumArtifacts>
        Artifacts{};
    Serializable::BoundedVector<ManifestDependency, TCapacityProfile::MaximumDependencyEdges>
        Dependencies{};
    Serializable::BoundedVector<std::uint64_t, TCapacityProfile::MaximumRequiredHealthConditions>
        RequiredHealthConditions{};
    Serializable::BoundedVector<ManifestSignatureDescriptor, TCapacityProfile::MaximumManifestSignatures>
        SignatureDescriptors{};

    ESPRESSIO_SERIALIZABLE_TYPE(Manifest<TCapacityProfile>)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("domainTag", DomainTag),
        ESPRESSIO_PROPERTY_REQUIRED("schemaVersion", SchemaVersion),
        ESPRESSIO_PROPERTY_REQUIRED("identifier", Identifier),
        ESPRESSIO_PROPERTY_REQUIRED("release", Release),
        ESPRESSIO_PROPERTY_REQUIRED("releaseChannel", ReleaseChannel),
        ESPRESSIO_PROPERTY_REQUIRED("securityGeneration", SecurityGeneration),
        ESPRESSIO_PROPERTY_REQUIRED("requiredOTAProtocol", RequiredOTAProtocol),
        ESPRESSIO_PROPERTY_REQUIRED("requiredOTAFeatures", RequiredOTAFeatures),
        ESPRESSIO_PROPERTY_REQUIRED("targetClauses", TargetClauses),
        ESPRESSIO_PROPERTY_REQUIRED("components", Components),
        ESPRESSIO_PROPERTY_REQUIRED("artifacts", Artifacts),
        ESPRESSIO_PROPERTY_REQUIRED("dependencies", Dependencies),
        ESPRESSIO_PROPERTY_REQUIRED("requiredHealthConditions", RequiredHealthConditions),
        ESPRESSIO_PROPERTY_REQUIRED("signatureDescriptors", SignatureDescriptors))
};

template<typename TCapacityProfile>
struct ManifestSignatureValue final
    : Serializable::Serializable<ManifestSignatureValue<TCapacityProfile>> {
    Serializable::BoundedBytes<TCapacityProfile::MaximumSignatureBytes> Bytes{};

    ESPRESSIO_SERIALIZABLE_TYPE(ManifestSignatureValue<TCapacityProfile>)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("bytes", Bytes))
};

template<typename TCapacityProfile>
struct SignedManifest final
    : Serializable::Serializable<SignedManifest<TCapacityProfile>> {
    Manifest<TCapacityProfile> Content{};
    Serializable::BoundedVector<ManifestSignatureValue<TCapacityProfile>, TCapacityProfile::MaximumManifestSignatures>
        Signatures{};

    ESPRESSIO_SERIALIZABLE_TYPE(SignedManifest<TCapacityProfile>)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("content", Content),
        ESPRESSIO_PROPERTY_REQUIRED("signatures", Signatures))
};

template<typename TCapacityProfile>
bool ManifestTargetClauseValid(const ManifestTargetClause<TCapacityProfile>& clause) noexcept {
    if (!ManifestDetail::MatchModeValid(clause.ProductTypeMode) ||
        !ManifestDetail::MatchModeValid(clause.HardwareFamilyMode) ||
        !ManifestDetail::RevisionModeValid(clause.HardwareRevisionMode) ||
        !ManifestDetail::MatchModeValid(clause.ArchitectureMode) ||
        !ManifestDetail::MatchModeValid(clause.SoftwareVariantMode) ||
        !ManifestDetail::MatchModeValid(clause.CurrentStorageLayoutMode) ||
        !ManifestDetail::MatchModeValid(clause.TargetStorageLayoutMode) ||
        !ManifestDetail::MatchModeValid(clause.CurrentPersistenceSchemaMode) ||
        !ManifestDetail::MatchModeValid(clause.TargetPersistenceSchemaMode)) return false;

    const auto exact = [](std::uint8_t mode) noexcept {
        return mode == static_cast<std::uint8_t>(TargetMatchMode::Exact);
    };
    const auto any = [](std::uint8_t mode) noexcept {
        return mode == static_cast<std::uint8_t>(TargetMatchMode::Any);
    };

    if ((exact(clause.ProductTypeMode) && clause.ProductType == 0U) ||
        (any(clause.ProductTypeMode) && clause.ProductType != 0U)) return false;
    if ((exact(clause.HardwareFamilyMode) && clause.HardwareFamily == 0U) ||
        (any(clause.HardwareFamilyMode) && clause.HardwareFamily != 0U)) return false;
    if ((exact(clause.ArchitectureMode) && clause.Architecture == 0U) ||
        (any(clause.ArchitectureMode) && clause.Architecture != 0U)) return false;
    if ((exact(clause.SoftwareVariantMode) && clause.SoftwareVariant == 0U) ||
        (any(clause.SoftwareVariantMode) && clause.SoftwareVariant != 0U)) return false;

    if (exact(clause.CurrentStorageLayoutMode)) {
        if (clause.CurrentStorageLayout == 0U || clause.CurrentStorageLayoutGeneration == 0U) return false;
    } else if (clause.CurrentStorageLayout != 0U || clause.CurrentStorageLayoutGeneration != 0U) return false;
    if (exact(clause.TargetStorageLayoutMode)) {
        if (clause.TargetStorageLayout == 0U || clause.TargetStorageLayoutGeneration == 0U) return false;
    } else if (clause.TargetStorageLayout != 0U || clause.TargetStorageLayoutGeneration != 0U) return false;
    if (exact(clause.CurrentPersistenceSchemaMode)) {
        if (clause.CurrentPersistenceSchema == 0U || clause.CurrentPersistenceSchemaGeneration == 0U) return false;
    } else if (clause.CurrentPersistenceSchema != 0U || clause.CurrentPersistenceSchemaGeneration != 0U) return false;
    if (exact(clause.TargetPersistenceSchemaMode)) {
        if (clause.TargetPersistenceSchema == 0U || clause.TargetPersistenceSchemaGeneration == 0U) return false;
    } else if (clause.TargetPersistenceSchema != 0U || clause.TargetPersistenceSchemaGeneration != 0U) return false;

    const auto revisionMode = static_cast<TargetRevisionMode>(clause.HardwareRevisionMode);
    if (revisionMode == TargetRevisionMode::Any &&
        (clause.HardwareRevisionMinimum != 0U || clause.HardwareRevisionMaximum != 0U)) return false;
    if (revisionMode == TargetRevisionMode::Exact &&
        (clause.HardwareRevisionMinimum == 0U || clause.HardwareRevisionMinimum != clause.HardwareRevisionMaximum)) return false;
    if (revisionMode == TargetRevisionMode::InclusiveRange &&
        (clause.HardwareRevisionMinimum == 0U || clause.HardwareRevisionMaximum == 0U ||
         clause.HardwareRevisionMinimum > clause.HardwareRevisionMaximum)) return false;

    for (const auto componentType : clause.RequiredComponentTypes) if (componentType == 0U) return false;
    return true;
}

template<typename TCapacityProfile>
ManifestStatus CanonicalizeManifest(Manifest<TCapacityProfile>& manifest) noexcept {
    using namespace ManifestDetail;

    for (auto& clause : manifest.TargetClauses) {
        if (!ManifestTargetClauseValid(clause)) return ManifestStatus::Invalid;
        InsertionSort(clause.RequiredComponentTypes,
            [](std::uint64_t left, std::uint64_t right) noexcept { return left < right; });
        if (HasAdjacentDuplicate(clause.RequiredComponentTypes,
            [](std::uint64_t left, std::uint64_t right) noexcept { return left == right; })) {
            return ManifestStatus::DuplicateReference;
        }
    }

    for (auto& component : manifest.Components) {
        if (component.Identifier == 0U || component.TypeId == 0U || component.ParameterSchemaVersion == 0U) {
            return ManifestStatus::Invalid;
        }
        InsertionSort(component.Artifacts,
            [](const auto& left, const auto& right) noexcept { return Less128(left, right); });
        if (HasAdjacentDuplicate(component.Artifacts,
            [](const auto& left, const auto& right) noexcept { return Equal128(left, right); })) {
            return ManifestStatus::DuplicateReference;
        }
        for (const auto& artifact : component.Artifacts) if (!NonZero128(artifact)) return ManifestStatus::Invalid;
    }

    for (const auto& artifact : manifest.Artifacts) {
        if (!NonZero128(artifact.Identifier) || artifact.DigestAlgorithm == 0U || artifact.Digest.empty()) {
            return ManifestStatus::Invalid;
        }
    }

    InsertionSort(manifest.Components,
        [](const auto& left, const auto& right) noexcept { return left.Identifier < right.Identifier; });
    if (HasAdjacentDuplicate(manifest.Components,
        [](const auto& left, const auto& right) noexcept { return left.Identifier == right.Identifier; })) {
        return ManifestStatus::DuplicateIdentifier;
    }

    InsertionSort(manifest.Artifacts,
        [](const auto& left, const auto& right) noexcept { return Less128(left.Identifier, right.Identifier); });
    if (HasAdjacentDuplicate(manifest.Artifacts,
        [](const auto& left, const auto& right) noexcept { return Equal128(left.Identifier, right.Identifier); })) {
        return ManifestStatus::DuplicateIdentifier;
    }

    InsertionSort(manifest.Dependencies,
        [](const auto& left, const auto& right) noexcept {
            return left.Component < right.Component ||
                   (left.Component == right.Component && left.DependsOn < right.DependsOn);
        });
    if (HasAdjacentDuplicate(manifest.Dependencies,
        [](const auto& left, const auto& right) noexcept {
            return left.Component == right.Component && left.DependsOn == right.DependsOn;
        })) return ManifestStatus::DuplicateReference;

    InsertionSort(manifest.RequiredHealthConditions,
        [](std::uint64_t left, std::uint64_t right) noexcept { return left < right; });
    if (HasAdjacentDuplicate(manifest.RequiredHealthConditions,
        [](std::uint64_t left, std::uint64_t right) noexcept { return left == right; })) {
        return ManifestStatus::DuplicateReference;
    }

    InsertionSort(manifest.SignatureDescriptors,
        [](const auto& left, const auto& right) noexcept {
            if (left.Algorithm != right.Algorithm) return left.Algorithm < right.Algorithm;
            if (left.TrustAnchor != right.TrustAnchor) return left.TrustAnchor < right.TrustAnchor;
            return left.TrustPolicy < right.TrustPolicy;
        });
    if (HasAdjacentDuplicate(manifest.SignatureDescriptors,
        [](const auto& left, const auto& right) noexcept {
            return left.Algorithm == right.Algorithm &&
                   left.TrustAnchor == right.TrustAnchor &&
                   left.TrustPolicy == right.TrustPolicy;
        })) return ManifestStatus::DuplicateReference;

    return ManifestStatus::Success;
}

template<typename TCapacityProfile>
ManifestStatus ValidateManifest(const Manifest<TCapacityProfile>& manifest) noexcept {
    using namespace ManifestDetail;
    if (manifest.DomainTag != ManifestDomainTag || manifest.SchemaVersion != ManifestSchemaV1.Value() ||
        !NonZero128(manifest.Identifier) || manifest.Release == 0U || manifest.RequiredOTAProtocol == 0U ||
        manifest.TargetClauses.empty() || manifest.Components.empty() || manifest.SignatureDescriptors.empty()) {
        return ManifestStatus::Invalid;
    }

    for (const auto& clause : manifest.TargetClauses) {
        if (!ManifestTargetClauseValid(clause)) return ManifestStatus::Invalid;
        if (!IsStrictlySorted(clause.RequiredComponentTypes,
                [](std::uint64_t left, std::uint64_t right) noexcept { return left < right; })) {
            return ManifestStatus::NonCanonical;
        }
    }

    if (!IsStrictlySorted(manifest.Components,
            [](const auto& left, const auto& right) noexcept { return left.Identifier < right.Identifier; })) {
        return ManifestStatus::NonCanonical;
    }
    if (!IsStrictlySorted(manifest.Artifacts,
            [](const auto& left, const auto& right) noexcept { return Less128(left.Identifier, right.Identifier); })) {
        return ManifestStatus::NonCanonical;
    }
    if (!IsStrictlySorted(manifest.Dependencies,
            [](const auto& left, const auto& right) noexcept {
                return left.Component < right.Component ||
                       (left.Component == right.Component && left.DependsOn < right.DependsOn);
            })) return ManifestStatus::NonCanonical;
    if (!IsStrictlySorted(manifest.RequiredHealthConditions,
            [](std::uint64_t left, std::uint64_t right) noexcept { return left < right; })) {
        return ManifestStatus::NonCanonical;
    }
    if (!IsStrictlySorted(manifest.SignatureDescriptors,
            [](const auto& left, const auto& right) noexcept {
                if (left.Algorithm != right.Algorithm) return left.Algorithm < right.Algorithm;
                if (left.TrustAnchor != right.TrustAnchor) return left.TrustAnchor < right.TrustAnchor;
                return left.TrustPolicy < right.TrustPolicy;
            })) return ManifestStatus::NonCanonical;

    for (const auto condition : manifest.RequiredHealthConditions) if (condition == 0U) return ManifestStatus::Invalid;
    for (const auto& descriptor : manifest.SignatureDescriptors) {
        if (descriptor.Algorithm == 0U || descriptor.TrustAnchor == 0U || descriptor.TrustPolicy == 0U) {
            return ManifestStatus::Invalid;
        }
    }

    for (const auto& artifact : manifest.Artifacts) {
        if (!NonZero128(artifact.Identifier) || artifact.DigestAlgorithm == 0U || artifact.Digest.empty()) {
            return ManifestStatus::Invalid;
        }
    }

    for (const auto& component : manifest.Components) {
        if (component.Identifier == 0U || component.TypeId == 0U || component.ParameterSchemaVersion == 0U) {
            return ManifestStatus::Invalid;
        }
        if (!IsStrictlySorted(component.Artifacts,
                [](const auto& left, const auto& right) noexcept { return Less128(left, right); })) {
            return ManifestStatus::NonCanonical;
        }
        for (const auto& referenced : component.Artifacts) {
            if (!NonZero128(referenced)) return ManifestStatus::Invalid;
            bool found = false;
            for (const auto& artifact : manifest.Artifacts) {
                if (Equal128(referenced, artifact.Identifier)) { found = true; break; }
            }
            if (!found) return ManifestStatus::MissingReference;
        }
    }

    for (const auto& dependency : manifest.Dependencies) {
        if (dependency.Component == 0U || dependency.DependsOn == 0U || dependency.Component == dependency.DependsOn) {
            return ManifestStatus::Invalid;
        }
        bool componentFound = false;
        bool dependencyFound = false;
        for (const auto& component : manifest.Components) {
            if (component.Identifier == dependency.Component) componentFound = true;
            if (component.Identifier == dependency.DependsOn) dependencyFound = true;
        }
        if (!componentFound || !dependencyFound) return ManifestStatus::MissingReference;
    }

    std::array<std::uint16_t, TCapacityProfile::MaximumComponents> indegree{};
    std::array<bool, TCapacityProfile::MaximumComponents> consumed{};
    for (const auto& edge : manifest.Dependencies) {
        for (std::size_t i = 0U; i < manifest.Components.size(); ++i) {
            if (manifest.Components[i].Identifier == edge.Component) {
                ++indegree[i];
                break;
            }
        }
    }
    std::size_t processed = 0U;
    while (processed < manifest.Components.size()) {
        std::size_t selected = manifest.Components.size();
        for (std::size_t i = 0U; i < manifest.Components.size(); ++i) {
            if (!consumed[i] && indegree[i] == 0U) { selected = i; break; }
        }
        if (selected == manifest.Components.size()) return ManifestStatus::DependencyCycle;
        consumed[selected] = true;
        ++processed;
        const std::uint32_t completed = manifest.Components[selected].Identifier;
        for (const auto& edge : manifest.Dependencies) {
            if (edge.DependsOn != completed) continue;
            for (std::size_t i = 0U; i < manifest.Components.size(); ++i) {
                if (manifest.Components[i].Identifier == edge.Component && indegree[i] != 0U) {
                    --indegree[i];
                    break;
                }
            }
        }
    }

    return ManifestStatus::Success;
}

template<typename TCapacityProfile>
ManifestStatus PrepareManifestForSigning(Manifest<TCapacityProfile>& manifest) noexcept {
    const auto canonical = CanonicalizeManifest(manifest);
    if (canonical != ManifestStatus::Success) return canonical;
    return ValidateManifest(manifest);
}

template<typename TCapacityProfile>
ManifestStatus SerializeCanonicalManifest(
    const Manifest<TCapacityProfile>& manifest,
    std::uint8_t* output,
    std::size_t capacity,
    std::size_t& written) noexcept {
    written = 0U;
    const auto valid = ValidateManifest(manifest);
    if (valid != ManifestStatus::Success) return valid;
    const auto encoded = Serializable::SerializeDirectBinary(manifest, output, capacity);
    if (!encoded) return encoded.Error == Serializable::SerializationErrorCode::ResourceLimitExceeded
        ? ManifestStatus::CapacityUnavailable
        : ManifestStatus::SerializationFailed;
    written = encoded.Bytes;
    return ManifestStatus::Success;
}

template<typename TCapacityProfile>
ManifestStatus ValidateSignedManifestEnvelope(const SignedManifest<TCapacityProfile>& envelope) noexcept {
    const auto content = ValidateManifest(envelope.Content);
    if (content != ManifestStatus::Success) return content;
    if (envelope.Signatures.size() != envelope.Content.SignatureDescriptors.size()) {
        return ManifestStatus::SignatureCountMismatch;
    }
    for (const auto& signature : envelope.Signatures) if (signature.Bytes.empty()) return ManifestStatus::Invalid;
    return ManifestStatus::Success;
}

} // namespace ESPressio::OTA
