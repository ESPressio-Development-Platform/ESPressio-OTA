#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "ESPressio_OTACheckpoint.hpp"
#include "ESPressio_OTADurable.hpp"
#include "ESPressio_OTAManifest.hpp"
#include "ESPressio_OTAPartialArtifactStore.hpp"
#include "ESPressio_OTAProviders.hpp"

namespace ESPressio::OTA {

enum class ArtifactAcquisitionPhase : std::uint8_t {
    Idle,
    PreparingCheckpoint,
    InspectingPartial,
    OpeningReplay,
    ReplayingPartial,
    OpeningSource,
    OpeningStore,
    Reading,
    Writing,
    Stabilizing,
    Checkpointing,
    Finalizing,
    SourceFailed,
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
    CleanupFailed,
    PartialInspectFailed,
    PartialReplayUnavailable,
    PartialReplayFailed,
    PartialStabilizeFailed,
    PartialSuspendFailed,
    RetryUnavailable
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
 * Baseline Sources/Stores continue to restart from offset zero. When the caller
 * explicitly supplies both Source offset capability and an IPartialArtifactStore,
 * the session may resume a durable/private contiguous prefix after proving the
 * checkpoint and current Store facts agree. No provider-order semantics exist
 * here; Source choice remains a Coordinator/caller concern.
 *
 * Partial replay is an acquisition recovery preflight. The later retained
 * Artifact verification session still rereads the complete finalized object
 * from byte zero and performs the authoritative full digest verification before
 * RecoveryPoint::ArtifactsVerified.
 */
template<typename TCapacityProfile>
class ArtifactAcquisitionSession final {
    static_assert(TCapacityProfile::IsValid, "ArtifactAcquisitionSession requires a valid OTA capacity profile");

    IArtifactSource* source_;
    IArtifactStore& store_;
    IPartialArtifactStore* partialStore_;
    ArtifactCheckpointStore<TCapacityProfile>& checkpoints_;
    ArtifactTransferWorkspace<TCapacityProfile>& workspace_;
    bool sourceOffsetRead_{false};

    ArtifactAcquisitionPhase phase_{ArtifactAcquisitionPhase::Idle};
    UpdateTransactionId transaction_{};
    ArtifactIdentifier artifact_{};
    std::uint64_t expectedLength_{0U};
    std::uint64_t sourceOffset_{0U};
    ExactLengthReadTracker tracker_{0U};
    ExactLengthReadTracker replayTracker_{0U};
    std::size_t bufferedBytes_{0U};
    std::size_t bufferOffset_{0U};
    std::uint64_t storedBytes_{0U};
    std::size_t checkpointSlot_{TCapacityProfile::MaximumArtifactCheckpoints};
    std::uint32_t checkpointRevision_{0U};
    std::uint64_t checkpointAcceptedPrefix_{0U};
    ArtifactAcquisitionPhase phaseAfterCheckpoint_{ArtifactAcquisitionPhase::Reading};
    bool active_{false};
    bool sourceOpen_{false};
    bool storeOpen_{false};
    bool replayOpen_{false};
    bool checkpointActive_{false};
    Result lastSourceFailure_{OutcomeClass::Failed, {}};

    ArtifactStoreOpenRequest StoreRequest() const noexcept {
        return {artifact_, expectedLength_};
    }

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
        if (!sourceOpen_ || source_ == nullptr) return;
        source_->Close();
        sourceOpen_ = false;
    }

    void CloseReplay() noexcept {
        if (!replayOpen_ || partialStore_ == nullptr) return;
        partialStore_->ClosePartialReplay();
        replayOpen_ = false;
    }

    void AbortLegacyStore() noexcept {
        if (!storeOpen_ || partialStore_ != nullptr) return;
        store_.Abort();
        storeOpen_ = false;
    }

    Result DiscardPartial() noexcept {
        if (partialStore_ == nullptr || !artifact_ || expectedLength_ == 0U) return Result::Success();
        if (storeOpen_) {
            const auto suspended = partialStore_->SuspendPartialWrite();
            if (!suspended) return suspended;
            storeOpen_ = false;
        }
        return partialStore_->DiscardPartial(StoreRequest());
    }

    Result Fail(Result primary) noexcept {
        CloseSource();
        CloseReplay();
        Result storeCleanup = Result::Success();
        if (partialStore_ != nullptr) storeCleanup = DiscardPartial();
        else AbortLegacyStore();
        const auto checkpointCleanup = RemoveCheckpoint();
        active_ = false;
        phase_ = ArtifactAcquisitionPhase::Failed;
        if (!storeCleanup) return storeCleanup;
        return checkpointCleanup ? primary : checkpointCleanup;
    }

    Result SaveCheckpoint(std::uint64_t acceptedPrefix) noexcept {
        if (!checkpointActive_ || checkpointSlot_ >= TCapacityProfile::MaximumArtifactCheckpoints) {
            return ArtifactAcquisitionDetail::Failure(
                OutcomeClass::Invalid, DiagnosticDomain::Persistence,
                ArtifactAcquisitionReason::CheckpointUnavailable);
        }
        if (acceptedPrefix > expectedLength_) {
            return ArtifactAcquisitionDetail::Failure(
                OutcomeClass::Invalid, DiagnosticDomain::Persistence,
                ArtifactAcquisitionReason::CheckpointMismatch);
        }
        if (checkpointRevision_ == std::numeric_limits<std::uint32_t>::max()) {
            return ArtifactAcquisitionDetail::Failure(
                OutcomeClass::CapacityUnavailable, DiagnosticDomain::Persistence,
                ArtifactAcquisitionReason::CheckpointRevisionExhausted);
        }

        ArtifactCheckpoint<TCapacityProfile> checkpoint;
        checkpoint.Transaction = transaction_.Value();
        checkpoint.Artifact = artifact_.Bytes();
        checkpoint.ExpectedLength = expectedLength_;
        checkpoint.AcceptedPrefixLength = acceptedPrefix;
        checkpoint.Revision = checkpointRevision_ + 1U;
        // Replay, not opaque verifier state, is the V1 baseline. Prefix digest
        // evidence remains an optional future/provider optimization.
        checkpoint.PrefixDigestAlgorithm = 0U;
        const auto saved = checkpoints_.Save(checkpointSlot_, checkpoint);
        if (saved != OTADurableStatus::Success) return ArtifactAcquisitionDetail::DurableResult(saved);
        checkpointRevision_ = checkpoint.Revision;
        checkpointAcceptedPrefix_ = acceptedPrefix;
        return Result::Success();
    }

    Result RestartFromZero() noexcept {
        CloseSource();
        CloseReplay();
        if (partialStore_ == nullptr) AbortLegacyStore();
        else if (storeOpen_) {
            const auto suspended = partialStore_->SuspendPartialWrite();
            if (!suspended) return Fail(suspended);
            storeOpen_ = false;
        }

        sourceOffset_ = 0U;
        storedBytes_ = 0U;
        bufferedBytes_ = 0U;
        bufferOffset_ = 0U;
        tracker_ = ExactLengthReadTracker{expectedLength_};
        replayTracker_ = ExactLengthReadTracker{0U};
        const auto checkpoint = SaveCheckpoint(0U);
        if (!checkpoint) return Fail(checkpoint);
        phase_ = ArtifactAcquisitionPhase::OpeningSource;
        return ArtifactAcquisitionDetail::Pending();
    }

    Result PrepareCheckpoint() noexcept {
        ArtifactCheckpoint<TCapacityProfile> existing;
        std::size_t slot = TCapacityProfile::MaximumArtifactCheckpoints;
        const auto found = checkpoints_.Find(transaction_, artifact_, existing, slot);
        if (found == OTADurableStatus::Success) {
            if (existing.ExpectedLength != expectedLength_ ||
                existing.AcceptedPrefixLength > expectedLength_) {
                return ArtifactAcquisitionDetail::Failure(
                    OutcomeClass::Invalid, DiagnosticDomain::Persistence,
                    ArtifactAcquisitionReason::CheckpointMismatch);
            }
            checkpointSlot_ = slot;
            checkpointRevision_ = existing.Revision;
            checkpointAcceptedPrefix_ = existing.AcceptedPrefixLength;
            checkpointActive_ = true;

            if (existing.AcceptedPrefixLength != 0U && partialStore_ != nullptr && sourceOffsetRead_) {
                phase_ = ArtifactAcquisitionPhase::InspectingPartial;
                return ArtifactAcquisitionDetail::Pending();
            }
            if (existing.AcceptedPrefixLength != 0U) return RestartFromZero();

            sourceOffset_ = 0U;
            tracker_ = ExactLengthReadTracker{expectedLength_};
            phase_ = ArtifactAcquisitionPhase::OpeningSource;
            return ArtifactAcquisitionDetail::Pending();
        }

        if (found != OTADurableStatus::NotFound) return ArtifactAcquisitionDetail::DurableResult(found);
        for (std::size_t i = 0U; i < TCapacityProfile::MaximumArtifactCheckpoints; ++i) {
            ArtifactCheckpoint<TCapacityProfile> candidate;
            const auto loaded = checkpoints_.Load(i, candidate);
            if (loaded == OTADurableStatus::NotFound) {
                checkpointSlot_ = i;
                checkpointRevision_ = 0U;
                checkpointAcceptedPrefix_ = 0U;
                checkpointActive_ = true;
                const auto checkpoint = SaveCheckpoint(0U);
                if (!checkpoint) return checkpoint;
                sourceOffset_ = 0U;
                tracker_ = ExactLengthReadTracker{expectedLength_};
                phase_ = ArtifactAcquisitionPhase::OpeningSource;
                return ArtifactAcquisitionDetail::Pending();
            }
            if (loaded != OTADurableStatus::Success) return ArtifactAcquisitionDetail::DurableResult(loaded);
        }
        return ArtifactAcquisitionDetail::Failure(
            OutcomeClass::CapacityUnavailable, DiagnosticDomain::Persistence,
            ArtifactAcquisitionReason::CheckpointUnavailable);
    }

    Result SuspendForSourceFailure(Result failure) noexcept {
        CloseSource();
        CloseReplay();
        if (partialStore_ != nullptr && storeOpen_) {
            const auto suspended = partialStore_->SuspendPartialWrite();
            if (!suspended) {
                return Fail(ArtifactAcquisitionDetail::Failure(
                    suspended.Outcome, DiagnosticDomain::Store,
                    ArtifactAcquisitionReason::PartialSuspendFailed));
            }
            storeOpen_ = false;
        } else if (partialStore_ == nullptr) {
            AbortLegacyStore();
            const auto reset = SaveCheckpoint(0U);
            if (!reset) return Fail(reset);
            storedBytes_ = 0U;
        }
        lastSourceFailure_ = failure;
        phase_ = ArtifactAcquisitionPhase::SourceFailed;
        return failure;
    }

public:
    ArtifactAcquisitionSession(
        IArtifactSource& source,
        IArtifactStore& store,
        ArtifactCheckpointStore<TCapacityProfile>& checkpoints,
        ArtifactTransferWorkspace<TCapacityProfile>& workspace) noexcept
        : source_(&source), store_(store), partialStore_(nullptr),
          checkpoints_(checkpoints), workspace_(workspace) {}

    ArtifactAcquisitionSession(
        IArtifactSource& source,
        bool sourceOffsetRead,
        IArtifactStore& store,
        IPartialArtifactStore& partialStore,
        ArtifactCheckpointStore<TCapacityProfile>& checkpoints,
        ArtifactTransferWorkspace<TCapacityProfile>& workspace) noexcept
        : source_(&source), store_(store), partialStore_(&partialStore),
          checkpoints_(checkpoints), workspace_(workspace), sourceOffsetRead_(sourceOffsetRead) {}

    ArtifactAcquisitionPhase Phase() const noexcept { return phase_; }
    bool IsActive() const noexcept { return active_; }
    bool IsComplete() const noexcept { return phase_ == ArtifactAcquisitionPhase::Complete; }
    bool AwaitingSourceRetry() const noexcept { return phase_ == ArtifactAcquisitionPhase::SourceFailed; }
    const Result& LastSourceFailure() const noexcept { return lastSourceFailure_; }
    std::uint64_t AcceptedSourceBytes() const noexcept { return sourceOffset_ + tracker_.Accepted(); }
    std::uint64_t StoredBytes() const noexcept { return storedBytes_; }
    std::uint64_t CheckpointedBytes() const noexcept { return checkpointAcceptedPrefix_; }
    ArtifactIdentifier Artifact() const noexcept { return artifact_; }

    Result Begin(
        UpdateTransactionId transaction,
        const ManifestArtifact<TCapacityProfile>& manifestArtifact) noexcept {
        const ArtifactIdentifier identifier{manifestArtifact.Identifier};
        if (active_ || checkpointActive_) {
            return ArtifactAcquisitionDetail::Failure(
                OutcomeClass::Unavailable, DiagnosticDomain::OTA, ArtifactAcquisitionReason::Busy);
        }
        if (!transaction || !identifier || manifestArtifact.ExpectedLength == 0U || source_ == nullptr) {
            return ArtifactAcquisitionDetail::Failure(
                OutcomeClass::Invalid, DiagnosticDomain::OTA, ArtifactAcquisitionReason::InvalidRequest);
        }

        phase_ = ArtifactAcquisitionPhase::PreparingCheckpoint;
        transaction_ = transaction;
        artifact_ = identifier;
        expectedLength_ = manifestArtifact.ExpectedLength;
        sourceOffset_ = 0U;
        tracker_ = ExactLengthReadTracker{expectedLength_};
        replayTracker_ = ExactLengthReadTracker{0U};
        bufferedBytes_ = 0U;
        bufferOffset_ = 0U;
        storedBytes_ = 0U;
        checkpointSlot_ = TCapacityProfile::MaximumArtifactCheckpoints;
        checkpointRevision_ = 0U;
        checkpointAcceptedPrefix_ = 0U;
        sourceOpen_ = false;
        storeOpen_ = false;
        replayOpen_ = false;
        checkpointActive_ = false;
        active_ = true;
        lastSourceFailure_ = {OutcomeClass::Failed, {}};
        return ArtifactAcquisitionDetail::Pending();
    }

    /**
     * Continue the same Artifact attempt with an explicitly selected Source.
     * No implicit provider ordering is consulted. If safe resume cannot be
     * proven for this Source/Store/checkpoint tuple, the attempt restarts at 0.
     */
    Result RetryWithSource(IArtifactSource& source, bool sourceOffsetRead) noexcept {
        if (!active_ || phase_ != ArtifactAcquisitionPhase::SourceFailed) {
            return ArtifactAcquisitionDetail::Failure(
                OutcomeClass::Invalid, DiagnosticDomain::OTA,
                ArtifactAcquisitionReason::RetryUnavailable);
        }
        source_ = &source;
        sourceOffsetRead_ = sourceOffsetRead;
        bufferedBytes_ = 0U;
        bufferOffset_ = 0U;

        if (checkpointAcceptedPrefix_ != 0U && partialStore_ != nullptr && sourceOffsetRead_) {
            phase_ = ArtifactAcquisitionPhase::InspectingPartial;
            return ArtifactAcquisitionDetail::Pending();
        }
        return RestartFromZero();
    }

    Result Advance() noexcept {
        if (!active_) {
            return IsComplete() ? Result::Success() : ArtifactAcquisitionDetail::Failure(
                OutcomeClass::Invalid, DiagnosticDomain::OTA, ArtifactAcquisitionReason::InvalidRequest);
        }

        switch (phase_) {
            case ArtifactAcquisitionPhase::PreparingCheckpoint: {
                const auto prepared = PrepareCheckpoint();
                if (!prepared) return Fail(prepared);
                return prepared;
            }

            case ArtifactAcquisitionPhase::InspectingPartial: {
                if (partialStore_ == nullptr || checkpointAcceptedPrefix_ == 0U || !sourceOffsetRead_) {
                    return RestartFromZero();
                }
                PartialArtifactInfo info;
                const auto inspected = partialStore_->InspectPartial(StoreRequest(), info);
                if (ArtifactAcquisitionDetail::Transient(inspected)) return inspected;
                if (!inspected || !info.IsValidFor(expectedLength_) || !info.Present || !info.Replayable ||
                    info.RetainedPrefixLength < checkpointAcceptedPrefix_) {
                    return RestartFromZero();
                }
                sourceOffset_ = checkpointAcceptedPrefix_;
                storedBytes_ = checkpointAcceptedPrefix_;
                replayTracker_ = ExactLengthReadTracker{checkpointAcceptedPrefix_};
                phase_ = ArtifactAcquisitionPhase::OpeningReplay;
                return ArtifactAcquisitionDetail::Pending();
            }

            case ArtifactAcquisitionPhase::OpeningReplay: {
                const auto opened = partialStore_->BeginPartialReplay(StoreRequest(), checkpointAcceptedPrefix_);
                if (ArtifactAcquisitionDetail::Transient(opened)) return opened;
                if (!opened) return RestartFromZero();
                replayOpen_ = true;
                phase_ = ArtifactAcquisitionPhase::ReplayingPartial;
                return ArtifactAcquisitionDetail::Pending();
            }

            case ArtifactAcquisitionPhase::ReplayingPartial: {
                const auto read = partialStore_->ReplayPartial(workspace_.Bytes.data(), workspace_.Bytes.size());
                if (!read.IsValidFor(workspace_.Bytes.size())) return RestartFromZero();
                const auto observed = replayTracker_.Observe(read, workspace_.Bytes.size());
                if (observed == ExactLengthReadStatus::Pending) return read.Detail;
                if (observed == ExactLengthReadStatus::Continue || observed == ExactLengthReadStatus::AwaitingEnd) {
                    return ArtifactAcquisitionDetail::Pending();
                }
                if (observed != ExactLengthReadStatus::Complete) return RestartFromZero();
                CloseReplay();
                tracker_ = ExactLengthReadTracker{expectedLength_ - sourceOffset_};
                phase_ = ArtifactAcquisitionPhase::OpeningSource;
                return ArtifactAcquisitionDetail::Pending();
            }

            case ArtifactAcquisitionPhase::OpeningSource: {
                const ArtifactSourceOpenRequest request{artifact_, expectedLength_, sourceOffset_};
                const auto opened = source_->Open(request);
                if (opened) {
                    sourceOpen_ = true;
                    phase_ = ArtifactAcquisitionPhase::OpeningStore;
                    return ArtifactAcquisitionDetail::Pending();
                }
                if (ArtifactAcquisitionDetail::Transient(opened)) return opened;
                if (sourceOffset_ != 0U &&
                    (opened.Outcome == OutcomeClass::Unsupported || opened.Outcome == OutcomeClass::Invalid)) {
                    return RestartFromZero();
                }
                return SuspendForSourceFailure(opened);
            }

            case ArtifactAcquisitionPhase::OpeningStore: {
                Result opened = Result::Success();
                if (partialStore_ != nullptr) {
                    opened = partialStore_->BeginPartialWrite(StoreRequest(), sourceOffset_);
                } else {
                    opened = store_.BeginWrite(StoreRequest());
                }
                if (opened) {
                    storeOpen_ = true;
                    storedBytes_ = sourceOffset_;
                    phase_ = ArtifactAcquisitionPhase::Reading;
                    return ArtifactAcquisitionDetail::Pending();
                }
                if (ArtifactAcquisitionDetail::Transient(opened)) return opened;
                return Fail(opened);
            }

            case ArtifactAcquisitionPhase::Reading: {
                const auto read = source_->Read(workspace_.Bytes.data(), workspace_.Bytes.size());
                if (!read.IsValidFor(workspace_.Bytes.size())) {
                    return SuspendForSourceFailure(ArtifactAcquisitionDetail::Failure(
                        OutcomeClass::Failed, DiagnosticDomain::Source,
                        ArtifactAcquisitionReason::InvalidSourceResult));
                }
                const auto observed = tracker_.Observe(read, workspace_.Bytes.size());
                if (observed == ExactLengthReadStatus::Pending) return read.Detail;
                if (observed == ExactLengthReadStatus::Failed) return SuspendForSourceFailure(read.Detail);
                if (observed == ExactLengthReadStatus::InvalidResult) {
                    return SuspendForSourceFailure(ArtifactAcquisitionDetail::Failure(
                        OutcomeClass::Failed, DiagnosticDomain::Source,
                        ArtifactAcquisitionReason::InvalidSourceResult));
                }
                if (observed == ExactLengthReadStatus::Truncated) {
                    return SuspendForSourceFailure(ArtifactAcquisitionDetail::Failure(
                        OutcomeClass::Failed, DiagnosticDomain::Source,
                        ArtifactAcquisitionReason::Truncated));
                }
                if (observed == ExactLengthReadStatus::Overrun) {
                    return SuspendForSourceFailure(ArtifactAcquisitionDetail::Failure(
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
                const auto written = partialStore_ != nullptr
                    ? partialStore_->WritePartial(workspace_.Bytes.data() + bufferOffset_, remaining)
                    : store_.Write(workspace_.Bytes.data() + bufferOffset_, remaining);
                if (!written.IsValidFor(remaining)) {
                    return Fail(ArtifactAcquisitionDetail::Failure(
                        OutcomeClass::Failed, DiagnosticDomain::Store,
                        ArtifactAcquisitionReason::InvalidStoreWriteResult));
                }
                if (written.Status == StreamWriteStatus::Pending) return written.Detail;
                if (written.Status == StreamWriteStatus::Failed) return Fail(written.Detail);
                bufferOffset_ += written.Bytes;
                storedBytes_ += written.Bytes;

                if (partialStore_ != nullptr) {
                    phaseAfterCheckpoint_ = bufferOffset_ == bufferedBytes_
                        ? ArtifactAcquisitionPhase::Reading
                        : ArtifactAcquisitionPhase::Writing;
                    phase_ = ArtifactAcquisitionPhase::Stabilizing;
                } else if (bufferOffset_ == bufferedBytes_) {
                    bufferedBytes_ = 0U;
                    bufferOffset_ = 0U;
                    phase_ = ArtifactAcquisitionPhase::Reading;
                }
                return ArtifactAcquisitionDetail::Pending();
            }

            case ArtifactAcquisitionPhase::Stabilizing: {
                const auto stable = partialStore_->StabilizePartial(storedBytes_);
                if (ArtifactAcquisitionDetail::Transient(stable)) return stable;
                if (!stable) {
                    return Fail(ArtifactAcquisitionDetail::Failure(
                        stable.Outcome, DiagnosticDomain::Store,
                        ArtifactAcquisitionReason::PartialStabilizeFailed));
                }
                phase_ = ArtifactAcquisitionPhase::Checkpointing;
                return ArtifactAcquisitionDetail::Pending();
            }

            case ArtifactAcquisitionPhase::Checkpointing: {
                const auto checkpoint = SaveCheckpoint(storedBytes_);
                if (!checkpoint) return Fail(checkpoint);
                if (phaseAfterCheckpoint_ == ArtifactAcquisitionPhase::Reading) {
                    bufferedBytes_ = 0U;
                    bufferOffset_ = 0U;
                }
                phase_ = phaseAfterCheckpoint_;
                return ArtifactAcquisitionDetail::Pending();
            }

            case ArtifactAcquisitionPhase::Finalizing: {
                const auto finalized = partialStore_ != nullptr
                    ? partialStore_->FinalizePartial()
                    : store_.Finalize();
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

            case ArtifactAcquisitionPhase::SourceFailed:
                return lastSourceFailure_;

            case ArtifactAcquisitionPhase::Idle:
            case ArtifactAcquisitionPhase::Complete:
            case ArtifactAcquisitionPhase::Failed:
                break;
        }
        return ArtifactAcquisitionDetail::Failure(
            OutcomeClass::Invalid, DiagnosticDomain::OTA, ArtifactAcquisitionReason::InvalidRequest);
    }

    Result Abort() noexcept {
        if (!active_ && !checkpointActive_) return Result::Success();
        CloseSource();
        CloseReplay();
        Result storeCleanup = Result::Success();
        if (partialStore_ != nullptr) storeCleanup = DiscardPartial();
        else AbortLegacyStore();
        const auto checkpoint = RemoveCheckpoint();
        active_ = false;
        phase_ = ArtifactAcquisitionPhase::Failed;
        if (!storeCleanup) return storeCleanup;
        return checkpoint ? Result::Success() : checkpoint;
    }
};

} // namespace ESPressio::OTA
