#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTA.hpp"

using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

class FakeAtomicRecordStore final : public ESPressio::Persistence::IAtomicRecordStore {
    struct Entry final {
        ESPressio::Persistence::AtomicRecordKey Key{};
        bool Used{false};
        std::array<std::uint8_t, Capacity::MaximumOTAControlRecordBytes> Bytes{};
        std::size_t Size{0U};
    };

    std::array<Entry, 8> entries_{};
    ESPressio::Persistence::AtomicRecordStatus nextReplace_{ESPressio::Persistence::AtomicRecordStatus::Success};

    Entry* Find(const ESPressio::Persistence::AtomicRecordKey& key) noexcept {
        for (auto& entry : entries_) if (entry.Used && entry.Key == key) return &entry;
        return nullptr;
    }

    const Entry* Find(const ESPressio::Persistence::AtomicRecordKey& key) const noexcept {
        for (const auto& entry : entries_) if (entry.Used && entry.Key == key) return &entry;
        return nullptr;
    }

public:
    ESPressio::Persistence::AtomicRecordCapabilities Capabilities() const noexcept override {
        return {true, true, Capacity::MaximumOTAControlRecordBytes, entries_.size()};
    }

    ESPressio::Persistence::AtomicRecordStatus Recover() noexcept override {
        return ESPressio::Persistence::AtomicRecordStatus::Success;
    }

    ESPressio::Persistence::AtomicRecordStatus Read(
        const ESPressio::Persistence::AtomicRecordKey& key,
        std::uint8_t* buffer,
        std::size_t capacity,
        std::size_t& bytesRead) noexcept override {
        bytesRead = 0U;
        const auto* entry = Find(key);
        if (entry == nullptr) return ESPressio::Persistence::AtomicRecordStatus::NotFound;
        if (buffer == nullptr || capacity < entry->Size) return ESPressio::Persistence::AtomicRecordStatus::BufferTooSmall;
        for (std::size_t i = 0U; i < entry->Size; ++i) buffer[i] = entry->Bytes[i];
        bytesRead = entry->Size;
        return ESPressio::Persistence::AtomicRecordStatus::Success;
    }

    ESPressio::Persistence::AtomicRecordStatus ReplaceAtomically(
        const ESPressio::Persistence::AtomicRecordKey& key,
        const std::uint8_t* data,
        std::size_t size) noexcept override {
        const auto requested = nextReplace_;
        nextReplace_ = ESPressio::Persistence::AtomicRecordStatus::Success;
        if (requested != ESPressio::Persistence::AtomicRecordStatus::Success) return requested;
        if (!key || data == nullptr || size > Capacity::MaximumOTAControlRecordBytes) {
            return ESPressio::Persistence::AtomicRecordStatus::InvalidArgument;
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
        if (entry == nullptr) return ESPressio::Persistence::AtomicRecordStatus::NoSpace;
        for (std::size_t i = 0U; i < size; ++i) entry->Bytes[i] = data[i];
        entry->Size = size;
        return ESPressio::Persistence::AtomicRecordStatus::Success;
    }

    ESPressio::Persistence::AtomicRecordStatus RemoveAfterCommit(
        const ESPressio::Persistence::AtomicRecordKey& key) noexcept override {
        auto* entry = Find(key);
        if (entry == nullptr) return ESPressio::Persistence::AtomicRecordStatus::Success;
        entry->Used = false;
        entry->Size = 0U;
        return ESPressio::Persistence::AtomicRecordStatus::Success;
    }

    void FailNextReplace(ESPressio::Persistence::AtomicRecordStatus status) noexcept {
        nextReplace_ = status;
    }

    bool Corrupt(const ESPressio::Persistence::AtomicRecordKey& key) noexcept {
        auto* entry = Find(key);
        if (entry == nullptr || entry->Size == 0U) return false;
        entry->Bytes[0] ^= 0xFFU;
        return true;
    }
};

CommittedBaseline FactoryBaseline() noexcept {
    CommittedBaseline baseline;
    baseline.Generation = UpdateGenerationId{1U};
    baseline.Security = SecurityGeneration{0U};
    return baseline;
}

ArtifactCheckpoint<Capacity> Checkpoint(UpdateTransactionId transaction, std::uint8_t artifactId) {
    ArtifactCheckpoint<Capacity> checkpoint;
    checkpoint.Transaction = transaction.Value();
    checkpoint.Artifact = Id(artifactId);
    checkpoint.ExpectedLength = 1024U;
    checkpoint.AcceptedPrefixLength = 256U;
    checkpoint.Revision = 1U;
    checkpoint.PrefixDigestAlgorithm = ESPressio::Security::DigestAlgorithm::SHA256.Value();
    for (std::size_t i = 0U; i < 32U; ++i) {
        (void)checkpoint.PrefixDigest.push_back(static_cast<std::uint8_t>(i + 1U));
    }
    return checkpoint;
}

} // namespace

int main() {
    static_assert(OTAControlEncodedBytesV1 <= Capacity::MaximumOTAControlRecordBytes);

    FakeAtomicRecordStore store;
    OTAControlStore<Capacity> control{store};

    if (control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 1;
    if (control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::AlreadyProvisioned) return 2;

    OTAControlRecord<Capacity> loaded;
    if (control.Load(loaded) != OTADurableStatus::Success) return 3;
    if (loaded.Committed.Generation != UpdateGenerationId{1U} || loaded.NextTransaction != UpdateTransactionId{1U} ||
        loaded.NextGeneration != UpdateGenerationId{2U} || loaded.HasActiveTransaction) return 4;

    std::array<std::uint8_t, OTAControlEncodedBytesV1> encoded{};
    std::size_t encodedBytes = 0U;
    if (SerializeOTAControlRecord(loaded, encoded.data(), encoded.size(), encodedBytes) != OTADurableStatus::Success ||
        encodedBytes != OTAControlEncodedBytesV1) return 5;
    auto corruptBytes = encoded;
    corruptBytes[20] ^= 0x01U;
    OTAControlRecord<Capacity> corruptOutput;
    if (DeserializeOTAControlRecord(corruptBytes.data(), corruptBytes.size(), corruptOutput) != OTADurableStatus::Corrupt) return 6;

    ActiveTransactionRecord first;
    if (control.BeginTransaction(ReleaseIdentifier{10U}, ManifestIdentifier{Id(10U)}, SecurityGeneration{1U}, first) != OTADurableStatus::Success) return 7;
    if (first.Transaction != UpdateTransactionId{1U} || first.CandidateGeneration != UpdateGenerationId{2U} ||
        first.PreviousCommittedGeneration != UpdateGenerationId{1U} || first.Point != RecoveryPoint::TransactionCreated) return 8;

    OTAControlStore<Capacity> afterRestart{store};
    if (afterRestart.Load(loaded) != OTADurableStatus::Success || !loaded.HasActiveTransaction ||
        loaded.Active.Transaction != first.Transaction) return 9;

    if (afterRestart.BeginTransaction(ReleaseIdentifier{11U}, ManifestIdentifier{Id(11U)}, SecurityGeneration{1U}, first) !=
        OTADurableStatus::ActiveTransactionExists) return 10;

    if (afterRestart.AdvanceRecoveryPoint(first.Transaction, RecoveryPoint::ManifestAccepted) != OTADurableStatus::Success) return 11;
    if (afterRestart.AdvanceRecoveryPoint(first.Transaction, RecoveryPoint::ArtifactsAcquired) != OTADurableStatus::Success) return 12;
    if (afterRestart.AdvanceRecoveryPoint(first.Transaction, RecoveryPoint::ArtifactsVerified) != OTADurableStatus::Success) return 13;
    if (afterRestart.AdvanceRecoveryPoint(first.Transaction, RecoveryPoint::StagingStarted) != OTADurableStatus::Success) return 14;
    if (afterRestart.AdvanceRecoveryPoint(first.Transaction, RecoveryPoint::Staged) != OTADurableStatus::Success) return 15;
    if (afterRestart.ArmActivation(first.Transaction, ESPressio::Platform::OTA::BootTargetIdentifier{2U}, ESPressio::Platform::OTA::BootTargetIdentifier{1U}) != OTADurableStatus::Success) return 16;
    if (afterRestart.Load(loaded) != OTADurableStatus::Success ||
        loaded.Active.CandidateBootTarget != ESPressio::Platform::OTA::BootTargetIdentifier{2U} ||
        loaded.Active.PreviousCommittedBootTarget != ESPressio::Platform::OTA::BootTargetIdentifier{1U}) return 160;
    if (afterRestart.MarkTrialEntered(first.Transaction) != OTADurableStatus::Success) return 17;
    if (afterRestart.PersistCommitIntent(first.Transaction) != OTADurableStatus::Success) return 18;

    if (afterRestart.Load(loaded) != OTADurableStatus::Success ||
        loaded.Intent != DurableIntent::CommitIntent || loaded.Active.Point != RecoveryPoint::CommitStarted) return 19;

    if (afterRestart.FinalizeCommit(first.Transaction) != OTADurableStatus::Success) return 20;
    if (afterRestart.Load(loaded) != OTADurableStatus::Success || loaded.HasActiveTransaction ||
        loaded.Committed.Generation != UpdateGenerationId{2U} || loaded.Committed.Release != ReleaseIdentifier{10U} ||
        loaded.Committed.Manifest != ManifestIdentifier{Id(10U)} || loaded.MinimumAcceptedSecurity != SecurityGeneration{1U}) return 21;

    ActiveTransactionRecord second;
    if (afterRestart.BeginTransaction(ReleaseIdentifier{20U}, ManifestIdentifier{Id(20U)}, SecurityGeneration{1U}, second) != OTADurableStatus::Success) return 22;
    if (second.Transaction != UpdateTransactionId{2U} || second.CandidateGeneration != UpdateGenerationId{3U}) return 23;
    if (afterRestart.PersistRollbackIntent(second.Transaction) != OTADurableStatus::InvalidTransition) return 230;
    if (afterRestart.AdvanceRecoveryPoint(second.Transaction, RecoveryPoint::Staged) != OTADurableStatus::Success) return 231;
    if (afterRestart.ArmActivation(second.Transaction, ESPressio::Platform::OTA::BootTargetIdentifier{3U}, ESPressio::Platform::OTA::BootTargetIdentifier{3U}) != OTADurableStatus::Invalid) return 232;
    if (afterRestart.ArmActivation(second.Transaction, ESPressio::Platform::OTA::BootTargetIdentifier{3U}, ESPressio::Platform::OTA::BootTargetIdentifier{2U}) != OTADurableStatus::Success) return 233;
    if (afterRestart.Load(loaded) != OTADurableStatus::Success ||
        loaded.Active.CandidateBootTarget != ESPressio::Platform::OTA::BootTargetIdentifier{3U} ||
        loaded.Active.PreviousCommittedBootTarget != ESPressio::Platform::OTA::BootTargetIdentifier{2U}) return 234;
    if (afterRestart.PersistRollbackIntent(second.Transaction) != OTADurableStatus::Success) return 24;
    if (afterRestart.FinalizeRollback(second.Transaction) != OTADurableStatus::Success) return 25;
    if (afterRestart.Load(loaded) != OTADurableStatus::Success || loaded.HasActiveTransaction ||
        loaded.Committed.Generation != UpdateGenerationId{2U} || loaded.MinimumAcceptedSecurity != SecurityGeneration{1U}) return 26;

    ActiveTransactionRecord rejected;
    if (afterRestart.BeginTransaction(ReleaseIdentifier{30U}, ManifestIdentifier{Id(30U)}, SecurityGeneration{0U}, rejected) !=
        OTADurableStatus::SecurityRollbackRejected) return 27;

    store.FailNextReplace(ESPressio::Persistence::AtomicRecordStatus::CommitAmbiguous);
    ActiveTransactionRecord ambiguous;
    if (afterRestart.BeginTransaction(ReleaseIdentifier{31U}, ManifestIdentifier{Id(31U)}, SecurityGeneration{1U}, ambiguous) !=
        OTADurableStatus::CommitAmbiguous) return 28;
    if (ambiguous.IsValid() || !afterRestart.HasAmbiguousCommit()) return 29;
    if (afterRestart.Load(loaded) != OTADurableStatus::Success || loaded.HasActiveTransaction ||
        loaded.NextTransaction != UpdateTransactionId{3U} || loaded.NextGeneration != UpdateGenerationId{4U}) return 30;

    // Unknown commit durability blocks further mutation in this service instance.
    if (afterRestart.BeginTransaction(ReleaseIdentifier{31U}, ManifestIdentifier{Id(31U)}, SecurityGeneration{1U}, ambiguous) !=
        OTADurableStatus::CommitAmbiguous) return 31;

    // Simulated restart/recovery establishes that the old complete record survived,
    // after which that uncommitted ID is safe to allocate again.
    OTAControlStore<Capacity> recoveredAfterAmbiguity{store};
    if (recoveredAfterAmbiguity.Load(loaded) != OTADurableStatus::Success || loaded.HasActiveTransaction) return 32;
    ActiveTransactionRecord third;
    if (recoveredAfterAmbiguity.BeginTransaction(
            ReleaseIdentifier{31U}, ManifestIdentifier{Id(31U)}, SecurityGeneration{1U}, third) != OTADurableStatus::Success) return 33;
    if (third.Transaction != UpdateTransactionId{3U} || third.CandidateGeneration != UpdateGenerationId{4U}) return 34;
    if (recoveredAfterAmbiguity.MarkRecoveryRequired() != OTADurableStatus::Success) return 35;
    if (recoveredAfterAmbiguity.Load(loaded) != OTADurableStatus::Success || loaded.Intent != DurableIntent::RecoveryRequired) return 36;

    ArtifactCheckpointStore<Capacity> checkpoints{store};
    const auto checkpoint = Checkpoint(third.Transaction, 7U);
    if (checkpoints.Save(1U, checkpoint) != OTADurableStatus::Success) return 37;
    ArtifactCheckpoint<Capacity> restored;
    if (checkpoints.Load(1U, restored) != OTADurableStatus::Success || restored.Transaction != third.Transaction.Value() ||
        restored.AcceptedPrefixLength != 256U) return 38;
    std::size_t slot = 0U;
    ArtifactCheckpoint<Capacity> found;
    if (checkpoints.Find(third.Transaction, ArtifactIdentifier{Id(7U)}, found, slot) != OTADurableStatus::Success || slot != 1U) return 39;
    if (checkpoints.Remove(1U) != OTADurableStatus::Success) return 40;
    if (checkpoints.Load(1U, restored) != OTADurableStatus::NotFound) return 41;

    if (!store.Corrupt(OTAControlStore<Capacity>::RecordKey())) return 42;
    OTAControlStore<Capacity> corruptedStore{store};
    if (corruptedStore.Load(loaded) != OTADurableStatus::Corrupt) return 43;

    return 0;
}
