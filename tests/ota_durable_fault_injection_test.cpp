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

CommittedBaseline FactoryBaseline() noexcept {
    CommittedBaseline baseline;
    baseline.Generation = UpdateGenerationId{1U};
    baseline.Security = SecurityGeneration{0U};
    return baseline;
}

enum class ReplaceFault : std::uint8_t {
    None,
    StorageFailureBefore,
    CommitAmbiguousBefore,
    CommitAmbiguousAfter
};

class CrashAtomicRecordStore final : public Persistence::IAtomicRecordStore {
    struct Entry final {
        Persistence::AtomicRecordKey Key{};
        bool Used{false};
        std::array<std::uint8_t, Capacity::MaximumOTAControlRecordBytes> Bytes{};
        std::size_t Size{0U};
    };

    std::array<Entry, 8> entries_{};
    ReplaceFault nextFault_{ReplaceFault::None};

    Entry* Find(const Persistence::AtomicRecordKey& key) noexcept {
        for (auto& entry : entries_) if (entry.Used && entry.Key == key) return &entry;
        return nullptr;
    }
    const Entry* Find(const Persistence::AtomicRecordKey& key) const noexcept {
        for (const auto& entry : entries_) if (entry.Used && entry.Key == key) return &entry;
        return nullptr;
    }

    Persistence::AtomicRecordStatus Apply(
        const Persistence::AtomicRecordKey& key,
        const std::uint8_t* data,
        std::size_t size) noexcept {
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

public:
    void FaultNext(ReplaceFault fault) noexcept { nextFault_ = fault; }

    Persistence::AtomicRecordCapabilities Capabilities() const noexcept override {
        return {true, true, Capacity::MaximumOTAControlRecordBytes, entries_.size()};
    }
    Persistence::AtomicRecordStatus Recover() noexcept override {
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus Read(
        const Persistence::AtomicRecordKey& key,
        std::uint8_t* buffer,
        std::size_t capacity,
        std::size_t& bytesRead) noexcept override {
        bytesRead = 0U;
        const auto* entry = Find(key);
        if (entry == nullptr) return Persistence::AtomicRecordStatus::NotFound;
        if (buffer == nullptr || capacity < entry->Size) return Persistence::AtomicRecordStatus::BufferTooSmall;
        for (std::size_t i = 0U; i < entry->Size; ++i) buffer[i] = entry->Bytes[i];
        bytesRead = entry->Size;
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus ReplaceAtomically(
        const Persistence::AtomicRecordKey& key,
        const std::uint8_t* data,
        std::size_t size) noexcept override {
        const auto fault = nextFault_;
        nextFault_ = ReplaceFault::None;
        if (fault == ReplaceFault::StorageFailureBefore) return Persistence::AtomicRecordStatus::StorageFailure;
        if (fault == ReplaceFault::CommitAmbiguousBefore) return Persistence::AtomicRecordStatus::CommitAmbiguous;
        const auto applied = Apply(key, data, size);
        if (applied != Persistence::AtomicRecordStatus::Success) return applied;
        if (fault == ReplaceFault::CommitAmbiguousAfter) return Persistence::AtomicRecordStatus::CommitAmbiguous;
        return Persistence::AtomicRecordStatus::Success;
    }
    Persistence::AtomicRecordStatus RemoveAfterCommit(
        const Persistence::AtomicRecordKey& key) noexcept override {
        auto* entry = Find(key);
        if (entry != nullptr) {
            entry->Used = false;
            entry->Size = 0U;
        }
        return Persistence::AtomicRecordStatus::Success;
    }
};

bool Begin(OTAControlStore<Capacity>& control, std::uint8_t id, ActiveTransactionRecord& active) {
    return control.BeginTransaction(
        ReleaseIdentifier{id}, ManifestIdentifier{Id(id)}, SecurityGeneration{1U}, active) == OTADurableStatus::Success;
}

bool Stage(OTAControlStore<Capacity>& control, const ActiveTransactionRecord& active) {
    return control.AdvanceRecoveryPoint(active.Transaction, RecoveryPoint::Staged) == OTADurableStatus::Success;
}

bool Arm(OTAControlStore<Capacity>& control, const ActiveTransactionRecord& active) {
    return control.ArmActivation(
        active.Transaction,
        Platform::OTA::BootTargetIdentifier{2U},
        Platform::OTA::BootTargetIdentifier{1U}) == OTADurableStatus::Success;
}

ArtifactCheckpoint<Capacity> Checkpoint(std::uint64_t transaction, std::uint32_t revision) {
    ArtifactCheckpoint<Capacity> checkpoint;
    checkpoint.Transaction = transaction;
    checkpoint.Artifact = Id(90U);
    checkpoint.ExpectedLength = 1024U;
    checkpoint.AcceptedPrefixLength = 0U;
    checkpoint.Revision = revision;
    checkpoint.PrefixDigestAlgorithm = 0U;
    return checkpoint;
}
} // namespace

int main() {
    // A failed ID allocation never exposes or consumes an ID. An ambiguous
    // post-commit allocation exposes no ID to the caller but is recoverable as
    // the new durable transaction after reconstruction.
    {
        CrashAtomicRecordStore store;
        OTAControlStore<Capacity> control{store};
        if (control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 1;

        store.FaultNext(ReplaceFault::StorageFailureBefore);
        ActiveTransactionRecord failed;
        if (control.BeginTransaction(ReleaseIdentifier{10U}, ManifestIdentifier{Id(10U)}, SecurityGeneration{1U}, failed) !=
            OTADurableStatus::StorageFailure || failed.IsValid()) return 2;
        OTAControlRecord<Capacity> record;
        if (control.Load(record) != OTADurableStatus::Success || record.HasActiveTransaction ||
            record.NextTransaction != UpdateTransactionId{1U} || record.NextGeneration != UpdateGenerationId{2U}) return 3;

        store.FaultNext(ReplaceFault::CommitAmbiguousAfter);
        ActiveTransactionRecord ambiguous;
        if (control.BeginTransaction(ReleaseIdentifier{11U}, ManifestIdentifier{Id(11U)}, SecurityGeneration{1U}, ambiguous) !=
            OTADurableStatus::CommitAmbiguous || ambiguous.IsValid() || !control.HasAmbiguousCommit()) return 4;
        if (control.Load(record) != OTADurableStatus::Success || !record.HasActiveTransaction ||
            record.Active.Transaction != UpdateTransactionId{1U} || record.Active.CandidateGeneration != UpdateGenerationId{2U} ||
            record.NextTransaction != UpdateTransactionId{2U} || record.NextGeneration != UpdateGenerationId{3U}) return 5;

        OTAControlStore<Capacity> recovered{store};
        if (recovered.Load(record) != OTADurableStatus::Success || record.Active.Point != RecoveryPoint::TransactionCreated) return 6;
    }

    // Forward recovery-point replacement is old-or-new across a crash.
    {
        CrashAtomicRecordStore store;
        OTAControlStore<Capacity> control{store};
        if (control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 10;
        ActiveTransactionRecord active;
        if (!Begin(control, 20U, active)) return 11;

        store.FaultNext(ReplaceFault::StorageFailureBefore);
        if (control.AdvanceRecoveryPoint(active.Transaction, RecoveryPoint::ManifestAccepted) != OTADurableStatus::StorageFailure) return 12;
        OTAControlRecord<Capacity> record;
        if (control.Load(record) != OTADurableStatus::Success || record.Active.Point != RecoveryPoint::TransactionCreated) return 13;

        store.FaultNext(ReplaceFault::CommitAmbiguousAfter);
        if (control.AdvanceRecoveryPoint(active.Transaction, RecoveryPoint::ManifestAccepted) != OTADurableStatus::CommitAmbiguous) return 14;
        if (control.Load(record) != OTADurableStatus::Success || record.Active.Point != RecoveryPoint::ManifestAccepted) return 15;
        OTAControlStore<Capacity> recovered{store};
        if (recovered.AdvanceRecoveryPoint(active.Transaction, RecoveryPoint::ArtifactsAcquired) != OTADurableStatus::Success) return 16;
    }

    // Checkpoint replacement is likewise atomic: failure-before retains the old
    // checkpoint; ambiguous-after is recovered as the complete newer record.
    {
        CrashAtomicRecordStore store;
        ArtifactCheckpointStore<Capacity> checkpoints{store};
        if (checkpoints.Save(0U, Checkpoint(30U, 1U)) != OTADurableStatus::Success) return 20;
        store.FaultNext(ReplaceFault::StorageFailureBefore);
        if (checkpoints.Save(0U, Checkpoint(30U, 2U)) != OTADurableStatus::StorageFailure) return 21;
        ArtifactCheckpoint<Capacity> loaded;
        if (checkpoints.Load(0U, loaded) != OTADurableStatus::Success || loaded.Revision != 1U) return 22;
        store.FaultNext(ReplaceFault::CommitAmbiguousAfter);
        if (checkpoints.Save(0U, Checkpoint(30U, 2U)) != OTADurableStatus::CommitAmbiguous) return 23;
        if (checkpoints.Load(0U, loaded) != OTADurableStatus::Success || loaded.Revision != 2U) return 24;
    }

    // Activation intent is durable before any boot selection. Both failure sides
    // leave a deterministic reconstructable state.
    {
        CrashAtomicRecordStore store;
        OTAControlStore<Capacity> control{store};
        if (control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 30;
        ActiveTransactionRecord active;
        if (!Begin(control, 40U, active) || !Stage(control, active)) return 31;
        store.FaultNext(ReplaceFault::StorageFailureBefore);
        if (control.ArmActivation(active.Transaction, Platform::OTA::BootTargetIdentifier{2U},
                                  Platform::OTA::BootTargetIdentifier{1U}) != OTADurableStatus::StorageFailure) return 32;
        OTAControlRecord<Capacity> record;
        if (control.Load(record) != OTADurableStatus::Success || record.Active.Point != RecoveryPoint::Staged ||
            record.Intent != DurableIntent::None || record.Active.CandidateBootTarget) return 33;
        store.FaultNext(ReplaceFault::CommitAmbiguousAfter);
        if (control.ArmActivation(active.Transaction, Platform::OTA::BootTargetIdentifier{2U},
                                  Platform::OTA::BootTargetIdentifier{1U}) != OTADurableStatus::CommitAmbiguous) return 34;
        if (control.Load(record) != OTADurableStatus::Success || record.Active.Point != RecoveryPoint::ActivationSelected ||
            record.Intent != DurableIntent::ActivationArmed ||
            record.Active.CandidateBootTarget != Platform::OTA::BootTargetIdentifier{2U}) return 35;
    }

    // Trial entry and CommitIntent each survive ambiguous-after-commit exactly as
    // their new durable facts; a fresh OTAControlStore can continue from them.
    {
        CrashAtomicRecordStore store;
        OTAControlStore<Capacity> control{store};
        if (control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 40;
        ActiveTransactionRecord active;
        if (!Begin(control, 50U, active) || !Stage(control, active) || !Arm(control, active)) return 41;
        store.FaultNext(ReplaceFault::CommitAmbiguousAfter);
        if (control.MarkTrialEntered(active.Transaction) != OTADurableStatus::CommitAmbiguous) return 42;
        OTAControlRecord<Capacity> record;
        if (control.Load(record) != OTADurableStatus::Success || record.Active.Point != RecoveryPoint::TrialBootEntered ||
            record.Intent != DurableIntent::None) return 43;

        OTAControlStore<Capacity> afterTrial{store};
        store.FaultNext(ReplaceFault::CommitAmbiguousAfter);
        if (afterTrial.PersistCommitIntent(active.Transaction) != OTADurableStatus::CommitAmbiguous) return 44;
        if (afterTrial.Load(record) != OTADurableStatus::Success || record.Active.Point != RecoveryPoint::CommitStarted ||
            record.Intent != DurableIntent::CommitIntent) return 45;
    }

    // FinalizeCommit intentionally collapses committed-baseline replacement,
    // security-floor advancement and active-transaction clearing into ONE atomic
    // control-record replacement. Failure-before preserves the entire old commit
    // intent; ambiguous-after reconstructs the entire new committed truth.
    {
        CrashAtomicRecordStore store;
        OTAControlStore<Capacity> control{store};
        if (control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 50;
        ActiveTransactionRecord active;
        if (!Begin(control, 60U, active) || !Stage(control, active) || !Arm(control, active) ||
            control.MarkTrialEntered(active.Transaction) != OTADurableStatus::Success ||
            control.PersistCommitIntent(active.Transaction) != OTADurableStatus::Success) return 51;

        store.FaultNext(ReplaceFault::StorageFailureBefore);
        if (control.FinalizeCommit(active.Transaction) != OTADurableStatus::StorageFailure) return 52;
        OTAControlRecord<Capacity> record;
        if (control.Load(record) != OTADurableStatus::Success || !record.HasActiveTransaction ||
            record.Intent != DurableIntent::CommitIntent || record.Committed.Generation != UpdateGenerationId{1U} ||
            record.MinimumAcceptedSecurity != SecurityGeneration{0U}) return 53;

        store.FaultNext(ReplaceFault::CommitAmbiguousAfter);
        if (control.FinalizeCommit(active.Transaction) != OTADurableStatus::CommitAmbiguous) return 54;
        if (control.Load(record) != OTADurableStatus::Success || record.HasActiveTransaction ||
            record.Intent != DurableIntent::None || record.Committed.Generation != active.CandidateGeneration ||
            record.Committed.Release != ReleaseIdentifier{60U} || record.MinimumAcceptedSecurity != SecurityGeneration{1U}) return 55;
        OTAControlStore<Capacity> recovered{store};
        if (recovered.Load(record) != OTADurableStatus::Success || record.HasActiveTransaction) return 56;
    }

    // Rollback intent and rollback completion are separately durable. The final
    // rollback replacement atomically preserves the old committed baseline and
    // security floor while clearing only the candidate transaction.
    {
        CrashAtomicRecordStore store;
        OTAControlStore<Capacity> control{store};
        if (control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 60;
        ActiveTransactionRecord active;
        if (!Begin(control, 70U, active) || !Stage(control, active) || !Arm(control, active)) return 61;

        store.FaultNext(ReplaceFault::StorageFailureBefore);
        if (control.PersistRollbackIntent(active.Transaction) != OTADurableStatus::StorageFailure) return 62;
        OTAControlRecord<Capacity> record;
        if (control.Load(record) != OTADurableStatus::Success || record.Intent != DurableIntent::ActivationArmed ||
            record.Active.Point != RecoveryPoint::ActivationSelected) return 63;

        store.FaultNext(ReplaceFault::CommitAmbiguousAfter);
        if (control.PersistRollbackIntent(active.Transaction) != OTADurableStatus::CommitAmbiguous) return 64;
        if (control.Load(record) != OTADurableStatus::Success || record.Intent != DurableIntent::RollbackIntent ||
            record.Active.Point != RecoveryPoint::RollbackStarted) return 65;

        OTAControlStore<Capacity> recovered{store};
        store.FaultNext(ReplaceFault::StorageFailureBefore);
        if (recovered.FinalizeRollback(active.Transaction) != OTADurableStatus::StorageFailure) return 66;
        if (recovered.Load(record) != OTADurableStatus::Success || !record.HasActiveTransaction ||
            record.Intent != DurableIntent::RollbackIntent) return 67;

        store.FaultNext(ReplaceFault::CommitAmbiguousAfter);
        if (recovered.FinalizeRollback(active.Transaction) != OTADurableStatus::CommitAmbiguous) return 68;
        if (recovered.Load(record) != OTADurableStatus::Success || record.HasActiveTransaction ||
            record.Committed.Generation != UpdateGenerationId{1U} ||
            record.MinimumAcceptedSecurity != SecurityGeneration{0U}) return 69;
    }

    return 0;
}
