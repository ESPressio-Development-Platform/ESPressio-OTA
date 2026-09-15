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
    CommitAmbiguousAfter
};

class AtomicMemoryStore final : public Persistence::IAtomicRecordStore {
    Persistence::AtomicRecordKey key_{};
    bool present_{false};
    std::array<std::uint8_t, Capacity::MaximumOTAControlRecordBytes> bytes_{};
    std::size_t size_{0U};
    ReplaceFault nextFault_{ReplaceFault::None};

public:
    void FaultNext(ReplaceFault fault) noexcept { nextFault_ = fault; }

    Persistence::AtomicRecordCapabilities Capabilities() const noexcept override {
        return {true, true, Capacity::MaximumOTAControlRecordBytes, 1U};
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
        if (!present_ || key != key_) return Persistence::AtomicRecordStatus::NotFound;
        if (buffer == nullptr || capacity < size_) return Persistence::AtomicRecordStatus::BufferTooSmall;
        for (std::size_t i = 0U; i < size_; ++i) buffer[i] = bytes_[i];
        bytesRead = size_;
        return Persistence::AtomicRecordStatus::Success;
    }

    Persistence::AtomicRecordStatus ReplaceAtomically(
        const Persistence::AtomicRecordKey& key,
        const std::uint8_t* data,
        std::size_t size) noexcept override {
        if (!key || data == nullptr || size > bytes_.size()) {
            return Persistence::AtomicRecordStatus::InvalidArgument;
        }
        const auto fault = nextFault_;
        nextFault_ = ReplaceFault::None;
        if (fault == ReplaceFault::StorageFailureBefore) {
            return Persistence::AtomicRecordStatus::StorageFailure;
        }
        key_ = key;
        present_ = true;
        size_ = size;
        for (std::size_t i = 0U; i < size; ++i) bytes_[i] = data[i];
        if (fault == ReplaceFault::CommitAmbiguousAfter) {
            return Persistence::AtomicRecordStatus::CommitAmbiguous;
        }
        return Persistence::AtomicRecordStatus::Success;
    }

    Persistence::AtomicRecordStatus RemoveAfterCommit(
        const Persistence::AtomicRecordKey& key) noexcept override {
        if (present_ && key == key_) {
            present_ = false;
            size_ = 0U;
        }
        return Persistence::AtomicRecordStatus::Success;
    }
};

namespace SimulatedV2 {
inline constexpr OTADurableSchemaVersion Schema{2U};
inline constexpr std::array<std::uint8_t, 4> Magic{{'O', 'T', 'A', 'C'}};
inline constexpr std::size_t EncodedBytes = 60U;

struct Record final {
    UpdateGenerationId Generation{};
    SecurityGeneration MinimumAcceptedSecurity{};
    SecurityGeneration CommittedSecurity{};
    ReleaseIdentifier Release{};
    ManifestIdentifier Manifest{};

    bool IsValid() const noexcept {
        return Generation && Release && Manifest && MinimumAcceptedSecurity <= CommittedSecurity;
    }
};

template<typename T>
void WriteLE(std::uint8_t*& output, T value) noexcept {
    for (std::size_t i = 0U; i < sizeof(T); ++i) {
        *output++ = static_cast<std::uint8_t>(value & static_cast<T>(0xFFU));
        value = static_cast<T>(value >> 8U);
    }
}

template<typename T>
bool ReadLE(const std::uint8_t*& input, const std::uint8_t* end, T& value) noexcept {
    if (static_cast<std::size_t>(end - input) < sizeof(T)) return false;
    value = 0;
    for (std::size_t i = 0U; i < sizeof(T); ++i) {
        value = static_cast<T>(value | (static_cast<T>(input[i]) << (i * 8U)));
    }
    input += sizeof(T);
    return true;
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0U; i < size; ++i) {
        crc ^= data[i];
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U & static_cast<std::uint32_t>(
                -(static_cast<std::int32_t>(crc & 1U))));
        }
    }
    return ~crc;
}

bool Encode(const Record& record, std::array<std::uint8_t, EncodedBytes>& output) noexcept {
    if (!record.IsValid()) return false;
    auto* cursor = output.data();
    for (const auto byte : Magic) *cursor++ = byte;
    WriteLE(cursor, Schema.Value());
    WriteLE(cursor, static_cast<std::uint16_t>(0U));
    WriteLE(cursor, record.Generation.Value());
    WriteLE(cursor, record.MinimumAcceptedSecurity.Value());
    WriteLE(cursor, record.CommittedSecurity.Value());
    WriteLE(cursor, record.Release.Value());
    for (const auto byte : record.Manifest.Bytes()) *cursor++ = byte;
    const auto payloadBytes = static_cast<std::size_t>(cursor - output.data());
    WriteLE(cursor, Crc32(output.data(), payloadBytes));
    return static_cast<std::size_t>(cursor - output.data()) == output.size();
}

bool Decode(const std::uint8_t* data, std::size_t size, Record& output) noexcept {
    if (data == nullptr || size != EncodedBytes) return false;
    const auto expectedCrc = Crc32(data, size - sizeof(std::uint32_t));
    const auto* cursor = data;
    const auto* payloadEnd = data + size - sizeof(std::uint32_t);
    for (const auto expected : Magic) {
        if (cursor == payloadEnd || *cursor++ != expected) return false;
    }
    std::uint16_t schema = 0U;
    std::uint16_t reserved = 0U;
    std::uint64_t generation = 0U;
    std::uint64_t minimumSecurity = 0U;
    std::uint64_t committedSecurity = 0U;
    std::uint64_t release = 0U;
    if (!ReadLE(cursor, payloadEnd, schema) || schema != Schema.Value() ||
        !ReadLE(cursor, payloadEnd, reserved) || reserved != 0U ||
        !ReadLE(cursor, payloadEnd, generation) ||
        !ReadLE(cursor, payloadEnd, minimumSecurity) ||
        !ReadLE(cursor, payloadEnd, committedSecurity) ||
        !ReadLE(cursor, payloadEnd, release)) return false;
    std::array<std::uint8_t, 16> manifest{};
    if (static_cast<std::size_t>(payloadEnd - cursor) != manifest.size()) return false;
    for (auto& byte : manifest) byte = *cursor++;
    const auto* crcCursor = payloadEnd;
    const auto* end = data + size;
    std::uint32_t storedCrc = 0U;
    if (!ReadLE(crcCursor, end, storedCrc) || storedCrc != expectedCrc) return false;

    Record candidate;
    candidate.Generation = UpdateGenerationId{generation};
    candidate.MinimumAcceptedSecurity = SecurityGeneration{minimumSecurity};
    candidate.CommittedSecurity = SecurityGeneration{committedSecurity};
    candidate.Release = ReleaseIdentifier{release};
    candidate.Manifest = ManifestIdentifier{manifest};
    if (!candidate.IsValid()) return false;
    output = candidate;
    return true;
}
} // namespace SimulatedV2

enum class ReadSchema : std::uint8_t {
    V1,
    V2
};

struct CandidateReadResult final {
    ReadSchema Schema{ReadSchema::V1};
    OTAControlRecord<Capacity> V1{};
    SimulatedV2::Record V2{};
};

bool ReadRaw(
    AtomicMemoryStore& store,
    std::array<std::uint8_t, Capacity::MaximumOTAControlRecordBytes>& bytes,
    std::size_t& size) noexcept {
    return store.Read(OTAControlStore<Capacity>::RecordKey(), bytes.data(), bytes.size(), size) ==
           Persistence::AtomicRecordStatus::Success;
}

bool CandidateRead(AtomicMemoryStore& store, CandidateReadResult& output) noexcept {
    std::array<std::uint8_t, Capacity::MaximumOTAControlRecordBytes> bytes{};
    std::size_t size = 0U;
    if (!ReadRaw(store, bytes, size) || size < 6U ||
        bytes[0] != 'O' || bytes[1] != 'T' || bytes[2] != 'A' || bytes[3] != 'C') return false;
    const auto schema = static_cast<std::uint16_t>(bytes[4]) |
                        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[5]) << 8U);
    if (schema == OTADurableSchemaV1.Value()) {
        OTAControlRecord<Capacity> record;
        if (DeserializeOTAControlRecord(bytes.data(), size, record) != OTADurableStatus::Success) return false;
        output.Schema = ReadSchema::V1;
        output.V1 = record;
        return true;
    }
    if (schema == SimulatedV2::Schema.Value()) {
        SimulatedV2::Record record;
        if (!SimulatedV2::Decode(bytes.data(), size, record)) return false;
        output.Schema = ReadSchema::V2;
        output.V2 = record;
        return true;
    }
    return false;
}

bool OldRuntimeRead(AtomicMemoryStore& store, OTAControlRecord<Capacity>& output) noexcept {
    std::array<std::uint8_t, Capacity::MaximumOTAControlRecordBytes> bytes{};
    std::size_t size = 0U;
    if (!ReadRaw(store, bytes, size)) return false;
    return DeserializeOTAControlRecord(bytes.data(), size, output) == OTADurableStatus::Success;
}

Persistence::AtomicRecordStatus MigrateCommittedV1ToV2(AtomicMemoryStore& store) noexcept {
    OTAControlRecord<Capacity> v1;
    if (!OldRuntimeRead(store, v1) || v1.HasActiveTransaction ||
        v1.Intent != DurableIntent::None || !v1.Committed.HasRelease || !v1.Committed.HasManifest) {
        return Persistence::AtomicRecordStatus::InvalidArgument;
    }
    SimulatedV2::Record v2;
    v2.Generation = v1.Committed.Generation;
    v2.MinimumAcceptedSecurity = v1.MinimumAcceptedSecurity;
    v2.CommittedSecurity = v1.Committed.Security;
    v2.Release = v1.Committed.Release;
    v2.Manifest = v1.Committed.Manifest;
    std::array<std::uint8_t, SimulatedV2::EncodedBytes> encoded{};
    if (!SimulatedV2::Encode(v2, encoded)) return Persistence::AtomicRecordStatus::InvalidArgument;
    return store.ReplaceAtomically(OTAControlStore<Capacity>::RecordKey(), encoded.data(), encoded.size());
}

bool BeginToTrial(
    OTAControlStore<Capacity>& control,
    std::uint8_t id,
    ActiveTransactionRecord& active) noexcept {
    if (control.BeginTransaction(
            ReleaseIdentifier{id}, ManifestIdentifier{Id(id)}, SecurityGeneration{1U}, active) !=
        OTADurableStatus::Success) return false;
    if (control.AdvanceRecoveryPoint(active.Transaction, RecoveryPoint::Staged) != OTADurableStatus::Success) return false;
    if (control.ArmActivation(
            active.Transaction,
            Platform::OTA::BootTargetIdentifier{2U},
            Platform::OTA::BootTargetIdentifier{1U}) != OTADurableStatus::Success) return false;
    return control.MarkTrialEntered(active.Transaction) == OTADurableStatus::Success;
}

bool IsCommittedCandidate(const CandidateReadResult& read, const ActiveTransactionRecord& active,
                          std::uint8_t release) noexcept {
    if (read.Schema == ReadSchema::V1) {
        return !read.V1.HasActiveTransaction && read.V1.Intent == DurableIntent::None &&
               read.V1.Committed.Generation == active.CandidateGeneration &&
               read.V1.Committed.Release == ReleaseIdentifier{release} &&
               read.V1.MinimumAcceptedSecurity == SecurityGeneration{1U};
    }
    return read.V2.Generation == active.CandidateGeneration &&
           read.V2.Release == ReleaseIdentifier{release} &&
           read.V2.MinimumAcceptedSecurity == SecurityGeneration{1U} &&
           read.V2.CommittedSecurity == SecurityGeneration{1U};
}

} // namespace

int main() {
    // old -> candidate -> Commit: the candidate explicitly reads inherited V1,
    // keeps V1 throughout Trial, commits safely in V1, then performs the first
    // irreversible V2 migration only after rollback is no longer required.
    {
        AtomicMemoryStore store;
        OTAControlStore<Capacity> oldRuntime{store};
        if (oldRuntime.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 1;
        ActiveTransactionRecord active;
        if (!BeginToTrial(oldRuntime, 10U, active)) return 2;

        CandidateReadResult inherited;
        if (!CandidateRead(store, inherited) || inherited.Schema != ReadSchema::V1 ||
            inherited.V1.Active.Point != RecoveryPoint::TrialBootEntered) return 3;

        // Candidate Trial-time write deliberately uses the old schema so the
        // previous committed runtime remains able to reconcile rollback state.
        OTAControlStore<Capacity> candidateTrial{store};
        if (candidateTrial.PersistCommitIntent(active.Transaction) != OTADurableStatus::Success) return 4;
        OTAControlRecord<Capacity> oldReadable;
        if (!OldRuntimeRead(store, oldReadable) || oldReadable.Intent != DurableIntent::CommitIntent ||
            oldReadable.Active.Point != RecoveryPoint::CommitStarted) return 5;

        if (candidateTrial.FinalizeCommit(active.Transaction) != OTADurableStatus::Success) return 6;
        CandidateReadResult committedV1;
        if (!CandidateRead(store, committedV1) || !IsCommittedCandidate(committedV1, active, 10U)) return 7;
        if (MigrateCommittedV1ToV2(store) != Persistence::AtomicRecordStatus::Success) return 8;
        CandidateReadResult committedV2;
        if (!CandidateRead(store, committedV2) || committedV2.Schema != ReadSchema::V2 ||
            !IsCommittedCandidate(committedV2, active, 10U)) return 9;
    }

    // old -> candidate -> Rollback: all candidate Trial-time writes remain V1.
    // A reconstructed old runtime can read RollbackIntent and complete rollback.
    {
        AtomicMemoryStore store;
        OTAControlStore<Capacity> oldRuntime{store};
        if (oldRuntime.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 20;
        ActiveTransactionRecord active;
        if (!BeginToTrial(oldRuntime, 20U, active)) return 21;
        CandidateReadResult inherited;
        if (!CandidateRead(store, inherited) || inherited.Schema != ReadSchema::V1) return 22;

        OTAControlStore<Capacity> candidateTrial{store};
        if (candidateTrial.PersistRollbackIntent(active.Transaction) != OTADurableStatus::Success) return 23;
        OTAControlRecord<Capacity> rollbackReadable;
        if (!OldRuntimeRead(store, rollbackReadable) || rollbackReadable.Intent != DurableIntent::RollbackIntent ||
            rollbackReadable.Active.Point != RecoveryPoint::RollbackStarted) return 24;

        OTAControlStore<Capacity> rolledBackOldRuntime{store};
        if (rolledBackOldRuntime.FinalizeRollback(active.Transaction) != OTADurableStatus::Success) return 25;
        OTAControlRecord<Capacity> final;
        if (!OldRuntimeRead(store, final) || final.HasActiveTransaction ||
            final.Committed.Generation != UpdateGenerationId{1U} ||
            final.MinimumAcceptedSecurity != SecurityGeneration{0U}) return 26;
    }

    // Crash before commit-time migration: V1 committed truth remains complete.
    // New runtime recovery reads it deterministically and may retry migration.
    {
        AtomicMemoryStore store;
        OTAControlStore<Capacity> runtime{store};
        if (runtime.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 30;
        ActiveTransactionRecord active;
        if (!BeginToTrial(runtime, 30U, active) ||
            runtime.PersistCommitIntent(active.Transaction) != OTADurableStatus::Success ||
            runtime.FinalizeCommit(active.Transaction) != OTADurableStatus::Success) return 31;

        store.FaultNext(ReplaceFault::StorageFailureBefore);
        if (MigrateCommittedV1ToV2(store) != Persistence::AtomicRecordStatus::StorageFailure) return 32;
        CandidateReadResult recovered;
        if (!CandidateRead(store, recovered) || recovered.Schema != ReadSchema::V1 ||
            !IsCommittedCandidate(recovered, active, 30U)) return 33;
        if (MigrateCommittedV1ToV2(store) != Persistence::AtomicRecordStatus::Success) return 34;
        if (!CandidateRead(store, recovered) || recovered.Schema != ReadSchema::V2 ||
            !IsCommittedCandidate(recovered, active, 30U)) return 35;
    }

    // Crash/ambiguous acknowledgement after atomic migration: storage contains
    // the complete V2 record even though the caller cannot know the write result.
    // Recovery identifies the explicit schema and accepts the new committed truth.
    {
        AtomicMemoryStore store;
        OTAControlStore<Capacity> runtime{store};
        if (runtime.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 40;
        ActiveTransactionRecord active;
        if (!BeginToTrial(runtime, 40U, active) ||
            runtime.PersistCommitIntent(active.Transaction) != OTADurableStatus::Success ||
            runtime.FinalizeCommit(active.Transaction) != OTADurableStatus::Success) return 41;

        store.FaultNext(ReplaceFault::CommitAmbiguousAfter);
        if (MigrateCommittedV1ToV2(store) != Persistence::AtomicRecordStatus::CommitAmbiguous) return 42;
        CandidateReadResult recovered;
        if (!CandidateRead(store, recovered) || recovered.Schema != ReadSchema::V2 ||
            !IsCommittedCandidate(recovered, active, 40U)) return 43;
    }

    return 0;
}
