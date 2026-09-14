#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAManifest.hpp"

namespace ESPressio::OTA {

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
 * Deserializes a complete signed Manifest envelope only when the supplied wire
 * object is within the configured global byte ceiling. Bounded DirectBinary
 * publishes the decoded object only after complete successful traversal.
 */
template<typename TCapacityProfile>
ManifestStatus DeserializeSignedManifest(
    const std::uint8_t* data,
    std::size_t size,
    SignedManifest<TCapacityProfile>& envelope) noexcept {
    if (data == nullptr || size == 0U) return ManifestStatus::Invalid;
    if (size > TCapacityProfile::MaximumManifestBytes) return ManifestStatus::CapacityUnavailable;

    SignedManifest<TCapacityProfile> candidate{};
    const auto decoded = Serializable::DeserializeBoundedDirectBinary(data, size, candidate);
    if (!decoded) return ManifestStatus::SerializationFailed;

    const auto valid = ValidateSignedManifestEnvelope(candidate);
    if (valid != ManifestStatus::Success) return valid;
    envelope = candidate;
    return ManifestStatus::Success;
}

} // namespace ESPressio::OTA
