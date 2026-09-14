#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "ESPressio_OTACapacityProfile.hpp"
#include "ESPressio_OTACheckpoint.hpp"
#include "ESPressio_OTATypes.hpp"
#include "ESPressio_IAtomicRecordStore.hpp"

namespace ESPressio::OTA {

enum class RecoveryPoint : std::uint8_t {
    None,
    TransactionCreated,
    ManifestAccepted,
    ArtifactsAcquired,
    ArtifactsVerified,
    StagingStarted,
    Staged,
    ActivationSelected,
    TrialBootEntered,
    CommitStarted,
    Committed,
    RollbackStarted,
    RolledBack
};

enum class DurableIntent : std::uint8_t {
    None,
    ActivationArmed,
    CommitIntent,
    RollbackIntent,
    RecoveryRequired
};

enum class OTADurableStatus : std::uint8_t {
    Success,
    NotFound,
    AlreadyProvisioned,
    Invalid,
    UnsupportedBackend,
    UnsupportedSchema,
    CapacityUnavailable,
    Corrupt,
    Busy,
    CommitAmbiguous,
    StorageFailure,
    Exhausted,
    ActiveTransactionExists,
    NoActiveTransaction,
    TransactionMismatch,
    InvalidTransition,
    SecurityRollbackRejected
};

struct CommittedBaseline final {
    UpdateGenerationId Generation{};
    bool HasRelease{false};
    ReleaseIdentifier Release{};
    bool HasManifest{false};
    ManifestIdentifier Manifest{};
    SecurityGeneration Security{};

    constexpr bool IsValid() const noexcept {
        if (!Generation) return false;
        if (HasRelease != bool(Release)) return false;
        if (HasManifest != bool(Manifest)) return false;
        return true;
    }
};

struct ActiveTransactionRecord final {
    UpdateTransactionId Transaction{};
    UpdateGenerationId CandidateGeneration{};
    UpdateGenerationId PreviousCommittedGeneration{};
    ReleaseIdentifier Release{};
    ManifestIdentifier Manifest{};
    SecurityGeneration CandidateSecurity{};
    RecoveryPoint Point{RecoveryPoint::None};

    constexpr bool IsValid() const noexcept {
        return bool(Transaction) && bool(CandidateGeneration) && bool(PreviousCommittedGeneration) &&
               bool(Release) && bool(Manifest) && Point != RecoveryPoint::None &&
               Point != RecoveryPoint::Committed && Point != RecoveryPoint::RolledBack;
    }
};

template<typename TCapacityProfile>
struct OTAControlRecord final {
    static_assert(TCapacityProfile::IsValid, "OTAControlRecord requires a valid OTA capacity profile");

    OTADurableSchemaVersion SchemaVersion{OTADurableSchemaV1};
    UpdateTransactionId NextTransaction{1U};
    UpdateGenerationId NextGeneration{};
    bool TransactionIdsExhausted{false};
    bool GenerationIdsExhausted{false};
    SecurityGeneration MinimumAcceptedSecurity{};
    CommittedBaseline Committed{};
    bool HasActiveTransaction{false};
    ActiveTransactionRecord Active{};
    DurableIntent Intent{DurableIntent::None};

    constexpr bool IsValid() const noexcept {
        if (SchemaVersion != OTADurableSchemaV1 || !NextTransaction || !NextGeneration || !Committed.IsValid()) return false;
        if (MinimumAcceptedSecurity > Committed.Security) return false;
        if (!TransactionIdsExhausted && HasActiveTransaction && NextTransaction <= Active.Transaction) return false;
        if (!GenerationIdsExhausted && NextGeneration <= Committed.Generation) return false;
        if (HasActiveTransaction) {
            if (!Active.IsValid() || Active.PreviousCommittedGeneration != Committed.Generation ||
                Active.CandidateGeneration == Committed.Generation ||
                Active.CandidateSecurity < MinimumAcceptedSecurity) return false;
            if (!GenerationIdsExhausted && NextGeneration <= Active.CandidateGeneration) return false;
        } else {
            if (Active.IsValid()) return false;
            if (Intent != DurableIntent::None && Intent != DurableIntent::RecoveryRequired) return false;
        }
        return true;
    }
};

inline constexpr std::array<std::uint8_t, 4> OTAControlMagic{{'O', 'T', 'A', 'C'}};
inline constexpr std::size_t OTAControlEncodedBytesV1 = 136U;
static_assert(OTAControlEncodedBytesV1 <= ConstrainedV1CapacityProfile::MaximumOTAControlRecordBytes,
              "OTA control record must fit the locked constrained durable-record capacity");

namespace DurableDetail {

template<typename T>
constexpr void WriteLE(std::uint8_t*& output, T value) noexcept {
    for (std::size_t i = 0U; i < sizeof(T); ++i) {
        *output++ = static_cast<std::uint8_t>(value & static_cast<T>(0xFFU));
        value = static_cast<T>(value >> 8U);
    }
}

template<typename T>
constexpr bool ReadLE(const std::uint8_t*& input, const std::uint8_t* end, T& value) noexcept {
    if (static_cast<std::size_t>(end - input) < sizeof(T)) return false;
    value = 0;
    for (std::size_t i = 0U; i < sizeof(T); ++i) {
        value = static_cast<T>(value | static_cast<T>(input[i]) << (i * 8U));
    }
    input += sizeof(T);
    return true;
}

inline std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0U; i < size; ++i) {
        crc ^= data[i];
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U & static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1U))));
        }
    }
    return ~crc;
}

inline OTADurableStatus MapPersistenceStatus(Persistence::AtomicRecordStatus status) noexcept {
    switch (status) {
        case Persistence::AtomicRecordStatus::Success: return OTADurableStatus::Success;
        case Persistence::AtomicRecordStatus::NotFound: return OTADurableStatus::NotFound;
        case Persistence::AtomicRecordStatus::InvalidArgument: return OTADurableStatus::Invalid;
        case Persistence::AtomicRecordStatus::NotSupported: return OTADurableStatus::UnsupportedBackend;
        case Persistence::AtomicRecordStatus::BufferTooSmall: return OTADurableStatus::CapacityUnavailable;
        case Persistence::AtomicRecordStatus::NoSpace: return OTADurableStatus::CapacityUnavailable;
        case Persistence::AtomicRecordStatus::Busy: return OTADurableStatus::Busy;
        case Persistence::AtomicRecordStatus::Corrupt: return OTADurableStatus::Corrupt;
        case Persistence::AtomicRecordStatus::CommitAmbiguous: return OTADurableStatus::CommitAmbiguous;
        case Persistence::AtomicRecordStatus::StorageFailure: return OTADurableStatus::StorageFailure;
    }
    return OTADurableStatus::StorageFailure;
}

inline constexpr bool IsForwardRecoveryPoint(RecoveryPoint point) noexcept {
    return point == RecoveryPoint::ManifestAccepted || point == RecoveryPoint::ArtifactsAcquired ||
           point == RecoveryPoint::ArtifactsVerified || point == RecoveryPoint::StagingStarted ||
           point == RecoveryPoint::Staged;
}

inline constexpr std::uint8_t EncodeFlags(bool active, bool txExhausted, bool generationExhausted,
                                          bool hasRelease, bool hasManifest) noexcept {
    return static_cast<std::uint8_t>((active ? 0x01U : 0U) |
                                     (txExhausted ? 0x02U : 0U) |
                                     (generationExhausted ? 0x04U : 0U) |
                                     (hasRelease ? 0x08U : 0U) |
                                     (hasManifest ? 0x10U : 0U));
}

} // namespace DurableDetail

template<typename TCapacityProfile>
OTADurableStatus SerializeOTAControlRecord(
    const OTAControlRecord<TCapacityProfile>& record,
    std::uint8_t* output,
    std::size_t capacity,
    std::size_t& written) noexcept {
    written = 0U;
    if (!record.IsValid()) return OTADurableStatus::Invalid;
    if (output == nullptr || capacity < OTAControlEncodedBytesV1 ||
        OTAControlEncodedBytesV1 > TCapacityProfile::MaximumOTAControlRecordBytes) {
        return OTADurableStatus::CapacityUnavailable;
    }

    auto* cursor = output;
    for (const auto byte : OTAControlMagic) *cursor++ = byte;
    DurableDetail::WriteLE(cursor, record.SchemaVersion.Value());
    *cursor++ = DurableDetail::EncodeFlags(record.HasActiveTransaction, record.TransactionIdsExhausted,
                                           record.GenerationIdsExhausted, record.Committed.HasRelease,
                                           record.Committed.HasManifest);
    *cursor++ = static_cast<std::uint8_t>(record.Intent);
    DurableDetail::WriteLE(cursor, record.NextTransaction.Value());
    DurableDetail::WriteLE(cursor, record.NextGeneration.Value());
    DurableDetail::WriteLE(cursor, record.MinimumAcceptedSecurity.Value());
    DurableDetail::WriteLE(cursor, record.Committed.Generation.Value());
    DurableDetail::WriteLE(cursor, record.Committed.Release.Value());
    for (const auto byte : record.Committed.Manifest.Bytes()) *cursor++ = byte;
    DurableDetail::WriteLE(cursor, record.Committed.Security.Value());
    DurableDetail::WriteLE(cursor, record.Active.Transaction.Value());
    DurableDetail::WriteLE(cursor, record.Active.CandidateGeneration.Value());
    DurableDetail::WriteLE(cursor, record.Active.PreviousCommittedGeneration.Value());
    DurableDetail::WriteLE(cursor, record.Active.Release.Value());
    for (const auto byte : record.Active.Manifest.Bytes()) *cursor++ = byte;
    DurableDetail::WriteLE(cursor, record.Active.CandidateSecurity.Value());
    *cursor++ = static_cast<std::uint8_t>(record.Active.Point);
    *cursor++ = 0U;
    DurableDetail::WriteLE(cursor, static_cast<std::uint16_t>(0U));

    const auto payloadBytes = static_cast<std::size_t>(cursor - output);
    const auto crc = DurableDetail::Crc32(output, payloadBytes);
    DurableDetail::WriteLE(cursor, crc);
    written = static_cast<std::size_t>(cursor - output);
    return written == OTAControlEncodedBytesV1 ? OTADurableStatus::Success : OTADurableStatus::Corrupt;
}

template<typename TCapacityProfile>
OTADurableStatus DeserializeOTAControlRecord(
    const std::uint8_t* data,
    std::size_t size,
    OTAControlRecord<TCapacityProfile>& output) noexcept {
    if (data == nullptr || size != OTAControlEncodedBytesV1 ||
        size > TCapacityProfile::MaximumOTAControlRecordBytes) return OTADurableStatus::Corrupt;

    const std::uint32_t expectedCrc = DurableDetail::Crc32(data, size - sizeof(std::uint32_t));
    const auto* crcCursor = data + size - sizeof(std::uint32_t);
    const auto* crcEnd = data + size;
    std::uint32_t storedCrc = 0U;
    if (!DurableDetail::ReadLE(crcCursor, crcEnd, storedCrc) || storedCrc != expectedCrc) {
        return OTADurableStatus::Corrupt;
    }

    const auto* cursor = data;
    const auto* end = data + size - sizeof(std::uint32_t);
    for (const auto expected : OTAControlMagic) {
        if (cursor == end || *cursor++ != expected) return OTADurableStatus::Corrupt;
    }

    std::uint16_t schema = 0U;
    if (!DurableDetail::ReadLE(cursor, end, schema)) return OTADurableStatus::Corrupt;
    if (schema != OTADurableSchemaV1.Value()) return OTADurableStatus::UnsupportedSchema;
    if (static_cast<std::size_t>(end - cursor) < 2U) return OTADurableStatus::Corrupt;
    const std::uint8_t flags = *cursor++;
    const auto intentRaw = *cursor++;
    if ((flags & 0xE0U) != 0U || intentRaw > static_cast<std::uint8_t>(DurableIntent::RecoveryRequired)) {
        return OTADurableStatus::Corrupt;
    }

    std::uint64_t nextTransaction = 0U;
    std::uint64_t nextGeneration = 0U;
    std::uint64_t minimumSecurity = 0U;
    std::uint64_t committedGeneration = 0U;
    std::uint64_t committedRelease = 0U;
    std::array<std::uint8_t, 16> committedManifest{};
    std::uint64_t committedSecurity = 0U;
    std::uint64_t activeTransaction = 0U;
    std::uint64_t activeGeneration = 0U;
    std::uint64_t previousGeneration = 0U;
    std::uint64_t activeRelease = 0U;
    std::array<std::uint8_t, 16> activeManifest{};
    std::uint64_t activeSecurity = 0U;

    if (!DurableDetail::ReadLE(cursor, end, nextTransaction) ||
        !DurableDetail::ReadLE(cursor, end, nextGeneration) ||
        !DurableDetail::ReadLE(cursor, end, minimumSecurity) ||
        !DurableDetail::ReadLE(cursor, end, committedGeneration) ||
        !DurableDetail::ReadLE(cursor, end, committedRelease)) return OTADurableStatus::Corrupt;
    if (static_cast<std::size_t>(end - cursor) < committedManifest.size()) return OTADurableStatus::Corrupt;
    for (auto& byte : committedManifest) byte = *cursor++;
    if (!DurableDetail::ReadLE(cursor, end, committedSecurity) ||
        !DurableDetail::ReadLE(cursor, end, activeTransaction) ||
        !DurableDetail::ReadLE(cursor, end, activeGeneration) ||
        !DurableDetail::ReadLE(cursor, end, previousGeneration) ||
        !DurableDetail::ReadLE(cursor, end, activeRelease)) return OTADurableStatus::Corrupt;
    if (static_cast<std::size_t>(end - cursor) < activeManifest.size()) return OTADurableStatus::Corrupt;
    for (auto& byte : activeManifest) byte = *cursor++;
    if (!DurableDetail::ReadLE(cursor, end, activeSecurity) || cursor == end) return OTADurableStatus::Corrupt;
    const auto recoveryRaw = *cursor++;
    if (recoveryRaw > static_cast<std::uint8_t>(RecoveryPoint::RolledBack)) return OTADurableStatus::Corrupt;
    if (cursor == end) return OTADurableStatus::Corrupt;
    if (*cursor++ != 0U) return OTADurableStatus::Corrupt;
    std::uint16_t reserved = 0U;
    if (!DurableDetail::ReadLE(cursor, end, reserved) || reserved != 0U || cursor != end) return OTADurableStatus::Corrupt;

    OTAControlRecord<TCapacityProfile> candidate;
    candidate.SchemaVersion = OTADurableSchemaVersion{schema};
    candidate.NextTransaction = UpdateTransactionId{nextTransaction};
    candidate.NextGeneration = UpdateGenerationId{nextGeneration};
    candidate.TransactionIdsExhausted = (flags & 0x02U) != 0U;
    candidate.GenerationIdsExhausted = (flags & 0x04U) != 0U;
    candidate.MinimumAcceptedSecurity = SecurityGeneration{minimumSecurity};
    candidate.Committed.Generation = UpdateGenerationId{committedGeneration};
    candidate.Committed.HasRelease = (flags & 0x08U) != 0U;
    candidate.Committed.Release = ReleaseIdentifier{committedRelease};
    candidate.Committed.HasManifest = (flags & 0x10U) != 0U;
    candidate.Committed.Manifest = ManifestIdentifier{committedManifest};
    candidate.Committed.Security = SecurityGeneration{committedSecurity};
    candidate.HasActiveTransaction = (flags & 0x01U) != 0U;
    candidate.Active.Transaction = UpdateTransactionId{activeTransaction};
    candidate.Active.CandidateGeneration = UpdateGenerationId{activeGeneration};
    candidate.Active.PreviousCommittedGeneration = UpdateGenerationId{previousGeneration};
    candidate.Active.Release = ReleaseIdentifier{activeRelease};
    candidate.Active.Manifest = ManifestIdentifier{activeManifest};
    candidate.Active.CandidateSecurity = SecurityGeneration{activeSecurity};
    candidate.Active.Point = static_cast<RecoveryPoint>(recoveryRaw);
    candidate.Intent = static_cast<DurableIntent>(intentRaw);

    if (!candidate.IsValid()) return OTADurableStatus::Corrupt;
    output = candidate;
    return OTADurableStatus::Success;
}

template<typename TCapacityProfile>
class OTAControlStore final {
    Persistence::IAtomicRecordStore& store_;
    inline static constexpr Persistence::AtomicRecordKey Key = [] {
        Persistence::AtomicRecordKey key;
        Persistence::AtomicRecordKey::TryCreate("ota.control.v1", key);
        return key;
    }();

    OTADurableStatus Ready() noexcept {
        const auto capabilities = store_.Capabilities();
        if (!capabilities.DurableOldOrNew || !capabilities.BoundedOperations ||
            capabilities.MaximumRecordBytes < OTAControlEncodedBytesV1 || capabilities.MaximumRecords == 0U) {
            return OTADurableStatus::UnsupportedBackend;
        }
        return DurableDetail::MapPersistenceStatus(store_.Recover());
    }

    OTADurableStatus ReadCommitted(OTAControlRecord<TCapacityProfile>& output) noexcept {
        std::array<std::uint8_t, OTAControlEncodedBytesV1> bytes{};
        std::size_t bytesRead = 0U;
        const auto status = DurableDetail::MapPersistenceStatus(
            store_.Read(Key, bytes.data(), bytes.size(), bytesRead));
        if (status != OTADurableStatus::Success) return status;
        return DeserializeOTAControlRecord(bytes.data(), bytesRead, output);
    }

    OTADurableStatus Commit(const OTAControlRecord<TCapacityProfile>& record) noexcept {
        std::array<std::uint8_t, OTAControlEncodedBytesV1> bytes{};
        std::size_t written = 0U;
        const auto encoded = SerializeOTAControlRecord(record, bytes.data(), bytes.size(), written);
        if (encoded != OTADurableStatus::Success) return encoded;
        return DurableDetail::MapPersistenceStatus(store_.ReplaceAtomically(Key, bytes.data(), written));
    }

    static void ClearActive(OTAControlRecord<TCapacityProfile>& record) noexcept {
        record.HasActiveTransaction = false;
        record.Active = {};
        record.Intent = DurableIntent::None;
    }

public:
    explicit OTAControlStore(Persistence::IAtomicRecordStore& store) noexcept : store_(store) {}

    static constexpr Persistence::AtomicRecordKey RecordKey() noexcept { return Key; }

    OTADurableStatus Load(OTAControlRecord<TCapacityProfile>& output) noexcept {
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        return ReadCommitted(output);
    }

    OTADurableStatus ProvisionBaseline(
        const CommittedBaseline& baseline,
        SecurityGeneration minimumAccepted) noexcept {
        if (!baseline.IsValid() || minimumAccepted > baseline.Security) return OTADurableStatus::Invalid;
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;

        OTAControlRecord<TCapacityProfile> existing;
        const auto read = ReadCommitted(existing);
        if (read == OTADurableStatus::Success) return OTADurableStatus::AlreadyProvisioned;
        if (read != OTADurableStatus::NotFound) return read;

        OTAControlRecord<TCapacityProfile> record;
        record.Committed = baseline;
        record.MinimumAcceptedSecurity = minimumAccepted;
        record.NextTransaction = UpdateTransactionId{1U};
        if (baseline.Generation.Value() == std::numeric_limits<std::uint64_t>::max()) {
            record.NextGeneration = baseline.Generation;
            record.GenerationIdsExhausted = true;
        } else {
            record.NextGeneration = UpdateGenerationId{baseline.Generation.Value() + 1U};
        }
        return Commit(record);
    }

    OTADurableStatus BeginTransaction(
        ReleaseIdentifier release,
        ManifestIdentifier manifest,
        SecurityGeneration candidateSecurity,
        ActiveTransactionRecord& allocated) noexcept {
        allocated = {};
        if (!release || !manifest) return OTADurableStatus::Invalid;

        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        OTAControlRecord<TCapacityProfile> record;
        auto status = ReadCommitted(record);
        if (status != OTADurableStatus::Success) return status;
        if (record.Intent == DurableIntent::RecoveryRequired) return OTADurableStatus::InvalidTransition;
        if (record.HasActiveTransaction) return OTADurableStatus::ActiveTransactionExists;
        if (candidateSecurity < record.MinimumAcceptedSecurity) return OTADurableStatus::SecurityRollbackRejected;
        if (record.TransactionIdsExhausted || record.GenerationIdsExhausted) return OTADurableStatus::Exhausted;

        ActiveTransactionRecord candidate;
        candidate.Transaction = record.NextTransaction;
        candidate.CandidateGeneration = record.NextGeneration;
        candidate.PreviousCommittedGeneration = record.Committed.Generation;
        candidate.Release = release;
        candidate.Manifest = manifest;
        candidate.CandidateSecurity = candidateSecurity;
        candidate.Point = RecoveryPoint::TransactionCreated;

        if (record.NextTransaction.Value() == std::numeric_limits<std::uint64_t>::max()) {
            record.TransactionIdsExhausted = true;
        } else {
            record.NextTransaction = UpdateTransactionId{record.NextTransaction.Value() + 1U};
        }
        if (record.NextGeneration.Value() == std::numeric_limits<std::uint64_t>::max()) {
            record.GenerationIdsExhausted = true;
        } else {
            record.NextGeneration = UpdateGenerationId{record.NextGeneration.Value() + 1U};
        }
        record.HasActiveTransaction = true;
        record.Active = candidate;
        record.Intent = DurableIntent::None;

        status = Commit(record);
        if (status == OTADurableStatus::Success) allocated = candidate;
        return status;
    }

    OTADurableStatus AdvanceRecoveryPoint(UpdateTransactionId transaction, RecoveryPoint point) noexcept {
        if (!transaction || !DurableDetail::IsForwardRecoveryPoint(point)) return OTADurableStatus::Invalid;
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        OTAControlRecord<TCapacityProfile> record;
        auto status = ReadCommitted(record);
        if (status != OTADurableStatus::Success) return status;
        if (!record.HasActiveTransaction) return OTADurableStatus::NoActiveTransaction;
        if (record.Active.Transaction != transaction) return OTADurableStatus::TransactionMismatch;
        if (record.Intent != DurableIntent::None ||
            static_cast<std::uint8_t>(point) <= static_cast<std::uint8_t>(record.Active.Point) ||
            record.Active.Point >= RecoveryPoint::ActivationSelected) return OTADurableStatus::InvalidTransition;
        record.Active.Point = point;
        return Commit(record);
    }

    OTADurableStatus ArmActivation(UpdateTransactionId transaction) noexcept {
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        OTAControlRecord<TCapacityProfile> record;
        auto status = ReadCommitted(record);
        if (status != OTADurableStatus::Success) return status;
        if (!record.HasActiveTransaction) return OTADurableStatus::NoActiveTransaction;
        if (record.Active.Transaction != transaction) return OTADurableStatus::TransactionMismatch;
        if (record.Intent != DurableIntent::None || record.Active.Point != RecoveryPoint::Staged) {
            return OTADurableStatus::InvalidTransition;
        }
        record.Active.Point = RecoveryPoint::ActivationSelected;
        record.Intent = DurableIntent::ActivationArmed;
        return Commit(record);
    }

    OTADurableStatus MarkTrialEntered(UpdateTransactionId transaction) noexcept {
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        OTAControlRecord<TCapacityProfile> record;
        auto status = ReadCommitted(record);
        if (status != OTADurableStatus::Success) return status;
        if (!record.HasActiveTransaction) return OTADurableStatus::NoActiveTransaction;
        if (record.Active.Transaction != transaction) return OTADurableStatus::TransactionMismatch;
        if (record.Intent != DurableIntent::ActivationArmed || record.Active.Point != RecoveryPoint::ActivationSelected) {
            return OTADurableStatus::InvalidTransition;
        }
        record.Active.Point = RecoveryPoint::TrialBootEntered;
        record.Intent = DurableIntent::None;
        return Commit(record);
    }

    OTADurableStatus PersistCommitIntent(UpdateTransactionId transaction) noexcept {
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        OTAControlRecord<TCapacityProfile> record;
        auto status = ReadCommitted(record);
        if (status != OTADurableStatus::Success) return status;
        if (!record.HasActiveTransaction) return OTADurableStatus::NoActiveTransaction;
        if (record.Active.Transaction != transaction) return OTADurableStatus::TransactionMismatch;
        if (record.Intent != DurableIntent::None ||
            (record.Active.Point != RecoveryPoint::TrialBootEntered &&
             record.Active.Point != RecoveryPoint::ActivationSelected)) {
            return OTADurableStatus::InvalidTransition;
        }
        record.Active.Point = RecoveryPoint::CommitStarted;
        record.Intent = DurableIntent::CommitIntent;
        return Commit(record);
    }

    OTADurableStatus FinalizeCommit(UpdateTransactionId transaction) noexcept {
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        OTAControlRecord<TCapacityProfile> record;
        auto status = ReadCommitted(record);
        if (status != OTADurableStatus::Success) return status;
        if (!record.HasActiveTransaction) return OTADurableStatus::NoActiveTransaction;
        if (record.Active.Transaction != transaction) return OTADurableStatus::TransactionMismatch;
        if (record.Intent != DurableIntent::CommitIntent || record.Active.Point != RecoveryPoint::CommitStarted) {
            return OTADurableStatus::InvalidTransition;
        }

        record.Committed.Generation = record.Active.CandidateGeneration;
        record.Committed.HasRelease = true;
        record.Committed.Release = record.Active.Release;
        record.Committed.HasManifest = true;
        record.Committed.Manifest = record.Active.Manifest;
        record.Committed.Security = record.Active.CandidateSecurity;
        if (record.MinimumAcceptedSecurity < record.Active.CandidateSecurity) {
            record.MinimumAcceptedSecurity = record.Active.CandidateSecurity;
        }
        ClearActive(record);
        return Commit(record);
    }

    OTADurableStatus PersistRollbackIntent(UpdateTransactionId transaction) noexcept {
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        OTAControlRecord<TCapacityProfile> record;
        auto status = ReadCommitted(record);
        if (status != OTADurableStatus::Success) return status;
        if (!record.HasActiveTransaction) return OTADurableStatus::NoActiveTransaction;
        if (record.Active.Transaction != transaction) return OTADurableStatus::TransactionMismatch;
        if (record.Intent == DurableIntent::CommitIntent) return OTADurableStatus::InvalidTransition;
        record.Active.Point = RecoveryPoint::RollbackStarted;
        record.Intent = DurableIntent::RollbackIntent;
        return Commit(record);
    }

    OTADurableStatus FinalizeRollback(UpdateTransactionId transaction) noexcept {
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        OTAControlRecord<TCapacityProfile> record;
        auto status = ReadCommitted(record);
        if (status != OTADurableStatus::Success) return status;
        if (!record.HasActiveTransaction) return OTADurableStatus::NoActiveTransaction;
        if (record.Active.Transaction != transaction) return OTADurableStatus::TransactionMismatch;
        if (record.Intent != DurableIntent::RollbackIntent || record.Active.Point != RecoveryPoint::RollbackStarted) {
            return OTADurableStatus::InvalidTransition;
        }
        ClearActive(record);
        return Commit(record);
    }

    OTADurableStatus MarkRecoveryRequired() noexcept {
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        OTAControlRecord<TCapacityProfile> record;
        auto status = ReadCommitted(record);
        if (status != OTADurableStatus::Success) return status;
        record.Intent = DurableIntent::RecoveryRequired;
        return Commit(record);
    }
};

template<typename TCapacityProfile>
class ArtifactCheckpointStore final {
    Persistence::IAtomicRecordStore& store_;

    static bool TryKey(std::size_t slot, Persistence::AtomicRecordKey& output) noexcept {
        if (slot >= TCapacityProfile::MaximumArtifactCheckpoints || slot > 0xFFFFU) return false;
        std::array<char, 14> raw{{'o','t','a','.','c','p','.','s','l','o','t','.',0,0}};
        raw[12] = static_cast<char>(slot & 0xFFU);
        raw[13] = static_cast<char>((slot >> 8U) & 0xFFU);
        return Persistence::AtomicRecordKey::TryCreate(std::string_view{raw.data(), raw.size()}, output);
    }

    OTADurableStatus Ready() noexcept {
        const auto capabilities = store_.Capabilities();
        if (!capabilities.DurableOldOrNew || !capabilities.BoundedOperations ||
            capabilities.MaximumRecordBytes < MaximumArtifactCheckpointEncodedBytes<TCapacityProfile> ||
            capabilities.MaximumRecords < TCapacityProfile::MaximumArtifactCheckpoints) {
            return OTADurableStatus::UnsupportedBackend;
        }
        return DurableDetail::MapPersistenceStatus(store_.Recover());
    }

    OTADurableStatus ReadSlot(std::size_t slot, ArtifactCheckpoint<TCapacityProfile>& output) noexcept {
        Persistence::AtomicRecordKey key;
        if (!TryKey(slot, key)) return OTADurableStatus::Invalid;
        std::array<std::uint8_t, TCapacityProfile::MaximumArtifactCheckpointRecordBytes> bytes{};
        std::size_t bytesRead = 0U;
        const auto read = DurableDetail::MapPersistenceStatus(store_.Read(key, bytes.data(), bytes.size(), bytesRead));
        if (read != OTADurableStatus::Success) return read;
        const auto decoded = DeserializeArtifactCheckpoint(bytes.data(), bytesRead, output);
        if (decoded == ArtifactCheckpointCodecStatus::Success) return OTADurableStatus::Success;
        if (decoded == ArtifactCheckpointCodecStatus::CapacityUnavailable) return OTADurableStatus::CapacityUnavailable;
        return OTADurableStatus::Corrupt;
    }

public:
    explicit ArtifactCheckpointStore(Persistence::IAtomicRecordStore& store) noexcept : store_(store) {}

    OTADurableStatus Save(std::size_t slot, const ArtifactCheckpoint<TCapacityProfile>& checkpoint) noexcept {
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        Persistence::AtomicRecordKey key;
        if (!TryKey(slot, key)) return OTADurableStatus::Invalid;
        std::array<std::uint8_t, TCapacityProfile::MaximumArtifactCheckpointRecordBytes> bytes{};
        std::size_t written = 0U;
        const auto encoded = SerializeArtifactCheckpoint(checkpoint, bytes.data(), bytes.size(), written);
        if (encoded == ArtifactCheckpointCodecStatus::Invalid) return OTADurableStatus::Invalid;
        if (encoded == ArtifactCheckpointCodecStatus::CapacityUnavailable) return OTADurableStatus::CapacityUnavailable;
        if (encoded != ArtifactCheckpointCodecStatus::Success) return OTADurableStatus::Corrupt;
        return DurableDetail::MapPersistenceStatus(store_.ReplaceAtomically(key, bytes.data(), written));
    }

    OTADurableStatus Load(std::size_t slot, ArtifactCheckpoint<TCapacityProfile>& output) noexcept {
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        return ReadSlot(slot, output);
    }

    OTADurableStatus Remove(std::size_t slot) noexcept {
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        Persistence::AtomicRecordKey key;
        if (!TryKey(slot, key)) return OTADurableStatus::Invalid;
        return DurableDetail::MapPersistenceStatus(store_.RemoveAfterCommit(key));
    }

    OTADurableStatus Find(
        UpdateTransactionId transaction,
        ArtifactIdentifier artifact,
        ArtifactCheckpoint<TCapacityProfile>& output,
        std::size_t& slot) noexcept {
        slot = TCapacityProfile::MaximumArtifactCheckpoints;
        if (!transaction || !artifact) return OTADurableStatus::Invalid;
        const auto ready = Ready();
        if (ready != OTADurableStatus::Success) return ready;
        for (std::size_t i = 0U; i < TCapacityProfile::MaximumArtifactCheckpoints; ++i) {
            ArtifactCheckpoint<TCapacityProfile> candidate;
            const auto status = ReadSlot(i, candidate);
            if (status == OTADurableStatus::NotFound) continue;
            if (status != OTADurableStatus::Success) return status;
            if (candidate.Transaction == transaction.Value() && candidate.Artifact == artifact.Bytes()) {
                output = candidate;
                slot = i;
                return OTADurableStatus::Success;
            }
        }
        return OTADurableStatus::NotFound;
    }
};

} // namespace ESPressio::OTA
