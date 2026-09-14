#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAProfile.hpp"
#include "ESPressio_Verification.hpp"

#include "ESPressio_BoundedContainers.hpp"
#include "ESPressio_DirectBinaryArchive.hpp"
#include "ESPressio_SerializableBase.hpp"
#include "ESPressio_SerializationMacros.hpp"
#include "ESPressio_SerializationTraits.hpp"

namespace ESPressio::OTA {

/** Stable domain marker mixed into the canonical profile fingerprint input. */
inline constexpr std::uint64_t UpdateTargetProfileDomainTag = 0x4553504F54415031ULL; // "ESPOTAP1"

template<typename TCapacityProfile>
struct UpdateTargetProfileCanonicalWire final
    : Serializable::Serializable<UpdateTargetProfileCanonicalWire<TCapacityProfile>> {
    static_assert(TCapacityProfile::IsValid, "UpdateTargetProfile wire requires a valid capacity profile");

    std::uint64_t DomainTag{UpdateTargetProfileDomainTag};
    std::uint16_t ProfileSchemaVersion{UpdateTargetProfileSchemaV1.Value()};
    std::uint64_t ProductType{0U};
    std::uint64_t HardwareFamily{0U};
    std::uint32_t HardwareRevision{0U};
    std::uint64_t Architecture{0U};
    std::uint64_t SoftwareVariant{0U};
    std::uint64_t StorageLayout{0U};
    std::uint32_t StorageLayoutGeneration{0U};
    std::uint64_t PersistenceSchema{0U};
    std::uint32_t PersistenceSchemaGeneration{0U};
    std::uint16_t OTAProtocol{0U};
    std::uint64_t OTAFeatures{0U};
    Serializable::BoundedVector<std::uint64_t, TCapacityProfile::MaximumSupportedComponentTypes>
        SupportedComponentTypes{};

    ESPRESSIO_SERIALIZABLE_TYPE(UpdateTargetProfileCanonicalWire)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("domainTag", DomainTag),
        ESPRESSIO_PROPERTY_REQUIRED("profileSchemaVersion", ProfileSchemaVersion),
        ESPRESSIO_PROPERTY_REQUIRED("productType", ProductType),
        ESPRESSIO_PROPERTY_REQUIRED("hardwareFamily", HardwareFamily),
        ESPRESSIO_PROPERTY_REQUIRED("hardwareRevision", HardwareRevision),
        ESPRESSIO_PROPERTY_REQUIRED("architecture", Architecture),
        ESPRESSIO_PROPERTY_REQUIRED("softwareVariant", SoftwareVariant),
        ESPRESSIO_PROPERTY_REQUIRED("storageLayout", StorageLayout),
        ESPRESSIO_PROPERTY_REQUIRED("storageLayoutGeneration", StorageLayoutGeneration),
        ESPRESSIO_PROPERTY_REQUIRED("persistenceSchema", PersistenceSchema),
        ESPRESSIO_PROPERTY_REQUIRED("persistenceSchemaGeneration", PersistenceSchemaGeneration),
        ESPRESSIO_PROPERTY_REQUIRED("otaProtocol", OTAProtocol),
        ESPRESSIO_PROPERTY_REQUIRED("otaFeatures", OTAFeatures),
        ESPRESSIO_PROPERTY_REQUIRED("supportedComponentTypes", SupportedComponentTypes))
};

enum class TargetProfileFingerprintStatus : std::uint8_t {
    Success,
    InvalidProfile,
    SerializationFailed,
    DigestUnsupported,
    DigestCapacityMismatch,
    DigestFailed
};

template<typename TCapacityProfile>
TargetProfileFingerprintStatus BuildCanonicalProfileWire(
    const UpdateTargetProfile<TCapacityProfile>& profile,
    UpdateTargetProfileCanonicalWire<TCapacityProfile>& wire) noexcept {
    if (!profile.IsStructurallyComplete()) return TargetProfileFingerprintStatus::InvalidProfile;

    wire.DomainTag = UpdateTargetProfileDomainTag;
    wire.ProfileSchemaVersion = UpdateTargetProfileSchemaV1.Value();
    wire.ProductType = profile.ProductType().Value();
    wire.HardwareFamily = profile.HardwareFamily().Value();
    wire.HardwareRevision = profile.HardwareRevision().Value();
    wire.Architecture = profile.Architecture().Value();
    wire.SoftwareVariant = profile.SoftwareVariant().Value();
    wire.StorageLayout = profile.StorageLayout().Value();
    wire.StorageLayoutGeneration = profile.StorageLayoutGeneration().Value();
    wire.PersistenceSchema = profile.PersistenceSchema().Value();
    wire.PersistenceSchemaGeneration = profile.PersistenceSchemaGeneration().Value();
    wire.OTAProtocol = profile.ProtocolVersion().Value();
    wire.OTAFeatures = profile.FeatureFlags();
    wire.SupportedComponentTypes.clear();
    for (std::size_t i = 0U; i < profile.SupportedComponentTypeCount(); ++i) {
        if (!wire.SupportedComponentTypes.push_back(profile.SupportedComponentType(i).Value())) {
            return TargetProfileFingerprintStatus::InvalidProfile;
        }
    }
    return TargetProfileFingerprintStatus::Success;
}

template<typename TCapacityProfile>
TargetProfileFingerprintStatus ComputeUpdateTargetProfileFingerprint(
    const UpdateTargetProfile<TCapacityProfile>& profile,
    Security::IStreamingDigest& digest,
    UpdateTargetProfileFingerprint& fingerprint) noexcept {
    using Wire = UpdateTargetProfileCanonicalWire<TCapacityProfile>;
    static_assert(Serializable::IsBoundedSerializable<Wire>,
                  "UpdateTargetProfile canonical wire form must remain bounded");

    Wire wire;
    const auto built = BuildCanonicalProfileWire(profile, wire);
    if (built != TargetProfileFingerprintStatus::Success) return built;

    std::array<std::uint8_t, Serializable::MaximumSerializedSize<Wire, Serializable::DirectBinary>> encoded{};
    const auto serialized = Serializable::SerializeDirectBinary(wire, encoded.data(), encoded.size());
    if (!serialized) return TargetProfileFingerprintStatus::SerializationFailed;

    if (!digest.Supports(Security::DigestAlgorithm::SHA256)) {
        return TargetProfileFingerprintStatus::DigestUnsupported;
    }
    if (digest.DigestSize(Security::DigestAlgorithm::SHA256) != 32U) {
        return TargetProfileFingerprintStatus::DigestCapacityMismatch;
    }
    if (!digest.Begin(Security::DigestAlgorithm::SHA256)) {
        return TargetProfileFingerprintStatus::DigestFailed;
    }
    if (!digest.Update({encoded.data(), serialized.Bytes})) {
        return TargetProfileFingerprintStatus::DigestFailed;
    }

    std::array<std::uint8_t, 32> bytes{};
    std::size_t written = 0U;
    if (!digest.Finalize({bytes.data(), bytes.size()}, written) || written != bytes.size()) {
        return TargetProfileFingerprintStatus::DigestFailed;
    }

    fingerprint = UpdateTargetProfileFingerprint{bytes};
    return TargetProfileFingerprintStatus::Success;
}

template<typename TCapacityProfile>
TargetProfileFingerprintStatus FinalizeUpdateTargetProfile(
    UpdateTargetProfile<TCapacityProfile>& profile,
    Security::IStreamingDigest& digest) noexcept {
    if (profile.Canonicalize() != TargetProfileStatus::Success) {
        return TargetProfileFingerprintStatus::InvalidProfile;
    }

    UpdateTargetProfileFingerprint fingerprint;
    const auto status = ComputeUpdateTargetProfileFingerprint(profile, digest, fingerprint);
    if (status != TargetProfileFingerprintStatus::Success) return status;
    if (profile.SetFingerprintAndFreeze(fingerprint) != TargetProfileStatus::Success) {
        return TargetProfileFingerprintStatus::InvalidProfile;
    }
    return TargetProfileFingerprintStatus::Success;
}

} // namespace ESPressio::OTA
