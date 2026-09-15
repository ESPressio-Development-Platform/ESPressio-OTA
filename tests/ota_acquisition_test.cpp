#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAAcquisition.hpp"

using namespace ESPressio;
using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;
static_assert(sizeof(ArtifactTransferWorkspace<Capacity>) == Capacity::TransferBufferBytes);

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
    const Entry* Find(const Persistence::AtomicRecordKey& key) const noexcept {
        for (const auto& entry : entries_) if (entry.Used && entry.Key == key) return &entry;
        return nullptr;
    }
public:
    Persistence::AtomicRecordCapabilities Capabilities() const noexcept override {
        return {true, true, Capacity::MaximumOTAControlRecordBytes, entries_.size()};
    }
    Persistence::AtomicRecordStatus Recover() noexcept override { return Persistence::AtomicRecordStatus::Success; }
    Persistence::AtomicRecordStatus Read(const Persistence::AtomicRecordKey& key, std::uint8_t* buffer,
                                         std::size_t capacity, std::size_t& bytesRead) noexcept override {
        bytesRead = 0U;
        const auto* entry = Find(key);
        if (entry == nullptr) return Persistence::AtomicRecordStatus::NotFound;
        if (buffer == nullptr || capacity < entry->Size) return Persistence::AtomicRecordStatus::BufferTooSmall;
        for (std::size_t i = 0U; i < entry->Size; ++i) buffer[i] = entry->Bytes[i];
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
                    entry = &candidate;
                    entry->Used = true;
                    entry->Key = key;
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
        if (entry != nullptr) {
            entry->Used = false;
            entry->Size = 0U;
        }
        return Persistence::AtomicRecordStatus::Success;
    }
};

class FakeSource final : public IArtifactSource {
public:
    std::array<std::uint8_t, 16> Data{};
    std::size_t Size{0U};
    std::size_t Position{0U};
    std::size_t Chunk{3U};
    std::uint64_t OpenOffset{99U};
    std::size_t OpenCalls{0U};
    std::size_t CloseCalls{0U};
    bool PendingOnce{false};
    bool PendingReturned{false};
    bool OverrunMode{false};

    Result Open(const ArtifactSourceOpenRequest& request) noexcept override {
        ++OpenCalls;
        OpenOffset = request.Offset;
        Position = 0U;
        PendingReturned = false;
        return request.IsValid() && request.Offset == 0U ? Result::Success()
                                                        : Result{OutcomeClass::Unsupported, {DiagnosticDomain::Source, 1U}};
    }

    StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (PendingOnce && !PendingReturned) {
            PendingReturned = true;
            return StreamReadResult::Pending();
        }
        if (OverrunMode && Position == 0U) {
            const std::size_t bytes = std::min<std::size_t>(Size, capacity);
            for (std::size_t i = 0U; i < bytes; ++i) output[i] = Data[i];
            Position += bytes;
            return StreamReadResult::Data(bytes);
        }
        if (Position == Size) return StreamReadResult::End();
        const std::size_t bytes = std::min({Chunk, Size - Position, capacity});
        for (std::size_t i = 0U; i < bytes; ++i) output[i] = Data[Position + i];
        Position += bytes;
        return StreamReadResult::Data(bytes);
    }

    void Close() noexcept override { ++CloseCalls; }
};

class FakeStore final : public IArtifactStore {
public:
    std::array<std::uint8_t, 32> Bytes{};
    std::size_t Size{0U};
    std::size_t WriteLimit{2U};
    std::size_t BeginCalls{0U};
    std::size_t WriteCalls{0U};
    std::size_t FinalizeCalls{0U};
    std::size_t AbortCalls{0U};
    bool PendingWriteOnce{false};
    bool PendingWriteReturned{false};
    bool PendingFinalizeOnce{false};
    bool PendingFinalizeReturned{false};
    bool Collision{false};

    Result BeginWrite(const ArtifactStoreOpenRequest& request) noexcept override {
        ++BeginCalls;
        Size = 0U;
        PendingWriteReturned = false;
        PendingFinalizeReturned = false;
        return request.IsValid() ? Result::Success()
                                 : Result{OutcomeClass::Invalid, {DiagnosticDomain::Store, 1U}};
    }

    StreamWriteResult Write(const std::uint8_t* data, std::size_t size) noexcept override {
        ++WriteCalls;
        if (PendingWriteOnce && !PendingWriteReturned) {
            PendingWriteReturned = true;
            return StreamWriteResult::Pending();
        }
        if (data == nullptr || size == 0U || Size == Bytes.size()) {
            return StreamWriteResult::Failed({OutcomeClass::Failed, {DiagnosticDomain::Store, 2U}});
        }
        const std::size_t accepted = std::min({WriteLimit, size, Bytes.size() - Size});
        for (std::size_t i = 0U; i < accepted; ++i) Bytes[Size + i] = data[i];
        Size += accepted;
        return StreamWriteResult::Accepted(accepted);
    }

    ArtifactStoreFinalizeResult Finalize() noexcept override {
        ++FinalizeCalls;
        if (PendingFinalizeOnce && !PendingFinalizeReturned) {
            PendingFinalizeReturned = true;
            return {ArtifactStoreFinalizeStatus::Pending, {OutcomeClass::Pending, {}}};
        }
        if (Collision) {
            return {ArtifactStoreFinalizeStatus::IdentifierCollision,
                    {OutcomeClass::Failed, {DiagnosticDomain::Store, 3U}}};
        }
        return {ArtifactStoreFinalizeStatus::Stored, Result::Success()};
    }

    void Abort() noexcept override { ++AbortCalls; }
    Result QueryAvailableBytes(std::uint64_t& availableBytes) const noexcept override {
        availableBytes = Bytes.size() - Size;
        return Result::Success();
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

bool RunUntilTerminal(ArtifactAcquisitionSession<Capacity>& session, Result& result) {
    for (std::size_t i = 0U; i < 128U; ++i) {
        result = session.Advance();
        if (session.IsComplete() || session.Phase() == ArtifactAcquisitionPhase::Failed) return true;
        if (result.Outcome != OutcomeClass::Pending && result.Outcome != OutcomeClass::Deferred) return false;
    }
    return false;
}

} // namespace

int main() {
    {
        FakeAtomicRecordStore persistence;
        ArtifactCheckpointStore<Capacity> checkpoints{persistence};
        FakeSource source;
        FakeStore store;
        ArtifactTransferWorkspace<Capacity> workspace;
        source.Size = 8U;
        source.Chunk = 3U;
        source.PendingOnce = true;
        for (std::size_t i = 0U; i < source.Size; ++i) source.Data[i] = static_cast<std::uint8_t>(0x20U + i);
        store.PendingWriteOnce = true;
        store.PendingFinalizeOnce = true;
        store.WriteLimit = 2U;

        ArtifactCheckpoint<Capacity> prior;
        prior.Transaction = 9U;
        prior.Artifact = Id(1U);
        prior.ExpectedLength = 8U;
        prior.AcceptedPrefixLength = 4U;
        prior.Revision = 7U;
        prior.PrefixDigestAlgorithm = Security::DigestAlgorithm::SHA256.Value();
        for (std::size_t i = 0U; i < 32U; ++i) (void)prior.PrefixDigest.push_back(static_cast<std::uint8_t>(i));
        if (checkpoints.Save(0U, prior) != OTADurableStatus::Success) return 1;

        ArtifactAcquisitionSession<Capacity> session{source, store, checkpoints, workspace};
        if (session.Begin(UpdateTransactionId{9U}, Artifact(1U, 8U)).Outcome != OutcomeClass::Pending) return 2;
        if (session.Advance().Outcome != OutcomeClass::Pending || source.OpenOffset != 0U) return 3;
        if (session.Advance().Outcome != OutcomeClass::Pending) return 4;
        if (session.Advance().Outcome != OutcomeClass::Pending) return 5;

        ArtifactCheckpoint<Capacity> restarted;
        if (checkpoints.Load(0U, restarted) != OTADurableStatus::Success || restarted.Revision != 8U ||
            restarted.AcceptedPrefixLength != 0U || restarted.PrefixDigestAlgorithm != 0U || !restarted.PrefixDigest.empty()) return 6;

        Result result;
        if (!RunUntilTerminal(session, result) || !session.IsComplete() || !result) return 7;
        if (store.Size != 8U || source.CloseCalls != 1U || store.AbortCalls != 0U) return 8;
        for (std::size_t i = 0U; i < 8U; ++i) if (store.Bytes[i] != source.Data[i]) return 9;
        ArtifactCheckpoint<Capacity> removed;
        if (checkpoints.Load(0U, removed) != OTADurableStatus::NotFound) return 10;
        if (session.AcceptedSourceBytes() != 8U || session.StoredBytes() != 8U) return 11;
    }

    {
        FakeAtomicRecordStore persistence;
        ArtifactCheckpointStore<Capacity> checkpoints{persistence};
        FakeSource source;
        FakeStore store;
        ArtifactTransferWorkspace<Capacity> workspace;
        source.Size = 3U;
        source.Chunk = 3U;
        ArtifactAcquisitionSession<Capacity> session{source, store, checkpoints, workspace};
        if (session.Begin(UpdateTransactionId{10U}, Artifact(2U, 4U)).Outcome != OutcomeClass::Pending) return 20;
        Result result;
        if (!RunUntilTerminal(session, result) || session.Phase() != ArtifactAcquisitionPhase::Failed) return 21;
        if (result.Detail.Domain != DiagnosticDomain::Source ||
            result.Detail.Reason != static_cast<std::uint32_t>(ArtifactAcquisitionReason::Truncated) || store.AbortCalls != 1U) return 22;
    }

    {
        FakeAtomicRecordStore persistence;
        ArtifactCheckpointStore<Capacity> checkpoints{persistence};
        FakeSource source;
        FakeStore store;
        ArtifactTransferWorkspace<Capacity> workspace;
        source.Size = 5U;
        source.OverrunMode = true;
        ArtifactAcquisitionSession<Capacity> session{source, store, checkpoints, workspace};
        if (session.Begin(UpdateTransactionId{11U}, Artifact(3U, 4U)).Outcome != OutcomeClass::Pending) return 30;
        Result result;
        if (!RunUntilTerminal(session, result) || session.Phase() != ArtifactAcquisitionPhase::Failed) return 31;
        if (result.Detail.Domain != DiagnosticDomain::Source ||
            result.Detail.Reason != static_cast<std::uint32_t>(ArtifactAcquisitionReason::Overrun) || store.AbortCalls != 1U) return 32;
    }

    {
        FakeAtomicRecordStore persistence;
        ArtifactCheckpointStore<Capacity> checkpoints{persistence};
        FakeSource source;
        FakeStore store;
        ArtifactTransferWorkspace<Capacity> workspace;
        source.Size = 4U;
        source.Chunk = 4U;
        store.Collision = true;
        ArtifactAcquisitionSession<Capacity> session{source, store, checkpoints, workspace};
        if (session.Begin(UpdateTransactionId{12U}, Artifact(4U, 4U)).Outcome != OutcomeClass::Pending) return 40;
        Result result;
        if (!RunUntilTerminal(session, result) || session.Phase() != ArtifactAcquisitionPhase::Failed) return 41;
        if (result.Detail.Domain != DiagnosticDomain::Store ||
            result.Detail.Reason != static_cast<std::uint32_t>(ArtifactAcquisitionReason::IdentifierCollision) || store.AbortCalls != 1U) return 42;
    }

    return 0;
}
