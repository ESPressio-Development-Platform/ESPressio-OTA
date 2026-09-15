#pragma once

#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAAcquisition.hpp"
#include "ESPressio_OTAManifest.hpp"
#include "ESPressio_OTAProviders.hpp"

namespace ESPressio::OTA {

struct ArtifactStoreReadRequest final {
    ArtifactIdentifier Identifier{};
    std::uint64_t ExpectedLength{0U};

    constexpr bool IsValid() const noexcept {
        return bool(Identifier) && ExpectedLength != 0U;
    }
};

/**
 * Artifact store whose immutable published objects can be replayed.
 *
 * The read surface is intentionally part of the same logical store object that
 * owns BeginWrite/Finalize. OpenRead(identifier) therefore addresses the exact
 * immutable object previously published under that identifier; it is not a
 * request to reacquire equivalent bytes from an external ArtifactSource.
 */
class IReadableArtifactStore : public IArtifactStore {
public:
    ~IReadableArtifactStore() override = default;
    virtual Result OpenRead(const ArtifactStoreReadRequest& request) noexcept = 0;
    virtual StreamReadResult ReadStored(std::uint8_t* output, std::size_t capacity) noexcept = 0;
    virtual void CloseRead() noexcept = 0;
};

enum class ArtifactVerificationPhase : std::uint8_t {
    Idle,
    OpeningStore,
    StartingDigest,
    Reading,
    FinalizingDigest,
    Complete,
    Failed
};

enum class ArtifactVerificationReason : std::uint32_t {
    None = 0U,
    InvalidRequest,
    Busy,
    StoreOpenFailed,
    UnsupportedDigestAlgorithm,
    DigestSizeMismatch,
    DigestBeginFailed,
    InvalidStoreReadResult,
    Truncated,
    Overrun,
    DigestUpdateFailed,
    DigestMismatch,
    DigestFinalizeFailed
};

namespace ArtifactVerificationDetail {

inline constexpr Result Pending() noexcept {
    return {OutcomeClass::Pending, {DiagnosticDomain::Security, 0U, 0, {}, 0U}};
}

inline constexpr Result Failure(OutcomeClass outcome, ArtifactVerificationReason reason) noexcept {
    return {outcome, {DiagnosticDomain::Security, static_cast<std::uint32_t>(reason), 0, {}, 0U}};
}

inline constexpr Result SecurityResult(
    const Security::VerificationResult& result,
    ArtifactVerificationReason fallback) noexcept {
    switch (result.Status) {
        case Security::VerificationStatus::Success:
            return Result::Success();
        case Security::VerificationStatus::UnsupportedAlgorithm:
            return {OutcomeClass::Unsupported,
                    {DiagnosticDomain::Security, static_cast<std::uint32_t>(ArtifactVerificationReason::UnsupportedDigestAlgorithm),
                     result.NativeCode, {}, 0U}};
        case Security::VerificationStatus::DigestMismatch:
            return {OutcomeClass::VerificationFailed,
                    {DiagnosticDomain::Security, static_cast<std::uint32_t>(ArtifactVerificationReason::DigestMismatch),
                     result.NativeCode, {}, 0U}};
        case Security::VerificationStatus::CapacityUnavailable:
            return {OutcomeClass::CapacityUnavailable,
                    {DiagnosticDomain::Security, static_cast<std::uint32_t>(fallback), result.NativeCode, {}, 0U}};
        case Security::VerificationStatus::InvalidArgument:
            return {OutcomeClass::Invalid,
                    {DiagnosticDomain::Security, static_cast<std::uint32_t>(fallback), result.NativeCode, {}, 0U}};
        case Security::VerificationStatus::TrustAnchorUnavailable:
        case Security::VerificationStatus::UntrustedSigner:
        case Security::VerificationStatus::InvalidSignature:
        case Security::VerificationStatus::Failed:
            return {OutcomeClass::VerificationFailed,
                    {DiagnosticDomain::Security, static_cast<std::uint32_t>(fallback), result.NativeCode, {}, 0U}};
    }
    return {OutcomeClass::VerificationFailed,
            {DiagnosticDomain::Security, static_cast<std::uint32_t>(fallback), result.NativeCode, {}, 0U}};
}

} // namespace ArtifactVerificationDetail

/**
 * Cooperative verification of exactly one immutable retained Artifact.
 *
 * Verification always replays the published store object from byte zero. Digest
 * implementation state is never persisted. The same caller-owned bounded byte
 * workspace used by acquisition may be reused once acquisition is inactive.
 */
template<typename TCapacityProfile>
class ArtifactVerificationSession final {
    static_assert(TCapacityProfile::IsValid, "ArtifactVerificationSession requires a valid OTA capacity profile");

    IReadableArtifactStore& store_;
    Security::IStreamingDigestVerifier& digest_;
    ArtifactTransferWorkspace<TCapacityProfile>& workspace_;

    ArtifactVerificationPhase phase_{ArtifactVerificationPhase::Idle};
    ArtifactIdentifier artifact_{};
    std::uint64_t expectedLength_{0U};
    Security::DigestAlgorithmIdentifier algorithm_{};
    Security::ByteView expectedDigest_{};
    ExactLengthReadTracker tracker_{0U};
    bool active_{false};
    bool storeOpen_{false};

    void CloseStore() noexcept {
        if (!storeOpen_) return;
        store_.CloseRead();
        storeOpen_ = false;
    }

    Result Fail(Result failure) noexcept {
        CloseStore();
        active_ = false;
        phase_ = ArtifactVerificationPhase::Failed;
        return failure;
    }

public:
    ArtifactVerificationSession(
        IReadableArtifactStore& store,
        Security::IStreamingDigestVerifier& digest,
        ArtifactTransferWorkspace<TCapacityProfile>& workspace) noexcept
        : store_(store), digest_(digest), workspace_(workspace) {}

    ArtifactVerificationPhase Phase() const noexcept { return phase_; }
    bool IsActive() const noexcept { return active_; }
    bool IsComplete() const noexcept { return phase_ == ArtifactVerificationPhase::Complete; }
    ArtifactIdentifier Artifact() const noexcept { return artifact_; }
    std::uint64_t VerifiedBytes() const noexcept { return tracker_.Accepted(); }

    Result Begin(const ManifestArtifact<TCapacityProfile>& manifestArtifact) noexcept {
        if (active_) {
            return ArtifactVerificationDetail::Failure(
                OutcomeClass::Unavailable, ArtifactVerificationReason::Busy);
        }
        const ArtifactIdentifier identifier{manifestArtifact.Identifier};
        const Security::DigestAlgorithmIdentifier algorithm{manifestArtifact.DigestAlgorithm};
        if (!identifier || manifestArtifact.ExpectedLength == 0U || !algorithm || manifestArtifact.Digest.empty()) {
            return ArtifactVerificationDetail::Failure(
                OutcomeClass::Invalid, ArtifactVerificationReason::InvalidRequest);
        }
        if (!digest_.Supports(algorithm)) {
            return ArtifactVerificationDetail::Failure(
                OutcomeClass::Unsupported, ArtifactVerificationReason::UnsupportedDigestAlgorithm);
        }
        const auto digestSize = digest_.DigestSize(algorithm);
        if (digestSize == 0U || digestSize != manifestArtifact.Digest.size()) {
            return ArtifactVerificationDetail::Failure(
                OutcomeClass::Invalid, ArtifactVerificationReason::DigestSizeMismatch);
        }

        artifact_ = identifier;
        expectedLength_ = manifestArtifact.ExpectedLength;
        algorithm_ = algorithm;
        expectedDigest_ = {manifestArtifact.Digest.data(), manifestArtifact.Digest.size()};
        tracker_ = ExactLengthReadTracker{expectedLength_};
        active_ = true;
        storeOpen_ = false;
        phase_ = ArtifactVerificationPhase::OpeningStore;
        return ArtifactVerificationDetail::Pending();
    }

    Result Advance() noexcept {
        if (!active_) {
            return IsComplete() ? Result::Success() : ArtifactVerificationDetail::Failure(
                OutcomeClass::Invalid, ArtifactVerificationReason::InvalidRequest);
        }

        switch (phase_) {
            case ArtifactVerificationPhase::OpeningStore: {
                const auto opened = store_.OpenRead({artifact_, expectedLength_});
                if (opened) {
                    storeOpen_ = true;
                    phase_ = ArtifactVerificationPhase::StartingDigest;
                    return ArtifactVerificationDetail::Pending();
                }
                if (opened.Outcome == OutcomeClass::Pending || opened.Outcome == OutcomeClass::Deferred) return opened;
                return Fail(opened);
            }
            case ArtifactVerificationPhase::StartingDigest: {
                const auto begun = digest_.Begin(algorithm_);
                if (!begun) {
                    return Fail(ArtifactVerificationDetail::SecurityResult(
                        begun, ArtifactVerificationReason::DigestBeginFailed));
                }
                phase_ = ArtifactVerificationPhase::Reading;
                return ArtifactVerificationDetail::Pending();
            }
            case ArtifactVerificationPhase::Reading: {
                const auto read = store_.ReadStored(workspace_.Bytes.data(), workspace_.Bytes.size());
                if (!read.IsValidFor(workspace_.Bytes.size())) {
                    return Fail(ArtifactVerificationDetail::Failure(
                        OutcomeClass::VerificationFailed, ArtifactVerificationReason::InvalidStoreReadResult));
                }
                const auto observed = tracker_.Observe(read, workspace_.Bytes.size());
                if (observed == ExactLengthReadStatus::Pending) return read.Detail;
                if (observed == ExactLengthReadStatus::Failed) return Fail(read.Detail);
                if (observed == ExactLengthReadStatus::InvalidResult) {
                    return Fail(ArtifactVerificationDetail::Failure(
                        OutcomeClass::VerificationFailed, ArtifactVerificationReason::InvalidStoreReadResult));
                }
                if (observed == ExactLengthReadStatus::Truncated) {
                    return Fail(ArtifactVerificationDetail::Failure(
                        OutcomeClass::VerificationFailed, ArtifactVerificationReason::Truncated));
                }
                if (observed == ExactLengthReadStatus::Overrun) {
                    return Fail(ArtifactVerificationDetail::Failure(
                        OutcomeClass::VerificationFailed, ArtifactVerificationReason::Overrun));
                }
                if (read.Status == StreamReadStatus::Data) {
                    const auto updated = digest_.Update({workspace_.Bytes.data(), read.Bytes});
                    if (!updated) {
                        return Fail(ArtifactVerificationDetail::SecurityResult(
                            updated, ArtifactVerificationReason::DigestUpdateFailed));
                    }
                    return ArtifactVerificationDetail::Pending();
                }
                if (observed == ExactLengthReadStatus::Complete) {
                    CloseStore();
                    phase_ = ArtifactVerificationPhase::FinalizingDigest;
                    return ArtifactVerificationDetail::Pending();
                }
                return ArtifactVerificationDetail::Pending();
            }
            case ArtifactVerificationPhase::FinalizingDigest: {
                const auto verified = digest_.VerifyFinal(expectedDigest_);
                if (!verified) {
                    return Fail(ArtifactVerificationDetail::SecurityResult(
                        verified, ArtifactVerificationReason::DigestFinalizeFailed));
                }
                active_ = false;
                phase_ = ArtifactVerificationPhase::Complete;
                return Result::Success();
            }
            case ArtifactVerificationPhase::Idle:
            case ArtifactVerificationPhase::Complete:
            case ArtifactVerificationPhase::Failed:
                break;
        }
        return ArtifactVerificationDetail::Failure(
            OutcomeClass::Invalid, ArtifactVerificationReason::InvalidRequest);
    }

    void Abort() noexcept {
        CloseStore();
        active_ = false;
        if (phase_ != ArtifactVerificationPhase::Complete) phase_ = ArtifactVerificationPhase::Failed;
    }
};

} // namespace ESPressio::OTA
