#pragma once

#include <cstdint>

#include "ESPressio_SerializableBase.hpp"
#include "ESPressio_SerializationMacros.hpp"

namespace ESPressio::OTA {

/**
 * Signed, bounded declaration of the semantic versions one candidate runtime can
 * read and the version it currently writes. Versions are linearly ordered V1
 * scalars; zero is never a valid serialized version.
 */
struct OTAVersionSupport final : Serializable::SerializableBase<OTAVersionSupport> {
    std::uint16_t MinimumReadableVersion{1U};
    std::uint16_t MaximumReadableVersion{1U};
    std::uint16_t CurrentWriteVersion{1U};

    constexpr OTAVersionSupport() noexcept = default;
    constexpr OTAVersionSupport(std::uint16_t minimumReadable,
                                std::uint16_t maximumReadable,
                                std::uint16_t currentWrite) noexcept
        : MinimumReadableVersion(minimumReadable),
          MaximumReadableVersion(maximumReadable),
          CurrentWriteVersion(currentWrite) {}

    constexpr bool IsValid() const noexcept {
        return MinimumReadableVersion != 0U &&
               MaximumReadableVersion != 0U &&
               CurrentWriteVersion != 0U &&
               MinimumReadableVersion <= MaximumReadableVersion &&
               CurrentWriteVersion >= MinimumReadableVersion &&
               CurrentWriteVersion <= MaximumReadableVersion;
    }

    constexpr bool CanRead(std::uint16_t version) const noexcept {
        return IsValid() && version != 0U &&
               version >= MinimumReadableVersion && version <= MaximumReadableVersion;
    }

    ESPRESSIO_SERIALIZABLE_TYPE(OTAVersionSupport)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("minimumReadableVersion", MinimumReadableVersion),
        ESPRESSIO_PROPERTY_REQUIRED("maximumReadableVersion", MaximumReadableVersion),
        ESPRESSIO_PROPERTY_REQUIRED("currentWriteVersion", CurrentWriteVersion))
};

inline constexpr OTAVersionSupport OTAV1OnlyVersionSupport{1U, 1U, 1U};

} // namespace ESPressio::OTA
