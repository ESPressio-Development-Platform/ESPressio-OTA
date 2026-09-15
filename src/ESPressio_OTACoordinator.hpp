#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "ESPressio_OTAAcquisition.hpp"
#include "ESPressio_OTAArtifactSourceSelection.hpp"
#include "ESPressio_OTAVerification.hpp"
#include "ESPressio_OTADurable.hpp"
#include "ESPressio_OTAManifest.hpp"
#include "ESPressio_OTAUpdatePlan.hpp"
#include "ESPressio_OTAPolicyHealth.hpp"
#include "ESPressio_OTAExecution.hpp"
#include "ESPressio_OTAComponentLifecycleExecution.hpp"
#include "ESPressio_OTAState.hpp"

#include <ESPressio_PlatformClock.hpp>
#include <ESPressio_PlatformOTA.hpp>

namespace ESPressio::OTA {

enum class CoordinatorCoreReason : std::uint32_t {
    None = 0U,
    NotInitialized,
    AlreadyInitialized,
    Busy,
    NoActiveTransaction,
    TransactionMismatch,
    RecoveryRequired,
    AwaitingExecutionIntegration,
    AwaitingRestart,
    VerifiedManifestRequired,
    VerifiedManifestMismatch,
    TargetProfileRequired,
    UpdatePlanUnavailable,
    RequiredHealthUnavailable,
    TrialDeadlineExpired,
    BootTargetMismatch,
    InvalidLifecycleTransition,
    ReentrantMutation,
    InvalidConfiguration,
    InvalidStartRequest,
    InvalidActivationRequest,
    CandidateCannotReadCurrentOTASchema,
    CandidateManifestSchemaUnsupported,
    StateProjectionFailed,
    ArtifactSourceSelectionFailed,
    ArtifactSourceRetryExhausted,
    CandidateTrialRejected
};

struct CoordinatorStartRequest final {
    ReleaseIdentifier Release{};
    ManifestIdentifier Manifest{};
    SecurityGeneration CandidateSecurity{};

    constexpr bool IsValid() const noexcept {
        return bool(Release) && bool(Manifest);
    }
};

struct CoordinatorActivationRequest final {
    UpdateTransactionId Transaction{};
    Platform::OTA::BootTargetIdentifier CandidateBootTarget{};
    bool RestartRequired{true};
    bool InterruptsCurrentRuntime{true};
    bool TrialBootRequired{true};

    constexpr bool IsValid() const noexcept {
        return bool(Transaction) && bool(CandidateBootTarget);
    }
};

struct CoordinatorStatusSnapshot final {
    CoordinatorAvailability Availability{CoordinatorAvailability::Unavailable};
    bool HasActiveTransaction{false};
    UpdateTransactionId Transaction{};
    UpdateGenerationId CandidateGeneration{};
    UpdateGenerationId CommittedGeneration{};
    RecoveryPoint DurableRecoveryPoint{RecoveryPoint::None};
    DurableIntent Intent{DurableIntent::None};
    bool HasLifecycle{false};
    UpdateLifecycle Lifecycle{UpdateLifecycle::Idle};
    bool VerifiedManifestBound{false};
    bool UpdatePlanReady{false};
};

namespace CoordinatorDetail {

inline constexpr Result CoreResult(OutcomeClass outcome, CoordinatorCoreReason reason) noexcept {
    return {outcome, {DiagnosticDomain::OTA, static_cast<std::uint32_t>(reason), 0, {}, 0U}};
}

inline constexpr Result DurableResult(OTADurableStatus status) noexcept {
    OutcomeClass outcome = OutcomeClass::PersistenceFailed;
    if (status == OTADurableStatus::CapacityUnavailable) outcome = OutcomeClass::CapacityUnavailable;
    if (status == OTADurableStatus::UnsupportedBackend) outcome = OutcomeClass::Unsupported;
    if (status == OTADurableStatus::Invalid || status == OTADurableStatus::InvalidTransition ||
        status == OTADurableStatus::TransactionMismatch || status == OTADurableStatus::NoActiveTransaction) {
        outcome = OutcomeClass::Invalid;
    }
    return {outcome, {DiagnosticDomain::Persistence, static_cast<std::uint32_t>(status), 0, {}, 0U}};
}

inline constexpr Result PlatformResult(const Platform::OTA::Result& result) noexcept {
    OutcomeClass outcome = OutcomeClass::PlatformFailed;
    switch (result.Code) {
        case Platform::OTA::Status::Success: return Result::Success();
        case Platform::OTA::Status::Pending: outcome = OutcomeClass::Pending; break;
        case Platform::OTA::Status::Unsupported: outcome = OutcomeClass::Unsupported; break;
        case Platform::OTA::Status::Invalid: outcome = OutcomeClass::Invalid; break;
        case Platform::OTA::Status::Busy: outcome = OutcomeClass::Deferred; break;
        case Platform::OTA::Status::CapacityUnavailable: outcome = OutcomeClass::CapacityUnavailable; break;
        case Platform::OTA::Status::Failed: outcome = OutcomeClass::PlatformFailed; break;
    }
    return {outcome, {DiagnosticDomain::Platform, static_cast<std::uint32_t>(result.Code), result.NativeCode, {}, 0U}};
}

inline constexpr bool StateSetSucceeded(State::StateSetStatus status) noexcept {
    return status == State::StateSetStatus::Changed || status == State::StateSetStatus::NoChange;
}

inline constexpr UpdateLifecycle LifecycleForRecoveryPoint(RecoveryPoint point) noexcept {
    switch (point) {
        case RecoveryPoint::TransactionCreated: return UpdateLifecycle::Checking;
        case RecoveryPoint::ManifestAccepted: return UpdateLifecycle::CandidateSelected;
        case RecoveryPoint::ArtifactsAcquired: return UpdateLifecycle::Verifying;
        case RecoveryPoint::ArtifactsVerified: return UpdateLifecycle::Preparing;
        case RecoveryPoint::StagingStarted: return UpdateLifecycle::Staging;
        case RecoveryPoint::Staged: return UpdateLifecycle::Staged;
        case RecoveryPoint::ActivationSelected: return UpdateLifecycle::AwaitingRestart;
        case RecoveryPoint::TrialBootEntered: return UpdateLifecycle::Trial;
        case RecoveryPoint::CommitStarted: return UpdateLifecycle::Committing;
        case RecoveryPoint::RollbackStarted: return UpdateLifecycle::RollingBack;
        case RecoveryPoint::Committed: return UpdateLifecycle::Completed;
        case RecoveryPoint::RolledBack: return UpdateLifecycle::RolledBack;
        case RecoveryPoint::None: return UpdateLifecycle::RecoveryRequired;
    }
    return UpdateLifecycle::RecoveryRequired;
}

inline constexpr bool LegalImplementedTransition(UpdateLifecycle from, UpdateLifecycle to) noexcept {
    if (from == to) return true;
    switch (from) {
        case UpdateLifecycle::Idle: return to == UpdateLifecycle::Checking;
        case UpdateLifecycle::Checking:
            return to == UpdateLifecycle::CandidateSelected || to == UpdateLifecycle::Cancelling ||
                   to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::CandidateSelected:
            return to == UpdateLifecycle::Acquiring || to == UpdateLifecycle::Verifying ||
                   to == UpdateLifecycle::Cancelling || to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::Acquiring:
            return to == UpdateLifecycle::Verifying || to == UpdateLifecycle::Cancelling ||
                   to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::Verifying:
            return to == UpdateLifecycle::Preparing || to == UpdateLifecycle::Cancelling ||
                   to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::Preparing:
            return to == UpdateLifecycle::Staging || to == UpdateLifecycle::Cancelling ||
                   to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::Staging:
            return to == UpdateLifecycle::Staged || to == UpdateLifecycle::Cancelling ||
                   to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::Staged:
            return to == UpdateLifecycle::ActivationPending || to == UpdateLifecycle::Activating ||
                   to == UpdateLifecycle::Cancelling || to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::ActivationPending:
            return to == UpdateLifecycle::Activating || to == UpdateLifecycle::Cancelling ||
                   to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::Activating:
            return to == UpdateLifecycle::AwaitingRestart || to == UpdateLifecycle::RollbackPending ||
                   to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::AwaitingRestart:
            return to == UpdateLifecycle::Trial || to == UpdateLifecycle::RollbackPending ||
                   to == UpdateLifecycle::RollingBack || to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::Trial:
            return to == UpdateLifecycle::HealthValidation || to == UpdateLifecycle::Committing ||
                   to == UpdateLifecycle::RollbackPending || to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::HealthValidation:
            return to == UpdateLifecycle::Committing || to == UpdateLifecycle::RollbackPending ||
                   to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::Committing:
            return to == UpdateLifecycle::Completed || to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::RollbackPending:
            return to == UpdateLifecycle::RollingBack || to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::RollingBack:
            return to == UpdateLifecycle::RolledBack || to == UpdateLifecycle::RollbackFailed ||
                   to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::Deferred:
            return to == UpdateLifecycle::ActivationPending || to == UpdateLifecycle::Cancelling ||
                   to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::Cancelling:
            return to == UpdateLifecycle::Cancelled || to == UpdateLifecycle::RecoveryRequired;
        case UpdateLifecycle::Completed:
        case UpdateLifecycle::Cancelled:
        case UpdateLifecycle::Failed:
        case UpdateLifecycle::RolledBack:
        case UpdateLifecycle::RollbackFailed:
            return false;
        case UpdateLifecycle::RecoveryRequired:
            return to == UpdateLifecycle::RecoveryRequired;
    }
    return false;
}

inline constexpr TerminalUpdateOutcome TerminalOutcomeFor(const Result& result) noexcept {
    switch (result.Outcome) {
        case OutcomeClass::Unsupported: return TerminalUpdateOutcome::Unsupported;
        case OutcomeClass::Rejected: return TerminalUpdateOutcome::Rejected;
        case OutcomeClass::CapacityUnavailable: return TerminalUpdateOutcome::CapacityUnavailable;
        default: return TerminalUpdateOutcome::Failed;
    }
}

} // namespace CoordinatorDetail

template<typename TCapacityProfile, typename TBootControl, typename TTrialBoot, typename TRestart, typename TClock>
class Coordinator final {
    static_assert(TCapacityProfile::IsValid, "Coordinator requires a valid OTA capacity profile");
    static_assert(Platform::OTA::IsBootControlProviderV<TBootControl>, "Coordinator requires BootControl");
    static_assert(Platform::OTA::IsTrialBootProviderV<TTrialBoot>, "Coordinator requires TrialBoot");
    static_assert(Platform::OTA::IsSystemRestartProviderV<TRestart>, "Coordinator requires SystemRestart");
    static_assert(Platform::Clock::IsClockSourceV<TClock>, "Coordinator requires Platform Clock");
    static_assert(TClock::PlatformCapabilities::template Contains<Platform::Capability::MonotonicClock>,
                  "Coordinator requires a monotonic Platform Clock");

    using ControlStore = OTAControlStore<TCapacityProfile>;
    using StagePolicies = PolicyGateSet<StagePolicyDecisionPoint,
        TCapacityProfile::MaximumPolicyProvidersPerDecisionPoint>;
    using ActivatePolicies = PolicyGateSet<ActivatePolicyDecisionPoint,
        TCapacityProfile::MaximumPolicyProvidersPerDecisionPoint>;
    using HealthChecks = HealthRegistry<TCapacityProfile::MaximumRequiredHealthConditions>;

    ControlStore& control_;
    OTAStateOwners& states_;
    TBootControl& boot_;
    TTrialBoot& trial_;
    TRestart& restart_;
    TClock& clock_;
    StagePolicies stagePolicies_{};
    ActivatePolicies& activatePolicies_;
    HealthChecks& health_;
    ComponentHandlerDirectory<TCapacityProfile>& handlers_;
    ArtifactAcquisitionSession<TCapacityProfile> acquisition_;
    ArtifactVerificationSession<TCapacityProfile> verification_;
    ComponentStagingSession<TCapacityProfile> staging_;
    ComponentLifecycleExecutionSession<TCapacityProfile> componentLifecycle_;
    IArtifactSourceSelector* artifactSourceSelector_{nullptr};
    std::size_t maximumArtifactSourceAttempts_{1U};
    std::size_t acquisitionSourceAttempt_{0U};
    std::uint64_t trialTimeoutNanoseconds_{0U};
    std::uint64_t healthReevaluationNanoseconds_{0U};

    bool initialized_{false};
    bool mutating_{false};
    bool hasLifecycle_{false};
    UpdateLifecycle lifecycle_{UpdateLifecycle::Idle};
    CoordinatorStatusSnapshot status_{};

    std::array<HealthConditionTypeId, TCapacityProfile::MaximumRequiredHealthConditions> requiredHealth_{};
    std::size_t requiredHealthCount_{0U};
    ManifestIdentifier boundManifest_{};
    bool verifiedManifestBound_{false};
    const Manifest<TCapacityProfile>* verifiedManifest_{nullptr};
    const UpdateTargetProfile<TCapacityProfile>* targetProfile_{nullptr};
    UpdatePlan<TCapacityProfile> plan_{};
    std::size_t acquisitionArtifactIndex_{0U};
    bool acquisitionStarted_{false};
    std::size_t verificationArtifactIndex_{0U};
    bool verificationStarted_{false};
    bool trialClockStarted_{false};
    Platform::Clock::Tick trialStartTick_{0U};
    Platform::Clock::Tick lastHealthEvaluationTick_{0U};
    bool healthEvaluated_{false};
    bool activationRestartIssued_{false};
    bool rollbackRestartIssued_{false};
    bool activationRecovered_{false};

    class MutationGuard final {
        Coordinator& owner_;
        bool acquired_{false};
    public:
        explicit MutationGuard(Coordinator& owner) noexcept : owner_(owner) {
            if (!owner_.mutating_) { owner_.mutating_ = true; acquired_ = true; }
        }
        ~MutationGuard() { if (acquired_) owner_.mutating_ = false; }
        explicit operator bool() const noexcept { return acquired_; }
    };

    static GenerationValue GenerationFromBaseline(const CommittedBaseline& baseline) noexcept {
        GenerationValue value;
        value.Generation = baseline.Generation.Value();
        value.HasRelease = baseline.HasRelease;
        value.Release = baseline.HasRelease ? baseline.Release.Value() : 0U;
        value.HasManifest = baseline.HasManifest;
        value.Manifest = baseline.HasManifest ? baseline.Manifest.Bytes() : std::array<std::uint8_t, 16>{};
        return value;
    }

    static GenerationValue GenerationFromCandidate(const ActiveTransactionRecord& active) noexcept {
        GenerationValue value;
        value.Generation = active.CandidateGeneration.Value();
        value.HasRelease = true;
        value.Release = active.Release.Value();
        value.HasManifest = true;
        value.Manifest = active.Manifest.Bytes();
        return value;
    }

    static CommittedGenerationValue CommittedFromBaseline(const CommittedBaseline& baseline) noexcept {
        CommittedGenerationValue value;
        value.Identity = GenerationFromBaseline(baseline);
        value.SecurityGeneration = baseline.Security.Value();
        return value;
    }

    static ActiveUpdateTransactionValue ActiveValue(const ActiveTransactionRecord& active) noexcept {
        ActiveUpdateTransactionValue value;
        value.Present = true;
        value.Transaction = active.Transaction.Value();
        value.CandidateGeneration = active.CandidateGeneration.Value();
        value.Release = active.Release.Value();
        value.Manifest = active.Manifest.Bytes();
        value.CandidateSecurityGeneration = active.CandidateSecurity.Value();
        return value;
    }

    static CandidateGenerationValue CandidateValue(const ActiveTransactionRecord& active) noexcept {
        CandidateGenerationValue value;
        value.Present = true;
        value.Transaction = active.Transaction.Value();
        value.Generation = active.CandidateGeneration.Value();
        value.Release = active.Release.Value();
        value.Manifest = active.Manifest.Bytes();
        value.SecurityGeneration = active.CandidateSecurity.Value();
        return value;
    }

    bool SetAvailability(CoordinatorAvailability availability, UpdateTransactionId transaction = {}) noexcept {
        CoordinatorAvailabilityValue value;
        value.Availability = availability;
        value.ActiveTransaction = transaction.Value();
        if (!CoordinatorDetail::StateSetSucceeded(states_.CoordinatorAvailability.Set(value))) return false;
        status_.Availability = availability;
        return true;
    }

    bool SetLifecycle(UpdateTransactionId transaction, UpdateLifecycle next, bool reconstruct) noexcept {
        if (!reconstruct && hasLifecycle_ && !CoordinatorDetail::LegalImplementedTransition(lifecycle_, next)) return false;
        ActiveUpdateLifecycleValue value;
        value.Present = true;
        value.Transaction = transaction.Value();
        value.Lifecycle = next;
        if (!CoordinatorDetail::StateSetSucceeded(states_.ActiveLifecycle.Set(value))) return false;
        hasLifecycle_ = true;
        lifecycle_ = next;
        status_.HasLifecycle = true;
        status_.Lifecycle = next;
        return true;
    }

    bool PublishActive(const OTAControlRecord<TCapacityProfile>& record,
                       UpdateLifecycle lifecycle,
                       bool executingCandidate,
                       bool reconstruct = false) noexcept {
        if (!record.HasActiveTransaction || !record.Active.IsValid()) return false;
        if (!CoordinatorDetail::StateSetSucceeded(states_.ActiveTransaction.Set(ActiveValue(record.Active)))) return false;
        if (!CoordinatorDetail::StateSetSucceeded(states_.CandidateGeneration.Set(CandidateValue(record.Active)))) return false;
        if (!SetLifecycle(record.Active.Transaction, lifecycle, reconstruct)) return false;
        const auto executing = executingCandidate ? GenerationFromCandidate(record.Active) : GenerationFromBaseline(record.Committed);
        if (!CoordinatorDetail::StateSetSucceeded(states_.ExecutingGeneration.Set(executing))) return false;
        if (!CoordinatorDetail::StateSetSucceeded(states_.CommittedGeneration.Set(CommittedFromBaseline(record.Committed)))) return false;
        MinimumAcceptedSecurityLevelValue floor;
        floor.SecurityGeneration = record.MinimumAcceptedSecurity.Value();
        if (!CoordinatorDetail::StateSetSucceeded(states_.MinimumAcceptedSecurity.Set(floor))) return false;
        if (!SetAvailability(CoordinatorAvailability::Busy, record.Active.Transaction)) return false;
        status_.HasActiveTransaction = true;
        status_.Transaction = record.Active.Transaction;
        status_.CandidateGeneration = record.Active.CandidateGeneration;
        status_.CommittedGeneration = record.Committed.Generation;
        status_.DurableRecoveryPoint = record.Active.Point;
        status_.Intent = record.Intent;
        status_.VerifiedManifestBound = verifiedManifestBound_ && boundManifest_ == record.Active.Manifest;
        return true;
    }

    bool PublishReady(const OTAControlRecord<TCapacityProfile>& record, bool clearScoped) noexcept {
        if (!CoordinatorDetail::StateSetSucceeded(states_.ExecutingGeneration.Set(GenerationFromBaseline(record.Committed)))) return false;
        if (!CoordinatorDetail::StateSetSucceeded(states_.CommittedGeneration.Set(CommittedFromBaseline(record.Committed)))) return false;
        MinimumAcceptedSecurityLevelValue floor;
        floor.SecurityGeneration = record.MinimumAcceptedSecurity.Value();
        if (!CoordinatorDetail::StateSetSucceeded(states_.MinimumAcceptedSecurity.Set(floor))) return false;
        if (clearScoped) {
            if (!CoordinatorDetail::StateSetSucceeded(states_.CandidateGeneration.Set(CandidateGenerationValue{}))) return false;
            if (!CoordinatorDetail::StateSetSucceeded(states_.ActiveLifecycle.Set(ActiveUpdateLifecycleValue{}))) return false;
            if (!CoordinatorDetail::StateSetSucceeded(states_.ActiveTransaction.Set(ActiveUpdateTransactionValue{}))) return false;
            if (!CoordinatorDetail::StateSetSucceeded(states_.UpdateProgress.Set(UpdateProgressValue{}))) return false;
        }
        if (!SetAvailability(CoordinatorAvailability::Ready)) return false;
        hasLifecycle_ = false;
        lifecycle_ = UpdateLifecycle::Idle;
        status_.HasActiveTransaction = false;
        status_.Transaction = {};
        status_.CandidateGeneration = {};
        status_.CommittedGeneration = record.Committed.Generation;
        status_.DurableRecoveryPoint = RecoveryPoint::None;
        status_.Intent = DurableIntent::None;
        status_.HasLifecycle = false;
        status_.Lifecycle = UpdateLifecycle::Idle;
        status_.VerifiedManifestBound = false;
        return true;
    }

    void ClearBoundExecutionState() noexcept {
        staging_.Reset();
        componentLifecycle_.Reset();
        activationRestartIssued_ = false;
        rollbackRestartIssued_ = false;
        activationRecovered_ = false;
        verifiedManifestBound_ = false;
        verifiedManifest_ = nullptr;
        targetProfile_ = nullptr;
        boundManifest_ = {};
        plan_ = {};
        status_.UpdatePlanReady = false;
        requiredHealthCount_ = 0U;
        acquisitionArtifactIndex_ = 0U;
        acquisitionStarted_ = false;
        acquisitionSourceAttempt_ = 0U;
        verificationArtifactIndex_ = 0U;
        verificationStarted_ = false;
        trialClockStarted_ = false;
        healthEvaluated_ = false;
    }

    bool PublishTerminal(const OTAControlRecord<TCapacityProfile>& record,
                         UpdateTransactionId transaction,
                         TerminalUpdateOutcome outcome,
                         UpdateOperation operation,
                         const Result& result) noexcept {
        if (!CoordinatorDetail::StateSetSucceeded(states_.CommittedGeneration.Set(CommittedFromBaseline(record.Committed)))) return false;
        if (!CoordinatorDetail::StateSetSucceeded(states_.ExecutingGeneration.Set(GenerationFromBaseline(record.Committed)))) return false;
        MinimumAcceptedSecurityLevelValue floor;
        floor.SecurityGeneration = record.MinimumAcceptedSecurity.Value();
        if (!CoordinatorDetail::StateSetSucceeded(states_.MinimumAcceptedSecurity.Set(floor))) return false;
        if (!CoordinatorDetail::StateSetSucceeded(states_.LastOutcome.Set(
                LastUpdateOutcomeValue::FromResult(transaction, outcome, operation, result)))) return false;
        if (!CoordinatorDetail::StateSetSucceeded(states_.CandidateGeneration.Set(CandidateGenerationValue{}))) return false;
        if (!CoordinatorDetail::StateSetSucceeded(states_.ActiveLifecycle.Set(ActiveUpdateLifecycleValue{}))) return false;
        if (!CoordinatorDetail::StateSetSucceeded(states_.ActiveTransaction.Set(ActiveUpdateTransactionValue{}))) return false;
        if (!CoordinatorDetail::StateSetSucceeded(states_.UpdateProgress.Set(UpdateProgressValue{}))) return false;
        if (!SetAvailability(CoordinatorAvailability::Ready)) return false;
        hasLifecycle_ = false;
        lifecycle_ = UpdateLifecycle::Idle;
        status_.HasActiveTransaction = false;
        status_.Transaction = {};
        status_.CandidateGeneration = {};
        status_.CommittedGeneration = record.Committed.Generation;
        status_.DurableRecoveryPoint = RecoveryPoint::None;
        status_.Intent = DurableIntent::None;
        status_.HasLifecycle = false;
        status_.Lifecycle = UpdateLifecycle::Idle;
        status_.VerifiedManifestBound = false;
        ClearBoundExecutionState();
        return true;
    }

    Result ProjectionFailure() noexcept {
        status_.Availability = CoordinatorAvailability::Unavailable;
        return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::StateProjectionFailed);
    }

    Result RecoveryRequired(OTAControlRecord<TCapacityProfile>* record) noexcept {
        (void)control_.MarkRecoveryRequired();
        UpdateTransactionId transaction{};
        if (record != nullptr && record->HasActiveTransaction) {
            transaction = record->Active.Transaction;
            bool executingCandidate = false;
            if (record->Active.Point >= RecoveryPoint::ActivationSelected &&
                record->Active.CandidateBootTarget) {
                executingCandidate = boot_.CurrentBootTarget() == record->Active.CandidateBootTarget;
            }
            (void)PublishActive(*record, UpdateLifecycle::RecoveryRequired,
                                executingCandidate, true);
        }
        (void)SetAvailability(CoordinatorAvailability::RecoveryRequired, transaction);
        status_.Availability = CoordinatorAvailability::RecoveryRequired;
        return CoordinatorDetail::CoreResult(OutcomeClass::Failed, CoordinatorCoreReason::RecoveryRequired);
    }

    Result FailAcquisition(OTAControlRecord<TCapacityProfile>& record, const Result& failure) noexcept {
        const auto cleanup = acquisition_.Abort();
        acquisitionStarted_ = false;
        acquisitionSourceAttempt_ = 0U;
        if (!cleanup) return RecoveryRequired(&record);
        const auto transaction = record.Active.Transaction;
        const auto abandoned = control_.AbandonTransaction(transaction);
        if (abandoned != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(abandoned);
        if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
        const auto terminal = CoordinatorDetail::TerminalOutcomeFor(failure);
        return PublishTerminal(record, transaction, terminal, UpdateOperation::Acquire, failure)
            ? failure : ProjectionFailure();
    }

    Result RetryAcquisitionSource(
        OTAControlRecord<TCapacityProfile>& record,
        const Result& failure) noexcept {
        if (artifactSourceSelector_ == nullptr) return FailAcquisition(record, failure);
        if (acquisitionSourceAttempt_ >= maximumArtifactSourceAttempts_) {
            return FailAcquisition(record, CoordinatorDetail::CoreResult(
                OutcomeClass::Unavailable, CoordinatorCoreReason::ArtifactSourceRetryExhausted));
        }
        if (verifiedManifest_ == nullptr || acquisitionArtifactIndex_ >= verifiedManifest_->Artifacts.size()) {
            return RecoveryRequired(&record);
        }

        const auto& artifact = verifiedManifest_->Artifacts[acquisitionArtifactIndex_];
        ArtifactSourceSelectionContext context;
        context.Transaction = record.Active.Transaction;
        context.Artifact = ArtifactIdentifier{artifact.Identifier};
        context.ExpectedLength = artifact.ExpectedLength;
        context.Attempt = acquisitionSourceAttempt_ + 1U;
        context.AcceptedCheckpointPrefix = acquisition_.CheckpointedBytes();
        context.HasPreviousFailure = true;
        context.PreviousFailure = failure;

        ArtifactSourceSelection selection;
        const auto selected = artifactSourceSelector_->Select(context, selection);
        if (selected.Outcome == OutcomeClass::Pending || selected.Outcome == OutcomeClass::Deferred) {
            return selected;
        }
        if (!selected) return FailAcquisition(record, selected);
        if (!selection) {
            return FailAcquisition(record, CoordinatorDetail::CoreResult(
                OutcomeClass::Unavailable, CoordinatorCoreReason::ArtifactSourceSelectionFailed));
        }

        ++acquisitionSourceAttempt_;
        const auto retry = acquisition_.RetryWithSource(*selection.Source, selection.OffsetRead);
        if (retry.Outcome == OutcomeClass::Pending || retry.Outcome == OutcomeClass::Deferred) return retry;
        if (!retry) return FailAcquisition(record, retry);
        return {OutcomeClass::Pending, {}};
    }

    Result AdvanceAcquisition(OTAControlRecord<TCapacityProfile>& record) noexcept {
        if (!verifiedManifestBound_ || verifiedManifest_ == nullptr || boundManifest_ != record.Active.Manifest) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::VerifiedManifestRequired);
        }
        if (!status_.UpdatePlanReady) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::UpdatePlanUnavailable);
        }
        if (record.Active.Point != RecoveryPoint::ManifestAccepted) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::InvalidLifecycleTransition);
        }

        const auto artifactCount = verifiedManifest_->Artifacts.size();
        if (!acquisitionStarted_ && acquisitionArtifactIndex_ >= artifactCount) {
            const auto acquired = control_.AdvanceRecoveryPoint(
                record.Active.Transaction, RecoveryPoint::ArtifactsAcquired);
            if (acquired != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(acquired);
            if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
            if (!PublishActive(record, UpdateLifecycle::Verifying, false)) return ProjectionFailure();
            return {OutcomeClass::Pending, {}};
        }

        if (!acquisitionStarted_) {
            const auto begun = acquisition_.Begin(
                record.Active.Transaction, verifiedManifest_->Artifacts[acquisitionArtifactIndex_]);
            if (begun.Outcome != OutcomeClass::Pending) return FailAcquisition(record, begun);
            acquisitionStarted_ = true;
            acquisitionSourceAttempt_ = 1U;
            if (!PublishActive(record, UpdateLifecycle::Acquiring, false)) {
                (void)acquisition_.Abort();
                acquisitionStarted_ = false;
                acquisitionSourceAttempt_ = 0U;
                return ProjectionFailure();
            }
            return begun;
        }

        const auto step = acquisition_.Advance();
        if (acquisition_.IsComplete()) {
            acquisitionStarted_ = false;
            acquisitionSourceAttempt_ = 0U;
            ++acquisitionArtifactIndex_;
            return {OutcomeClass::Pending, {}};
        }
        if (acquisition_.Phase() == ArtifactAcquisitionPhase::Failed) {
            return FailAcquisition(record, step);
        }
        if (acquisition_.Phase() == ArtifactAcquisitionPhase::SourceFailed) {
            return RetryAcquisitionSource(record, step);
        }
        if (step.Outcome == OutcomeClass::Pending || step.Outcome == OutcomeClass::Deferred) return step;
        if (!step) return FailAcquisition(record, step);
        return {OutcomeClass::Pending, {}};
    }

    Result FailVerification(OTAControlRecord<TCapacityProfile>& record, const Result& failure) noexcept {
        verification_.Abort();
        verificationStarted_ = false;
        const auto transaction = record.Active.Transaction;
        const auto abandoned = control_.AbandonTransaction(transaction);
        if (abandoned != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(abandoned);
        if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
        const auto terminal = CoordinatorDetail::TerminalOutcomeFor(failure);
        return PublishTerminal(record, transaction, terminal, UpdateOperation::Verify, failure)
            ? failure : ProjectionFailure();
    }

    Result AdvanceVerification(OTAControlRecord<TCapacityProfile>& record) noexcept {
        if (!verifiedManifestBound_ || verifiedManifest_ == nullptr || boundManifest_ != record.Active.Manifest) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::VerifiedManifestRequired);
        }
        if (!status_.UpdatePlanReady) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::UpdatePlanUnavailable);
        }
        if (record.Active.Point != RecoveryPoint::ArtifactsAcquired) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::InvalidLifecycleTransition);
        }

        const auto artifactCount = verifiedManifest_->Artifacts.size();
        if (!verificationStarted_ && verificationArtifactIndex_ >= artifactCount) {
            const auto verified = control_.AdvanceRecoveryPoint(
                record.Active.Transaction, RecoveryPoint::ArtifactsVerified);
            if (verified != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(verified);
            if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
            if (!PublishActive(record, UpdateLifecycle::Preparing, false)) return ProjectionFailure();
            return {OutcomeClass::Pending, {}};
        }

        if (!verificationStarted_) {
            const auto begun = verification_.Begin(verifiedManifest_->Artifacts[verificationArtifactIndex_]);
            if (begun.Outcome != OutcomeClass::Pending) return FailVerification(record, begun);
            verificationStarted_ = true;
            if (!PublishActive(record, UpdateLifecycle::Verifying, false)) {
                verification_.Abort();
                verificationStarted_ = false;
                return ProjectionFailure();
            }
            return begun;
        }

        const auto step = verification_.Advance();
        if (verification_.IsComplete()) {
            verificationStarted_ = false;
            ++verificationArtifactIndex_;
            return {OutcomeClass::Pending, {}};
        }
        if (verification_.Phase() == ArtifactVerificationPhase::Failed) {
            return FailVerification(record, step);
        }
        if (step.Outcome == OutcomeClass::Pending || step.Outcome == OutcomeClass::Deferred) return step;
        if (!step) return FailVerification(record, step);
        return {OutcomeClass::Pending, {}};
    }

    Result FailStaging(OTAControlRecord<TCapacityProfile>& record, const Result& failure) noexcept {
        const auto transaction = record.Active.Transaction;
        const auto abandoned = control_.AbandonTransaction(transaction);
        if (abandoned != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(abandoned);
        if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
        const auto terminal = CoordinatorDetail::TerminalOutcomeFor(failure);
        return PublishTerminal(record, transaction, terminal, UpdateOperation::Stage, failure)
            ? failure : ProjectionFailure();
    }

    Result CompleteStaging(OTAControlRecord<TCapacityProfile>& record) noexcept {
        const auto staged = control_.AdvanceRecoveryPoint(record.Active.Transaction, RecoveryPoint::Staged);
        if (staged != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(staged);
        if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
        if (!PublishActive(record, UpdateLifecycle::Staged, false)) return ProjectionFailure();
        return {OutcomeClass::Pending, {}};
    }

    Result AdvanceStaging(OTAControlRecord<TCapacityProfile>& record) noexcept {
        if (!verifiedManifestBound_ || verifiedManifest_ == nullptr || boundManifest_ != record.Active.Manifest) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::VerifiedManifestRequired);
        }
        if (targetProfile_ == nullptr || !targetProfile_->IsFrozen()) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::TargetProfileRequired);
        }
        if (!status_.UpdatePlanReady || !plan_.IsReady()) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::UpdatePlanUnavailable);
        }

        if (record.Active.Point == RecoveryPoint::ArtifactsVerified) {
            const auto preflight = PreflightUpdatePlan(
                plan_, *targetProfile_, record.Active.Transaction, record.Active.CandidateGeneration);
            if (preflight.Outcome == OutcomeClass::Pending || preflight.Outcome == OutcomeClass::Deferred) return preflight;
            if (!preflight) return FailStaging(record, preflight);

            const auto started = control_.AdvanceRecoveryPoint(
                record.Active.Transaction, RecoveryPoint::StagingStarted);
            if (started != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(started);
            if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
            if (!PublishActive(record, UpdateLifecycle::Staging, false)) return ProjectionFailure();

            const auto begun = staging_.Begin(
                *verifiedManifest_, plan_, *targetProfile_, record.Active.Transaction,
                record.Active.CandidateGeneration, record.Committed.Generation, false);
            if (staging_.IsComplete()) return CompleteStaging(record);
            if (begun.Outcome == OutcomeClass::Pending || begun.Outcome == OutcomeClass::Deferred) return begun;
            if (!begun) return FailStaging(record, begun);
            return {OutcomeClass::Pending, {}};
        }

        if (record.Active.Point != RecoveryPoint::StagingStarted) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::InvalidLifecycleTransition);
        }

        if (!staging_.IsActive() && !staging_.IsComplete()) {
            const auto begun = staging_.Begin(
                *verifiedManifest_, plan_, *targetProfile_, record.Active.Transaction,
                record.Active.CandidateGeneration, record.Committed.Generation, true);
            if (staging_.IsComplete()) return CompleteStaging(record);
            if (!staging_.IsActive()) {
                if (begun.Outcome == OutcomeClass::Pending || begun.Outcome == OutcomeClass::Deferred) return begun;
                return RecoveryRequired(&record);
            }
            if (begun.Outcome != OutcomeClass::Pending && begun.Outcome != OutcomeClass::Deferred && !begun) {
                return RecoveryRequired(&record);
            }
        }

        const auto step = staging_.Advance();
        if (staging_.IsComplete()) return CompleteStaging(record);
        if (staging_.Phase() == ComponentStagingPhase::Failed) return FailStaging(record, step);
        if (step.Outcome == OutcomeClass::Pending || step.Outcome == OutcomeClass::Deferred) return step;
        if (!step) return FailStaging(record, step);
        return {OutcomeClass::Pending, {}};
    }

    Result AdvanceComponentLifecycle(
        OTAControlRecord<TCapacityProfile>& record,
        ComponentLifecycleOperation operation) noexcept {
        if (!verifiedManifestBound_ || verifiedManifest_ == nullptr || boundManifest_ != record.Active.Manifest) {
            return CoordinatorDetail::CoreResult(
                OutcomeClass::Unavailable, CoordinatorCoreReason::VerifiedManifestRequired);
        }
        if (targetProfile_ == nullptr || !targetProfile_->IsFrozen()) {
            return CoordinatorDetail::CoreResult(
                OutcomeClass::Unavailable, CoordinatorCoreReason::TargetProfileRequired);
        }
        if (!status_.UpdatePlanReady || !plan_.IsReady()) {
            return CoordinatorDetail::CoreResult(
                OutcomeClass::Unavailable, CoordinatorCoreReason::UpdatePlanUnavailable);
        }

        if ((componentLifecycle_.IsActive() || componentLifecycle_.IsComplete()) &&
            componentLifecycle_.Operation() != operation) {
            componentLifecycle_.Reset();
        }
        if (!componentLifecycle_.IsActive() && !componentLifecycle_.IsComplete()) {
            const auto begun = componentLifecycle_.Begin(
                *verifiedManifest_, plan_, *targetProfile_, record.Active.Transaction,
                record.Active.CandidateGeneration, operation, true);
            if (componentLifecycle_.IsComplete()) return Result::Success();
            if (!componentLifecycle_.IsActive()) return begun;
            if (begun.Outcome != OutcomeClass::Pending &&
                begun.Outcome != OutcomeClass::Deferred && !begun) return begun;
        }

        const auto step = componentLifecycle_.Advance();
        if (componentLifecycle_.IsComplete()) return Result::Success();
        if (step.Outcome == OutcomeClass::Pending || step.Outcome == OutcomeClass::Deferred) return step;
        if (!step) return step;
        return {OutcomeClass::Pending, {}};
    }

    static std::uint64_t TicksToNanoseconds(Platform::Clock::Tick ticks) noexcept {
        constexpr std::uint64_t frequency = Platform::Clock::FrequencyHz<TClock>;
        constexpr std::uint64_t billion = Platform::Clock::NanosecondsPerSecond;
        const std::uint64_t seconds = ticks / frequency;
        const std::uint64_t remainder = ticks % frequency;
        if (seconds > std::numeric_limits<std::uint64_t>::max() / billion) return std::numeric_limits<std::uint64_t>::max();
        const std::uint64_t base = seconds * billion;
        std::uint64_t fractional = 0U;
        if constexpr (frequency <= billion) {
            fractional = (remainder * billion) / frequency;
        } else {
            const long double scaled = (static_cast<long double>(remainder) * static_cast<long double>(billion)) /
                                       static_cast<long double>(frequency);
            fractional = static_cast<std::uint64_t>(scaled);
        }
        return base > std::numeric_limits<std::uint64_t>::max() - fractional
            ? std::numeric_limits<std::uint64_t>::max() : base + fractional;
    }

    std::uint64_t TrialElapsed() const noexcept {
        if (!trialClockStarted_) return 0U;
        return TicksToNanoseconds(Platform::Clock::Elapsed<TClock>(trialStartTick_, clock_.Now()));
    }

    bool HealthDue() const noexcept {
        if (!healthEvaluated_ || healthReevaluationNanoseconds_ == 0U) return true;
        return TicksToNanoseconds(Platform::Clock::Elapsed<TClock>(lastHealthEvaluationTick_, clock_.Now())) >=
               healthReevaluationNanoseconds_;
    }

    Result StartRollback(OTAControlRecord<TCapacityProfile>& record, const Result& cause) noexcept {
        componentLifecycle_.Reset();
        rollbackRestartIssued_ = false;
        const auto persisted = control_.PersistRollbackIntent(record.Active.Transaction);
        if (persisted != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(persisted);
        if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
        const bool executingCandidate =
            boot_.CurrentBootTarget() == record.Active.CandidateBootTarget;
        if (!PublishActive(record, UpdateLifecycle::RollbackPending, executingCandidate)) return ProjectionFailure();
        return cause;
    }

    Result EvaluateHealth(OTAControlRecord<TCapacityProfile>& record) noexcept {
        if (!verifiedManifestBound_ || boundManifest_ != record.Active.Manifest) {
            if (!PublishActive(record, UpdateLifecycle::HealthValidation, true, true)) return ProjectionFailure();
            return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::VerifiedManifestRequired);
        }
        const auto elapsed = TrialElapsed();
        if (elapsed >= trialTimeoutNanoseconds_) {
            const Result expired{OutcomeClass::Failed,
                {DiagnosticDomain::Health, static_cast<std::uint32_t>(HealthCoreReason::TrialDeadlineExpired), 0, {}, 0U}};
            return StartRollback(record, expired);
        }
        if (!HealthDue()) {
            if (!PublishActive(record, UpdateLifecycle::HealthValidation, true, true)) return ProjectionFailure();
            return {OutcomeClass::Pending, {DiagnosticDomain::Health, 0U, 0, {}, 0U}};
        }

        HealthCommonContext common;
        common.Transaction = record.Active.Transaction;
        common.CandidateGeneration = record.Active.CandidateGeneration;
        common.Release = record.Active.Release;
        common.Manifest = record.Active.Manifest;
        common.ExecutingGeneration = record.Active.CandidateGeneration;
        common.CommittedGeneration = record.Committed.Generation;
        common.TrialElapsedNanoseconds = elapsed;
        common.TrialRemainingNanoseconds = trialTimeoutNanoseconds_ - elapsed;
        common.TrialDeadlineExpired = false;

        HealthDecision decisive = HealthDecision::Pass();
        HealthConditionTypeId decisiveCondition{};
        std::uint8_t rank = 0U;
        for (std::size_t i = 0U; i < requiredHealthCount_; ++i) {
            auto* evaluator = health_.Find(requiredHealth_[i]);
            if (evaluator == nullptr) {
                const Result unsupported{OutcomeClass::Unsupported,
                    {DiagnosticDomain::Health, static_cast<std::uint32_t>(HealthCoreReason::MissingRequiredCondition), 0, {}, 0U}};
                return StartRollback(record, unsupported);
            }
            const auto evaluation = evaluator->Evaluate(common);
            const auto currentRank = PolicyHealthDetail::HealthRank(evaluation.Decision.Result);
            if (currentRank > rank || (currentRank == rank && currentRank != 0U &&
                (!decisiveCondition || evaluation.Condition < decisiveCondition))) {
                decisive = evaluation.Decision;
                decisiveCondition = evaluation.Condition;
                rank = currentRank;
            }
        }
        healthEvaluated_ = true;
        lastHealthEvaluationTick_ = clock_.Now();
        if (!PublishActive(record, UpdateLifecycle::HealthValidation, true, true)) return ProjectionFailure();
        if (decisive.Result == HealthResult::Fail) return StartRollback(record, {OutcomeClass::Failed, decisive.Detail});
        if (decisive.Result == HealthResult::Pending) return {OutcomeClass::Pending, decisive.Detail};

        const auto persisted = control_.PersistCommitIntent(record.Active.Transaction);
        if (persisted != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(persisted);
        if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
        if (!PublishActive(record, UpdateLifecycle::Committing, true, true)) return ProjectionFailure();
        return {OutcomeClass::Pending, {DiagnosticDomain::OTA, 0U, 0, {}, 0U}};
    }

public:
    Coordinator(ControlStore& control,
                OTAStateOwners& states,
                TBootControl& boot,
                TTrialBoot& trial,
                TRestart& restart,
                TClock& clock,
                ActivatePolicies& activatePolicies,
                HealthChecks& health,
                ComponentHandlerDirectory<TCapacityProfile>& handlers,
                IArtifactSource& artifactSource,
                IReadableArtifactStore& artifactStore,
                ArtifactCheckpointStore<TCapacityProfile>& artifactCheckpoints,
                ArtifactTransferWorkspace<TCapacityProfile>& artifactWorkspace,
                Security::IStreamingDigestVerifier& digestVerifier,
                std::uint64_t trialTimeoutNanoseconds,
                std::uint64_t healthReevaluationNanoseconds = 0U) noexcept
        : control_(control), states_(states), boot_(boot), trial_(trial), restart_(restart), clock_(clock),
          activatePolicies_(activatePolicies), health_(health), handlers_(handlers),
          acquisition_(artifactSource, artifactStore, artifactCheckpoints, artifactWorkspace),
          verification_(artifactStore, digestVerifier, artifactWorkspace),
          staging_(artifactStore, stagePolicies_),
          componentLifecycle_(artifactStore),
          trialTimeoutNanoseconds_(trialTimeoutNanoseconds),
          healthReevaluationNanoseconds_(healthReevaluationNanoseconds) {}

    Coordinator(ControlStore& control,
                OTAStateOwners& states,
                TBootControl& boot,
                TTrialBoot& trial,
                TRestart& restart,
                TClock& clock,
                ActivatePolicies& activatePolicies,
                HealthChecks& health,
                ComponentHandlerDirectory<TCapacityProfile>& handlers,
                IArtifactSource& artifactSource,
                bool artifactSourceOffsetRead,
                IReadableArtifactStore& artifactStore,
                IPartialArtifactStore& partialArtifactStore,
                ArtifactCheckpointStore<TCapacityProfile>& artifactCheckpoints,
                ArtifactTransferWorkspace<TCapacityProfile>& artifactWorkspace,
                Security::IStreamingDigestVerifier& digestVerifier,
                std::uint64_t trialTimeoutNanoseconds,
                std::uint64_t healthReevaluationNanoseconds = 0U) noexcept
        : control_(control), states_(states), boot_(boot), trial_(trial), restart_(restart), clock_(clock),
          activatePolicies_(activatePolicies), health_(health), handlers_(handlers),
          acquisition_(artifactSource, artifactSourceOffsetRead, artifactStore, partialArtifactStore,
                       artifactCheckpoints, artifactWorkspace),
          verification_(artifactStore, digestVerifier, artifactWorkspace),
          staging_(artifactStore, stagePolicies_),
          componentLifecycle_(artifactStore),
          trialTimeoutNanoseconds_(trialTimeoutNanoseconds),
          healthReevaluationNanoseconds_(healthReevaluationNanoseconds) {}

    PolicyGateRegistrationStatus AddStagePolicy(IPolicyGate<StagePolicyDecisionPoint>& gate) noexcept {
        return stagePolicies_.Add(gate);
    }

    Result ConfigureArtifactSourceSelection(
        IArtifactSourceSelector& selector,
        std::size_t maximumAttempts) noexcept {
        MutationGuard guard{*this};
        if (!guard) {
            return CoordinatorDetail::CoreResult(
                OutcomeClass::Unavailable, CoordinatorCoreReason::ReentrantMutation);
        }
        if (maximumAttempts == 0U) {
            return CoordinatorDetail::CoreResult(
                OutcomeClass::Invalid, CoordinatorCoreReason::InvalidConfiguration);
        }
        if (status_.HasActiveTransaction || acquisitionStarted_ || acquisition_.IsActive()) {
            return CoordinatorDetail::CoreResult(
                OutcomeClass::Unavailable, CoordinatorCoreReason::Busy);
        }
        artifactSourceSelector_ = &selector;
        maximumArtifactSourceAttempts_ = maximumAttempts;
        acquisitionSourceAttempt_ = 0U;
        return Result::Success();
    }

    Result Initialize() noexcept {
        MutationGuard guard{*this};
        if (!guard) return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::ReentrantMutation);
        if (initialized_) return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::AlreadyInitialized);
        if (!states_ || trialTimeoutNanoseconds_ == 0U) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::InvalidConfiguration);
        }
        OTAControlRecord<TCapacityProfile> record;
        const auto loaded = control_.Load(record);
        if (loaded != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(loaded);
        initialized_ = true;
        status_.CommittedGeneration = record.Committed.Generation;
        if (record.Intent == DurableIntent::RecoveryRequired) return RecoveryRequired(&record);
        if (!record.HasActiveTransaction) return PublishReady(record, true) ? Result::Success() : ProjectionFailure();

        const auto current = boot_.CurrentBootTarget();
        bool executingCandidate = false;
        UpdateLifecycle lifecycle = CoordinatorDetail::LifecycleForRecoveryPoint(record.Active.Point);
        if (record.Intent == DurableIntent::ActivationArmed) {
            if (current == record.Active.CandidateBootTarget) {
                if (!trial_.IsCurrentBootTrial()) return RecoveryRequired(&record);
                executingCandidate = true;
            } else if (current == record.Active.PreviousCommittedBootTarget) {
                activationRecovered_ = true;
            } else {
                return RecoveryRequired(&record);
            }
            lifecycle = UpdateLifecycle::Activating;
        } else if (record.Intent == DurableIntent::CommitIntent) {
            if (current != record.Active.CandidateBootTarget) return RecoveryRequired(&record);
            executingCandidate = true;
            lifecycle = UpdateLifecycle::Committing;
        } else if (record.Intent == DurableIntent::RollbackIntent) {
            if (current == record.Active.CandidateBootTarget) {
                executingCandidate = true;
            } else if (current != record.Active.PreviousCommittedBootTarget) {
                return RecoveryRequired(&record);
            }
            lifecycle = UpdateLifecycle::RollingBack;
        } else if (record.Active.Point == RecoveryPoint::TrialBootEntered) {
            if (current != record.Active.CandidateBootTarget || !trial_.IsCurrentBootTrial()) return RecoveryRequired(&record);
            executingCandidate = true;
            lifecycle = UpdateLifecycle::Trial;
            trialClockStarted_ = true;
            trialStartTick_ = clock_.Now();
            lastHealthEvaluationTick_ = trialStartTick_;
        } else if (record.Active.Point >= RecoveryPoint::ActivationSelected) return RecoveryRequired(&record);

        if (!PublishActive(record, lifecycle, executingCandidate, true)) return ProjectionFailure();
        return Result::Success();
    }

    CoordinatorStatusSnapshot Status() const noexcept { return status_; }

    Result BindVerifiedManifest(
        const Manifest<TCapacityProfile>& manifest,
        const UpdateTargetProfile<TCapacityProfile>& targetProfile) noexcept {
        MutationGuard guard{*this};
        if (!guard) return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::ReentrantMutation);
        if (!initialized_) return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::NotInitialized);
        if (acquisitionStarted_ || acquisition_.IsActive() || verificationStarted_ || verification_.IsActive() ||
            staging_.IsActive() || componentLifecycle_.IsActive()) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::Busy);
        }
        if (!targetProfile.IsFrozen()) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::TargetProfileRequired);
        }
        if (ValidateManifest(manifest) != ManifestStatus::Success) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::VerifiedManifestMismatch);
        }
        OTAControlRecord<TCapacityProfile> record;
        const auto loaded = control_.Load(record);
        if (loaded != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(loaded);
        if (!record.HasActiveTransaction) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::NoActiveTransaction);
        }
        const ManifestIdentifier identifier{manifest.Identifier};
        if (identifier != record.Active.Manifest || manifest.Release != record.Active.Release.Value() ||
            manifest.SecurityGeneration != record.Active.CandidateSecurity.Value()) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::VerifiedManifestMismatch);
        }

        UpdatePlan<TCapacityProfile> candidatePlan;
        const auto planStatus = BuildUpdatePlan(manifest, handlers_, candidatePlan);
        if (planStatus != UpdatePlanStatus::Success) {
            OutcomeClass outcome = OutcomeClass::Invalid;
            if (planStatus == UpdatePlanStatus::HandlerUnavailable) outcome = OutcomeClass::Unsupported;
            if (planStatus == UpdatePlanStatus::HandlerDirectoryNotFrozen) outcome = OutcomeClass::Unavailable;
            if (planStatus == UpdatePlanStatus::CapacityUnavailable) outcome = OutcomeClass::CapacityUnavailable;
            return {outcome, {DiagnosticDomain::Component, static_cast<std::uint32_t>(planStatus), 0, {}, 0U}};
        }

        for (const auto rawCondition : manifest.RequiredHealthConditions) {
            if (health_.Find(HealthConditionTypeId{rawCondition}) == nullptr) {
                return CoordinatorDetail::CoreResult(
                    OutcomeClass::Unsupported, CoordinatorCoreReason::RequiredHealthUnavailable);
            }
        }

        if (record.Intent == DurableIntent::None &&
            record.Active.Point < RecoveryPoint::StagingStarted) {
            const auto preflight = PreflightUpdatePlan(
                candidatePlan, targetProfile, record.Active.Transaction, record.Active.CandidateGeneration);
            if (!preflight) return preflight;
        }

        if (record.Active.Point == RecoveryPoint::TransactionCreated) {
            const auto accepted = control_.AdvanceRecoveryPoint(
                record.Active.Transaction, RecoveryPoint::ManifestAccepted);
            if (accepted != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(accepted);
            if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
        } else if (record.Active.Point < RecoveryPoint::ManifestAccepted) {
            return CoordinatorDetail::CoreResult(
                OutcomeClass::Invalid, CoordinatorCoreReason::InvalidLifecycleTransition);
        }

        requiredHealthCount_ = manifest.RequiredHealthConditions.size();
        for (std::size_t i = 0U; i < requiredHealthCount_; ++i) {
            requiredHealth_[i] = HealthConditionTypeId{manifest.RequiredHealthConditions[i]};
        }
        plan_ = candidatePlan;
        verifiedManifest_ = &manifest;
        targetProfile_ = &targetProfile;
        boundManifest_ = identifier;
        verifiedManifestBound_ = true;
        acquisitionArtifactIndex_ = 0U;
        acquisitionStarted_ = false;
        acquisitionSourceAttempt_ = 0U;
        verificationArtifactIndex_ = 0U;
        verificationStarted_ = false;
        staging_.Reset();
        componentLifecycle_.Reset();
        activationRestartIssued_ = false;
        rollbackRestartIssued_ = false;
        status_.VerifiedManifestBound = true;
        status_.UpdatePlanReady = true;

        bool executingCandidate = false;
        if (record.Active.Point >= RecoveryPoint::ActivationSelected &&
            record.Active.CandidateBootTarget) {
            executingCandidate = boot_.CurrentBootTarget() == record.Active.CandidateBootTarget;
        }
        auto lifecycle = CoordinatorDetail::LifecycleForRecoveryPoint(record.Active.Point);
        if (record.Intent == DurableIntent::ActivationArmed) lifecycle = UpdateLifecycle::Activating;
        else if (record.Intent == DurableIntent::CommitIntent) lifecycle = UpdateLifecycle::Committing;
        else if (record.Intent == DurableIntent::RollbackIntent) lifecycle = UpdateLifecycle::RollingBack;
        if (!PublishActive(record, lifecycle, executingCandidate, true)) {
            return ProjectionFailure();
        }
        status_.VerifiedManifestBound = true;
        status_.UpdatePlanReady = true;
        return Result::Success();
    }

    Result Start(const CoordinatorStartRequest& request, UpdateTransactionId& transaction) noexcept {
        transaction = {};
        MutationGuard guard{*this};
        if (!guard) return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::ReentrantMutation);
        if (!initialized_) return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::NotInitialized);
        if (!request.IsValid()) return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::InvalidStartRequest);
        if (status_.Availability == CoordinatorAvailability::RecoveryRequired) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::RecoveryRequired);
        }
        OTAControlRecord<TCapacityProfile> record;
        const auto loaded = control_.Load(record);
        if (loaded != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(loaded);
        if (record.HasActiveTransaction) return CoordinatorDetail::CoreResult(OutcomeClass::Deferred, CoordinatorCoreReason::Busy);
        if (record.Intent == DurableIntent::RecoveryRequired) return RecoveryRequired(&record);

        ActiveTransactionRecord active;
        const auto begun = control_.BeginTransaction(request.Release, request.Manifest, request.CandidateSecurity, active);
        if (begun != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(begun);
        if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
        ClearBoundExecutionState();
        hasLifecycle_ = false;
        lifecycle_ = UpdateLifecycle::Idle;
        if (!PublishActive(record, UpdateLifecycle::Checking, false, true)) return ProjectionFailure();
        transaction = active.Transaction;
        return Result::Success();
    }

    Result Cancel(UpdateTransactionId transaction) noexcept {
        MutationGuard guard{*this};
        if (!guard) return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::ReentrantMutation);
        if (!initialized_) return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::NotInitialized);
        OTAControlRecord<TCapacityProfile> record;
        const auto loaded = control_.Load(record);
        if (loaded != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(loaded);
        if (!record.HasActiveTransaction) return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::NoActiveTransaction);
        if (!transaction || record.Active.Transaction != transaction) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::TransactionMismatch);
        }
        if (record.Intent == DurableIntent::CommitIntent) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::InvalidLifecycleTransition);
        }
        if (record.Active.Point < RecoveryPoint::ActivationSelected) {
            if (acquisitionStarted_ || acquisition_.IsActive()) {
                const auto aborted = acquisition_.Abort();
                if (!aborted) return RecoveryRequired(&record);
                acquisitionStarted_ = false;
                acquisitionSourceAttempt_ = 0U;
            }
            if (verificationStarted_ || verification_.IsActive()) {
                verification_.Abort();
                verificationStarted_ = false;
            }
            staging_.Reset();
            componentLifecycle_.Reset();
            const auto abandoned = control_.AbandonTransaction(transaction);
            if (abandoned != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(abandoned);
            if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
            const Result cancelled = Result::Success();
            return PublishTerminal(record, transaction, TerminalUpdateOutcome::Cancelled,
                                   UpdateOperation::Check, cancelled) ? cancelled : ProjectionFailure();
        }
        if (record.Intent == DurableIntent::RollbackIntent) return {OutcomeClass::Pending, {}};
        return StartRollback(record, {OutcomeClass::Pending, {}});
    }

    Result BeginActivation(const CoordinatorActivationRequest& request) noexcept {
        MutationGuard guard{*this};
        if (!guard) return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::ReentrantMutation);
        if (!initialized_) return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::NotInitialized);
        if (!request.IsValid()) return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::InvalidActivationRequest);
        OTAControlRecord<TCapacityProfile> record;
        const auto loaded = control_.Load(record);
        if (loaded != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(loaded);
        if (!record.HasActiveTransaction) return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::NoActiveTransaction);
        if (record.Active.Transaction != request.Transaction) return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::TransactionMismatch);
        if (record.Intent != DurableIntent::None || record.Active.Point != RecoveryPoint::Staged) {
            return CoordinatorDetail::CoreResult(OutcomeClass::Invalid, CoordinatorCoreReason::InvalidLifecycleTransition);
        }
        if (!verifiedManifestBound_ || verifiedManifest_ == nullptr ||
            boundManifest_ != record.Active.Manifest) {
            return CoordinatorDetail::CoreResult(
                OutcomeClass::Unavailable, CoordinatorCoreReason::VerifiedManifestRequired);
        }
        if (!verifiedManifest_->CandidateDurableSchemaSupport.CanRead(record.SchemaVersion.Value())) {
            return CoordinatorDetail::CoreResult(
                OutcomeClass::Unsupported, CoordinatorCoreReason::CandidateCannotReadCurrentOTASchema);
        }
        if (!verifiedManifest_->CandidateManifestSchemaSupport.CanRead(verifiedManifest_->SchemaVersion)) {
            return CoordinatorDetail::CoreResult(
                OutcomeClass::Unsupported, CoordinatorCoreReason::CandidateManifestSchemaUnsupported);
        }
        const auto previous = boot_.CommittedBootTarget();
        if (!previous || boot_.CurrentBootTarget() != previous || request.CandidateBootTarget == previous) return RecoveryRequired(&record);

        PolicyContext<ActivatePolicyDecisionPoint> context;
        context.Common.Transaction = record.Active.Transaction;
        context.Common.Release = record.Active.Release;
        context.Common.Manifest = record.Active.Manifest;
        context.Common.CandidateGeneration = record.Active.CandidateGeneration;
        context.Common.CandidateSecurityGeneration = record.Active.CandidateSecurity;
        context.Common.CommittedGeneration = record.Committed.Generation;
        context.RestartRequired = request.RestartRequired;
        context.InterruptsCurrentRuntime = request.InterruptsCurrentRuntime;
        context.TrialBootAvailable = true;
        context.TrialBootRequired = request.TrialBootRequired;
        const auto policy = activatePolicies_.Evaluate(context);
        if (policy.Decision.Verdict == PolicyVerdict::Defer) {
            if (!PublishActive(record, UpdateLifecycle::ActivationPending, false)) return ProjectionFailure();
            return {OutcomeClass::Deferred, policy.Decision.Detail};
        }
        if (policy.Decision.Verdict == PolicyVerdict::Reject) {
            const auto transaction = record.Active.Transaction;
            const auto abandoned = control_.AbandonTransaction(transaction);
            if (abandoned != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(abandoned);
            if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
            const Result rejected{OutcomeClass::Rejected, policy.Decision.Detail};
            return PublishTerminal(record, transaction, TerminalUpdateOutcome::Rejected,
                                   UpdateOperation::Activate, rejected) ? rejected : ProjectionFailure();
        }
        const auto armed = control_.ArmActivation(record.Active.Transaction, request.CandidateBootTarget, previous);
        if (armed != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(armed);
        if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
        componentLifecycle_.Reset();
        activationRestartIssued_ = false;
        activationRecovered_ = false;
        if (!PublishActive(record, UpdateLifecycle::Activating, false)) return ProjectionFailure();
        return {OutcomeClass::Pending, {}};
    }

    Result Advance() noexcept {
        MutationGuard guard{*this};
        if (!guard) return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::ReentrantMutation);
        if (!initialized_) return CoordinatorDetail::CoreResult(OutcomeClass::Unavailable, CoordinatorCoreReason::NotInitialized);
        OTAControlRecord<TCapacityProfile> record;
        const auto loaded = control_.Load(record);
        if (loaded != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(loaded);
        if (record.Intent == DurableIntent::RecoveryRequired) return RecoveryRequired(&record);
        if (!record.HasActiveTransaction) return Result::Success();

        if (record.Intent == DurableIntent::ActivationArmed) {
            if (activationRecovered_) {
                if constexpr (!Platform::OTA::HasTrialBootStateInspectionV<TTrialBoot>) {
                    return RecoveryRequired(&record);
                } else {
                    const auto inspected = trial_.InspectBootTargetTrialState(
                        record.Active.CandidateBootTarget);
                    if (!inspected) return RecoveryRequired(&record);
                    switch (inspected.State) {
                        case Platform::OTA::TrialBootState::NeverAttempted:
                        case Platform::OTA::TrialBootState::Armed:
                            activationRecovered_ = false;
                            break;
                        case Platform::OTA::TrialBootState::Rejected:
                            activationRecovered_ = false;
                            return StartRollback(record, CoordinatorDetail::CoreResult(
                                OutcomeClass::Failed, CoordinatorCoreReason::CandidateTrialRejected));
                        case Platform::OTA::TrialBootState::Unknown:
                        case Platform::OTA::TrialBootState::Untracked:
                        case Platform::OTA::TrialBootState::PendingValidation:
                        case Platform::OTA::TrialBootState::Accepted:
                            return RecoveryRequired(&record);
                    }
                }
            }

            const auto componentStep = AdvanceComponentLifecycle(
                record, ComponentLifecycleOperation::Activate);
            if (componentStep.Outcome == OutcomeClass::Pending ||
                componentStep.Outcome == OutcomeClass::Deferred ||
                componentStep.Outcome == OutcomeClass::Unavailable) return componentStep;
            if (!componentStep) return StartRollback(record, componentStep);

            const auto current = boot_.CurrentBootTarget();
            if (current == record.Active.CandidateBootTarget) {
                if (!trial_.IsCurrentBootTrial()) return RecoveryRequired(&record);
                if (componentLifecycle_.RestartRequired() && !activationRestartIssued_) {
                    const auto restart = CoordinatorDetail::PlatformResult(
                        restart_.Restart(Platform::OTA::RestartReason::ActivateCandidate));
                    if (restart.Outcome != OutcomeClass::Success && restart.Outcome != OutcomeClass::Pending) return restart;
                    activationRestartIssued_ = true;
                    if (!PublishActive(record, UpdateLifecycle::AwaitingRestart, true, true)) return ProjectionFailure();
                    return CoordinatorDetail::CoreResult(
                        OutcomeClass::Pending, CoordinatorCoreReason::AwaitingRestart);
                }
                const auto marked = control_.MarkTrialEntered(record.Active.Transaction);
                if (marked != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(marked);
                if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
                componentLifecycle_.Reset();
                trialClockStarted_ = true;
                trialStartTick_ = clock_.Now();
                lastHealthEvaluationTick_ = trialStartTick_;
                healthEvaluated_ = false;
                if (!PublishActive(record, UpdateLifecycle::Trial, true, true)) return ProjectionFailure();
                return {OutcomeClass::Pending, {}};
            }
            if (current != record.Active.PreviousCommittedBootTarget) return RecoveryRequired(&record);
            if (boot_.NextBootTarget() != record.Active.CandidateBootTarget) {
                const auto mapped = CoordinatorDetail::PlatformResult(
                    boot_.SelectNextBootTarget(record.Active.CandidateBootTarget));
                if (mapped.Outcome != OutcomeClass::Success) return mapped;
                if (!PublishActive(record, UpdateLifecycle::AwaitingRestart, false, true)) return ProjectionFailure();
                return {OutcomeClass::Pending, {}};
            }
            const auto mapped = CoordinatorDetail::PlatformResult(
                restart_.Restart(Platform::OTA::RestartReason::ActivateCandidate));
            if (mapped.Outcome != OutcomeClass::Success && mapped.Outcome != OutcomeClass::Pending) return mapped;
            if (!PublishActive(record, UpdateLifecycle::AwaitingRestart, false, true)) return ProjectionFailure();
            return CoordinatorDetail::CoreResult(OutcomeClass::Pending, CoordinatorCoreReason::AwaitingRestart);
        }

        if (record.Intent == DurableIntent::CommitIntent) {
            if (boot_.CurrentBootTarget() != record.Active.CandidateBootTarget) return RecoveryRequired(&record);
            if (trial_.IsCurrentBootTrial()) {
                const auto platform = CoordinatorDetail::PlatformResult(trial_.MarkCurrentBootValid());
                if (platform.Outcome != OutcomeClass::Success) return platform;
            }

            const auto componentStep = AdvanceComponentLifecycle(
                record, ComponentLifecycleOperation::Commit);
            if (componentStep.Outcome == OutcomeClass::Pending ||
                componentStep.Outcome == OutcomeClass::Deferred ||
                componentStep.Outcome == OutcomeClass::Unavailable) return componentStep;
            if (!componentStep || componentLifecycle_.RestartRequired()) return RecoveryRequired(&record);

            const auto transaction = record.Active.Transaction;
            const auto finalized = control_.FinalizeCommit(transaction);
            if (finalized != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(finalized);
            if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
            const Result success = Result::Success();
            return PublishTerminal(record, transaction, TerminalUpdateOutcome::Completed,
                                   UpdateOperation::Commit, success) ? success : ProjectionFailure();
        }

        if (record.Intent == DurableIntent::RollbackIntent) {
            const auto componentStep = AdvanceComponentLifecycle(
                record, ComponentLifecycleOperation::Rollback);
            if (componentStep.Outcome == OutcomeClass::Pending ||
                componentStep.Outcome == OutcomeClass::Deferred ||
                componentStep.Outcome == OutcomeClass::Unavailable) return componentStep;
            if (!componentStep) return RecoveryRequired(&record);

            const auto current = boot_.CurrentBootTarget();
            if (current == record.Active.PreviousCommittedBootTarget) {
                if (componentLifecycle_.RestartRequired() && !rollbackRestartIssued_) {
                    const auto restart = CoordinatorDetail::PlatformResult(
                        restart_.Restart(Platform::OTA::RestartReason::Rollback));
                    if (restart.Outcome != OutcomeClass::Success && restart.Outcome != OutcomeClass::Pending) return restart;
                    rollbackRestartIssued_ = true;
                    if (!PublishActive(record, UpdateLifecycle::RollingBack, false, true)) return ProjectionFailure();
                    return CoordinatorDetail::CoreResult(
                        OutcomeClass::Pending, CoordinatorCoreReason::AwaitingRestart);
                }
                const auto transaction = record.Active.Transaction;
                const auto finalized = control_.FinalizeRollback(transaction);
                if (finalized != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(finalized);
                if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
                const Result success = Result::Success();
                return PublishTerminal(record, transaction, TerminalUpdateOutcome::RolledBack,
                                       UpdateOperation::Rollback, success) ? success : ProjectionFailure();
            }
            if (current != record.Active.CandidateBootTarget) return RecoveryRequired(&record);
            if (trial_.IsCurrentBootTrial()) {
                const auto mapped = CoordinatorDetail::PlatformResult(trial_.MarkCurrentBootInvalid());
                if (mapped.Outcome != OutcomeClass::Success && mapped.Outcome != OutcomeClass::Pending) return mapped;
                if (!PublishActive(record, UpdateLifecycle::RollingBack, true, true)) return ProjectionFailure();
                return {OutcomeClass::Pending, {}};
            }
            if (boot_.NextBootTarget() != record.Active.PreviousCommittedBootTarget) {
                const auto mapped = CoordinatorDetail::PlatformResult(
                    boot_.SelectNextBootTarget(record.Active.PreviousCommittedBootTarget));
                if (mapped.Outcome != OutcomeClass::Success) return mapped;
                if (!PublishActive(record, UpdateLifecycle::RollingBack, true, true)) return ProjectionFailure();
                return {OutcomeClass::Pending, {}};
            }
            const auto mapped = CoordinatorDetail::PlatformResult(
                restart_.Restart(Platform::OTA::RestartReason::Rollback));
            if (mapped.Outcome != OutcomeClass::Success && mapped.Outcome != OutcomeClass::Pending) return mapped;
            if (!PublishActive(record, UpdateLifecycle::RollingBack, true, true)) return ProjectionFailure();
            return CoordinatorDetail::CoreResult(OutcomeClass::Pending, CoordinatorCoreReason::AwaitingRestart);
        }

        if (record.Active.Point == RecoveryPoint::TrialBootEntered) {
            if (boot_.CurrentBootTarget() != record.Active.CandidateBootTarget || !trial_.IsCurrentBootTrial()) return RecoveryRequired(&record);
            if (!trialClockStarted_) {
                trialClockStarted_ = true;
                trialStartTick_ = clock_.Now();
                lastHealthEvaluationTick_ = trialStartTick_;
            }
            return EvaluateHealth(record);
        }

        if (record.Active.Point == RecoveryPoint::ManifestAccepted) return AdvanceAcquisition(record);
        if (record.Active.Point == RecoveryPoint::ArtifactsAcquired) return AdvanceVerification(record);
        if (record.Active.Point == RecoveryPoint::ArtifactsVerified ||
            record.Active.Point == RecoveryPoint::StagingStarted) return AdvanceStaging(record);
        if (record.Active.Point == RecoveryPoint::Staged) {
            if (!PublishActive(record, UpdateLifecycle::Staged, false, true)) return ProjectionFailure();
            return {OutcomeClass::Pending, {}};
        }
        if (record.Active.Point < RecoveryPoint::ActivationSelected) {
            if (!PublishActive(record, CoordinatorDetail::LifecycleForRecoveryPoint(record.Active.Point), false, true)) return ProjectionFailure();
            return CoordinatorDetail::CoreResult(OutcomeClass::Pending, CoordinatorCoreReason::AwaitingExecutionIntegration);
        }
        return RecoveryRequired(&record);
    }
};

} // namespace ESPressio::OTA
