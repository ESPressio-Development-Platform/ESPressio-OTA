#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAAcquisition.hpp"

using namespace ESPressio;
using namespace ESPressio::OTA;

namespace {
using Capacity = ConstrainedV1CapacityProfile;

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

class FakeAtomicRecordStore final : public Persistence::IAtomicRecordStore {
    struct Entry final {
        Persistence::AtomicRecordKey Key{};
        bool Used{false};
        std::array<std::uint8_t, Capacity::MaximumOTAControlRecordBytes> Bytes{};
        std::size_t Size{0U};
    };
    std::array<Entry, 8> entries_{};

    Entry* Find(const Persistence::AtomicRecordKey& key) noexcept {
        for (auto& entry : entries_) if (entry.Used && entry.Key == key) return &entry;
        return nullptr;
    }
public:
    Persistence::AtomicRecordCapabilities Capabilities() const noexcept override {
        return {true, true, Capacity::MaximumOTAControlRecordBytes, entries_.size()};
    }
    Persistence::AtomicRecordStatus Recover() noexcept override { return Persistence::AtomicRecordStatus::Success; }
    Persistence::AtomicRecordStatus Read(const Persistence::AtomicRecordKey& key, std::uint8_t* output,
                                         std::size_t capacity, std::size_t& bytesRead) noexcept override {
        bytesRead = 0U;
        auto* entry = Find(key);
        if (entry == nullptr) return Persistence::AtomicRecordStatus::NotFound;
        if (output == nullptr || capacity < entry->Size) return Persistence::AtomicRecordStatus::BufferTooSmall;
        for (std::size_t i = 0U; i < entry->Size; ++i) output[i] = entry->Bytes[i];
        bytesRead = entry->Size;
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus ReplaceAtomically(const Persistence::AtomicRecordKey& key,
                                                       const std::uint8_t* data, std::size_t size) noexcept override {
        if (!key || data == nullptr || size > Capacity::MaximumOTAControlRecordBytes) {
            return Persistence::AtomicRecordStatus::InvalidArgument;
        }
        auto* entry = Find(key);
        if (entry == nullptr) {
            for (auto& candidate : entries_) {
                if (!candidate.Used) {
                    candidate.Used = true;
                    candidate.Key = key;
                    entry = &candidate;
                    break;
                }
            }
        }
        if (entry == nullptr) return Persistence::AtomicRecordStatus::NoSpace;
        for (std::size_t i = 0U; i < size; ++i) entry->Bytes[i] = data[i];
        entry->Size = size;
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus RemoveAfterCommit(const Persistence::AtomicRecordKey& key) noexcept override {
        auto* entry = Find(key);
        if (entry != nullptr) { entry->Used = false; entry->Size = 0U; }
        return Persistence::AtomicRecordStatus::Success;
    }
};

class OffsetSource final : public IArtifactSource {
public:
    std::array<std::uint8_t, 32> Data{};
    std::size_t Size{0U};
    std::size_t Position{0U};
    std::size_t Chunk{2U};
    std::size_t FailAt{32U};
    bool FailedAlready{false};
    std::uint64_t LastOpenOffset{99U};
    std::size_t OpenCalls{0U};
    std::size_t CloseCalls{0U};

    Result Open(const ArtifactSourceOpenRequest& request) noexcept override {
        ++OpenCalls;
        LastOpenOffset = request.Offset;
        if (!request.IsValid() || request.Offset > Size) {
            return {OutcomeClass::Invalid, {DiagnosticDomain::Source, 1U}};
        }
        Position = static_cast<std::size_t>(request.Offset);
        return Result::Success();
    }

    StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (!FailedAlready && Position >= FailAt) {
            FailedAlready = true;
            return StreamReadResult::Failed(
                {OutcomeClass::Unavailable, {DiagnosticDomain::Source, 2U}});
        }
        if (Position == Size) return StreamReadResult::End();
        if (output == nullptr || capacity == 0U) {
            return StreamReadResult::Failed(
                {OutcomeClass::Invalid, {DiagnosticDomain::Source, 3U}});
        }
        const auto count = std::min({Chunk, capacity, Size - Position});
        for (std::size_t i = 0U; i < count; ++i) output[i] = Data[Position + i];
        Position += count;
        return StreamReadResult::Data(count);
    }

    void Close() noexcept override { ++CloseCalls; }
};

class PartialStore final : public IArtifactStore, public IPartialArtifactStore {
public:
    std::array<std::uint8_t, 32> Bytes{};
    ArtifactIdentifier Identifier{};
    std::uint64_t Expected{0U};
    std::size_t PartialSize{0U};
    std::size_t StableSize{0U};
    std::size_t ReplayPosition{0U};
    std::size_t ReplayLimit{0U};
    std::size_t WriteLimit{2U};
    bool HasPartial{false};
    bool WriterOpen{false};
    bool ReplayOpen{false};
    bool Published{false};
    std::size_t BeginOffsetsCount{0U};
    std::array<std::uint64_t, 8> BeginOffsets{};

    bool Matches(const ArtifactStoreOpenRequest& request) const noexcept {
        return request.IsValid() && request.Identifier == Identifier && request.ExpectedLength == Expected;
    }

    Result BeginWrite(const ArtifactStoreOpenRequest& request) noexcept override {
        return BeginPartialWrite(request, 0U);
    }
    StreamWriteResult Write(const std::uint8_t* data, std::size_t size) noexcept override {
        return WritePartial(data, size);
    }
    ArtifactStoreFinalizeResult Finalize() noexcept override { return FinalizePartial(); }
    void Abort() noexcept override {
        WriterOpen = false;
        if (!Published) { HasPartial = false; PartialSize = 0U; StableSize = 0U; }
    }
    Result QueryAvailableBytes(std::uint64_t& availableBytes) const noexcept override {
        availableBytes = Bytes.size() - PartialSize;
        return Result::Success();
    }

    Result InspectPartial(const ArtifactStoreOpenRequest& request, PartialArtifactInfo& info) noexcept override {
        if (!request.IsValid()) return {OutcomeClass::Invalid, {DiagnosticDomain::Store, 10U}};
        if (!HasPartial || !Matches(request)) {
            info = {};
            return Result::Success();
        }
        info.Present = true;
        info.Replayable = true;
        info.RetainedPrefixLength = PartialSize;
        return Result::Success();
    }

    Result BeginPartialWrite(const ArtifactStoreOpenRequest& request,
                             std::uint64_t acceptedPrefixLength) noexcept override {
        if (!request.IsValid() || acceptedPrefixLength > request.ExpectedLength ||
            acceptedPrefixLength > Bytes.size()) {
            return {OutcomeClass::Invalid, {DiagnosticDomain::Store, 11U}};
        }
        if (acceptedPrefixLength != 0U && (!HasPartial || !Matches(request) || acceptedPrefixLength > PartialSize)) {
            return {OutcomeClass::Unavailable, {DiagnosticDomain::Store, 12U}};
        }
        if (acceptedPrefixLength == 0U) {
            Identifier = request.Identifier;
            Expected = request.ExpectedLength;
            PartialSize = 0U;
            StableSize = 0U;
            HasPartial = true;
            Published = false;
        } else {
            PartialSize = static_cast<std::size_t>(acceptedPrefixLength);
            if (StableSize > PartialSize) StableSize = PartialSize;
        }
        if (BeginOffsetsCount < BeginOffsets.size()) BeginOffsets[BeginOffsetsCount++] = acceptedPrefixLength;
        WriterOpen = true;
        return Result::Success();
    }

    StreamWriteResult WritePartial(const std::uint8_t* data, std::size_t size) noexcept override {
        if (!WriterOpen || data == nullptr || size == 0U || PartialSize >= Bytes.size()) {
            return StreamWriteResult::Failed(
                {OutcomeClass::Failed, {DiagnosticDomain::Store, 13U}});
        }
        const auto count = std::min({WriteLimit, size, Bytes.size() - PartialSize});
        for (std::size_t i = 0U; i < count; ++i) Bytes[PartialSize + i] = data[i];
        PartialSize += count;
        return StreamWriteResult::Accepted(count);
    }

    Result StabilizePartial(std::uint64_t retainedPrefixLength) noexcept override {
        if (!WriterOpen || retainedPrefixLength > PartialSize) {
            return {OutcomeClass::Failed, {DiagnosticDomain::Store, 14U}};
        }
        StableSize = static_cast<std::size_t>(retainedPrefixLength);
        return Result::Success();
    }

    Result SuspendPartialWrite() noexcept override {
        WriterOpen = false;
        return Result::Success();
    }

    Result BeginPartialReplay(const ArtifactStoreOpenRequest& request,
                              std::uint64_t acceptedPrefixLength) noexcept override {
        if (!HasPartial || !Matches(request) || acceptedPrefixLength > PartialSize) {
            return {OutcomeClass::Unavailable, {DiagnosticDomain::Store, 15U}};
        }
        ReplayPosition = 0U;
        ReplayLimit = static_cast<std::size_t>(acceptedPrefixLength);
        ReplayOpen = true;
        return Result::Success();
    }

    StreamReadResult ReplayPartial(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (!ReplayOpen || output == nullptr || capacity == 0U) {
            return StreamReadResult::Failed(
                {OutcomeClass::Failed, {DiagnosticDomain::Store, 16U}});
        }
        if (ReplayPosition == ReplayLimit) return StreamReadResult::End();
        const auto count = std::min<std::size_t>(2U, std::min(capacity, ReplayLimit - ReplayPosition));
        for (std::size_t i = 0U; i < count; ++i) output[i] = Bytes[ReplayPosition + i];
        ReplayPosition += count;
        return StreamReadResult::Data(count);
    }

    void ClosePartialReplay() noexcept override { ReplayOpen = false; }

    ArtifactStoreFinalizeResult FinalizePartial() noexcept override {
        if (!WriterOpen || PartialSize != Expected) {
            return {ArtifactStoreFinalizeStatus::Failed,
                    {OutcomeClass::Failed, {DiagnosticDomain::Store, 17U}}};
        }
        WriterOpen = false;
        HasPartial = false;
        Published = true;
        return {ArtifactStoreFinalizeStatus::Stored, Result::Success()};
    }

    Result DiscardPartial(const ArtifactStoreOpenRequest& request) noexcept override {
        WriterOpen = false;
        ReplayOpen = false;
        if (HasPartial && Matches(request) && !Published) {
            HasPartial = false;
            PartialSize = 0U;
            StableSize = 0U;
        }
        return Result::Success();
    }

    void SeedPartial(const ArtifactStoreOpenRequest& request,
                     const std::uint8_t* data,
                     std::size_t retained,
                     std::size_t stable) noexcept {
        Identifier = request.Identifier;
        Expected = request.ExpectedLength;
        PartialSize = retained;
        StableSize = stable;
        HasPartial = true;
        Published = false;
        for (std::size_t i = 0U; i < retained; ++i) Bytes[i] = data[i];
    }
};

ManifestArtifact<Capacity> Artifact(std::uint8_t id, std::uint64_t length) {
    ManifestArtifact<Capacity> artifact;
    artifact.Identifier = Id(id);
    artifact.ExpectedLength = length;
    artifact.DigestAlgorithm = Security::DigestAlgorithm::SHA256.Value();
    for (std::size_t i = 0U; i < 32U; ++i) (void)artifact.Digest.push_back(static_cast<std::uint8_t>(i + 1U));
    return artifact;
}

bool AdvanceUntilBoundary(ArtifactAcquisitionSession<Capacity>& session, Result& result,
                          std::size_t maximum = 256U) {
    for (std::size_t i = 0U; i < maximum; ++i) {
        result = session.Advance();
        if (session.IsComplete() || session.Phase() == ArtifactAcquisitionPhase::SourceFailed ||
            session.Phase() == ArtifactAcquisitionPhase::Failed) return true;
        if (result.Outcome != OutcomeClass::Pending && result.Outcome != OutcomeClass::Deferred) return false;
    }
    return false;
}

void Fill(OffsetSource& source, std::size_t size, std::uint8_t base) {
    source.Size = size;
    for (std::size_t i = 0U; i < size; ++i) source.Data[i] = static_cast<std::uint8_t>(base + i);
}

bool SameBytes(const PartialStore& store, const OffsetSource& source, std::size_t size) {
    for (std::size_t i = 0U; i < size; ++i) if (store.Bytes[i] != source.Data[i]) return false;
    return true;
}

ArtifactCheckpoint<Capacity> Checkpoint(std::uint64_t transaction, std::uint8_t id,
                                        std::uint64_t expected, std::uint64_t accepted,
                                        std::uint32_t revision) {
    ArtifactCheckpoint<Capacity> checkpoint;
    checkpoint.Transaction = transaction;
    checkpoint.Artifact = Id(id);
    checkpoint.ExpectedLength = expected;
    checkpoint.AcceptedPrefixLength = accepted;
    checkpoint.Revision = revision;
    return checkpoint;
}

} // namespace

int main() {
    // Resume from a durable checkpoint using a privately retained/replayable
    // prefix. The selected Source must be opened at exactly N.
    {
        FakeAtomicRecordStore persistence;
        ArtifactCheckpointStore<Capacity> checkpoints{persistence};
        PartialStore store;
        ArtifactTransferWorkspace<Capacity> workspace;
        OffsetSource source;
        Fill(source, 8U, 0x20U);
        const auto artifact = Artifact(1U, 8U);
        const ArtifactStoreOpenRequest request{ArtifactIdentifier{artifact.Identifier}, artifact.ExpectedLength};
        store.SeedPartial(request, source.Data.data(), 4U, 4U);
        if (checkpoints.Save(0U, Checkpoint(9U, 1U, 8U, 4U, 3U)) != OTADurableStatus::Success) return 1;

        ArtifactAcquisitionSession<Capacity> session{
            source, true, store, store, checkpoints, workspace};
        if (session.Begin(UpdateTransactionId{9U}, artifact).Outcome != OutcomeClass::Pending) return 2;
        Result result;
        if (!AdvanceUntilBoundary(session, result) || !session.IsComplete() || !result) return 3;
        if (source.LastOpenOffset != 4U || !store.Published || !SameBytes(store, source, 8U)) return 4;
        if (store.BeginOffsetsCount == 0U || store.BeginOffsets[0] != 4U) return 5;
        ArtifactCheckpoint<Capacity> removed;
        if (checkpoints.Load(0U, removed) != OTADurableStatus::NotFound) return 6;
    }

    // The physical store may be ahead of durable checkpoint truth. Recovery
    // replays only [0,N), then BeginPartialWrite(N) discards the uncheckpointed
    // suffix and downloads it again. Checkpoint never leads retained bytes.
    {
        FakeAtomicRecordStore persistence;
        ArtifactCheckpointStore<Capacity> checkpoints{persistence};
        PartialStore store;
        ArtifactTransferWorkspace<Capacity> workspace;
        OffsetSource source;
        Fill(source, 8U, 0x40U);
        const auto artifact = Artifact(2U, 8U);
        const ArtifactStoreOpenRequest request{ArtifactIdentifier{artifact.Identifier}, artifact.ExpectedLength};
        store.SeedPartial(request, source.Data.data(), 6U, 6U);
        // Make the uncheckpointed suffix visibly wrong; successful recovery must
        // overwrite bytes [4,6) from the Source after truncating to checkpoint N.
        store.Bytes[4] = 0xEEU;
        store.Bytes[5] = 0xEFU;
        if (checkpoints.Save(0U, Checkpoint(10U, 2U, 8U, 4U, 7U)) != OTADurableStatus::Success) return 10;

        ArtifactAcquisitionSession<Capacity> session{
            source, true, store, store, checkpoints, workspace};
        if (session.Begin(UpdateTransactionId{10U}, artifact).Outcome != OutcomeClass::Pending) return 11;
        Result result;
        if (!AdvanceUntilBoundary(session, result) || !session.IsComplete()) return 12;
        if (source.LastOpenOffset != 4U || store.BeginOffsets[0] != 4U || !SameBytes(store, source, 8U)) return 13;
    }

    // Source A supplies a checkpointed prefix and then becomes unavailable.
    // Explicit caller selection of Source B resumes the same Artifact at N;
    // Source declaration order is never consulted.
    {
        FakeAtomicRecordStore persistence;
        ArtifactCheckpointStore<Capacity> checkpoints{persistence};
        PartialStore store;
        ArtifactTransferWorkspace<Capacity> workspace;
        OffsetSource sourceA;
        OffsetSource sourceB;
        Fill(sourceA, 10U, 0x60U);
        Fill(sourceB, 10U, 0x60U);
        sourceA.FailAt = 4U;
        sourceA.Chunk = 2U;
        sourceB.Chunk = 2U;
        const auto artifact = Artifact(3U, 10U);

        ArtifactAcquisitionSession<Capacity> session{
            sourceA, true, store, store, checkpoints, workspace};
        if (session.Begin(UpdateTransactionId{11U}, artifact).Outcome != OutcomeClass::Pending) return 20;
        Result result;
        if (!AdvanceUntilBoundary(session, result) || !session.AwaitingSourceRetry()) return 21;
        if (result.Outcome != OutcomeClass::Unavailable || session.CheckpointedBytes() != 4U ||
            session.StoredBytes() != 4U) return 22;
        ArtifactCheckpoint<Capacity> retained;
        if (checkpoints.Load(0U, retained) != OTADurableStatus::Success || retained.AcceptedPrefixLength != 4U) return 23;

        if (session.RetryWithSource(sourceB, true).Outcome != OutcomeClass::Pending) return 24;
        if (!AdvanceUntilBoundary(session, result) || !session.IsComplete() || !result) return 25;
        if (sourceB.LastOpenOffset != 4U || !SameBytes(store, sourceB, 10U)) return 26;
        if (sourceA.CloseCalls != 1U || sourceB.CloseCalls != 1U) return 27;
    }

    // Offset support is a runtime prerequisite. When the explicitly selected
    // replacement Source cannot offset-read, retry safely restarts from zero.
    {
        FakeAtomicRecordStore persistence;
        ArtifactCheckpointStore<Capacity> checkpoints{persistence};
        PartialStore store;
        ArtifactTransferWorkspace<Capacity> workspace;
        OffsetSource sourceA;
        OffsetSource sourceB;
        Fill(sourceA, 8U, 0x80U);
        Fill(sourceB, 8U, 0x80U);
        sourceA.FailAt = 4U;
        const auto artifact = Artifact(4U, 8U);

        ArtifactAcquisitionSession<Capacity> session{
            sourceA, true, store, store, checkpoints, workspace};
        if (session.Begin(UpdateTransactionId{12U}, artifact).Outcome != OutcomeClass::Pending) return 30;
        Result result;
        if (!AdvanceUntilBoundary(session, result) || !session.AwaitingSourceRetry()) return 31;
        if (session.RetryWithSource(sourceB, false).Outcome != OutcomeClass::Pending) return 32;
        if (!AdvanceUntilBoundary(session, result) || !session.IsComplete()) return 33;
        if (sourceB.LastOpenOffset != 0U || !SameBytes(store, sourceB, 8U)) return 34;
    }

    return 0;
}
