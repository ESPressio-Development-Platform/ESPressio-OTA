#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTA.hpp"

using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;
using Checkpoint = ArtifactCheckpoint<Capacity>;

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

constexpr Result Failure(OutcomeClass outcome, DiagnosticDomain domain = DiagnosticDomain::OTA) noexcept {
    return {outcome, {domain, 1U, 0, {}, 0U}};
}

class FakeManifestSource final : public IManifestSource {
    std::array<std::uint8_t, 3> bytes_{{1U, 2U, 3U}};
    std::size_t offset_{0U};
    bool open_{false};
public:
    Result Open(ManifestIdentifier manifest) noexcept override {
        if (!manifest) return Failure(OutcomeClass::Invalid, DiagnosticDomain::Source);
        offset_ = 0U;
        open_ = true;
        return Result::Success();
    }
    StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (!open_ || output == nullptr || capacity == 0U) {
            return StreamReadResult::Failed(Failure(OutcomeClass::Invalid, DiagnosticDomain::Source));
        }
        if (offset_ == bytes_.size()) return StreamReadResult::End();
        output[0] = bytes_[offset_++];
        return StreamReadResult::Data(1U);
    }
    void Close() noexcept override { open_ = false; }
};

class FakeArtifactSource final : public IArtifactSource {
    std::array<std::uint8_t, 5> bytes_{{10U, 11U, 12U, 13U, 14U}};
    std::size_t offset_{0U};
    bool open_{false};
    bool pendingOnce_{false};
    bool offsetReads_{false};
public:
    explicit FakeArtifactSource(bool offsetReads) noexcept : offsetReads_(offsetReads) {}
    Result Open(const ArtifactSourceOpenRequest& request) noexcept override {
        if (!request.IsValid() || request.ExpectedLength != bytes_.size()) {
            return Failure(OutcomeClass::Invalid, DiagnosticDomain::Source);
        }
        if (request.Offset != 0U && !offsetReads_) {
            return Failure(OutcomeClass::Unsupported, DiagnosticDomain::Source);
        }
        offset_ = static_cast<std::size_t>(request.Offset);
        open_ = true;
        pendingOnce_ = true;
        return Result::Success();
    }
    StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (!open_ || output == nullptr || capacity == 0U) {
            return StreamReadResult::Failed(Failure(OutcomeClass::Invalid, DiagnosticDomain::Source));
        }
        if (pendingOnce_) {
            pendingOnce_ = false;
            return StreamReadResult::Pending();
        }
        if (offset_ == bytes_.size()) return StreamReadResult::End();
        const std::size_t count = (bytes_.size() - offset_) < capacity ? (bytes_.size() - offset_) : capacity;
        for (std::size_t i = 0U; i < count; ++i) output[i] = bytes_[offset_ + i];
        offset_ += count;
        return StreamReadResult::Data(count);
    }
    void Close() noexcept override { open_ = false; }
};

class FakeArtifactStore final : public IArtifactStore {
    std::array<std::uint8_t, 16> stored_{};
    std::array<std::uint8_t, 16> working_{};
    ArtifactIdentifier storedId_{};
    ArtifactIdentifier workingId_{};
    std::size_t storedSize_{0U};
    std::size_t workingSize_{0U};
    std::size_t expected_{0U};
    bool writing_{false};
public:
    Result BeginWrite(const ArtifactStoreOpenRequest& request) noexcept override {
        if (!request.IsValid() || request.ExpectedLength > working_.size()) {
            return Failure(OutcomeClass::CapacityUnavailable, DiagnosticDomain::Store);
        }
        workingId_ = request.Identifier;
        workingSize_ = 0U;
        expected_ = static_cast<std::size_t>(request.ExpectedLength);
        writing_ = true;
        return Result::Success();
    }
    StreamWriteResult Write(const std::uint8_t* data, std::size_t size) noexcept override {
        if (!writing_ || data == nullptr || size == 0U || size > expected_ - workingSize_) {
            return StreamWriteResult::Failed(Failure(OutcomeClass::Invalid, DiagnosticDomain::Store));
        }
        for (std::size_t i = 0U; i < size; ++i) working_[workingSize_ + i] = data[i];
        workingSize_ += size;
        return StreamWriteResult::Accepted(size);
    }
    ArtifactStoreFinalizeResult Finalize() noexcept override {
        if (!writing_ || workingSize_ != expected_) {
            return {ArtifactStoreFinalizeStatus::Failed, Failure(OutcomeClass::Failed, DiagnosticDomain::Store)};
        }
        writing_ = false;
        if (storedId_) {
            if (storedId_ != workingId_) {
                return {ArtifactStoreFinalizeStatus::Failed, Failure(OutcomeClass::Failed, DiagnosticDomain::Store)};
            }
            if (storedSize_ != workingSize_) {
                return {ArtifactStoreFinalizeStatus::IdentifierCollision,
                        Failure(OutcomeClass::Invalid, DiagnosticDomain::Store)};
            }
            for (std::size_t i = 0U; i < storedSize_; ++i) {
                if (stored_[i] != working_[i]) {
                    return {ArtifactStoreFinalizeStatus::IdentifierCollision,
                            Failure(OutcomeClass::Invalid, DiagnosticDomain::Store)};
                }
            }
            return {ArtifactStoreFinalizeStatus::AlreadyPresent, Result::Success()};
        }
        storedId_ = workingId_;
        storedSize_ = workingSize_;
        for (std::size_t i = 0U; i < storedSize_; ++i) stored_[i] = working_[i];
        return {ArtifactStoreFinalizeStatus::Stored, Result::Success()};
    }
    void Abort() noexcept override { writing_ = false; workingSize_ = 0U; }
    Result QueryAvailableBytes(std::uint64_t& availableBytes) const noexcept override {
        availableBytes = working_.size() - storedSize_;
        return Result::Success();
    }
};

class FakeVerifiedReader final : public IVerifiedArtifactReader<Capacity> {
    VerifiedArtifactDescriptor<Capacity> descriptor_{};
    std::array<std::uint8_t, 4> bytes_{{1U, 2U, 3U, 4U}};
    std::size_t offset_{0U};
public:
    FakeVerifiedReader() {
        descriptor_.Identifier = ArtifactIdentifier{Id(5U)};
        descriptor_.Length = bytes_.size();
        descriptor_.DigestAlgorithm = ESPressio::Security::DigestAlgorithm::SHA256;
        for (std::size_t i = 0U; i < 32U; ++i) (void)descriptor_.Digest.push_back(static_cast<std::uint8_t>(i));
    }
    const VerifiedArtifactDescriptor<Capacity>& Descriptor() const noexcept override { return descriptor_; }
    Result Reset() noexcept override { offset_ = 0U; return Result::Success(); }
    StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (output == nullptr || capacity == 0U) {
            return StreamReadResult::Failed(Failure(OutcomeClass::Invalid, DiagnosticDomain::Source));
        }
        if (offset_ == bytes_.size()) return StreamReadResult::End();
        const std::size_t count = (bytes_.size() - offset_) < capacity ? (bytes_.size() - offset_) : capacity;
        for (std::size_t i = 0U; i < count; ++i) output[i] = bytes_[offset_ + i];
        offset_ += count;
        return StreamReadResult::Data(count);
    }
};

class FakeCatalog final : public IUpdateCatalog<Capacity> {
    bool open_{false};
    bool emitted_{false};
public:
    Result Begin(const CatalogQuery<Capacity>& query) noexcept override {
        if (!query.IsValid()) return Failure(OutcomeClass::Invalid, DiagnosticDomain::Catalog);
        open_ = true;
        emitted_ = false;
        return Result::Success();
    }
    CatalogReadResult Next(CatalogCandidate<Capacity>& candidate) noexcept override {
        if (!open_) return {CatalogReadStatus::Failed, Failure(OutcomeClass::Failed, DiagnosticDomain::Catalog)};
        if (emitted_) return {CatalogReadStatus::End, Result::Success()};
        candidate = {};
        candidate.Release = ReleaseIdentifier{9U};
        candidate.Manifest = ManifestIdentifier{Id(8U)};
        candidate.RequiredOTAProtocol = OTAProtocolV1;
        emitted_ = true;
        return {CatalogReadStatus::Candidate, Result::Success()};
    }
    void Close() noexcept override { open_ = false; }
};

class FakeDistributor final : public IArtifactDistributor<Capacity> {
    std::size_t recipients_{0U};
    bool active_{false};
public:
    Result Begin(
        const ArtifactDistributionRequest<Capacity>& request,
        IVerifiedArtifactReader<Capacity>& artifact) noexcept override {
        if (!request.IsValid() || !artifact.Descriptor().IsValid() ||
            request.Identifier != artifact.Descriptor().Identifier ||
            request.ExpectedLength != artifact.Descriptor().Length) {
            return Failure(OutcomeClass::Invalid, DiagnosticDomain::Distributor);
        }
        recipients_ = request.RecipientCount;
        active_ = true;
        return Result::Success();
    }
    DistributionRecipientResult Poll(std::size_t recipientIndex) noexcept override {
        if (!active_ || recipientIndex >= recipients_) {
            return {DistributionRecipientStatus::Failed,
                    Failure(OutcomeClass::Invalid, DiagnosticDomain::Distributor)};
        }
        return {DistributionRecipientStatus::ArtifactReceived, Result::Success()};
    }
    void Close() noexcept override { active_ = false; }
};

UpdateTargetProfile<Capacity> BuildFrozenProfile() {
    UpdateTargetProfile<Capacity> profile;
    (void)profile.SetSystemIdentity(
        ESPressio::System::ProductTypeIdentifier{1U},
        ESPressio::System::HardwareFamilyIdentifier{2U},
        ESPressio::System::HardwareRevision{3U},
        ESPressio::System::ArchitectureIdentifier{4U},
        ESPressio::System::SoftwareVariantIdentifier{5U});
    (void)profile.SetStorageLayout(
        ESPressio::Platform::OTA::StorageLayoutIdentifier{6U},
        ESPressio::Platform::OTA::StorageLayoutGeneration{1U});
    (void)profile.SetPersistenceSchema(
        ESPressio::Persistence::SchemaIdentifier{7U},
        ESPressio::Persistence::SchemaGeneration{1U});
    (void)profile.SetOTASupport(OTAProtocolV1, 0U);
    std::array<std::uint8_t, 32> fingerprintBytes{};
    fingerprintBytes[0] = 1U;
    (void)profile.SetFingerprintAndFreeze(UpdateTargetProfileFingerprint{fingerprintBytes});
    return profile;
}

constexpr ESPressio::System::DeviceIdentifier Device(std::uint8_t value) noexcept {
    ESPressio::System::DeviceIdentifier::Storage bytes{};
    bytes[15] = value;
    return ESPressio::System::DeviceIdentifier{bytes};
}

static_assert(ESPressio::Serializable::IsBoundedSerializable<Checkpoint>);
static_assert(ESPressio::Serializable::MaximumSerializedSize<Checkpoint, ESPressio::Serializable::DirectBinary>
              <= Capacity::MaximumArtifactCheckpointRecordBytes);

} // namespace

int main() {
    std::array<std::uint8_t, 8> buffer{};

    FakeManifestSource manifestSource;
    if (!manifestSource.Open(ManifestIdentifier{Id(1U)})) return 1;
    std::size_t manifestBytes = 0U;
    for (;;) {
        const auto result = manifestSource.Read(buffer.data(), buffer.size());
        if (!result.IsValidFor(buffer.size())) return 2;
        if (result.Status == StreamReadStatus::End) break;
        if (result.Status != StreamReadStatus::Data) return 3;
        manifestBytes += result.Bytes;
    }
    if (manifestBytes != 3U) return 4;
    manifestSource.Close();

    FakeArtifactSource sequential{false};
    const ArtifactSourceOpenRequest sourceRequest{ArtifactIdentifier{Id(2U)}, 5U, 0U};
    if (!sequential.Open(sourceRequest)) return 5;
    ExactLengthReadTracker tracker{5U};
    auto read = sequential.Read(buffer.data(), 2U);
    if (tracker.Observe(read, 2U) != ExactLengthReadStatus::Pending) return 6;
    do {
        read = sequential.Read(buffer.data(), 2U);
        const auto observed = tracker.Observe(read, 2U);
        if (observed == ExactLengthReadStatus::Complete) break;
        if (observed != ExactLengthReadStatus::Continue && observed != ExactLengthReadStatus::AwaitingEnd) return 7;
    } while (true);
    if (tracker.Accepted() != 5U) return 8;
    sequential.Close();

    const ArtifactSourceOpenRequest unsupportedOffset{ArtifactIdentifier{Id(2U)}, 5U, 2U};
    if (sequential.Open(unsupportedOffset).Outcome != OutcomeClass::Unsupported) return 9;
    FakeArtifactSource offsetSource{true};
    if (!offsetSource.Open(unsupportedOffset)) return 10;
    read = offsetSource.Read(buffer.data(), buffer.size());
    if (read.Status != StreamReadStatus::Pending) return 11;
    read = offsetSource.Read(buffer.data(), buffer.size());
    if (read.Status != StreamReadStatus::Data || read.Bytes != 3U) return 12;

    ExactLengthReadTracker truncated{5U};
    if (truncated.Observe(StreamReadResult::Data(3U), 3U) != ExactLengthReadStatus::Continue) return 13;
    if (truncated.Observe(StreamReadResult::End(), 3U) != ExactLengthReadStatus::Truncated) return 14;
    ExactLengthReadTracker overrun{4U};
    if (overrun.Observe(StreamReadResult::Data(5U), 5U) != ExactLengthReadStatus::Overrun) return 15;

    FakeArtifactStore store;
    const ArtifactStoreOpenRequest storeRequest{ArtifactIdentifier{Id(3U)}, 4U};
    const std::array<std::uint8_t, 4> firstBytes{{1U, 2U, 3U, 4U}};
    if (!store.BeginWrite(storeRequest)) return 16;
    if (!store.Write(firstBytes.data(), firstBytes.size()).IsValidFor(firstBytes.size())) return 17;
    if (store.Finalize().Status != ArtifactStoreFinalizeStatus::Stored) return 18;

    if (!store.BeginWrite(storeRequest)) return 19;
    (void)store.Write(firstBytes.data(), firstBytes.size());
    if (store.Finalize().Status != ArtifactStoreFinalizeStatus::AlreadyPresent) return 20;

    const std::array<std::uint8_t, 4> otherBytes{{1U, 2U, 3U, 5U}};
    if (!store.BeginWrite(storeRequest)) return 21;
    (void)store.Write(otherBytes.data(), otherBytes.size());
    if (store.Finalize().Status != ArtifactStoreFinalizeStatus::IdentifierCollision) return 22;

    std::uint64_t available = 0U;
    if (!store.QueryAvailableBytes(available) || available != 12U) return 23;

    auto profile = BuildFrozenProfile();
    if (!profile.IsFrozen()) return 24;
    CatalogQuery<Capacity> query;
    query.TargetProfile = &profile;
    query.RunningGeneration = UpdateGenerationId{1U};
    FakeCatalog catalog;
    if (!catalog.Begin(query)) return 25;
    CatalogCandidate<Capacity> candidate;
    auto catalogResult = catalog.Next(candidate);
    if (catalogResult.Status != CatalogReadStatus::Candidate || !candidate.IsValid()) return 26;
    catalogResult = catalog.Next(candidate);
    if (catalogResult.Status != CatalogReadStatus::End) return 27;
    catalog.Close();

    FakeVerifiedReader verified;
    if (!verified.Descriptor().IsValid()) return 28;
    ArtifactDistributionRequest<Capacity> distribution;
    distribution.Identifier = verified.Descriptor().Identifier;
    distribution.ExpectedLength = verified.Descriptor().Length;
    distribution.Recipients[0] = Device(1U);
    distribution.Recipients[1] = Device(2U);
    distribution.RecipientCount = 2U;
    if (!distribution.IsValid()) return 29;
    FakeDistributor distributor;
    if (!distributor.Begin(distribution, verified)) return 30;
    if (distributor.Poll(0U).Status != DistributionRecipientStatus::ArtifactReceived) return 31;
    if (distributor.Poll(1U).Status != DistributionRecipientStatus::ArtifactReceived) return 32;
    distributor.Close();
    distribution.Recipients[1] = distribution.Recipients[0];
    if (distribution.IsValid()) return 33;

    Checkpoint checkpoint;
    checkpoint.Transaction = 1U;
    checkpoint.Artifact = Id(4U);
    checkpoint.ExpectedLength = 100U;
    checkpoint.AcceptedPrefixLength = 50U;
    checkpoint.Revision = 1U;
    checkpoint.PrefixDigestAlgorithm = ESPressio::Security::DigestAlgorithm::SHA256.Value();
    for (std::size_t i = 0U; i < 32U; ++i) (void)checkpoint.PrefixDigest.push_back(static_cast<std::uint8_t>(i + 1U));
    if (!ArtifactCheckpointValid(checkpoint)) return 34;

    std::array<std::uint8_t, Capacity::MaximumArtifactCheckpointRecordBytes> checkpointBytes{};
    const auto encoded = ESPressio::Serializable::SerializeDirectBinary(
        checkpoint, checkpointBytes.data(), checkpointBytes.size());
    if (!encoded) return 35;
    Checkpoint restored;
    const auto decoded = ESPressio::Serializable::DeserializeBoundedDirectBinaryIntoScratch(
        checkpointBytes.data(), encoded.Bytes, restored);
    if (!decoded || !ArtifactCheckpointValid(restored) || restored.AcceptedPrefixLength != 50U) return 36;

    restored.AcceptedPrefixLength = restored.ExpectedLength + 1U;
    if (ArtifactCheckpointValid(restored)) return 37;

    Checkpoint emptyPrefix;
    emptyPrefix.Transaction = 1U;
    emptyPrefix.Artifact = Id(4U);
    emptyPrefix.ExpectedLength = 100U;
    emptyPrefix.Revision = 1U;
    if (!ArtifactCheckpointValid(emptyPrefix)) return 38;
    emptyPrefix.PrefixDigestAlgorithm = ESPressio::Security::DigestAlgorithm::SHA256.Value();
    if (ArtifactCheckpointValid(emptyPrefix)) return 39;

    return 0;
}
