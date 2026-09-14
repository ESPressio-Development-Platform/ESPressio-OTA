#pragma once

#include <cstddef>
#include <cstdint>

#include "ESPressio_OTACapacityProfile.hpp"
#include "ESPressio_OTATypes.hpp"
#include "ESPressio_BoundedContainers.hpp"
#include "ESPressio_BoundedDeserializer.hpp"
#include "ESPressio_SerializableBase.hpp"
#include "ESPressio_SerializationMacros.hpp"
#include "ESPressio_SerializationTraits.hpp"

namespace ESPressio::OTA {

inline constexpr std::uint16_t ArtifactCheckpointSchemaV1 = 1U;

template<typename TCapacityProfile>
struct ArtifactCheckpoint final
    : Serializable::Serializable<ArtifactCheckpoint<TCapacityProfile>> {
    static_assert(TCapacityProfile::IsValid, "ArtifactCheckpoint requires a valid OTA capacity profile");

    std::uint16_t SchemaVersion{ArtifactCheckpointSchemaV1};
    std::uint64_t Transaction{0U};
    std::array<std::uint8_t, 16> Artifact{};
    std::uint64_t ExpectedLength{0U};
    std::uint64_t AcceptedPrefixLength{0U};
    std::uint32_t Revision{0U};
    std::uint16_t PrefixDigestAlgorithm{0U};
    Serializable::BoundedBytes<TCapacityProfile::MaximumDigestBytes> PrefixDigest{};

    ESPRESSIO_SERIALIZABLE_TYPE(ArtifactCheckpoint<TCapacityProfile>)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("v", SchemaVersion),
        ESPRESSIO_PROPERTY_REQUIRED("tx", Transaction),
        ESPRESSIO_PROPERTY_REQUIRED("id", Artifact),
        ESPRESSIO_PROPERTY_REQUIRED("len", ExpectedLength),
        ESPRESSIO_PROPERTY_REQUIRED("prefix", AcceptedPrefixLength),
        ESPRESSIO_PROPERTY_REQUIRED("rev", Revision),
        ESPRESSIO_PROPERTY_REQUIRED("alg", PrefixDigestAlgorithm),
        ESPRESSIO_PROPERTY_REQUIRED("dig", PrefixDigest))
};

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

} // namespace ESPressio::OTA
