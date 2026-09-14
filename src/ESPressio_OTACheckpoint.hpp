#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTACapacityProfile.hpp"
#include "ESPressio_OTATypes.hpp"
#include "ESPressio_BoundedContainers.hpp"

namespace ESPressio::OTA {

inline constexpr std::uint16_t ArtifactCheckpointSchemaV1 = 1U;
inline constexpr std::array<std::uint8_t, 4> ArtifactCheckpointMagic{{'O', 'T', 'C', 'P'}};

enum class ArtifactCheckpointCodecStatus : std::uint8_t {
    Success,
    Invalid,
    CapacityUnavailable,
    Malformed
};

template<typename TCapacityProfile>
struct ArtifactCheckpoint final {
    static_assert(TCapacityProfile::IsValid, "ArtifactCheckpoint requires a valid OTA capacity profile");

    std::uint16_t SchemaVersion{ArtifactCheckpointSchemaV1};
    std::uint64_t Transaction{0U};
    std::array<std::uint8_t, 16> Artifact{};
    std::uint64_t ExpectedLength{0U};
    std::uint64_t AcceptedPrefixLength{0U};
    std::uint32_t Revision{0U};
    std::uint16_t PrefixDigestAlgorithm{0U};
    Serializable::BoundedBytes<TCapacityProfile::MaximumDigestBytes> PrefixDigest{};
};

template<typename TCapacityProfile>
inline constexpr std::size_t MaximumArtifactCheckpointEncodedBytes =
    4U + 2U + 8U + 16U + 8U + 8U + 4U + 2U + 1U + TCapacityProfile::MaximumDigestBytes;

template<typename TCapacityProfile>
constexpr bool ArtifactCheckpointValid(const ArtifactCheckpoint<TCapacityProfile>& checkpoint) noexcept {
    if (checkpoint.SchemaVersion != ArtifactCheckpointSchemaV1 ||
        checkpoint.Transaction == 0U ||
        checkpoint.ExpectedLength == 0U ||
        checkpoint.AcceptedPrefixLength > checkpoint.ExpectedLength ||
        checkpoint.Revision == 0U) return false;

    bool artifactNonZero = false;
    for (const auto byte : checkpoint.Artifact) if (byte != 0U) { artifactNonZero = true; break; }
    if (!artifactNonZero) return false;

    if (checkpoint.AcceptedPrefixLength == 0U) {
        return checkpoint.PrefixDigestAlgorithm == 0U && checkpoint.PrefixDigest.empty();
    }
    return checkpoint.PrefixDigestAlgorithm != 0U && !checkpoint.PrefixDigest.empty();
}

namespace CheckpointDetail {

template<typename T>
constexpr void WriteLE(std::uint8_t*& output, T value) noexcept {
    for (std::size_t i = 0U; i < sizeof(T); ++i) {
        *output++ = static_cast<std::uint8_t>(value & static_cast<T>(0xFFU));
        value = static_cast<T>(value >> 8U);
    }
}

template<typename T>
constexpr bool ReadLE(const std::uint8_t*& input, const std::uint8_t* end, T& value) noexcept {
    if (static_cast<std::size_t>(end - input) < sizeof(T)) return false;
    value = 0;
    for (std::size_t i = 0U; i < sizeof(T); ++i) {
        value = static_cast<T>(value | static_cast<T>(input[i]) << (i * 8U));
    }
    input += sizeof(T);
    return true;
}

} // namespace CheckpointDetail

/** Compact persistence codec; this private durable record is not a general OTA wire object. */
template<typename TCapacityProfile>
ArtifactCheckpointCodecStatus SerializeArtifactCheckpoint(
    const ArtifactCheckpoint<TCapacityProfile>& checkpoint,
    std::uint8_t* output,
    std::size_t capacity,
    std::size_t& written) noexcept {
    written = 0U;
    if (!ArtifactCheckpointValid(checkpoint)) return ArtifactCheckpointCodecStatus::Invalid;
    const std::size_t required = MaximumArtifactCheckpointEncodedBytes<TCapacityProfile>
        - TCapacityProfile::MaximumDigestBytes + checkpoint.PrefixDigest.size();
    if (output == nullptr || capacity < required ||
        required > TCapacityProfile::MaximumArtifactCheckpointRecordBytes) {
        return ArtifactCheckpointCodecStatus::CapacityUnavailable;
    }

    auto* cursor = output;
    for (const auto byte : ArtifactCheckpointMagic) *cursor++ = byte;
    CheckpointDetail::WriteLE(cursor, checkpoint.SchemaVersion);
    CheckpointDetail::WriteLE(cursor, checkpoint.Transaction);
    for (const auto byte : checkpoint.Artifact) *cursor++ = byte;
    CheckpointDetail::WriteLE(cursor, checkpoint.ExpectedLength);
    CheckpointDetail::WriteLE(cursor, checkpoint.AcceptedPrefixLength);
    CheckpointDetail::WriteLE(cursor, checkpoint.Revision);
    CheckpointDetail::WriteLE(cursor, checkpoint.PrefixDigestAlgorithm);
    *cursor++ = static_cast<std::uint8_t>(checkpoint.PrefixDigest.size());
    for (const auto byte : checkpoint.PrefixDigest) *cursor++ = byte;
    written = static_cast<std::size_t>(cursor - output);
    return ArtifactCheckpointCodecStatus::Success;
}

template<typename TCapacityProfile>
ArtifactCheckpointCodecStatus DeserializeArtifactCheckpoint(
    const std::uint8_t* data,
    std::size_t size,
    ArtifactCheckpoint<TCapacityProfile>& output) noexcept {
    constexpr std::size_t fixedBytes = MaximumArtifactCheckpointEncodedBytes<TCapacityProfile>
        - TCapacityProfile::MaximumDigestBytes;
    if (data == nullptr || size < fixedBytes ||
        size > TCapacityProfile::MaximumArtifactCheckpointRecordBytes) {
        return ArtifactCheckpointCodecStatus::Malformed;
    }

    const auto* cursor = data;
    const auto* end = data + size;
    for (const auto expected : ArtifactCheckpointMagic) {
        if (cursor == end || *cursor++ != expected) return ArtifactCheckpointCodecStatus::Malformed;
    }

    ArtifactCheckpoint<TCapacityProfile> candidate;
    if (!CheckpointDetail::ReadLE(cursor, end, candidate.SchemaVersion) ||
        !CheckpointDetail::ReadLE(cursor, end, candidate.Transaction)) {
        return ArtifactCheckpointCodecStatus::Malformed;
    }
    if (static_cast<std::size_t>(end - cursor) < candidate.Artifact.size()) {
        return ArtifactCheckpointCodecStatus::Malformed;
    }
    for (auto& byte : candidate.Artifact) byte = *cursor++;
    if (!CheckpointDetail::ReadLE(cursor, end, candidate.ExpectedLength) ||
        !CheckpointDetail::ReadLE(cursor, end, candidate.AcceptedPrefixLength) ||
        !CheckpointDetail::ReadLE(cursor, end, candidate.Revision) ||
        !CheckpointDetail::ReadLE(cursor, end, candidate.PrefixDigestAlgorithm) ||
        cursor == end) {
        return ArtifactCheckpointCodecStatus::Malformed;
    }

    const std::size_t digestLength = *cursor++;
    if (digestLength > TCapacityProfile::MaximumDigestBytes ||
        static_cast<std::size_t>(end - cursor) != digestLength) {
        return ArtifactCheckpointCodecStatus::Malformed;
    }
    for (std::size_t i = 0U; i < digestLength; ++i) {
        if (!candidate.PrefixDigest.push_back(*cursor++)) return ArtifactCheckpointCodecStatus::Malformed;
    }
    if (!ArtifactCheckpointValid(candidate)) return ArtifactCheckpointCodecStatus::Invalid;
    output = candidate;
    return ArtifactCheckpointCodecStatus::Success;
}

static_assert(MaximumArtifactCheckpointEncodedBytes<ConstrainedV1CapacityProfile>
              <= ConstrainedV1CapacityProfile::MaximumArtifactCheckpointRecordBytes,
              "Constrained V1 checkpoint codec must fit its locked durable-record budget");

} // namespace ESPressio::OTA
