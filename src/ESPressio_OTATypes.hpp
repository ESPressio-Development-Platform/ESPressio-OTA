#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ESPressio::OTA {

namespace Detail {

template <std::size_t N>
constexpr bool FixedBytesEqual(const std::array<std::uint8_t, N>& left,
                               const std::array<std::uint8_t, N>& right) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (left[i] != right[i]) return false;
    }
    return true;
}

template <std::size_t N>
constexpr bool FixedBytesLess(const std::array<std::uint8_t, N>& left,
                              const std::array<std::uint8_t, N>& right) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (left[i] < right[i]) return true;
        if (left[i] > right[i]) return false;
    }
    return false;
}

template <typename TTag, typename TValue>
class StrongScalar final {
    TValue value_{};
public:
    constexpr StrongScalar() noexcept = default;
    constexpr explicit StrongScalar(TValue value) noexcept : value_(value) {}
    constexpr TValue Value() const noexcept { return value_; }
    constexpr explicit operator bool() const noexcept { return value_ != TValue{}; }
    constexpr bool operator==(StrongScalar other) const noexcept { return value_ == other.value_; }
    constexpr bool operator!=(StrongScalar other) const noexcept { return !(*this == other); }
    constexpr bool operator<(StrongScalar other) const noexcept { return value_ < other.value_; }
    constexpr bool operator<=(StrongScalar other) const noexcept { return value_ <= other.value_; }
    constexpr bool operator>(StrongScalar other) const noexcept { return value_ > other.value_; }
    constexpr bool operator>=(StrongScalar other) const noexcept { return value_ >= other.value_; }
};

template <typename TTag>
class Strong128 final {
    std::array<std::uint8_t, 16> bytes_{};
public:
    constexpr Strong128() noexcept = default;
    constexpr explicit Strong128(std::array<std::uint8_t, 16> bytes) noexcept : bytes_(bytes) {}
    constexpr const std::array<std::uint8_t, 16>& Bytes() const noexcept { return bytes_; }
    constexpr explicit operator bool() const noexcept {
        for (auto byte : bytes_) if (byte != 0U) return true;
        return false;
    }
    constexpr bool operator==(const Strong128& other) const noexcept {
        return FixedBytesEqual(bytes_, other.bytes_);
    }
    constexpr bool operator!=(const Strong128& other) const noexcept { return !(*this == other); }
    constexpr bool operator<(const Strong128& other) const noexcept {
        return FixedBytesLess(bytes_, other.bytes_);
    }
};

} // namespace Detail

struct ComponentTypeIdTag;
struct StageTypeIdTag;
struct HealthConditionTypeIdTag;
struct PolicyDecisionPointTypeIdTag;
struct ReleaseIdentifierTag;
struct ReleaseChannelIdentifierTag;
struct UpdateGenerationIdTag;
struct UpdateTransactionIdTag;
struct SecurityGenerationTag;
struct ManifestSchemaVersionTag;
struct OTAProtocolVersionTag;
struct OTADurableSchemaVersionTag;
struct ComponentIdentifierTag;
struct ArtifactIdentifierTag;
struct ManifestIdentifierTag;
struct UpdateTargetProfileFingerprintTag;

using ComponentTypeId = Detail::StrongScalar<ComponentTypeIdTag, std::uint64_t>;
using StageTypeId = Detail::StrongScalar<StageTypeIdTag, std::uint64_t>;
using HealthConditionTypeId = Detail::StrongScalar<HealthConditionTypeIdTag, std::uint64_t>;
using PolicyDecisionPointTypeId = Detail::StrongScalar<PolicyDecisionPointTypeIdTag, std::uint64_t>;
using ReleaseIdentifier = Detail::StrongScalar<ReleaseIdentifierTag, std::uint64_t>;
using ReleaseChannelIdentifier = Detail::StrongScalar<ReleaseChannelIdentifierTag, std::uint32_t>;
using UpdateGenerationId = Detail::StrongScalar<UpdateGenerationIdTag, std::uint64_t>;
using UpdateTransactionId = Detail::StrongScalar<UpdateTransactionIdTag, std::uint64_t>;
using SecurityGeneration = Detail::StrongScalar<SecurityGenerationTag, std::uint64_t>;
using ManifestSchemaVersion = Detail::StrongScalar<ManifestSchemaVersionTag, std::uint16_t>;
using OTAProtocolVersion = Detail::StrongScalar<OTAProtocolVersionTag, std::uint16_t>;
using OTADurableSchemaVersion = Detail::StrongScalar<OTADurableSchemaVersionTag, std::uint16_t>;
using ComponentIdentifier = Detail::StrongScalar<ComponentIdentifierTag, std::uint32_t>;
using ArtifactIdentifier = Detail::Strong128<ArtifactIdentifierTag>;
using ManifestIdentifier = Detail::Strong128<ManifestIdentifierTag>;

class UpdateTargetProfileFingerprint final {
    std::array<std::uint8_t, 32> bytes_{};
public:
    constexpr UpdateTargetProfileFingerprint() noexcept = default;
    constexpr explicit UpdateTargetProfileFingerprint(std::array<std::uint8_t, 32> bytes) noexcept : bytes_(bytes) {}
    constexpr const std::array<std::uint8_t, 32>& Bytes() const noexcept { return bytes_; }
    constexpr explicit operator bool() const noexcept {
        for (auto byte : bytes_) if (byte != 0U) return true;
        return false;
    }
    constexpr bool operator==(const UpdateTargetProfileFingerprint& other) const noexcept {
        return Detail::FixedBytesEqual(bytes_, other.bytes_);
    }
    constexpr bool operator!=(const UpdateTargetProfileFingerprint& other) const noexcept { return !(*this == other); }
};

static_assert(sizeof(ComponentTypeId) == 8U);
static_assert(sizeof(StageTypeId) == 8U);
static_assert(sizeof(HealthConditionTypeId) == 8U);
static_assert(sizeof(PolicyDecisionPointTypeId) == 8U);
static_assert(sizeof(ReleaseIdentifier) == 8U);
static_assert(sizeof(ReleaseChannelIdentifier) == 4U);
static_assert(sizeof(UpdateGenerationId) == 8U);
static_assert(sizeof(UpdateTransactionId) == 8U);
static_assert(sizeof(SecurityGeneration) == 8U);
static_assert(sizeof(ManifestSchemaVersion) == 2U);
static_assert(sizeof(OTAProtocolVersion) == 2U);
static_assert(sizeof(OTADurableSchemaVersion) == 2U);
static_assert(sizeof(ComponentIdentifier) == 4U);
static_assert(sizeof(ArtifactIdentifier) == 16U);
static_assert(sizeof(ManifestIdentifier) == 16U);
static_assert(sizeof(UpdateTargetProfileFingerprint) == 32U);

inline constexpr ManifestSchemaVersion ManifestSchemaV1{1U};
inline constexpr OTAProtocolVersion OTAProtocolV1{1U};
inline constexpr OTADurableSchemaVersion OTADurableSchemaV1{1U};

using OTAFeatureFlags = std::uint64_t;

enum class OutcomeClass : std::uint8_t {
    Success,
    Pending,
    Deferred,
    Rejected,
    Unsupported,
    Invalid,
    CapacityUnavailable,
    Unavailable,
    VerificationFailed,
    PersistenceFailed,
    PlatformFailed,
    Failed
};

enum class DiagnosticDomain : std::uint8_t {
    OTA,
    Policy,
    Security,
    Persistence,
    Platform,
    Source,
    Store,
    Distributor,
    Catalog,
    Manifest,
    Component,
    Health
};

enum class DiagnosticContextKey : std::uint16_t {
    None,
    ArtifactIndex,
    ComponentIdentifier,
    ComponentTypeId,
    ManifestIdentifierHigh,
    ManifestIdentifierLow,
    TransactionId,
    GenerationId,
    ExpectedBytes,
    ActualBytes,
    RequiredCapacity,
    AvailableCapacity,
    RetryAttempt,
    RecipientIndex
};

struct DiagnosticContextEntry final {
    DiagnosticContextKey Key{DiagnosticContextKey::None};
    std::uint64_t Value{0};
};

struct Diagnostic final {
    DiagnosticDomain Domain{DiagnosticDomain::OTA};
    std::uint32_t Reason{0};
    std::int32_t NativeCode{0};
    std::array<DiagnosticContextEntry, 4> Context{};
    std::uint8_t ContextCount{0};

    constexpr bool Add(DiagnosticContextKey key, std::uint64_t value) noexcept {
        if (ContextCount >= Context.size()) return false;
        Context[ContextCount++] = {key, value};
        return true;
    }
};

struct Result final {
    OutcomeClass Outcome{OutcomeClass::Failed};
    Diagnostic Detail{};
    constexpr explicit operator bool() const noexcept { return Outcome == OutcomeClass::Success; }
    static constexpr Result Success() noexcept { return {OutcomeClass::Success, {}}; }
};

enum class UpdateOperation : std::uint8_t {
    Check,
    Acquire,
    Distribute,
    Verify,
    Stage,
    Activate,
    Commit,
    Rollback
};

enum class ComponentKind : std::uint8_t {
    ApplicationFirmware,
    Filesystem,
    Bootloader,
    PartitionLayout,
    Data,
    Configuration,
    PlatformSpecific
};

enum class ComponentMultiplicity : std::uint8_t {
    Single,
    Multiple
};

enum class ProgressMode : std::uint8_t {
    Determinate,
    Indeterminate,
    Discrete
};

enum class ProgressUnit : std::uint8_t {
    None,
    Bytes,
    Items,
    Steps
};

enum class CompatibilityResult : std::uint8_t {
    DefinitelyIncompatible,
    PossiblyCompatible,
    Unknown
};

enum class PolicyVerdict : std::uint8_t {
    Allow,
    Defer,
    Reject
};

enum class HealthResult : std::uint8_t {
    Pass,
    Pending,
    Fail
};

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

} // namespace ESPressio::OTA
