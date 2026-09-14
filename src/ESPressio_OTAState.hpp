#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ESPressio_OTACapacityProfile.hpp"
#include "ESPressio_OTATypes.hpp"

#include <ESPressio_PrimitivePolicy.hpp>
#include <ESPressio_SerializationMacros.hpp>
#include <ESPressio_States.hpp>
#include <ESPressio_TypeDirectory.hpp>

namespace ESPressio::OTA {

enum class CoordinatorAvailability : std::uint8_t {
    Ready,
    Busy,
    RecoveryRequired,
    Unavailable
};

enum class UpdateLifecycle : std::uint8_t {
    Idle,
    Checking,
    CandidateSelected,
    Preparing,
    Acquiring,
    Verifying,
    Staging,
    Staged,
    ActivationPending,
    Activating,
    AwaitingRestart,
    Trial,
    HealthValidation,
    Committing,
    Completed,
    Deferred,
    Cancelling,
    Cancelled,
    Failed,
    RollbackPending,
    RollingBack,
    RolledBack,
    RollbackFailed,
    RecoveryRequired
};

enum class TerminalUpdateOutcome : std::uint8_t {
    None,
    Completed,
    Cancelled,
    Failed,
    Unsupported,
    Rejected,
    CapacityUnavailable,
    RolledBack,
    RollbackFailed,
    RecoveryRequired
};

struct OTAStateConvergence final {
    using PolicyCategory = Primitive::StateConvergencePolicyTag;
    using RequiredEvidence = Primitive::NoRemoteEvidence;
    using Supersession = Primitive::LatestAuthoritativeValue;
    using ExhaustionDisposition = Primitive::DormantNeedsConvergence;
    static constexpr std::uint64_t MaximumResidenceNanoseconds = 0U;
    static constexpr std::uint16_t MaximumAttempts = 1U;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds = 0U;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds = 0U;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds = 0U;
};

struct CoordinatorAvailabilityValue final {
    CoordinatorAvailability Availability{CoordinatorAvailability::Unavailable};
    std::uint64_t ActiveTransaction{0U};

    constexpr bool operator==(const CoordinatorAvailabilityValue& other) const noexcept {
        return Availability == other.Availability && ActiveTransaction == other.ActiveTransaction;
    }

    ESPRESSIO_SERIALIZABLE_TYPE(CoordinatorAvailabilityValue)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("availability", Availability),
        ESPRESSIO_PROPERTY_REQUIRED("activeTransaction", ActiveTransaction))
};

struct ActiveUpdateTransactionValue final {
    bool Present{false};
    std::uint64_t Transaction{0U};
    std::uint64_t CandidateGeneration{0U};
    std::uint64_t Release{0U};
    std::array<std::uint8_t, 16> Manifest{};
    std::uint64_t CandidateSecurityGeneration{0U};

    constexpr bool operator==(const ActiveUpdateTransactionValue& other) const noexcept {
        return Present == other.Present && Transaction == other.Transaction &&
               CandidateGeneration == other.CandidateGeneration && Release == other.Release &&
               Detail::FixedBytesEqual(Manifest, other.Manifest) &&
               CandidateSecurityGeneration == other.CandidateSecurityGeneration;
    }

    constexpr bool IsCanonical() const noexcept {
        if (!Present) {
            return Transaction == 0U && CandidateGeneration == 0U && Release == 0U &&
                   !ManifestIdentifier{Manifest} && CandidateSecurityGeneration == 0U;
        }
        return Transaction != 0U && CandidateGeneration != 0U && Release != 0U && bool(ManifestIdentifier{Manifest});
    }

    ESPRESSIO_SERIALIZABLE_TYPE(ActiveUpdateTransactionValue)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("present", Present),
        ESPRESSIO_PROPERTY_REQUIRED("transaction", Transaction),
        ESPRESSIO_PROPERTY_REQUIRED("candidateGeneration", CandidateGeneration),
        ESPRESSIO_PROPERTY_REQUIRED("release", Release),
        ESPRESSIO_PROPERTY_REQUIRED("manifest", Manifest),
        ESPRESSIO_PROPERTY_REQUIRED("candidateSecurityGeneration", CandidateSecurityGeneration))
};

struct ActiveUpdateLifecycleValue final {
    bool Present{false};
    std::uint64_t Transaction{0U};
    UpdateLifecycle Lifecycle{UpdateLifecycle::Idle};

    constexpr bool operator==(const ActiveUpdateLifecycleValue& other) const noexcept {
        return Present == other.Present && Transaction == other.Transaction && Lifecycle == other.Lifecycle;
    }

    constexpr bool IsCanonical() const noexcept {
        return Present ? Transaction != 0U : Transaction == 0U && Lifecycle == UpdateLifecycle::Idle;
    }

    ESPRESSIO_SERIALIZABLE_TYPE(ActiveUpdateLifecycleValue)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("present", Present),
        ESPRESSIO_PROPERTY_REQUIRED("transaction", Transaction),
        ESPRESSIO_PROPERTY_REQUIRED("lifecycle", Lifecycle))
};

struct GenerationValue final {
    std::uint64_t Generation{0U};
    bool HasRelease{false};
    std::uint64_t Release{0U};
    bool HasManifest{false};
    std::array<std::uint8_t, 16> Manifest{};

    constexpr bool operator==(const GenerationValue& other) const noexcept {
        return Generation == other.Generation && HasRelease == other.HasRelease && Release == other.Release &&
               HasManifest == other.HasManifest && Detail::FixedBytesEqual(Manifest, other.Manifest);
    }

    constexpr bool IsCanonical() const noexcept {
        if (Generation == 0U) return false;
        if (HasRelease != (Release != 0U)) return false;
        if (HasManifest != bool(ManifestIdentifier{Manifest})) return false;
        return true;
    }

    ESPRESSIO_SERIALIZABLE_TYPE(GenerationValue)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("generation", Generation),
        ESPRESSIO_PROPERTY_REQUIRED("hasRelease", HasRelease),
        ESPRESSIO_PROPERTY_REQUIRED("release", Release),
        ESPRESSIO_PROPERTY_REQUIRED("hasManifest", HasManifest),
        ESPRESSIO_PROPERTY_REQUIRED("manifest", Manifest))
};

struct CommittedGenerationValue final {
    GenerationValue Identity{};
    std::uint64_t SecurityGeneration{0U};

    constexpr bool operator==(const CommittedGenerationValue& other) const noexcept {
        return Identity == other.Identity && SecurityGeneration == other.SecurityGeneration;
    }

    constexpr bool IsCanonical() const noexcept { return Identity.IsCanonical(); }

    ESPRESSIO_SERIALIZABLE_TYPE(CommittedGenerationValue)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("identity", Identity),
        ESPRESSIO_PROPERTY_REQUIRED("securityGeneration", SecurityGeneration))
};

struct CandidateGenerationValue final {
    bool Present{false};
    std::uint64_t Transaction{0U};
    std::uint64_t Generation{0U};
    std::uint64_t Release{0U};
    std::array<std::uint8_t, 16> Manifest{};
    std::uint64_t SecurityGeneration{0U};

    constexpr bool operator==(const CandidateGenerationValue& other) const noexcept {
        return Present == other.Present && Transaction == other.Transaction && Generation == other.Generation &&
               Release == other.Release && Detail::FixedBytesEqual(Manifest, other.Manifest) &&
               SecurityGeneration == other.SecurityGeneration;
    }

    constexpr bool IsCanonical() const noexcept {
        if (!Present) {
            return Transaction == 0U && Generation == 0U && Release == 0U &&
                   !ManifestIdentifier{Manifest} && SecurityGeneration == 0U;
        }
        return Transaction != 0U && Generation != 0U && Release != 0U && bool(ManifestIdentifier{Manifest});
    }

    ESPRESSIO_SERIALIZABLE_TYPE(CandidateGenerationValue)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("present", Present),
        ESPRESSIO_PROPERTY_REQUIRED("transaction", Transaction),
        ESPRESSIO_PROPERTY_REQUIRED("generation", Generation),
        ESPRESSIO_PROPERTY_REQUIRED("release", Release),
        ESPRESSIO_PROPERTY_REQUIRED("manifest", Manifest),
        ESPRESSIO_PROPERTY_REQUIRED("securityGeneration", SecurityGeneration))
};

struct MinimumAcceptedSecurityLevelValue final {
    std::uint64_t SecurityGeneration{0U};

    constexpr bool operator==(const MinimumAcceptedSecurityLevelValue& other) const noexcept {
        return SecurityGeneration == other.SecurityGeneration;
    }

    ESPRESSIO_SERIALIZABLE_TYPE(MinimumAcceptedSecurityLevelValue)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("securityGeneration", SecurityGeneration))
};

struct UpdateProgressValue final {
    bool Present{false};
    std::uint64_t Transaction{0U};
    UpdateOperation Operation{UpdateOperation::Check};
    std::uint64_t StageType{0U};
    std::uint16_t StagePosition{0U};
    ProgressMode Mode{ProgressMode::Indeterminate};
    ProgressUnit Unit{ProgressUnit::None};
    std::uint64_t Current{0U};
    std::uint64_t Total{0U};
    bool HasComponent{false};
    std::uint32_t Component{0U};

    constexpr bool operator==(const UpdateProgressValue& other) const noexcept {
        return Present == other.Present && Transaction == other.Transaction && Operation == other.Operation &&
               StageType == other.StageType && StagePosition == other.StagePosition && Mode == other.Mode &&
               Unit == other.Unit && Current == other.Current && Total == other.Total &&
               HasComponent == other.HasComponent && Component == other.Component;
    }

    constexpr bool IsCanonical() const noexcept {
        if (!Present) {
            return Transaction == 0U && StageType == 0U && StagePosition == 0U &&
                   Mode == ProgressMode::Indeterminate && Unit == ProgressUnit::None &&
                   Current == 0U && Total == 0U && !HasComponent && Component == 0U;
        }
        if (Transaction == 0U || StageType == 0U || HasComponent != (Component != 0U)) return false;
        if (Mode == ProgressMode::Indeterminate) return Current == 0U && Total == 0U;
        return Total != 0U && Current <= Total;
    }

    ESPRESSIO_SERIALIZABLE_TYPE(UpdateProgressValue)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("present", Present),
        ESPRESSIO_PROPERTY_REQUIRED("transaction", Transaction),
        ESPRESSIO_PROPERTY_REQUIRED("operation", Operation),
        ESPRESSIO_PROPERTY_REQUIRED("stageType", StageType),
        ESPRESSIO_PROPERTY_REQUIRED("stagePosition", StagePosition),
        ESPRESSIO_PROPERTY_REQUIRED("mode", Mode),
        ESPRESSIO_PROPERTY_REQUIRED("unit", Unit),
        ESPRESSIO_PROPERTY_REQUIRED("current", Current),
        ESPRESSIO_PROPERTY_REQUIRED("total", Total),
        ESPRESSIO_PROPERTY_REQUIRED("hasComponent", HasComponent),
        ESPRESSIO_PROPERTY_REQUIRED("component", Component))
};

struct OutcomeDiagnosticContextValue final {
    DiagnosticContextKey Key{DiagnosticContextKey::None};
    std::uint64_t Value{0U};

    constexpr bool operator==(const OutcomeDiagnosticContextValue& other) const noexcept {
        return Key == other.Key && Value == other.Value;
    }

    ESPRESSIO_SERIALIZABLE_TYPE(OutcomeDiagnosticContextValue)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("key", Key),
        ESPRESSIO_PROPERTY_REQUIRED("value", Value))
};

struct LastUpdateOutcomeValue final {
    bool Present{false};
    std::uint64_t Transaction{0U};
    TerminalUpdateOutcome Outcome{TerminalUpdateOutcome::None};
    UpdateOperation Operation{UpdateOperation::Check};
    DiagnosticDomain Domain{DiagnosticDomain::OTA};
    std::uint32_t Reason{0U};
    std::int32_t NativeCode{0};
    std::array<OutcomeDiagnosticContextValue, 4> Context{};
    std::uint8_t ContextCount{0U};

    constexpr bool operator==(const LastUpdateOutcomeValue& other) const noexcept {
        if (Present != other.Present || Transaction != other.Transaction || Outcome != other.Outcome ||
            Operation != other.Operation || Domain != other.Domain || Reason != other.Reason ||
            NativeCode != other.NativeCode || ContextCount != other.ContextCount) return false;
        for (std::size_t i = 0U; i < Context.size(); ++i) if (!(Context[i] == other.Context[i])) return false;
        return true;
    }

    constexpr bool IsCanonical() const noexcept {
        if (ContextCount > Context.size()) return false;
        if (!Present) return Transaction == 0U && Outcome == TerminalUpdateOutcome::None && Reason == 0U &&
                             NativeCode == 0 && ContextCount == 0U;
        return Transaction != 0U && Outcome != TerminalUpdateOutcome::None;
    }

    static LastUpdateOutcomeValue FromResult(
        UpdateTransactionId transaction,
        TerminalUpdateOutcome outcome,
        UpdateOperation operation,
        const Result& result) noexcept {
        LastUpdateOutcomeValue value;
        value.Present = true;
        value.Transaction = transaction.Value();
        value.Outcome = outcome;
        value.Operation = operation;
        value.Domain = result.Detail.Domain;
        value.Reason = result.Detail.Reason;
        value.NativeCode = result.Detail.NativeCode;
        value.ContextCount = result.Detail.ContextCount > value.Context.size()
            ? static_cast<std::uint8_t>(value.Context.size())
            : result.Detail.ContextCount;
        for (std::size_t i = 0U; i < value.ContextCount; ++i) {
            value.Context[i] = {result.Detail.Context[i].Key, result.Detail.Context[i].Value};
        }
        return value;
    }

    ESPRESSIO_SERIALIZABLE_TYPE(LastUpdateOutcomeValue)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("present", Present),
        ESPRESSIO_PROPERTY_REQUIRED("transaction", Transaction),
        ESPRESSIO_PROPERTY_REQUIRED("outcome", Outcome),
        ESPRESSIO_PROPERTY_REQUIRED("operation", Operation),
        ESPRESSIO_PROPERTY_REQUIRED("domain", Domain),
        ESPRESSIO_PROPERTY_REQUIRED("reason", Reason),
        ESPRESSIO_PROPERTY_REQUIRED("nativeCode", NativeCode),
        ESPRESSIO_PROPERTY_REQUIRED("context", Context),
        ESPRESSIO_PROPERTY_REQUIRED("contextCount", ContextCount))
};

#define ESPRESSIO_OTA_DECLARE_STATE(TName, TValue, TId, TCanonicalName) \
    struct TName final : State::TransmissibleState<TName, TValue> { \
        static constexpr ::ESPressio::State::StateTypeId TypeId{TId}; \
        static constexpr std::string_view CanonicalName{TCanonicalName}; \
        using ConvergencePolicy = OTAStateConvergence; \
    }

ESPRESSIO_OTA_DECLARE_STATE(CoordinatorAvailabilityState, CoordinatorAvailabilityValue,
                            0x4553504F54410001ULL, "ESPressio.OTA.State.CoordinatorAvailability");
ESPRESSIO_OTA_DECLARE_STATE(ActiveUpdateTransactionState, ActiveUpdateTransactionValue,
                            0x4553504F54410002ULL, "ESPressio.OTA.State.ActiveUpdateTransaction");
ESPRESSIO_OTA_DECLARE_STATE(ActiveUpdateLifecycleState, ActiveUpdateLifecycleValue,
                            0x4553504F54410003ULL, "ESPressio.OTA.State.ActiveUpdateLifecycle");
ESPRESSIO_OTA_DECLARE_STATE(ExecutingGenerationState, GenerationValue,
                            0x4553504F54410004ULL, "ESPressio.OTA.State.ExecutingGeneration");
ESPRESSIO_OTA_DECLARE_STATE(CommittedGenerationState, CommittedGenerationValue,
                            0x4553504F54410005ULL, "ESPressio.OTA.State.CommittedGeneration");
ESPRESSIO_OTA_DECLARE_STATE(CandidateGenerationState, CandidateGenerationValue,
                            0x4553504F54410006ULL, "ESPressio.OTA.State.CandidateGeneration");
ESPRESSIO_OTA_DECLARE_STATE(MinimumAcceptedSecurityLevelState, MinimumAcceptedSecurityLevelValue,
                            0x4553504F54410007ULL, "ESPressio.OTA.State.MinimumAcceptedSecurityLevel");
ESPRESSIO_OTA_DECLARE_STATE(UpdateProgressState, UpdateProgressValue,
                            0x4553504F54410008ULL, "ESPressio.OTA.State.UpdateProgress");
ESPRESSIO_OTA_DECLARE_STATE(LastUpdateOutcomeState, LastUpdateOutcomeValue,
                            0x4553504F54410009ULL, "ESPressio.OTA.State.LastUpdateOutcome");

#undef ESPRESSIO_OTA_DECLARE_STATE

template<std::size_t TDirectoryCapacity>
Primitive::TypeDirectoryRegistrationStatus RegisterOTAStateTypes(
    Primitive::TypeDirectory<TDirectoryCapacity>& directory) noexcept {
    Primitive::TypeDirectoryRegistrationStatus status{};
#define ESPRESSIO_OTA_REGISTER_STATE(TState) \
    status = directory.template Register<TState>(); \
    if (status != Primitive::TypeDirectoryRegistrationStatus::Success) return status
    ESPRESSIO_OTA_REGISTER_STATE(CoordinatorAvailabilityState);
    ESPRESSIO_OTA_REGISTER_STATE(ActiveUpdateTransactionState);
    ESPRESSIO_OTA_REGISTER_STATE(ActiveUpdateLifecycleState);
    ESPRESSIO_OTA_REGISTER_STATE(ExecutingGenerationState);
    ESPRESSIO_OTA_REGISTER_STATE(CommittedGenerationState);
    ESPRESSIO_OTA_REGISTER_STATE(CandidateGenerationState);
    ESPRESSIO_OTA_REGISTER_STATE(MinimumAcceptedSecurityLevelState);
    ESPRESSIO_OTA_REGISTER_STATE(UpdateProgressState);
    ESPRESSIO_OTA_REGISTER_STATE(LastUpdateOutcomeState);
#undef ESPRESSIO_OTA_REGISTER_STATE
    return Primitive::TypeDirectoryRegistrationStatus::Success;
}

template<std::size_t TMaximumRemoteOwners = 0U, std::size_t TMaximumSubscribers = 0U>
using OTAStateRuntime = State::Runtime<
    State::TypeConfiguration<CoordinatorAvailabilityState,
        State::MaximumRemoteOwners<TMaximumRemoteOwners>, State::MaximumSubscribers<TMaximumSubscribers>>,
    State::TypeConfiguration<ActiveUpdateTransactionState,
        State::MaximumRemoteOwners<TMaximumRemoteOwners>, State::MaximumSubscribers<TMaximumSubscribers>>,
    State::TypeConfiguration<ActiveUpdateLifecycleState,
        State::MaximumRemoteOwners<TMaximumRemoteOwners>, State::MaximumSubscribers<TMaximumSubscribers>>,
    State::TypeConfiguration<ExecutingGenerationState,
        State::MaximumRemoteOwners<TMaximumRemoteOwners>, State::MaximumSubscribers<TMaximumSubscribers>>,
    State::TypeConfiguration<CommittedGenerationState,
        State::MaximumRemoteOwners<TMaximumRemoteOwners>, State::MaximumSubscribers<TMaximumSubscribers>>,
    State::TypeConfiguration<CandidateGenerationState,
        State::MaximumRemoteOwners<TMaximumRemoteOwners>, State::MaximumSubscribers<TMaximumSubscribers>>,
    State::TypeConfiguration<MinimumAcceptedSecurityLevelState,
        State::MaximumRemoteOwners<TMaximumRemoteOwners>, State::MaximumSubscribers<TMaximumSubscribers>>,
    State::TypeConfiguration<UpdateProgressState,
        State::MaximumRemoteOwners<TMaximumRemoteOwners>, State::MaximumSubscribers<TMaximumSubscribers>>,
    State::TypeConfiguration<LastUpdateOutcomeState,
        State::MaximumRemoteOwners<TMaximumRemoteOwners>, State::MaximumSubscribers<TMaximumSubscribers>>>;

struct OTAStateOwners final {
    State::StateOwner<CoordinatorAvailabilityState> CoordinatorAvailability{};
    State::StateOwner<ActiveUpdateTransactionState> ActiveTransaction{};
    State::StateOwner<ActiveUpdateLifecycleState> ActiveLifecycle{};
    State::StateOwner<ExecutingGenerationState> ExecutingGeneration{};
    State::StateOwner<CommittedGenerationState> CommittedGeneration{};
    State::StateOwner<CandidateGenerationState> CandidateGeneration{};
    State::StateOwner<MinimumAcceptedSecurityLevelState> MinimumAcceptedSecurity{};
    State::StateOwner<UpdateProgressState> UpdateProgress{};
    State::StateOwner<LastUpdateOutcomeState> LastOutcome{};

    explicit operator bool() const noexcept {
        return bool(CoordinatorAvailability) && bool(ActiveTransaction) && bool(ActiveLifecycle) &&
               bool(ExecutingGeneration) && bool(CommittedGeneration) && bool(CandidateGeneration) &&
               bool(MinimumAcceptedSecurity) && bool(UpdateProgress) && bool(LastOutcome);
    }
};

template<std::size_t TMaximumRemoteOwners, std::size_t TMaximumSubscribers>
bool BindOTAStateOwners(
    OTAStateRuntime<TMaximumRemoteOwners, TMaximumSubscribers>& runtime,
    OTAStateOwners& owners) noexcept {
    owners.CoordinatorAvailability = runtime.template BindOwner<CoordinatorAvailabilityState>();
    owners.ActiveTransaction = runtime.template BindOwner<ActiveUpdateTransactionState>();
    owners.ActiveLifecycle = runtime.template BindOwner<ActiveUpdateLifecycleState>();
    owners.ExecutingGeneration = runtime.template BindOwner<ExecutingGenerationState>();
    owners.CommittedGeneration = runtime.template BindOwner<CommittedGenerationState>();
    owners.CandidateGeneration = runtime.template BindOwner<CandidateGenerationState>();
    owners.MinimumAcceptedSecurity = runtime.template BindOwner<MinimumAcceptedSecurityLevelState>();
    owners.UpdateProgress = runtime.template BindOwner<UpdateProgressState>();
    owners.LastOutcome = runtime.template BindOwner<LastUpdateOutcomeState>();
    return bool(owners);
}

} // namespace ESPressio::OTA
