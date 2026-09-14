#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTACapacityProfile.hpp"
#include "ESPressio_OTATypes.hpp"

#include "ESPressio_SystemClassificationIdentifiers.hpp"
#include "ESPressio_PlatformOTA.hpp"
#include "ESPressio_PlatformOTALayoutGeneration.hpp"
#include "ESPressio_PersistenceTypes.hpp"

namespace ESPressio::OTA {

enum class TargetProfileStatus : std::uint8_t {
    Success,
    Frozen,
    Invalid,
    DuplicateComponentType,
    CapacityUnavailable
};

template<typename TCapacityProfile>
class UpdateTargetProfile final {
    static_assert(TCapacityProfile::IsValid, "UpdateTargetProfile requires a valid OTA capacity profile");

    System::ProductTypeIdentifier productType_{};
    System::HardwareFamilyIdentifier hardwareFamily_{};
    System::HardwareRevision hardwareRevision_{};
    System::ArchitectureIdentifier architecture_{};
    System::SoftwareVariantIdentifier softwareVariant_{};
    Platform::OTA::StorageLayoutIdentifier storageLayout_{};
    Platform::OTA::StorageLayoutGeneration storageLayoutGeneration_{};
    Persistence::SchemaIdentifier persistenceSchema_{};
    Persistence::SchemaGeneration persistenceSchemaGeneration_{};
    OTAProtocolVersion otaProtocol_{OTAProtocolV1};
    OTAFeatureFlags featureFlags_{0U};
    std::array<ComponentTypeId, TCapacityProfile::MaximumSupportedComponentTypes> supportedComponentTypes_{};
    std::size_t supportedComponentTypeCount_{0U};
    UpdateTargetProfileFingerprint fingerprint_{};
    bool frozen_{false};

    constexpr void SortComponentTypes() noexcept {
        for (std::size_t i = 1U; i < supportedComponentTypeCount_; ++i) {
            ComponentTypeId current = supportedComponentTypes_[i];
            std::size_t position = i;
            while (position != 0U && current < supportedComponentTypes_[position - 1U]) {
                supportedComponentTypes_[position] = supportedComponentTypes_[position - 1U];
                --position;
            }
            supportedComponentTypes_[position] = current;
        }
    }

public:
    constexpr bool IsFrozen() const noexcept { return frozen_; }
    constexpr const UpdateTargetProfileFingerprint& Fingerprint() const noexcept { return fingerprint_; }

    constexpr System::ProductTypeIdentifier ProductType() const noexcept { return productType_; }
    constexpr System::HardwareFamilyIdentifier HardwareFamily() const noexcept { return hardwareFamily_; }
    constexpr System::HardwareRevision HardwareRevision() const noexcept { return hardwareRevision_; }
    constexpr System::ArchitectureIdentifier Architecture() const noexcept { return architecture_; }
    constexpr System::SoftwareVariantIdentifier SoftwareVariant() const noexcept { return softwareVariant_; }
    constexpr Platform::OTA::StorageLayoutIdentifier StorageLayout() const noexcept { return storageLayout_; }
    constexpr Platform::OTA::StorageLayoutGeneration StorageLayoutGeneration() const noexcept { return storageLayoutGeneration_; }
    constexpr Persistence::SchemaIdentifier PersistenceSchema() const noexcept { return persistenceSchema_; }
    constexpr Persistence::SchemaGeneration PersistenceSchemaGeneration() const noexcept { return persistenceSchemaGeneration_; }
    constexpr OTAProtocolVersion ProtocolVersion() const noexcept { return otaProtocol_; }
    constexpr OTAFeatureFlags FeatureFlags() const noexcept { return featureFlags_; }
    constexpr std::size_t SupportedComponentTypeCount() const noexcept { return supportedComponentTypeCount_; }
    constexpr ComponentTypeId SupportedComponentType(std::size_t index) const noexcept {
        return index < supportedComponentTypeCount_ ? supportedComponentTypes_[index] : ComponentTypeId{};
    }

    constexpr TargetProfileStatus SetSystemIdentity(
        System::ProductTypeIdentifier productType,
        System::HardwareFamilyIdentifier hardwareFamily,
        System::HardwareRevision hardwareRevision,
        System::ArchitectureIdentifier architecture,
        System::SoftwareVariantIdentifier softwareVariant) noexcept {
        if (frozen_) return TargetProfileStatus::Frozen;
        if (!productType || !hardwareFamily || !hardwareRevision || !architecture || !softwareVariant) {
            return TargetProfileStatus::Invalid;
        }
        productType_ = productType;
        hardwareFamily_ = hardwareFamily;
        hardwareRevision_ = hardwareRevision;
        architecture_ = architecture;
        softwareVariant_ = softwareVariant;
        return TargetProfileStatus::Success;
    }

    constexpr TargetProfileStatus SetStorageLayout(
        Platform::OTA::StorageLayoutIdentifier layout,
        Platform::OTA::StorageLayoutGeneration generation) noexcept {
        if (frozen_) return TargetProfileStatus::Frozen;
        if (!layout || !generation) return TargetProfileStatus::Invalid;
        storageLayout_ = layout;
        storageLayoutGeneration_ = generation;
        return TargetProfileStatus::Success;
    }

    constexpr TargetProfileStatus SetPersistenceSchema(
        Persistence::SchemaIdentifier schema,
        Persistence::SchemaGeneration generation) noexcept {
        if (frozen_) return TargetProfileStatus::Frozen;
        if (!schema || !generation) return TargetProfileStatus::Invalid;
        persistenceSchema_ = schema;
        persistenceSchemaGeneration_ = generation;
        return TargetProfileStatus::Success;
    }

    constexpr TargetProfileStatus SetOTASupport(
        OTAProtocolVersion protocol,
        OTAFeatureFlags features) noexcept {
        if (frozen_) return TargetProfileStatus::Frozen;
        if (!protocol) return TargetProfileStatus::Invalid;
        otaProtocol_ = protocol;
        featureFlags_ = features;
        return TargetProfileStatus::Success;
    }

    constexpr TargetProfileStatus AddSupportedComponentType(ComponentTypeId type) noexcept {
        if (frozen_) return TargetProfileStatus::Frozen;
        if (!type) return TargetProfileStatus::Invalid;
        for (std::size_t i = 0U; i < supportedComponentTypeCount_; ++i) {
            if (supportedComponentTypes_[i] == type) return TargetProfileStatus::DuplicateComponentType;
        }
        if (supportedComponentTypeCount_ == supportedComponentTypes_.size()) {
            return TargetProfileStatus::CapacityUnavailable;
        }
        supportedComponentTypes_[supportedComponentTypeCount_++] = type;
        return TargetProfileStatus::Success;
    }

    constexpr bool IsStructurallyComplete() const noexcept {
        return bool(productType_) && bool(hardwareFamily_) && bool(hardwareRevision_) &&
               bool(architecture_) && bool(softwareVariant_) && bool(storageLayout_) &&
               bool(storageLayoutGeneration_) && bool(persistenceSchema_) &&
               bool(persistenceSchemaGeneration_) && bool(otaProtocol_);
    }

    /**
     * Canonicalizes the bounded set before fingerprint calculation.
     * The profile is not externally frozen until SetFingerprintAndFreeze succeeds.
     */
    constexpr TargetProfileStatus Canonicalize() noexcept {
        if (frozen_) return TargetProfileStatus::Frozen;
        if (!IsStructurallyComplete()) return TargetProfileStatus::Invalid;
        SortComponentTypes();
        return TargetProfileStatus::Success;
    }

    constexpr TargetProfileStatus SetFingerprintAndFreeze(UpdateTargetProfileFingerprint fingerprint) noexcept {
        if (frozen_) return TargetProfileStatus::Frozen;
        if (!IsStructurallyComplete() || !fingerprint) return TargetProfileStatus::Invalid;
        SortComponentTypes();
        fingerprint_ = fingerprint;
        frozen_ = true;
        return TargetProfileStatus::Success;
    }
};

} // namespace ESPressio::OTA
