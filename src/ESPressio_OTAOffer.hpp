#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTACompatibilityClaim.hpp"
#include "ESPressio_OTAManifestWire.hpp"

#include "ESPressio_BoundedContainers.hpp"
#include "ESPressio_SerializableBase.hpp"
#include "ESPressio_SerializationMacros.hpp"
#include "ESPressio_SerializationTraits.hpp"

namespace ESPressio::OTA {

enum class UpdateOfferKind : std::uint8_t {
    ManifestReference = 1U,
    EmbeddedSignedManifest = 2U
};

enum class UpdateOfferStatus : std::uint8_t {
    Success,
    Invalid,
    CapacityUnavailable,
    ManifestMismatch,
    AdvisoryMismatch,
    SerializationFailed
};

struct UpdateOfferAdvisorySummary final
    : Serializable::Serializable<UpdateOfferAdvisorySummary> {
    std::uint64_t Release{0U};
    std::uint32_t ReleaseChannel{0U};
    std::uint64_t SecurityGeneration{0U};
    std::uint16_t RequiredOTAProtocol{0U};
    std::uint64_t RequiredOTAFeatures{0U};

    constexpr bool IsValid() const noexcept {
        return Release != 0U && RequiredOTAProtocol != 0U;
    }

    ESPRESSIO_SERIALIZABLE_TYPE(UpdateOfferAdvisorySummary)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("release", Release),
        ESPRESSIO_PROPERTY_REQUIRED("releaseChannel", ReleaseChannel),
        ESPRESSIO_PROPERTY_REQUIRED("securityGeneration", SecurityGeneration),
        ESPRESSIO_PROPERTY_REQUIRED("requiredOTAProtocol", RequiredOTAProtocol),
        ESPRESSIO_PROPERTY_REQUIRED("requiredOTAFeatures", RequiredOTAFeatures))
};

template<typename TCapacityProfile>
struct UpdateOffer final
    : Serializable::Serializable<UpdateOffer<TCapacityProfile>> {
    static_assert(TCapacityProfile::IsValid, "UpdateOffer requires a valid OTA capacity profile");

    std::uint8_t Kind{static_cast<std::uint8_t>(UpdateOfferKind::ManifestReference)};
    std::array<std::uint8_t, 16> Manifest{};

    std::uint8_t CompatibilityClaimSchema{0U};
    Serializable::BoundedBytes<TCapacityProfile::MaximumCompatibilityClaimTokenBytes>
        CompatibilityClaim{};

    std::uint8_t HasAdvisorySummary{0U};
    UpdateOfferAdvisorySummary AdvisorySummary{};

    // Complete canonical DirectBinary SignedManifest envelope when Kind is EmbeddedSignedManifest.
    Serializable::BoundedBytes<TCapacityProfile::MaximumManifestBytes> EmbeddedManifest{};

    ESPRESSIO_SERIALIZABLE_TYPE(UpdateOffer<TCapacityProfile>)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("kind", Kind),
        ESPRESSIO_PROPERTY_REQUIRED("manifest", Manifest),
        ESPRESSIO_PROPERTY_REQUIRED("compatibilityClaimSchema", CompatibilityClaimSchema),
        ESPRESSIO_PROPERTY_REQUIRED("compatibilityClaim", CompatibilityClaim),
        ESPRESSIO_PROPERTY_REQUIRED("hasAdvisorySummary", HasAdvisorySummary),
        ESPRESSIO_PROPERTY_REQUIRED("advisorySummary", AdvisorySummary),
        ESPRESSIO_PROPERTY_REQUIRED("embeddedManifest", EmbeddedManifest))
};

namespace UpdateOfferDetail {

inline constexpr bool NonZeroIdentifier(const std::array<std::uint8_t, 16>& identifier) noexcept {
    for (const auto byte : identifier) if (byte != 0U) return true;
    return false;
}

inline constexpr bool EqualIdentifier(
    const std::array<std::uint8_t, 16>& left,
    const std::array<std::uint8_t, 16>& right) noexcept {
    for (std::size_t i = 0U; i < left.size(); ++i) if (left[i] != right[i]) return false;
    return true;
}

template<typename TCapacityProfile>
UpdateOfferStatus ValidateCompatibilityClaimInput(
    const CompatibilityClaimToken<TCapacityProfile>* token) noexcept {
    if (token == nullptr || token->IsEmpty()) return UpdateOfferStatus::Success;
    if (token->TokenSchemaVersion == 0U || token->TokenLength == 0U ||
        token->TokenLength > TCapacityProfile::MaximumCompatibilityClaimTokenBytes) {
        return UpdateOfferStatus::Invalid;
    }
    return UpdateOfferStatus::Success;
}

template<typename TCapacityProfile>
void ResetOffer(UpdateOffer<TCapacityProfile>& offer) noexcept {
    offer.Kind = static_cast<std::uint8_t>(UpdateOfferKind::ManifestReference);
    offer.Manifest.fill(0U);
    offer.CompatibilityClaimSchema = 0U;
    offer.CompatibilityClaim.clear();
    offer.HasAdvisorySummary = 0U;
    offer.AdvisorySummary.Release = 0U;
    offer.AdvisorySummary.ReleaseChannel = 0U;
    offer.AdvisorySummary.SecurityGeneration = 0U;
    offer.AdvisorySummary.RequiredOTAProtocol = 0U;
    offer.AdvisorySummary.RequiredOTAFeatures = 0U;
    offer.EmbeddedManifest.clear();
}

template<typename TCapacityProfile>
void SetCompatibilityClaimValidated(
    UpdateOffer<TCapacityProfile>& offer,
    const CompatibilityClaimToken<TCapacityProfile>* token) noexcept {
    offer.CompatibilityClaim.clear();
    offer.CompatibilityClaimSchema = 0U;
    if (token == nullptr || token->IsEmpty()) return;
    for (std::size_t i = 0U; i < token->TokenLength; ++i) {
        (void)offer.CompatibilityClaim.push_back(token->TokenBytes[i]);
    }
    offer.CompatibilityClaimSchema = token->TokenSchemaVersion;
}

template<typename TCapacityProfile>
void SetAdvisorySummary(
    UpdateOffer<TCapacityProfile>& offer,
    const UpdateOfferAdvisorySummary* summary) noexcept {
    if (summary == nullptr) {
        offer.HasAdvisorySummary = 0U;
        offer.AdvisorySummary.Release = 0U;
        offer.AdvisorySummary.ReleaseChannel = 0U;
        offer.AdvisorySummary.SecurityGeneration = 0U;
        offer.AdvisorySummary.RequiredOTAProtocol = 0U;
        offer.AdvisorySummary.RequiredOTAFeatures = 0U;
        return;
    }
    offer.HasAdvisorySummary = 1U;
    offer.AdvisorySummary = *summary;
}

} // namespace UpdateOfferDetail

template<typename TCapacityProfile>
UpdateOfferStatus BuildManifestReferenceOffer(
    ManifestIdentifier manifest,
    const CompatibilityClaimToken<TCapacityProfile>* claim,
    const UpdateOfferAdvisorySummary* summary,
    UpdateOffer<TCapacityProfile>& offer) noexcept {
    if (!manifest) return UpdateOfferStatus::Invalid;
    if (summary != nullptr && !summary->IsValid()) return UpdateOfferStatus::Invalid;
    const auto claimStatus = UpdateOfferDetail::ValidateCompatibilityClaimInput(claim);
    if (claimStatus != UpdateOfferStatus::Success) return claimStatus;

    // All fallible validation is complete. Populate caller-owned output directly;
    // no second ~8 KiB UpdateOffer temporary is created on the stack.
    UpdateOfferDetail::ResetOffer(offer);
    offer.Kind = static_cast<std::uint8_t>(UpdateOfferKind::ManifestReference);
    offer.Manifest = manifest.Bytes();
    UpdateOfferDetail::SetCompatibilityClaimValidated(offer, claim);
    UpdateOfferDetail::SetAdvisorySummary(offer, summary);
    return UpdateOfferStatus::Success;
}

template<typename TCapacityProfile>
UpdateOfferStatus BuildEmbeddedManifestOffer(
    const SignedManifest<TCapacityProfile>& envelope,
    const CompatibilityClaimToken<TCapacityProfile>* claim,
    const UpdateOfferAdvisorySummary* summary,
    ManifestWireWorkspace<TCapacityProfile>& workspace,
    UpdateOffer<TCapacityProfile>& offer) noexcept {
    if (summary != nullptr && !summary->IsValid()) return UpdateOfferStatus::Invalid;
    const auto valid = ValidateSignedManifestEnvelope(envelope);
    if (valid != ManifestStatus::Success) return UpdateOfferStatus::Invalid;
    const auto claimStatus = UpdateOfferDetail::ValidateCompatibilityClaimInput(claim);
    if (claimStatus != UpdateOfferStatus::Success) return claimStatus;

    std::size_t encodedBytes = 0U;
    const auto encodedStatus = SerializeSignedManifest(
        envelope, workspace.Bytes.data(), workspace.Bytes.size(), encodedBytes);
    if (encodedStatus == ManifestStatus::CapacityUnavailable) return UpdateOfferStatus::CapacityUnavailable;
    if (encodedStatus != ManifestStatus::Success) return UpdateOfferStatus::SerializationFailed;

    // Serialization/validation succeeded. Publish directly into caller-owned output.
    UpdateOfferDetail::ResetOffer(offer);
    offer.Kind = static_cast<std::uint8_t>(UpdateOfferKind::EmbeddedSignedManifest);
    offer.Manifest = envelope.Content.Identifier;
    UpdateOfferDetail::SetCompatibilityClaimValidated(offer, claim);
    UpdateOfferDetail::SetAdvisorySummary(offer, summary);
    for (std::size_t i = 0U; i < encodedBytes; ++i) {
        (void)offer.EmbeddedManifest.push_back(workspace.Bytes[i]);
    }
    return UpdateOfferStatus::Success;
}

template<typename TCapacityProfile>
UpdateOfferStatus ValidateUpdateOfferStructure(const UpdateOffer<TCapacityProfile>& offer) noexcept {
    if (!UpdateOfferDetail::NonZeroIdentifier(offer.Manifest)) return UpdateOfferStatus::Invalid;
    if (offer.CompatibilityClaim.empty()) {
        if (offer.CompatibilityClaimSchema != 0U) return UpdateOfferStatus::Invalid;
    } else if (offer.CompatibilityClaimSchema == 0U) {
        return UpdateOfferStatus::Invalid;
    }

    if (offer.HasAdvisorySummary > 1U) return UpdateOfferStatus::Invalid;
    if (offer.HasAdvisorySummary == 0U) {
        if (offer.AdvisorySummary.Release != 0U || offer.AdvisorySummary.ReleaseChannel != 0U ||
            offer.AdvisorySummary.SecurityGeneration != 0U || offer.AdvisorySummary.RequiredOTAProtocol != 0U ||
            offer.AdvisorySummary.RequiredOTAFeatures != 0U) return UpdateOfferStatus::Invalid;
    } else if (!offer.AdvisorySummary.IsValid()) {
        return UpdateOfferStatus::Invalid;
    }

    const auto kind = static_cast<UpdateOfferKind>(offer.Kind);
    if (kind == UpdateOfferKind::ManifestReference) {
        return offer.EmbeddedManifest.empty() ? UpdateOfferStatus::Success : UpdateOfferStatus::Invalid;
    }
    return kind == UpdateOfferKind::EmbeddedSignedManifest && !offer.EmbeddedManifest.empty()
        ? UpdateOfferStatus::Success
        : UpdateOfferStatus::Invalid;
}

template<typename TCapacityProfile>
UpdateOfferStatus DecodeEmbeddedManifestOfferIntoScratch(
    const UpdateOffer<TCapacityProfile>& offer,
    SignedManifest<TCapacityProfile>& scratch) noexcept {
    const auto structure = ValidateUpdateOfferStructure(offer);
    if (structure != UpdateOfferStatus::Success) return structure;
    if (static_cast<UpdateOfferKind>(offer.Kind) != UpdateOfferKind::EmbeddedSignedManifest) {
        return UpdateOfferStatus::Invalid;
    }
    const auto decoded = DeserializeSignedManifestIntoScratch(
        offer.EmbeddedManifest.data(), offer.EmbeddedManifest.size(), scratch);
    if (decoded == ManifestStatus::CapacityUnavailable) return UpdateOfferStatus::CapacityUnavailable;
    if (decoded != ManifestStatus::Success) return UpdateOfferStatus::SerializationFailed;
    return UpdateOfferDetail::EqualIdentifier(offer.Manifest, scratch.Content.Identifier)
        ? UpdateOfferStatus::Success
        : UpdateOfferStatus::ManifestMismatch;
}

template<typename TCapacityProfile>
UpdateOfferStatus ValidateUpdateOffer(
    const UpdateOffer<TCapacityProfile>& offer,
    SignedManifest<TCapacityProfile>& scratch) noexcept {
    const auto structure = ValidateUpdateOfferStructure(offer);
    if (structure != UpdateOfferStatus::Success) return structure;
    if (static_cast<UpdateOfferKind>(offer.Kind) == UpdateOfferKind::ManifestReference) {
        return UpdateOfferStatus::Success;
    }
    return DecodeEmbeddedManifestOfferIntoScratch(offer, scratch);
}

template<typename TCapacityProfile>
UpdateOfferStatus ValidateUpdateOfferAgainstVerifiedManifest(
    const UpdateOffer<TCapacityProfile>& offer,
    const Manifest<TCapacityProfile>& verifiedManifest,
    SignedManifest<TCapacityProfile>& scratch) noexcept {
    const auto basic = ValidateUpdateOffer(offer, scratch);
    if (basic != UpdateOfferStatus::Success) return basic;
    if (!UpdateOfferDetail::EqualIdentifier(offer.Manifest, verifiedManifest.Identifier)) {
        return UpdateOfferStatus::ManifestMismatch;
    }
    if (offer.HasAdvisorySummary != 0U) {
        const auto& advisory = offer.AdvisorySummary;
        if (advisory.Release != verifiedManifest.Release ||
            advisory.ReleaseChannel != verifiedManifest.ReleaseChannel ||
            advisory.SecurityGeneration != verifiedManifest.SecurityGeneration ||
            advisory.RequiredOTAProtocol != verifiedManifest.RequiredOTAProtocol ||
            advisory.RequiredOTAFeatures != verifiedManifest.RequiredOTAFeatures) {
            return UpdateOfferStatus::AdvisoryMismatch;
        }
    }
    return UpdateOfferStatus::Success;
}

} // namespace ESPressio::OTA
