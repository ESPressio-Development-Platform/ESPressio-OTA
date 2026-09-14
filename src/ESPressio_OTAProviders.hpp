#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTACompatibilityClaim.hpp"
#include "ESPressio_OTAProfile.hpp"
#include "ESPressio_OTATypes.hpp"
#include "ESPressio_Verification.hpp"
#include "ESPressio_DeviceIdentifier.hpp"
#include "ESPressio_BoundedContainers.hpp"

namespace ESPressio::OTA {

enum class StreamReadStatus : std::uint8_t {
    Data,
    Pending,
    End,
    Failed
};

struct StreamReadResult final {
    StreamReadStatus Status{StreamReadStatus::Failed};
    std::size_t Bytes{0U};
    Result Detail{OutcomeClass::Failed, {}};

    constexpr bool IsValidFor(std::size_t capacity) const noexcept {
        if (Status == StreamReadStatus::Data) {
            return Bytes != 0U && Bytes <= capacity && Detail.Outcome == OutcomeClass::Success;
        }
        if (Bytes != 0U) return false;
        if (Status == StreamReadStatus::Pending) return Detail.Outcome == OutcomeClass::Pending;
        if (Status == StreamReadStatus::End) return Detail.Outcome == OutcomeClass::Success;
        return Detail.Outcome != OutcomeClass::Success && Detail.Outcome != OutcomeClass::Pending;
    }

    static constexpr StreamReadResult Data(std::size_t bytes) noexcept {
        return {StreamReadStatus::Data, bytes, Result::Success()};
    }
    static constexpr StreamReadResult Pending() noexcept {
        return {StreamReadStatus::Pending, 0U, {OutcomeClass::Pending, {}}};
    }
    static constexpr StreamReadResult End() noexcept {
        return {StreamReadStatus::End, 0U, Result::Success()};
    }
    static constexpr StreamReadResult Failed(Result result) noexcept {
        return {StreamReadStatus::Failed, 0U, result};
    }
};

enum class ExactLengthReadStatus : std::uint8_t {
    Continue,
    Pending,
    AwaitingEnd,
    Complete,
    Truncated,
    Overrun,
    Failed,
    InvalidResult
};

/** Tracks a known-length pull stream and requires a final End after exactly N bytes. */
class ExactLengthReadTracker final {
    std::uint64_t expected_{0U};
    std::uint64_t accepted_{0U};
public:
    constexpr explicit ExactLengthReadTracker(std::uint64_t expected) noexcept : expected_(expected) {}
    constexpr std::uint64_t Expected() const noexcept { return expected_; }
    constexpr std::uint64_t Accepted() const noexcept { return accepted_; }

    constexpr ExactLengthReadStatus Observe(
        const StreamReadResult& result,
        std::size_t suppliedCapacity) noexcept {
        if (!result.IsValidFor(suppliedCapacity)) return ExactLengthReadStatus::InvalidResult;
        switch (result.Status) {
            case StreamReadStatus::Pending:
                return ExactLengthReadStatus::Pending;
            case StreamReadStatus::Failed:
                return ExactLengthReadStatus::Failed;
            case StreamReadStatus::End:
                return accepted_ == expected_
                    ? ExactLengthReadStatus::Complete
                    : ExactLengthReadStatus::Truncated;
            case StreamReadStatus::Data:
                if (result.Bytes > expected_ - accepted_) return ExactLengthReadStatus::Overrun;
                accepted_ += result.Bytes;
                return accepted_ == expected_
                    ? ExactLengthReadStatus::AwaitingEnd
                    : ExactLengthReadStatus::Continue;
        }
        return ExactLengthReadStatus::InvalidResult;
    }
};

class IManifestSource {
public:
    virtual ~IManifestSource() = default;
    virtual Result Open(ManifestIdentifier manifest) noexcept = 0;
    virtual StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept = 0;
    virtual void Close() noexcept = 0;
};

struct ArtifactSourceOpenRequest final {
    ArtifactIdentifier Identifier{};
    std::uint64_t ExpectedLength{0U};
    std::uint64_t Offset{0U};

    constexpr bool IsValid() const noexcept {
        return bool(Identifier) && ExpectedLength != 0U && Offset <= ExpectedLength;
    }
};

class IArtifactSource {
public:
    virtual ~IArtifactSource() = default;
    virtual Result Open(const ArtifactSourceOpenRequest& request) noexcept = 0;
    virtual StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept = 0;
    virtual void Close() noexcept = 0;
};

enum class StreamWriteStatus : std::uint8_t {
    Accepted,
    Pending,
    Failed
};

struct StreamWriteResult final {
    StreamWriteStatus Status{StreamWriteStatus::Failed};
    std::size_t Bytes{0U};
    Result Detail{OutcomeClass::Failed, {}};

    constexpr bool IsValidFor(std::size_t offeredBytes) const noexcept {
        if (Status == StreamWriteStatus::Accepted) {
            return Bytes != 0U && Bytes <= offeredBytes && Detail.Outcome == OutcomeClass::Success;
        }
        if (Bytes != 0U) return false;
        if (Status == StreamWriteStatus::Pending) return Detail.Outcome == OutcomeClass::Pending;
        return Detail.Outcome != OutcomeClass::Success && Detail.Outcome != OutcomeClass::Pending;
    }

    static constexpr StreamWriteResult Accepted(std::size_t bytes) noexcept {
        return {StreamWriteStatus::Accepted, bytes, Result::Success()};
    }
    static constexpr StreamWriteResult Pending() noexcept {
        return {StreamWriteStatus::Pending, 0U, {OutcomeClass::Pending, {}}};
    }
    static constexpr StreamWriteResult Failed(Result result) noexcept {
        return {StreamWriteStatus::Failed, 0U, result};
    }
};

struct ArtifactStoreOpenRequest final {
    ArtifactIdentifier Identifier{};
    std::uint64_t ExpectedLength{0U};

    constexpr bool IsValid() const noexcept {
        return bool(Identifier) && ExpectedLength != 0U;
    }
};

enum class ArtifactStoreFinalizeStatus : std::uint8_t {
    Stored,
    AlreadyPresent,
    Pending,
    IdentifierCollision,
    Failed
};

struct ArtifactStoreFinalizeResult final {
    ArtifactStoreFinalizeStatus Status{ArtifactStoreFinalizeStatus::Failed};
    Result Detail{OutcomeClass::Failed, {}};
};

class IArtifactStore {
public:
    virtual ~IArtifactStore() = default;
    virtual Result BeginWrite(const ArtifactStoreOpenRequest& request) noexcept = 0;
    virtual StreamWriteResult Write(const std::uint8_t* data, std::size_t size) noexcept = 0;
    virtual ArtifactStoreFinalizeResult Finalize() noexcept = 0;
    virtual void Abort() noexcept = 0;
    virtual Result QueryAvailableBytes(std::uint64_t& availableBytes) const noexcept = 0;
};

template<typename TCapacityProfile>
struct VerifiedArtifactDescriptor final {
    static_assert(TCapacityProfile::IsValid, "VerifiedArtifactDescriptor requires a valid OTA capacity profile");
    ArtifactIdentifier Identifier{};
    std::uint64_t Length{0U};
    Security::DigestAlgorithmIdentifier DigestAlgorithm{};
    Serializable::BoundedBytes<TCapacityProfile::MaximumDigestBytes> Digest{};

    constexpr bool IsValid() const noexcept {
        return bool(Identifier) && Length != 0U && bool(DigestAlgorithm) && !Digest.empty();
    }
};

template<typename TCapacityProfile>
class IVerifiedArtifactReader {
public:
    virtual ~IVerifiedArtifactReader() = default;
    virtual const VerifiedArtifactDescriptor<TCapacityProfile>& Descriptor() const noexcept = 0;
    virtual Result Reset() noexcept = 0;
    virtual StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept = 0;
};

template<typename TCapacityProfile>
struct CatalogQuery final {
    const UpdateTargetProfile<TCapacityProfile>* TargetProfile{nullptr};
    UpdateGenerationId RunningGeneration{};
    ReleaseIdentifier RunningRelease{};
    bool HasReleaseChannel{false};
    ReleaseChannelIdentifier ReleaseChannel{};
    std::uint64_t Subscription{0U};
    std::uint64_t ContextFlags{0U};

    bool IsValid() const noexcept {
        return TargetProfile != nullptr && TargetProfile->IsFrozen() && bool(RunningGeneration);
    }
};

template<typename TCapacityProfile>
struct CatalogCandidate final {
    ReleaseIdentifier Release{};
    ManifestIdentifier Manifest{};
    ReleaseChannelIdentifier ReleaseChannel{};
    SecurityGeneration SecurityGenerationValue{};
    OTAProtocolVersion RequiredOTAProtocol{};
    OTAFeatureFlags RequiredOTAFeatures{0U};
    CompatibilityClaimToken<TCapacityProfile> CompatibilityClaim{};

    constexpr bool IsValid() const noexcept {
        return bool(Release) && bool(Manifest) && bool(RequiredOTAProtocol);
    }
};

enum class CatalogReadStatus : std::uint8_t {
    Candidate,
    Pending,
    End,
    Unavailable,
    Truncated,
    Failed
};

struct CatalogReadResult final {
    CatalogReadStatus Status{CatalogReadStatus::Failed};
    Result Detail{OutcomeClass::Failed, {}};
};

template<typename TCapacityProfile>
class IUpdateCatalog {
public:
    virtual ~IUpdateCatalog() = default;
    virtual Result Begin(const CatalogQuery<TCapacityProfile>& query) noexcept = 0;
    virtual CatalogReadResult Next(CatalogCandidate<TCapacityProfile>& candidate) noexcept = 0;
    virtual void Close() noexcept = 0;
};

enum class DistributionRecipientStatus : std::uint8_t {
    Pending,
    AcceptedForTransfer,
    Deferred,
    Rejected,
    Transferring,
    ArtifactReceived,
    Failed
};

struct DistributionRecipientResult final {
    DistributionRecipientStatus Status{DistributionRecipientStatus::Failed};
    Result Detail{OutcomeClass::Failed, {}};
};

template<typename TCapacityProfile>
struct ArtifactDistributionRequest final {
    ArtifactIdentifier Identifier{};
    std::uint64_t ExpectedLength{0U};
    std::array<System::DeviceIdentifier, TCapacityProfile::MaximumDistributionRecipients> Recipients{};
    std::size_t RecipientCount{0U};

    bool IsValid() const noexcept {
        if (!Identifier || ExpectedLength == 0U || RecipientCount == 0U ||
            RecipientCount > Recipients.size()) return false;
        for (std::size_t i = 0U; i < RecipientCount; ++i) {
            if (!Recipients[i]) return false;
            for (std::size_t j = 0U; j < i; ++j) if (Recipients[i] == Recipients[j]) return false;
        }
        return true;
    }
};

template<typename TCapacityProfile>
class IArtifactDistributor {
public:
    virtual ~IArtifactDistributor() = default;
    virtual Result Begin(
        const ArtifactDistributionRequest<TCapacityProfile>& request,
        IVerifiedArtifactReader<TCapacityProfile>& artifact) noexcept = 0;
    virtual DistributionRecipientResult Poll(std::size_t recipientIndex) noexcept = 0;
    virtual void Close() noexcept = 0;
};

} // namespace ESPressio::OTA
