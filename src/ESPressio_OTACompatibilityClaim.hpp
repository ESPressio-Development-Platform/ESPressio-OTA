#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTACapacityProfile.hpp"
#include "ESPressio_OTAProfile.hpp"
#include "ESPressio_OTATypes.hpp"

namespace ESPressio::OTA {

inline constexpr std::uint8_t CompatibilityClaimTokenSchemaV1 = 1U;
inline constexpr std::size_t CompatibilityClaimBloomBytes = 16U;

enum class CompatibilityClaimTokenRole : std::uint8_t {
    DeviceProfile = 1U,
    CandidateClause = 2U
};

enum class CompatibilityClaimStatus : std::uint8_t {
    Success,
    Invalid,
    NotFrozen,
    DuplicateComponentType,
    CapacityUnavailable,
    MalformedToken,
    UnsupportedSchema
};

enum class RevisionConstraintMode : std::uint8_t {
    Any = 0U,
    Exact = 1U,
    InclusiveRange = 2U
};

template<typename T>
struct OptionalExactConstraint final {
    bool HasValue{false};
    T Value{};

    constexpr void Any() noexcept {
        HasValue = false;
        Value = T{};
    }

    constexpr void Exact(T value) noexcept {
        HasValue = true;
        Value = value;
    }
};

struct HardwareRevisionConstraint final {
    RevisionConstraintMode Mode{RevisionConstraintMode::Any};
    System::HardwareRevision Minimum{};
    System::HardwareRevision Maximum{};

    constexpr bool IsValid() const noexcept {
        switch (Mode) {
            case RevisionConstraintMode::Any:
                return true;
            case RevisionConstraintMode::Exact:
                return bool(Minimum) && Minimum == Maximum;
            case RevisionConstraintMode::InclusiveRange:
                return bool(Minimum) && bool(Maximum) && Minimum <= Maximum;
        }
        return false;
    }

    constexpr void Any() noexcept {
        Mode = RevisionConstraintMode::Any;
        Minimum = {};
        Maximum = {};
    }

    constexpr void Exact(System::HardwareRevision revision) noexcept {
        Mode = RevisionConstraintMode::Exact;
        Minimum = revision;
        Maximum = revision;
    }

    constexpr void Range(System::HardwareRevision minimum, System::HardwareRevision maximum) noexcept {
        Mode = RevisionConstraintMode::InclusiveRange;
        Minimum = minimum;
        Maximum = maximum;
    }
};

template<typename TCapacityProfile>
class CandidateCompatibilityClaim final {
    static_assert(TCapacityProfile::IsValid, "CandidateCompatibilityClaim requires a valid OTA capacity profile");

    System::ProductTypeIdentifier productType_{};
    OptionalExactConstraint<System::HardwareFamilyIdentifier> hardwareFamily_{};
    HardwareRevisionConstraint hardwareRevision_{};
    OptionalExactConstraint<System::ArchitectureIdentifier> architecture_{};
    OptionalExactConstraint<System::SoftwareVariantIdentifier> softwareVariant_{};
    OptionalExactConstraint<Platform::OTA::StorageLayoutIdentifier> currentStorageLayout_{};
    OptionalExactConstraint<Platform::OTA::StorageLayoutGeneration> currentStorageLayoutGeneration_{};
    OptionalExactConstraint<Persistence::SchemaIdentifier> currentPersistenceSchema_{};
    OptionalExactConstraint<Persistence::SchemaGeneration> currentPersistenceSchemaGeneration_{};
    UpdateTargetProfileSchemaVersion minimumProfileSchema_{UpdateTargetProfileSchemaV1};
    OTAProtocolVersion minimumOTAProtocol_{OTAProtocolV1};
    OTAFeatureFlags requiredFeatures_{0U};
    std::array<ComponentTypeId, TCapacityProfile::MaximumSupportedComponentTypes> requiredComponentTypes_{};
    std::size_t requiredComponentTypeCount_{0U};

public:
    constexpr System::ProductTypeIdentifier ProductType() const noexcept { return productType_; }
    constexpr const auto& HardwareFamily() const noexcept { return hardwareFamily_; }
    constexpr const auto& HardwareRevision() const noexcept { return hardwareRevision_; }
    constexpr const auto& Architecture() const noexcept { return architecture_; }
    constexpr const auto& SoftwareVariant() const noexcept { return softwareVariant_; }
    constexpr const auto& CurrentStorageLayout() const noexcept { return currentStorageLayout_; }
    constexpr const auto& CurrentStorageLayoutGeneration() const noexcept { return currentStorageLayoutGeneration_; }
    constexpr const auto& CurrentPersistenceSchema() const noexcept { return currentPersistenceSchema_; }
    constexpr const auto& CurrentPersistenceSchemaGeneration() const noexcept { return currentPersistenceSchemaGeneration_; }
    constexpr UpdateTargetProfileSchemaVersion MinimumProfileSchema() const noexcept { return minimumProfileSchema_; }
    constexpr OTAProtocolVersion MinimumOTAProtocol() const noexcept { return minimumOTAProtocol_; }
    constexpr OTAFeatureFlags RequiredFeatures() const noexcept { return requiredFeatures_; }
    constexpr std::size_t RequiredComponentTypeCount() const noexcept { return requiredComponentTypeCount_; }
    constexpr ComponentTypeId RequiredComponentType(std::size_t index) const noexcept {
        return index < requiredComponentTypeCount_ ? requiredComponentTypes_[index] : ComponentTypeId{};
    }

    constexpr CompatibilityClaimStatus SetProductType(System::ProductTypeIdentifier value) noexcept {
        if (!value) return CompatibilityClaimStatus::Invalid;
        productType_ = value;
        return CompatibilityClaimStatus::Success;
    }

    constexpr void SetHardwareFamilyAny() noexcept { hardwareFamily_.Any(); }
    constexpr CompatibilityClaimStatus SetHardwareFamilyExact(System::HardwareFamilyIdentifier value) noexcept {
        if (!value) return CompatibilityClaimStatus::Invalid;
        hardwareFamily_.Exact(value);
        return CompatibilityClaimStatus::Success;
    }

    constexpr void SetHardwareRevisionAny() noexcept { hardwareRevision_.Any(); }
    constexpr CompatibilityClaimStatus SetHardwareRevisionExact(System::HardwareRevision value) noexcept {
        if (!value) return CompatibilityClaimStatus::Invalid;
        hardwareRevision_.Exact(value);
        return CompatibilityClaimStatus::Success;
    }
    constexpr CompatibilityClaimStatus SetHardwareRevisionRange(
        System::HardwareRevision minimum,
        System::HardwareRevision maximum) noexcept {
        if (!minimum || !maximum || maximum < minimum) return CompatibilityClaimStatus::Invalid;
        hardwareRevision_.Range(minimum, maximum);
        return CompatibilityClaimStatus::Success;
    }

    constexpr void SetArchitectureAny() noexcept { architecture_.Any(); }
    constexpr CompatibilityClaimStatus SetArchitectureExact(System::ArchitectureIdentifier value) noexcept {
        if (!value) return CompatibilityClaimStatus::Invalid;
        architecture_.Exact(value);
        return CompatibilityClaimStatus::Success;
    }

    constexpr void SetSoftwareVariantAny() noexcept { softwareVariant_.Any(); }
    constexpr CompatibilityClaimStatus SetSoftwareVariantExact(System::SoftwareVariantIdentifier value) noexcept {
        if (!value) return CompatibilityClaimStatus::Invalid;
        softwareVariant_.Exact(value);
        return CompatibilityClaimStatus::Success;
    }

    constexpr void SetCurrentStorageLayoutAny() noexcept {
        currentStorageLayout_.Any();
        currentStorageLayoutGeneration_.Any();
    }
    constexpr CompatibilityClaimStatus SetCurrentStorageLayoutExact(
        Platform::OTA::StorageLayoutIdentifier layout,
        Platform::OTA::StorageLayoutGeneration generation = {}) noexcept {
        if (!layout) return CompatibilityClaimStatus::Invalid;
        currentStorageLayout_.Exact(layout);
        if (generation) currentStorageLayoutGeneration_.Exact(generation);
        else currentStorageLayoutGeneration_.Any();
        return CompatibilityClaimStatus::Success;
    }

    constexpr void SetCurrentPersistenceSchemaAny() noexcept {
        currentPersistenceSchema_.Any();
        currentPersistenceSchemaGeneration_.Any();
    }
    constexpr CompatibilityClaimStatus SetCurrentPersistenceSchemaExact(
        Persistence::SchemaIdentifier schema,
        Persistence::SchemaGeneration generation = {}) noexcept {
        if (!schema) return CompatibilityClaimStatus::Invalid;
        currentPersistenceSchema_.Exact(schema);
        if (generation) currentPersistenceSchemaGeneration_.Exact(generation);
        else currentPersistenceSchemaGeneration_.Any();
        return CompatibilityClaimStatus::Success;
    }

    constexpr CompatibilityClaimStatus SetMinimumProfileSchema(UpdateTargetProfileSchemaVersion version) noexcept {
        if (!version) return CompatibilityClaimStatus::Invalid;
        minimumProfileSchema_ = version;
        return CompatibilityClaimStatus::Success;
    }

    constexpr CompatibilityClaimStatus SetMinimumOTAProtocol(OTAProtocolVersion version) noexcept {
        if (!version) return CompatibilityClaimStatus::Invalid;
        minimumOTAProtocol_ = version;
        return CompatibilityClaimStatus::Success;
    }

    constexpr void SetRequiredFeatures(OTAFeatureFlags features) noexcept { requiredFeatures_ = features; }

    constexpr CompatibilityClaimStatus AddRequiredComponentType(ComponentTypeId type) noexcept {
        if (!type) return CompatibilityClaimStatus::Invalid;
        for (std::size_t i = 0U; i < requiredComponentTypeCount_; ++i) {
            if (requiredComponentTypes_[i] == type) return CompatibilityClaimStatus::DuplicateComponentType;
        }
        if (requiredComponentTypeCount_ == requiredComponentTypes_.size()) {
            return CompatibilityClaimStatus::CapacityUnavailable;
        }
        requiredComponentTypes_[requiredComponentTypeCount_++] = type;
        return CompatibilityClaimStatus::Success;
    }

    constexpr bool IsValid() const noexcept {
        return bool(productType_) && hardwareRevision_.IsValid() &&
               bool(minimumProfileSchema_) && bool(minimumOTAProtocol_) &&
               (!currentStorageLayoutGeneration_.HasValue || currentStorageLayout_.HasValue) &&
               (!currentPersistenceSchemaGeneration_.HasValue || currentPersistenceSchema_.HasValue);
    }
};

template<typename TCapacityProfile>
struct CompatibilityClaimToken final {
    static_assert(TCapacityProfile::MaximumCompatibilityClaimTokenBytes <= 255U,
                  "Claim-token length is encoded as one byte");
    std::uint8_t TokenSchemaVersion{CompatibilityClaimTokenSchemaV1};
    std::uint8_t TokenLength{0U};
    std::array<std::uint8_t, TCapacityProfile::MaximumCompatibilityClaimTokenBytes> TokenBytes{};

    constexpr bool IsEmpty() const noexcept { return TokenLength == 0U; }
};

namespace CompatibilityClaimDetail {

inline constexpr std::uint16_t CandidateHasHardwareFamily = 1U << 0U;
inline constexpr std::uint16_t CandidateHasArchitecture = 1U << 1U;
inline constexpr std::uint16_t CandidateHasSoftwareVariant = 1U << 2U;
inline constexpr std::uint16_t CandidateHasStorageLayout = 1U << 3U;
inline constexpr std::uint16_t CandidateHasStorageLayoutGeneration = 1U << 4U;
inline constexpr std::uint16_t CandidateHasPersistenceSchema = 1U << 5U;
inline constexpr std::uint16_t CandidateHasPersistenceSchemaGeneration = 1U << 6U;

class Writer final {
    std::uint8_t* data_{nullptr};
    std::size_t capacity_{0U};
    std::size_t size_{0U};
public:
    constexpr Writer(std::uint8_t* data, std::size_t capacity) noexcept : data_(data), capacity_(capacity) {}
    constexpr bool U8(std::uint8_t value) noexcept {
        if (size_ == capacity_) return false;
        data_[size_++] = value;
        return true;
    }
    constexpr bool U16(std::uint16_t value) noexcept {
        return U8(static_cast<std::uint8_t>(value)) && U8(static_cast<std::uint8_t>(value >> 8U));
    }
    constexpr bool U32(std::uint32_t value) noexcept {
        for (unsigned shift = 0U; shift < 32U; shift += 8U) if (!U8(static_cast<std::uint8_t>(value >> shift))) return false;
        return true;
    }
    constexpr bool U64(std::uint64_t value) noexcept {
        for (unsigned shift = 0U; shift < 64U; shift += 8U) if (!U8(static_cast<std::uint8_t>(value >> shift))) return false;
        return true;
    }
    constexpr bool Bytes(const std::uint8_t* data, std::size_t count) noexcept {
        for (std::size_t i = 0U; i < count; ++i) if (!U8(data[i])) return false;
        return true;
    }
    constexpr std::size_t Size() const noexcept { return size_; }
};

class Reader final {
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0U};
    std::size_t position_{0U};
public:
    constexpr Reader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
    constexpr bool U8(std::uint8_t& value) noexcept {
        if (position_ == size_) return false;
        value = data_[position_++];
        return true;
    }
    constexpr bool U16(std::uint16_t& value) noexcept {
        std::uint8_t a{}, b{};
        if (!U8(a) || !U8(b)) return false;
        value = static_cast<std::uint16_t>(a) | (static_cast<std::uint16_t>(b) << 8U);
        return true;
    }
    constexpr bool U32(std::uint32_t& value) noexcept {
        value = 0U;
        for (unsigned shift = 0U; shift < 32U; shift += 8U) { std::uint8_t byte{}; if (!U8(byte)) return false; value |= static_cast<std::uint32_t>(byte) << shift; }
        return true;
    }
    constexpr bool U64(std::uint64_t& value) noexcept {
        value = 0U;
        for (unsigned shift = 0U; shift < 64U; shift += 8U) { std::uint8_t byte{}; if (!U8(byte)) return false; value |= static_cast<std::uint64_t>(byte) << shift; }
        return true;
    }
    constexpr bool Bytes(std::uint8_t* output, std::size_t count) noexcept {
        for (std::size_t i = 0U; i < count; ++i) if (!U8(output[i])) return false;
        return true;
    }
    constexpr bool Complete() const noexcept { return position_ == size_; }
};

constexpr std::uint64_t Mix64(std::uint64_t value) noexcept {
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return value;
}

constexpr void AddBloom(std::array<std::uint8_t, CompatibilityClaimBloomBytes>& bloom, std::uint64_t value) noexcept {
    const std::uint64_t first = Mix64(value);
    const std::uint64_t second = Mix64(value ^ 0x9e3779b97f4a7c15ULL);
    const std::size_t bitA = static_cast<std::size_t>(first % (CompatibilityClaimBloomBytes * 8U));
    const std::size_t bitB = static_cast<std::size_t>(second % (CompatibilityClaimBloomBytes * 8U));
    bloom[bitA / 8U] = static_cast<std::uint8_t>(bloom[bitA / 8U] | (1U << (bitA % 8U)));
    bloom[bitB / 8U] = static_cast<std::uint8_t>(bloom[bitB / 8U] | (1U << (bitB % 8U)));
}

constexpr bool RequiredBloomMayBeContained(
    const std::array<std::uint8_t, CompatibilityClaimBloomBytes>& device,
    const std::array<std::uint8_t, CompatibilityClaimBloomBytes>& required) noexcept {
    for (std::size_t i = 0U; i < CompatibilityClaimBloomBytes; ++i) {
        if ((required[i] & static_cast<std::uint8_t>(~device[i])) != 0U) return false;
    }
    return true;
}

} // namespace CompatibilityClaimDetail

template<typename TCapacityProfile>
CompatibilityClaimStatus BuildDeviceCompatibilityClaimToken(
    const UpdateTargetProfile<TCapacityProfile>& profile,
    CompatibilityClaimToken<TCapacityProfile>& token) noexcept {
    if (!profile.IsFrozen()) return CompatibilityClaimStatus::NotFrozen;
    if (!profile.IsStructurallyComplete() || !profile.Fingerprint()) return CompatibilityClaimStatus::Invalid;

    token = {};
    token.TokenSchemaVersion = CompatibilityClaimTokenSchemaV1;
    CompatibilityClaimDetail::Writer writer{token.TokenBytes.data(), token.TokenBytes.size()};
    std::array<std::uint8_t, CompatibilityClaimBloomBytes> bloom{};
    for (std::size_t i = 0U; i < profile.SupportedComponentTypeCount(); ++i) {
        CompatibilityClaimDetail::AddBloom(bloom, profile.SupportedComponentType(i).Value());
    }

    const auto& fingerprint = profile.Fingerprint().Bytes();
    const bool ok =
        writer.U8(static_cast<std::uint8_t>(CompatibilityClaimTokenRole::DeviceProfile)) &&
        writer.U16(UpdateTargetProfileSchemaV1.Value()) &&
        writer.U64(profile.ProductType().Value()) &&
        writer.U64(profile.HardwareFamily().Value()) &&
        writer.U32(profile.HardwareRevision().Value()) &&
        writer.U64(profile.Architecture().Value()) &&
        writer.U64(profile.SoftwareVariant().Value()) &&
        writer.U64(profile.StorageLayout().Value()) &&
        writer.U32(profile.StorageLayoutGeneration().Value()) &&
        writer.U64(profile.PersistenceSchema().Value()) &&
        writer.U32(profile.PersistenceSchemaGeneration().Value()) &&
        writer.U16(profile.ProtocolVersion().Value()) &&
        writer.U64(profile.FeatureFlags()) &&
        writer.Bytes(bloom.data(), bloom.size()) &&
        writer.Bytes(fingerprint.data(), fingerprint.size());

    if (!ok || writer.Size() > 255U) return CompatibilityClaimStatus::CapacityUnavailable;
    token.TokenLength = static_cast<std::uint8_t>(writer.Size());
    return CompatibilityClaimStatus::Success;
}

template<typename TCapacityProfile>
CompatibilityClaimStatus BuildCandidateCompatibilityClaimToken(
    const CandidateCompatibilityClaim<TCapacityProfile>& claim,
    CompatibilityClaimToken<TCapacityProfile>& token) noexcept {
    if (!claim.IsValid()) return CompatibilityClaimStatus::Invalid;

    token = {};
    token.TokenSchemaVersion = CompatibilityClaimTokenSchemaV1;
    CompatibilityClaimDetail::Writer writer{token.TokenBytes.data(), token.TokenBytes.size()};

    std::uint16_t flags = 0U;
    if (claim.HardwareFamily().HasValue) flags |= CompatibilityClaimDetail::CandidateHasHardwareFamily;
    if (claim.Architecture().HasValue) flags |= CompatibilityClaimDetail::CandidateHasArchitecture;
    if (claim.SoftwareVariant().HasValue) flags |= CompatibilityClaimDetail::CandidateHasSoftwareVariant;
    if (claim.CurrentStorageLayout().HasValue) flags |= CompatibilityClaimDetail::CandidateHasStorageLayout;
    if (claim.CurrentStorageLayoutGeneration().HasValue) flags |= CompatibilityClaimDetail::CandidateHasStorageLayoutGeneration;
    if (claim.CurrentPersistenceSchema().HasValue) flags |= CompatibilityClaimDetail::CandidateHasPersistenceSchema;
    if (claim.CurrentPersistenceSchemaGeneration().HasValue) flags |= CompatibilityClaimDetail::CandidateHasPersistenceSchemaGeneration;

    std::array<std::uint8_t, CompatibilityClaimBloomBytes> bloom{};
    for (std::size_t i = 0U; i < claim.RequiredComponentTypeCount(); ++i) {
        CompatibilityClaimDetail::AddBloom(bloom, claim.RequiredComponentType(i).Value());
    }

    const bool ok =
        writer.U8(static_cast<std::uint8_t>(CompatibilityClaimTokenRole::CandidateClause)) &&
        writer.U16(flags) &&
        writer.U16(claim.MinimumProfileSchema().Value()) &&
        writer.U64(claim.ProductType().Value()) &&
        writer.U64(claim.HardwareFamily().Value.Value()) &&
        writer.U8(static_cast<std::uint8_t>(claim.HardwareRevision().Mode)) &&
        writer.U32(claim.HardwareRevision().Minimum.Value()) &&
        writer.U32(claim.HardwareRevision().Maximum.Value()) &&
        writer.U64(claim.Architecture().Value.Value()) &&
        writer.U64(claim.SoftwareVariant().Value.Value()) &&
        writer.U64(claim.CurrentStorageLayout().Value.Value()) &&
        writer.U32(claim.CurrentStorageLayoutGeneration().Value.Value()) &&
        writer.U64(claim.CurrentPersistenceSchema().Value.Value()) &&
        writer.U32(claim.CurrentPersistenceSchemaGeneration().Value.Value()) &&
        writer.U16(claim.MinimumOTAProtocol().Value()) &&
        writer.U64(claim.RequiredFeatures()) &&
        writer.Bytes(bloom.data(), bloom.size());

    if (!ok || writer.Size() > 255U) return CompatibilityClaimStatus::CapacityUnavailable;
    token.TokenLength = static_cast<std::uint8_t>(writer.Size());
    return CompatibilityClaimStatus::Success;
}

template<typename TCapacityProfile>
CompatibilityResult EvaluateCompatibilityClaimTokens(
    const CompatibilityClaimToken<TCapacityProfile>& deviceToken,
    const CompatibilityClaimToken<TCapacityProfile>& candidateToken) noexcept {
    if (deviceToken.TokenSchemaVersion != CompatibilityClaimTokenSchemaV1 ||
        candidateToken.TokenSchemaVersion != CompatibilityClaimTokenSchemaV1) {
        return CompatibilityResult::Unknown;
    }
    if (deviceToken.TokenLength == 0U || candidateToken.TokenLength == 0U ||
        deviceToken.TokenLength > deviceToken.TokenBytes.size() ||
        candidateToken.TokenLength > candidateToken.TokenBytes.size()) {
        return CompatibilityResult::Unknown;
    }

    CompatibilityClaimDetail::Reader device{deviceToken.TokenBytes.data(), deviceToken.TokenLength};
    CompatibilityClaimDetail::Reader candidate{candidateToken.TokenBytes.data(), candidateToken.TokenLength};

    std::uint8_t deviceRole{}, candidateRole{};
    std::uint16_t deviceProfileSchema{}, candidateFlags{}, minimumProfileSchema{};
    std::uint64_t deviceProduct{}, candidateProduct{}, deviceFamily{}, candidateFamily{};
    std::uint32_t deviceRevision{}, candidateRevisionMin{}, candidateRevisionMax{};
    std::uint8_t revisionMode{};
    std::uint64_t deviceArchitecture{}, candidateArchitecture{}, deviceVariant{}, candidateVariant{};
    std::uint64_t deviceLayout{}, candidateLayout{}, devicePersistence{}, candidatePersistence{};
    std::uint32_t deviceLayoutGeneration{}, candidateLayoutGeneration{};
    std::uint32_t devicePersistenceGeneration{}, candidatePersistenceGeneration{};
    std::uint16_t deviceProtocol{}, candidateMinimumProtocol{};
    std::uint64_t deviceFeatures{}, candidateRequiredFeatures{};
    std::array<std::uint8_t, CompatibilityClaimBloomBytes> deviceBloom{}, candidateBloom{};
    std::array<std::uint8_t, 32> ignoredFingerprint{};

    const bool deviceOk =
        device.U8(deviceRole) && device.U16(deviceProfileSchema) &&
        device.U64(deviceProduct) && device.U64(deviceFamily) && device.U32(deviceRevision) &&
        device.U64(deviceArchitecture) && device.U64(deviceVariant) &&
        device.U64(deviceLayout) && device.U32(deviceLayoutGeneration) &&
        device.U64(devicePersistence) && device.U32(devicePersistenceGeneration) &&
        device.U16(deviceProtocol) && device.U64(deviceFeatures) &&
        device.Bytes(deviceBloom.data(), deviceBloom.size()) &&
        device.Bytes(ignoredFingerprint.data(), ignoredFingerprint.size()) && device.Complete();

    const bool candidateOk =
        candidate.U8(candidateRole) && candidate.U16(candidateFlags) && candidate.U16(minimumProfileSchema) &&
        candidate.U64(candidateProduct) && candidate.U64(candidateFamily) && candidate.U8(revisionMode) &&
        candidate.U32(candidateRevisionMin) && candidate.U32(candidateRevisionMax) &&
        candidate.U64(candidateArchitecture) && candidate.U64(candidateVariant) &&
        candidate.U64(candidateLayout) && candidate.U32(candidateLayoutGeneration) &&
        candidate.U64(candidatePersistence) && candidate.U32(candidatePersistenceGeneration) &&
        candidate.U16(candidateMinimumProtocol) && candidate.U64(candidateRequiredFeatures) &&
        candidate.Bytes(candidateBloom.data(), candidateBloom.size()) && candidate.Complete();

    if (!deviceOk || !candidateOk ||
        deviceRole != static_cast<std::uint8_t>(CompatibilityClaimTokenRole::DeviceProfile) ||
        candidateRole != static_cast<std::uint8_t>(CompatibilityClaimTokenRole::CandidateClause)) {
        return CompatibilityResult::Unknown;
    }

    if (deviceProfileSchema < minimumProfileSchema || deviceProduct != candidateProduct) {
        return CompatibilityResult::DefinitelyIncompatible;
    }
    if ((candidateFlags & CompatibilityClaimDetail::CandidateHasHardwareFamily) != 0U && deviceFamily != candidateFamily) {
        return CompatibilityResult::DefinitelyIncompatible;
    }
    switch (static_cast<RevisionConstraintMode>(revisionMode)) {
        case RevisionConstraintMode::Any:
            break;
        case RevisionConstraintMode::Exact:
            if (deviceRevision != candidateRevisionMin || candidateRevisionMin != candidateRevisionMax)
                return CompatibilityResult::DefinitelyIncompatible;
            break;
        case RevisionConstraintMode::InclusiveRange:
            if (candidateRevisionMin == 0U || candidateRevisionMax < candidateRevisionMin ||
                deviceRevision < candidateRevisionMin || deviceRevision > candidateRevisionMax)
                return CompatibilityResult::DefinitelyIncompatible;
            break;
        default:
            return CompatibilityResult::Unknown;
    }
    if ((candidateFlags & CompatibilityClaimDetail::CandidateHasArchitecture) != 0U && deviceArchitecture != candidateArchitecture) {
        return CompatibilityResult::DefinitelyIncompatible;
    }
    if ((candidateFlags & CompatibilityClaimDetail::CandidateHasSoftwareVariant) != 0U && deviceVariant != candidateVariant) {
        return CompatibilityResult::DefinitelyIncompatible;
    }
    if ((candidateFlags & CompatibilityClaimDetail::CandidateHasStorageLayout) != 0U && deviceLayout != candidateLayout) {
        return CompatibilityResult::DefinitelyIncompatible;
    }
    if ((candidateFlags & CompatibilityClaimDetail::CandidateHasStorageLayoutGeneration) != 0U && deviceLayoutGeneration != candidateLayoutGeneration) {
        return CompatibilityResult::DefinitelyIncompatible;
    }
    if ((candidateFlags & CompatibilityClaimDetail::CandidateHasPersistenceSchema) != 0U && devicePersistence != candidatePersistence) {
        return CompatibilityResult::DefinitelyIncompatible;
    }
    if ((candidateFlags & CompatibilityClaimDetail::CandidateHasPersistenceSchemaGeneration) != 0U && devicePersistenceGeneration != candidatePersistenceGeneration) {
        return CompatibilityResult::DefinitelyIncompatible;
    }
    if (deviceProtocol < candidateMinimumProtocol ||
        (candidateRequiredFeatures & ~deviceFeatures) != 0U ||
        !CompatibilityClaimDetail::RequiredBloomMayBeContained(deviceBloom, candidateBloom)) {
        return CompatibilityResult::DefinitelyIncompatible;
    }

    return CompatibilityResult::PossiblyCompatible;
}

static_assert(CompatibilityClaimBloomBytes == 16U, "Compatibility Claim Token V1 bloom width is wire-stable");

} // namespace ESPressio::OTA
