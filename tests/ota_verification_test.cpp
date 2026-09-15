#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTA.hpp"

using namespace ESPressio;
using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

constexpr Result Failure(OutcomeClass outcome, DiagnosticDomain domain, std::uint32_t reason) noexcept {
    return {outcome, {domain, reason, 0, {}, 0U}};
}

class ReadableStore final : public IReadableArtifactStore {
    std::array<std::uint8_t, 64> stored_{};
    std::array<std::uint8_t, 64> working_{};
    ArtifactIdentifier storedId_{};
    ArtifactIdentifier workingId_{};
    std::size_t storedSize_{0U};
    std::size_t workingSize_{0U};
    std::size_t expectedWrite_{0U};
    std::size_t readOffset_{0U};
    std::size_t readLimit_{3U};
    bool writing_{false};
    bool reading_{false};
    bool pendingReadOnce_{false};
    bool pendingReturned_{false};
    bool truncate_{false};
    bool overrun_{false};
public:
    void ConfigureRead(std::size_t limit, bool pendingOnce, bool truncate, bool overrun) noexcept {
        readLimit_ = limit;
        pendingReadOnce_ = pendingOnce;
        truncate_ = truncate;
        overrun_ = overrun;
    }

    Result BeginWrite(const ArtifactStoreOpenRequest& request) noexcept override {
        if (!request.IsValid() || request.ExpectedLength > working_.size()) {
            return Failure(OutcomeClass::CapacityUnavailable, DiagnosticDomain::Store, 1U);
        }
        workingId_ = request.Identifier;
        expectedWrite_ = static_cast<std::size_t>(request.ExpectedLength);
        workingSize_ = 0U;
        writing_ = true;
        return Result::Success();
    }

    StreamWriteResult Write(const std::uint8_t* data, std::size_t size) noexcept override {
        if (!writing_ || data == nullptr || size == 0U || size > expectedWrite_ - workingSize_) {
            return StreamWriteResult::Failed(Failure(OutcomeClass::Invalid, DiagnosticDomain::Store, 2U));
        }
        for (std::size_t i = 0U; i < size; ++i) working_[workingSize_ + i] = data[i];
        workingSize_ += size;
        return StreamWriteResult::Accepted(size);
    }

    ArtifactStoreFinalizeResult Finalize() noexcept override {
        if (!writing_ || workingSize_ != expectedWrite_) {
            return {ArtifactStoreFinalizeStatus::Failed,
                    Failure(OutcomeClass::Failed, DiagnosticDomain::Store, 3U)};
        }
        writing_ = false;
        storedId_ = workingId_;
        storedSize_ = workingSize_;
        for (std::size_t i = 0U; i < storedSize_; ++i) stored_[i] = working_[i];
        return {ArtifactStoreFinalizeStatus::Stored, Result::Success()};
    }

    void Abort() noexcept override {
        writing_ = false;
        workingSize_ = 0U;
    }

    Result QueryAvailableBytes(std::uint64_t& availableBytes) const noexcept override {
        availableBytes = stored_.size() - storedSize_;
        return Result::Success();
    }

    Result OpenRead(const ArtifactStoreReadRequest& request) noexcept override {
        if (!request.IsValid() || request.Identifier != storedId_) {
            return Failure(OutcomeClass::Unavailable, DiagnosticDomain::Store, 4U);
        }
        if (request.ExpectedLength != storedSize_) {
            return Failure(OutcomeClass::VerificationFailed, DiagnosticDomain::Store, 5U);
        }
        readOffset_ = 0U;
        reading_ = true;
        pendingReturned_ = false;
        return Result::Success();
    }

    StreamReadResult ReadStored(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (!reading_ || output == nullptr || capacity == 0U) {
            return StreamReadResult::Failed(Failure(OutcomeClass::Invalid, DiagnosticDomain::Store, 6U));
        }
        if (pendingReadOnce_ && !pendingReturned_) {
            pendingReturned_ = true;
            return StreamReadResult::Pending();
        }
        const std::size_t visibleSize = truncate_ && storedSize_ != 0U ? storedSize_ - 1U : storedSize_;
        if (readOffset_ == visibleSize) {
            if (overrun_) {
                output[0] = 0xEEU;
                ++readOffset_;
                return StreamReadResult::Data(1U);
            }
            return StreamReadResult::End();
        }
        if (readOffset_ > visibleSize) return StreamReadResult::End();
        const auto count = std::min({readLimit_, visibleSize - readOffset_, capacity});
        for (std::size_t i = 0U; i < count; ++i) output[i] = stored_[readOffset_ + i];
        readOffset_ += count;
        return StreamReadResult::Data(count);
    }

    void CloseRead() noexcept override { reading_ = false; }
};

class SumDigestVerifier final : public Security::IStreamingDigestVerifier {
    std::uint8_t sum_{0U};
    bool active_{false};
    bool support_{true};
public:
    void SetSupported(bool supported) noexcept { support_ = supported; }

    bool Supports(Security::DigestAlgorithmIdentifier algorithm) const noexcept override {
        return support_ && algorithm == Security::DigestAlgorithm::SHA256;
    }

    std::size_t DigestSize(Security::DigestAlgorithmIdentifier algorithm) const noexcept override {
        return Supports(algorithm) ? 32U : 0U;
    }

    Security::VerificationResult Begin(Security::DigestAlgorithmIdentifier algorithm) noexcept override {
        if (!Supports(algorithm)) return {Security::VerificationStatus::UnsupportedAlgorithm, 0};
        sum_ = 0U;
        active_ = true;
        return Security::VerificationResult::Ok();
    }

    Security::VerificationResult Update(Security::ByteView bytes) noexcept override {
        if (!active_ || !bytes.IsValid()) return {Security::VerificationStatus::InvalidArgument, 0};
        for (std::size_t i = 0U; i < bytes.Size; ++i) sum_ = static_cast<std::uint8_t>(sum_ + bytes.Data[i]);
        return Security::VerificationResult::Ok();
    }

    Security::VerificationResult VerifyFinal(Security::ByteView expectedDigest) noexcept override {
        if (!active_ || !expectedDigest.IsValid() || expectedDigest.Size != 32U) {
            return {Security::VerificationStatus::InvalidArgument, 0};
        }
        active_ = false;
        if (expectedDigest.Data[0] != sum_) return {Security::VerificationStatus::DigestMismatch, 0};
        for (std::size_t i = 1U; i < expectedDigest.Size; ++i) {
            if (expectedDigest.Data[i] != static_cast<std::uint8_t>(sum_ ^ static_cast<std::uint8_t>(i))) {
                return {Security::VerificationStatus::DigestMismatch, 0};
            }
        }
        return Security::VerificationResult::Ok();
    }
};

ManifestArtifact<Capacity> Artifact(std::uint8_t id, const std::uint8_t* bytes, std::size_t size) {
    ManifestArtifact<Capacity> artifact;
    artifact.Identifier = Id(id);
    artifact.ExpectedLength = size;
    artifact.DigestAlgorithm = Security::DigestAlgorithm::SHA256.Value();
    std::uint8_t sum = 0U;
    for (std::size_t i = 0U; i < size; ++i) sum = static_cast<std::uint8_t>(sum + bytes[i]);
    (void)artifact.Digest.push_back(sum);
    for (std::size_t i = 1U; i < 32U; ++i) {
        (void)artifact.Digest.push_back(static_cast<std::uint8_t>(sum ^ static_cast<std::uint8_t>(i)));
    }
    return artifact;
}

bool StoreBytes(ReadableStore& store, ArtifactIdentifier id, const std::uint8_t* data, std::size_t size) {
    if (!store.BeginWrite({id, size})) return false;
    const auto written = store.Write(data, size);
    if (!written.IsValidFor(size) || written.Status != StreamWriteStatus::Accepted || written.Bytes != size) return false;
    return store.Finalize().Status == ArtifactStoreFinalizeStatus::Stored;
}

Result RunToTerminal(ArtifactVerificationSession<Capacity>& session) {
    for (std::size_t i = 0U; i < 64U; ++i) {
        const auto result = session.Advance();
        if (session.IsComplete() || session.Phase() == ArtifactVerificationPhase::Failed) return result;
        if (result.Outcome != OutcomeClass::Pending && result.Outcome != OutcomeClass::Deferred) return result;
    }
    return Failure(OutcomeClass::Failed, DiagnosticDomain::OTA, 99U);
}

} // namespace

int main() {
    const std::array<std::uint8_t, 7> bytes{{3U, 1U, 4U, 1U, 5U, 9U, 2U}};

    {
        ReadableStore store;
        if (!StoreBytes(store, ArtifactIdentifier{Id(1U)}, bytes.data(), bytes.size())) return 1;
        store.ConfigureRead(2U, true, false, false);
        SumDigestVerifier digest;
        ArtifactTransferWorkspace<Capacity> workspace;
        ArtifactVerificationSession<Capacity> session{store, digest, workspace};
        const auto artifact = Artifact(1U, bytes.data(), bytes.size());
        if (session.Begin(artifact).Outcome != OutcomeClass::Pending) return 2;
        const auto result = RunToTerminal(session);
        if (!result || !session.IsComplete() || session.VerifiedBytes() != bytes.size()) return 3;
    }

    {
        ReadableStore store;
        if (!StoreBytes(store, ArtifactIdentifier{Id(2U)}, bytes.data(), bytes.size())) return 4;
        SumDigestVerifier digest;
        ArtifactTransferWorkspace<Capacity> workspace;
        ArtifactVerificationSession<Capacity> session{store, digest, workspace};
        auto artifact = Artifact(2U, bytes.data(), bytes.size());
        artifact.Digest[0] ^= 0xFFU;
        if (session.Begin(artifact).Outcome != OutcomeClass::Pending) return 5;
        const auto result = RunToTerminal(session);
        if (result.Outcome != OutcomeClass::VerificationFailed ||
            result.Detail.Reason != static_cast<std::uint32_t>(ArtifactVerificationReason::DigestMismatch)) return 6;
    }

    {
        ReadableStore store;
        if (!StoreBytes(store, ArtifactIdentifier{Id(3U)}, bytes.data(), bytes.size())) return 7;
        store.ConfigureRead(3U, false, true, false);
        SumDigestVerifier digest;
        ArtifactTransferWorkspace<Capacity> workspace;
        ArtifactVerificationSession<Capacity> session{store, digest, workspace};
        const auto artifact = Artifact(3U, bytes.data(), bytes.size());
        if (session.Begin(artifact).Outcome != OutcomeClass::Pending) return 8;
        const auto result = RunToTerminal(session);
        if (result.Outcome != OutcomeClass::VerificationFailed ||
            result.Detail.Reason != static_cast<std::uint32_t>(ArtifactVerificationReason::Truncated)) return 9;
    }

    {
        ReadableStore store;
        if (!StoreBytes(store, ArtifactIdentifier{Id(4U)}, bytes.data(), bytes.size())) return 10;
        store.ConfigureRead(bytes.size(), false, false, true);
        SumDigestVerifier digest;
        ArtifactTransferWorkspace<Capacity> workspace;
        ArtifactVerificationSession<Capacity> session{store, digest, workspace};
        const auto artifact = Artifact(4U, bytes.data(), bytes.size());
        if (session.Begin(artifact).Outcome != OutcomeClass::Pending) return 11;
        const auto result = RunToTerminal(session);
        if (result.Outcome != OutcomeClass::VerificationFailed ||
            result.Detail.Reason != static_cast<std::uint32_t>(ArtifactVerificationReason::Overrun)) return 12;
    }

    {
        ReadableStore store;
        if (!StoreBytes(store, ArtifactIdentifier{Id(5U)}, bytes.data(), bytes.size())) return 13;
        SumDigestVerifier digest;
        digest.SetSupported(false);
        ArtifactTransferWorkspace<Capacity> workspace;
        ArtifactVerificationSession<Capacity> session{store, digest, workspace};
        const auto artifact = Artifact(5U, bytes.data(), bytes.size());
        const auto result = session.Begin(artifact);
        if (result.Outcome != OutcomeClass::Unsupported ||
            result.Detail.Reason != static_cast<std::uint32_t>(ArtifactVerificationReason::UnsupportedDigestAlgorithm)) return 14;
    }

    return 0;
}
