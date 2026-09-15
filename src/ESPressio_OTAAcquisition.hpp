#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "ESPressio_OTACheckpoint.hpp"
#include "ESPressio_OTADurable.hpp"
#include "ESPressio_OTAManifest.hpp"
#include "ESPressio_OTAProviders.hpp"

namespace ESPressio::OTA {

enum class ArtifactAcquisitionPhase : std::uint8_t {
    Idle,
    OpeningSource,
    OpeningStore,
    Checkpointing,
    Reading,
    Writing,
    Finalizing,
    Complete,
    Failed
};

enum class ArtifactAcquisitionReason : std::uint32_t {
    None = 0U,
    InvalidRequest,
    Busy,
    SourceOpenFailed,
    StoreOpenFailed,
    CheckpointUnavailable,
    CheckpointMismatch,
    CheckpointRevisionExhausted,
    InvalidSourceResult,
    Truncated,
    Overrun,
    StoreWriteFailed,
    InvalidStoreWriteResult,
    IdentifierCollision,
    StoreFinalizeFailed,
    CleanupFailed
};

template<typename TCapacityProfile>
struct ArtifactTransferWorkspace final {
    static_assert(TCapacityProfile::IsValid, "ArtifactTransferWorkspace requires a valid OTA capacity profile");
    std::array<std::uint8_t, TCapacityProfile::TransferBufferBytes> Bytes{};
};

namespace ArtifactAcquisitionDetail {

inline constexpr Result Pending() noexcept {
    return {OutcomeClass::Pending, {DiagnosticDomain::OTA, 0U, 0, {}, 0U}};
}

inline constexpr Result Failure(
    OutcomeClass outcome,
    DiagnosticDomain domain,
    ArtifactAcquisitionReason reason) noexcept {
    return {outcome, {domain, static_cast<std::uint32_t>(reason), 0, {}, 0U}};
}

inline constexpr Result DurableResult(OTADurableStatus status) noexcept {
    if (status == OTADurableStatus::Success || status == OTADurableStatus::NotFound) return Result::Success();
    OutcomeClass outcome = OutcomeClass::PersistenceFailed;
    if (status == OTADurableStatus::CapacityUnavailable || status == OTADurableStatus::Exhausted) {
        outcome = OutcomeClass::CapacityUnavailable;
    } else if (status == OTADurableStatus::UnsupportedBackend) {
        outcome = OutcomeClass::Unsupported;
    } else if (status == OTADurableStatus::Invalid || status == OTADurableStatus::InvalidTransition ||
               status == OTADurableStatus::TransactionMismatch) {
        outcome = OutcomeClass::Invalid;
    }
    return {outcome, {DiagnosticDomain::Persistence, static_cast<std::uint32_t>(status), 0, {}, 0U}};
}

inline constexpr bool Transient(const Result& result) noexcept {
    return result.Outcome == OutcomeClass::Pending || result.Outcome == OutcomeClass::Deferred;
}

} // namespace ArtifactAcquisitionDetail

/**
 * Cooperative, allocation-free acquisition of exactly one Manifest Artifact.
 *
 * The baseline intentionally restarts interrupted writes from offset zero. The
 * durable checkpoint therefore records a zero accepted prefix only. A non-zero
 * checkpoint from a stronger resumable path is safely superseded by a new
 * zero-prefix revision before new bytes are requested.
 *
 * The caller owns the transfer workspace; this session never contains an
 * Artifact-sized buffer.
 */
template<typename TCapacityProfile>
class ArtifactAcquisitionSession final {
    static_assert(TCapacityProfile::IsValid, "ArtifactAcquisitionSession requires a valid OTA capacity profile");

    IArtifactSource& source_;
    IArtifactStore& store_;
    ArtifactCheckpointStore<TCapacityProfile>& checkpoints_;
    ArtifactTransferWorkspace<TCapacityProfile>& workspace_;

    ArtifactAcquisitionPhase phase_{ArtifactAcquisitionPhase::Idle};
    UpdateTransactionId transaction_{};
    ArtifactIdentifier artifact_{};
    std::uint64_t expectedLength_{0U};
    ExactLengthReadTracker tracker_{0U};
    std::size_t bufferedBytes_{0U};
    std::size_t bufferOffset_{0U};
    std::uint64_t storedBytes_{0U};
    std::size_t checkpointSlot_{TCapacityProfile::MaximumArtifactCheckpoints};
    std::uint32_t checkpointRevision_{0U};
    bool active_{false};
    bool sourceOpen_{false};
    bool storeOpen_{false};
    bool checkpointActive_{false};

    Result RemoveCheckpoint() noexcept {
        if (!checkpointActive_) return Result::Success();
        const auto status = checkpoints_.Remove(checkpointSlot_);
        if (status == OTADurableStatus::Success || status == OTADurableStatus::NotFound) {
            checkpointActive_ = false;
            checkpointSlot_ = TCapacityProfile::MaximumArtifactCheckpoints;
            return Result::Success();
        }
        return ArtifactAcquisitionDetail::DurableResult(status);
    }

    void CloseSource() noexcept {
        if (!sourceOpen_) return;
        source_.Close();
        sourceOpen_ = false;
    }

    void AbortStore() noexcept {
        if (!storeOpen_) return;
        store_.Abort();
        storeOpen_ = false;
    }

    Result Fail(Result primary) noexcept {
        CloseSource();
        AbortStore();
        const auto cleanup = RemoveCheckpoint();
        active_ = false;
        phase_ = ArtifactAcquisitionPhase::Failed;
        return cleanup ? primary : cleanup;
    }

    Result PrepareCheckpoint() noexcept {
        ArtifactCheckpoint<TCapacityProfile> existing;
        std::size_t slot = TCapacityProfile::MaximumArtifactCheckpoints;
        const auto found = checkpoints_.Find(transaction_, artifact_, existing, slot);
        if (found == OTADurableStatus::Success) {
            if (existing.ExpectedLength != expectedLength_) {
                return ArtifactAcquisitionDetail::Failure(
                    OutcomeClass::Invalid, DiagnosticDomain::Persistence,
                    ArtifactAcquisitionReason::CheckpointMismatch);
            }
            if (existing.Revision == std::numeric_limits<std::uint32_t>::max()) {
                return ArtifactAcquisitionDetail::Failure(
                    OutcomeClass::CapacityUnavailable, DiagnosticDomain::Persistence,
                    ArtifactAcquisitionReason::CheckpointRevisionExhausted);
            }
            checkpointSlot_ = slot;
            checkpointRevision_ = existing.Revision + 1U;
        } else if (found == OTADurableStatus::NotFound) {
            for (std::size_t i = 0U; i < TCapacityProfile::MaximumArtifactCheckpoints; ++i) {
                ArtifactCheckpoint<TCapacityProfile> candidate;
                const auto loaded = checkpoints_.Load(i, candidate);
                if (loaded == OTADurableStatus::NotFound) {
                    checkpointSlot_ = i;
                    checkpointRevision_ = 1U;
                    break;
                }
                if (loaded != OTADurableStatus::Success) return ArtifactAcquisitionDetail::DurableResult(loaded);
            }
            if (checkpointSlot_ == TCapacityProfile::MaximumArtifactCheckpoints) {
                return ArtifactAcquisitionDetail::Failure(
                    OutcomeClass::CapacityUnavailable, DiagnosticDomain::Persistence,
                    ArtifactAcquisitionReason::CheckpointUnavailable);
            }
        } else {
            return ArtifactAcquisitionDetail::DurableResult(found);
        }

        ArtifactCheckpoint<TCapacityProfile> checkpoint;
        checkpoint.Transaction = transaction_.Value();
        checkpoint.Artifact = artifact_.Bytes();
        checkpoint.ExpectedLength = expectedLength_;
        checkpoint.AcceptedPrefixLength = 0U;
        checkpoint.Revision = checkpointRevision_;
        checkpoint.PrefixDigestAlgorithm = 0U;
        const auto saved = checkpoints_.Save(checkpointSlot_, checkpoint);
        if (saved != OTADurableStatus::Success) return ArtifactAcquisitionDetail::DurableResult(saved);
        checkpointActive_ = true;
        return Result::Success();
    }

public:
    ArtifactAcquisitionSession(
        IArtifactSource& source,
        IArtifactStore& store,
        ArtifactCheckpointStore<TCapacityProfile>& checkpoints,
        ArtifactTransferWorkspace<TCapacityProfile>& workspace) noexcept
        : source_(source), store_(store), checkpoints_(checkpoints), workspace_(workspace) {}

    ArtifactAcquisitionPhase Phase() const noexcept { return phase_; }
    bool IsActive() const noexcept { return active_; }
    bool IsComplete() const noexcept { return phase_ == ArtifactAcquisitionPhase::Complete; }
    std::uint64_t AcceptedSourceBytes() const noexcept { return tracker_.Accepted(); }
    std::uint64_t StoredBytes() const noexcept { return storedBytes_; }
    ArtifactIdentifier Artifact() const noexcept { return artifact_; }

    Result Begin(
        UpdateTransactionId transaction,
        const ManifestArtifact<TCapacityProfile>& manifestArtifact) noexcept {
        const ArtifactIdentifier identifier{manifestArtifact.Identifier};
        if (active_) {
            return ArtifactAcquisitionDetail::Failure(
                OutcomeClass::Unavailable, DiagnosticDomain::OTA, ArtifactAcquisitionReason::Busy);
        }
        if (!transaction || !identifier || manifestArtifact.ExpectedLength == 0U) {
            return ArtifactAcquisitionDetail::Failure(
                OutcomeClass::Invalid, DiagnosticDomain::OTA, ArtifactAcquisitionReason::InvalidRequest);
        }

        phase_ = ArtifactAcquisitionPhase::OpeningSource;
        transaction_ = transaction;
        artifact_ = identifier;
        expectedLength_ = manifestArtifact.ExpectedLength;
        tracker_ = ExactLengthReadTracker{expectedLength_};
        bufferedBytes_ = 0U;
        bufferOffset_ = 0U;
        storedBytes_ = 0U;
        checkpointSlot_ = TCapacityProfile::MaximumArtifactCheckpoints;
        checkpointRevision_ = 0U;
        sourceOpen_ = false;
        storeOpen_ = false;
        checkpointActive_ = false;
        active_ = true;
        return ArtifactAcquisitionDetail::Pending();
    }

    Result Advance() noexcept {
        if (!active_) {
            return IsComplete() ? Result::Success() : ArtifactAcquisitionDetail::Failure(
                OutcomeClass::Invalid, DiagnosticDomain::OTA, ArtifactAcquisitionReason::InvalidRequest);
        }

        switch (phase_) {
            case ArtifactAcquisitionPhase::OpeningSource: {
                const ArtifactSourceOpenRequest request{artifact_, expectedLength_, 0U};
                const auto opened = source_.Open(request);
                if (opened) {
                    sourceOpen_ = true;
                    phase_ = ArtifactAcquisitionPhase::OpeningStore;
                    return ArtifactAcquisitionDetail::Pending();
                }
                if (ArtifactAcquisitionDetail::Transient(opened)) return opened;
                return Fail(opened);
            }
            case ArtifactAcquisitionPhase::OpeningStore: {
                const ArtifactStoreOpenRequest request{artifact_, expectedLength_};
                const auto opened = store_.BeginWrite(request);
                if (opened) {
                    storeOpen_ = true;
                    phase_ = ArtifactAcquisitionPhase::Checkpointing;
                    return ArtifactAcquisitionDetail::Pending();
                }
                if (ArtifactAcquisitionDetail::Transient(opened)) return opened;
                return Fail(opened);
            }
            case ArtifactAcquisitionPhase::Checkpointing: {
                const auto checkpoint = PrepareCheckpoint();
                if (!checkpoint) return Fail(checkpoint);
                phase_ = ArtifactAcquisitionPhase::Reading;
                return ArtifactAcquisitionDetail::Pending();
            }
            case ArtifactAcquisitionPhase::Reading: {
                const auto read = source_.Read(workspace_.Bytes.data(), workspace_.Bytes.size());
                if (!read.IsValidFor(workspace_.Bytes.size())) {
                    return Fail(ArtifactAcquisitionDetail::Failure(
                        OutcomeClass::Failed, DiagnosticDomain::Source,
                        ArtifactAcquisitionReason::InvalidSourceResult));
                }
                const auto observed = tracker_.Observe(read, workspace_.Bytes.size());
                if (observed == ExactLengthReadStatus::Pending) return read.Detail;
                if (observed == ExactLengthReadStatus::Failed) return Fail(read.Detail);
                if (observed == ExactLengthReadStatus::InvalidResult) {
                    return Fail(ArtifactAcquisitionDetail::Failure(
                        OutcomeClass::Failed, DiagnosticDomain::Source,
                        ArtifactAcquisitionReason::InvalidSourceResult));
                }
                if (observed == ExactLengthReadStatus::Truncated) {
                    return Fail(ArtifactAcquisitionDetail::Failure(
                        OutcomeClass::Failed, DiagnosticDomain::Source,
                        ArtifactAcquisitionReason::Truncated));
                }
                if (observed == ExactLengthReadStatus::Overrun) {
                    return Fail(ArtifactAcquisitionDetail::Failure(
                        OutcomeClass::Failed, DiagnosticDomain::Source,
                        ArtifactAcquisitionReason::Overrun));
                }
                if (read.Status == StreamReadStatus::Data) {
                    bufferedBytes_ = read.Bytes;
                    bufferOffset_ = 0U;
                    phase_ = ArtifactAcquisitionPhase::Writing;
                    return ArtifactAcquisitionDetail::Pending();
                }
                if (observed == ExactLengthReadStatus::Complete) {
                    CloseSource();
                    phase_ = ArtifactAcquisitionPhase::Finalizing;
                    return ArtifactAcquisitionDetail::Pending();
                }
                return ArtifactAcquisitionDetail::Pending();
            }
            case ArtifactAcquisitionPhase::Writing: {
                const std::size_t remaining = bufferedBytes_ - bufferOffset_;
                const auto written = store_.Write(workspace_.Bytes.data() + bufferOffset_, remaining);
                if (!written.IsValidFor(remaining)) {
                    return Fail(ArtifactAcquisitionDetail::Failure(
                        OutcomeClass::Failed, DiagnosticDomain::Store,
                        ArtifactAcquisitionReason::InvalidStoreWriteResult));
                }
                if (written.Status == StreamWriteStatus::Pending) return written.Detail;
                if (written.Status == StreamWriteStatus::Failed) return Fail(written.Detail);
                bufferOffset_ += written.Bytes;
                storedBytes_ += written.Bytes;
                if (bufferOffset_ == bufferedBytes_) {
                    bufferedBytes_ = 0U;
                    bufferOffset_ = 0U;
                    phase_ = ArtifactAcquisitionPhase::Reading;
                }
                return ArtifactAcquisitionDetail::Pending();
            }
            case ArtifactAcquisitionPhase::Finalizing: {
                const auto finalized = store_.Finalize();
                if (finalized.Status == ArtifactStoreFinalizeStatus::Pending) return finalized.Detail;
                if (finalized.Status == ArtifactStoreFinalizeStatus::IdentifierCollision) {
                    return Fail(ArtifactAcquisitionDetail::Failure(
                        OutcomeClass::Failed, DiagnosticDomain::Store,
                        ArtifactAcquisitionReason::IdentifierCollision));
                }
                if (finalized.Status == ArtifactStoreFinalizeStatus::Failed) return Fail(finalized.Detail);
                if (finalized.Status != ArtifactStoreFinalizeStatus::Stored &&
                    finalized.Status != ArtifactStoreFinalizeStatus::AlreadyPresent) {
                    return Fail(ArtifactAcquisitionDetail::Failure(
                        OutcomeClass::Failed, DiagnosticDomain::Store,
                        ArtifactAcquisitionReason::StoreFinalizeFailed));
                }
                storeOpen_ = false;
                const auto checkpoint = RemoveCheckpoint();
                if (!checkpoint) return Fail(checkpoint);
                active_ = false;
                phase_ = ArtifactAcquisitionPhase::Complete;
                return Result::Success();
            }
            case ArtifactAcquisitionPhase::Idle:
            case ArtifactAcquisitionPhase::Complete:
            case ArtifactAcquisitionPhase::Failed:
                break;
        }
        return ArtifactAcquisitionDetail::Failure(
            OutcomeClass::Invalid, DiagnosticDomain::OTA, ArtifactAcquisitionReason::InvalidRequest);
    }

    Result Abort() noexcept {
        if (!active_) return Result::Success();
        CloseSource();
        AbortStore();
        const auto checkpoint = RemoveCheckpoint();
        active_ = false;
        phase_ = ArtifactAcquisitionPhase::Failed;
        return checkpoint ? Result::Success() : checkpoint;
    }
};

} // namespace ESPressio::OTA
