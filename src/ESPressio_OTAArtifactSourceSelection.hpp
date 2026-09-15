#pragma once

#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAProviders.hpp"

namespace ESPressio::OTA {

/** Bounded facts supplied to explicit OTA Artifact source-selection policy. */
struct ArtifactSourceSelectionContext final {
    UpdateTransactionId Transaction{};
    ArtifactIdentifier Artifact{};
    std::uint64_t ExpectedLength{0U};
    std::size_t Attempt{0U};
    std::uint64_t AcceptedCheckpointPrefix{0U};
    bool HasPreviousFailure{false};
    Result PreviousFailure{OutcomeClass::Success, {}};

    constexpr bool IsValid() const noexcept {
        return bool(Transaction) && bool(Artifact) && ExpectedLength != 0U && Attempt != 0U &&
               AcceptedCheckpointPrefix <= ExpectedLength;
    }
};

/** One explicitly selected Source and the structural capability needed by V1 resume. */
struct ArtifactSourceSelection final {
    IArtifactSource* Source{nullptr};
    bool OffsetRead{false};

    constexpr explicit operator bool() const noexcept { return Source != nullptr; }
};

/**
 * Application/Coordinator policy surface for selecting an Artifact Source.
 *
 * Selection order is entirely implementation/policy-owned by this object.
 * ESPressio-OTA never interprets composition declaration order as priority.
 * Returning the same Source permits a bounded retry; returning another Source
 * is failover. The Coordinator owns the finite attempt budget around Select().
 */
class IArtifactSourceSelector {
public:
    virtual ~IArtifactSourceSelector() = default;
    virtual Result Select(
        const ArtifactSourceSelectionContext& context,
        ArtifactSourceSelection& selection) noexcept = 0;
};

} // namespace ESPressio::OTA
