#pragma once

#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAProviders.hpp"

namespace ESPressio::OTA {

/**
 * Current runtime facts for one unpublished contiguous Artifact prefix.
 *
 * A partial prefix is acquisition-private state. Present never means that the
 * Artifact exists in the committed ArtifactStore namespace, and it therefore
 * must not satisfy normal read-back/distribution semantics.
 */
struct PartialArtifactInfo final {
    bool Present{false};
    bool Replayable{false};
    std::uint64_t RetainedPrefixLength{0U};

    constexpr bool IsValidFor(std::uint64_t expectedLength) const noexcept {
        if (!Present) return !Replayable && RetainedPrefixLength == 0U;
        return RetainedPrefixLength <= expectedLength;
    }
};

/**
 * Optional private extension used only when a Store can retain an unpublished
 * contiguous prefix across acquisition-session boundaries.
 *
 * This is intentionally not a composition-level "Resumable" promise. Safe
 * resume remains a runtime conclusion over this Store's current facts, the OTA
 * checkpoint, the selected Source's offset capability and the later mandatory
 * full-Artifact verification pass.
 */
class IPartialArtifactStore {
public:
    virtual ~IPartialArtifactStore() = default;

    /** Inspect private partial state for exactly this immutable Artifact. */
    virtual Result InspectPartial(
        const ArtifactStoreOpenRequest& request,
        PartialArtifactInfo& info) noexcept = 0;

    /**
     * Open the private partial writer at exactly acceptedPrefixLength.
     *
     * If more bytes are physically retained than the checkpoint accepts, the
     * implementation must truncate/ignore the uncheckpointed suffix before the
     * call succeeds. A zero prefix starts/restarts the private partial object.
     */
    virtual Result BeginPartialWrite(
        const ArtifactStoreOpenRequest& request,
        std::uint64_t acceptedPrefixLength) noexcept = 0;

    /** Append bytes to the currently open private contiguous prefix. */
    virtual StreamWriteResult WritePartial(
        const std::uint8_t* data,
        std::size_t size) noexcept = 0;

    /**
     * Establish the Store's required current stability/durability for [0,N).
     * OTA advances its durable AcceptedPrefixLength only after this succeeds.
     */
    virtual Result StabilizePartial(std::uint64_t retainedPrefixLength) noexcept = 0;

    /**
     * End the private writer while retaining the unpublished prefix for a
     * later retry/failover/recovery attempt.
     */
    virtual Result SuspendPartialWrite() noexcept = 0;

    /** Open private replay of exactly [0,acceptedPrefixLength). */
    virtual Result BeginPartialReplay(
        const ArtifactStoreOpenRequest& request,
        std::uint64_t acceptedPrefixLength) noexcept = 0;

    /** Read the private replay stream cooperatively. */
    virtual StreamReadResult ReplayPartial(
        std::uint8_t* output,
        std::size_t capacity) noexcept = 0;

    /** End a private replay session. */
    virtual void ClosePartialReplay() noexcept = 0;

    /**
     * Publish a byte-complete private object into the normal ArtifactStore
     * namespace. Stored still does not mean trusted; OTA's normal Verify phase
     * remains mandatory before ArtifactsVerified.
     */
    virtual ArtifactStoreFinalizeResult FinalizePartial() noexcept = 0;

    /**
     * Remove only acquisition-private partial state for this request. This must
     * never remove a committed complete Artifact sharing the same identifier.
     */
    virtual Result DiscardPartial(const ArtifactStoreOpenRequest& request) noexcept = 0;
};

} // namespace ESPressio::OTA
