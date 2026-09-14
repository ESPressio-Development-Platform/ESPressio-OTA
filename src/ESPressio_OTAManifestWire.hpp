#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAManifest.hpp"
#include "ESPressio_BoundedScratchDeserializer.hpp"

namespace ESPressio::OTA {

/** Caller-owned byte workspace for canonical Manifest serialization/signature verification. */
template<typename TCapacityProfile>
struct ManifestWireWorkspace final {
    static_assert(TCapacityProfile::IsValid, "ManifestWireWorkspace requires a valid OTA capacity profile");
    std::array<std::uint8_t, TCapacityProfile::MaximumManifestBytes> Bytes{};
};

/**
 * Serializes a validated signed Manifest envelope without ever exceeding the
 * configured global Manifest byte ceiling, even when the caller supplies a
 * larger output buffer.
 */
template<typename TCapacityProfile>
ManifestStatus SerializeSignedManifest(
    const SignedManifest<TCapacityProfile>& envelope,
    std::uint8_t* output,
    std::size_t capacity,
    std::size_t& written) noexcept {
    written = 0U;
    const auto valid = ValidateSignedManifestEnvelope(envelope);
    if (valid != ManifestStatus::Success) return valid;
    if (output == nullptr || capacity == 0U) return ManifestStatus::Invalid;

    const std::size_t effectiveCapacity =
        capacity < TCapacityProfile::MaximumManifestBytes
            ? capacity
            : TCapacityProfile::MaximumManifestBytes;
    const auto encoded = Serializable::SerializeDirectBinary(envelope, output, effectiveCapacity);
    if (!encoded) {
        return encoded.Error == Serializable::SerializationErrorCode::ResourceLimitExceeded
            ? ManifestStatus::CapacityUnavailable
            : ManifestStatus::SerializationFailed;
    }
    written = encoded.Bytes;
    return ManifestStatus::Success;
}

/**
 * Decodes a complete signed Manifest directly into caller-owned scratch storage.
 *
 * On failure `scratch` may be partially modified and MUST be discarded/reset by
 * the caller. No additional SignedManifest temporary is created by OTA or the
 * scratch DirectBinary path.
 */
template<typename TCapacityProfile>
ManifestStatus DeserializeSignedManifestIntoScratch(
    const std::uint8_t* data,
    std::size_t size,
    SignedManifest<TCapacityProfile>& scratch) noexcept {
    if (data == nullptr || size == 0U) return ManifestStatus::Invalid;
    if (size > TCapacityProfile::MaximumManifestBytes) return ManifestStatus::CapacityUnavailable;

    const auto decoded = Serializable::DeserializeBoundedDirectBinaryIntoScratch(data, size, scratch);
    if (!decoded) return decoded.Error == Serializable::SerializationErrorCode::ResourceLimitExceeded
        ? ManifestStatus::CapacityUnavailable
        : ManifestStatus::SerializationFailed;

    return ValidateSignedManifestEnvelope(scratch);
}

/**
 * Verifies a canonical signed Manifest using an explicitly caller-owned byte
 * workspace. This is the constrained-target verification path; it avoids the
 * hidden MaximumManifestBytes automatic buffer used by convenience-style code.
 */
template<typename TCapacityProfile>
ManifestStatus VerifySignedManifestWithWorkspace(
    const SignedManifest<TCapacityProfile>& envelope,
    Security::ISignatureVerifier& verifier,
    const Security::ITrustAnchorProvider& anchors,
    const Security::ITrustPolicy& policy,
    ManifestWireWorkspace<TCapacityProfile>& workspace) noexcept {
    const auto valid = ValidateSignedManifestEnvelope(envelope);
    if (valid != ManifestStatus::Success) return valid;

    std::size_t canonicalBytes = 0U;
    const auto encoded = SerializeCanonicalManifest(
        envelope.Content, workspace.Bytes.data(), workspace.Bytes.size(), canonicalBytes);
    if (encoded != ManifestStatus::Success) return encoded;

    for (std::size_t i = 0U; i < envelope.Signatures.size(); ++i) {
        const auto& descriptor = envelope.Content.SignatureDescriptors[i];
        const Security::SignatureAlgorithmIdentifier algorithm{descriptor.Algorithm};
        const Security::TrustAnchorIdentifier anchorId{descriptor.TrustAnchor};
        const Security::TrustPolicyIdentifier policyId{descriptor.TrustPolicy};
        if (!verifier.Supports(algorithm)) continue;

        Security::TrustAnchorView anchor{};
        if (!anchors.Resolve(anchorId, anchor)) continue;
        if (!policy.Authorize(policyId, Security::TrustPurpose::SoftwareUpdateManifest, anchorId)) continue;
        const auto& signature = envelope.Signatures[i].Bytes;
        if (verifier.Verify(
                algorithm,
                Security::ByteView{workspace.Bytes.data(), canonicalBytes},
                Security::ByteView{signature.data(), signature.size()},
                anchor)) {
            return ManifestStatus::Success;
        }
    }
    return ManifestStatus::NoTrustedSignature;
}

} // namespace ESPressio::OTA
