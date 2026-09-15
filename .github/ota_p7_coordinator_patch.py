from pathlib import Path

path = Path("src/ESPressio_OTACoordinator.hpp")
text = path.read_text()


def replace_once(old: str, new: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"expected exactly one match, found {count}: {old[:120]!r}")
    text = text.replace(old, new, 1)

replace_once(
    '#include "ESPressio_OTAExecution.hpp"\n#include "ESPressio_OTAState.hpp"',
    '#include "ESPressio_OTAExecution.hpp"\n#include "ESPressio_OTAComponentLifecycleExecution.hpp"\n#include "ESPressio_OTAState.hpp"')

replace_once(
    '    ComponentStagingSession<TCapacityProfile> staging_;\n    std::uint64_t trialTimeoutNanoseconds_{0U};',
    '    ComponentStagingSession<TCapacityProfile> staging_;\n    ComponentLifecycleExecutionSession<TCapacityProfile> componentLifecycle_;\n    std::uint64_t trialTimeoutNanoseconds_{0U};')

replace_once(
    '    bool healthEvaluated_{false};\n',
    '    bool healthEvaluated_{false};\n    bool activationRestartIssued_{false};\n    bool rollbackRestartIssued_{false};\n')

replace_once(
'''    void ClearBoundExecutionState() noexcept {
        verifiedManifestBound_ = false;''',
'''    void ClearBoundExecutionState() noexcept {
        staging_.Reset();
        componentLifecycle_.Reset();
        activationRestartIssued_ = false;
        rollbackRestartIssued_ = false;
        verifiedManifestBound_ = false;''')

replace_once(
'''            (void)PublishActive(*record, UpdateLifecycle::RecoveryRequired,
                                record->Active.Point >= RecoveryPoint::TrialBootEntered, true);''',
'''            bool executingCandidate = false;
            if (record->Active.Point >= RecoveryPoint::ActivationSelected &&
                record->Active.CandidateBootTarget) {
                executingCandidate = boot_.CurrentBootTarget() == record->Active.CandidateBootTarget;
            }
            (void)PublishActive(*record, UpdateLifecycle::RecoveryRequired,
                                executingCandidate, true);''')

marker = '''    static std::uint64_t TicksToNanoseconds(Platform::Clock::Tick ticks) noexcept {'''
helper = '''    Result AdvanceComponentLifecycle(
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

'''
replace_once(marker, helper + marker)

replace_once(
'''    Result StartRollback(OTAControlRecord<TCapacityProfile>& record, const Result& cause) noexcept {
        const auto persisted = control_.PersistRollbackIntent(record.Active.Transaction);''',
'''    Result StartRollback(OTAControlRecord<TCapacityProfile>& record, const Result& cause) noexcept {
        componentLifecycle_.Reset();
        rollbackRestartIssued_ = false;
        const auto persisted = control_.PersistRollbackIntent(record.Active.Transaction);''')

replace_once(
'''        if (!PublishActive(record, UpdateLifecycle::RollbackPending, true)) return ProjectionFailure();
        return cause;''',
'''        const bool executingCandidate =
            boot_.CurrentBootTarget() == record.Active.CandidateBootTarget;
        if (!PublishActive(record, UpdateLifecycle::RollbackPending, executingCandidate)) return ProjectionFailure();
        return cause;''')

replace_once(
'''          verification_(artifactStore, digestVerifier, artifactWorkspace),
          staging_(artifactStore, stagePolicies_),
          trialTimeoutNanoseconds_(trialTimeoutNanoseconds),''',
'''          verification_(artifactStore, digestVerifier, artifactWorkspace),
          staging_(artifactStore, stagePolicies_),
          componentLifecycle_(artifactStore),
          trialTimeoutNanoseconds_(trialTimeoutNanoseconds),''')

old_init = '''        if (record.Intent == DurableIntent::ActivationArmed) {
            if (current == record.Active.CandidateBootTarget) {
                if (!trial_.IsCurrentBootTrial()) return RecoveryRequired(&record);
                const auto marked = control_.MarkTrialEntered(record.Active.Transaction);
                if (marked != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(marked);
                if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
                executingCandidate = true;
                lifecycle = UpdateLifecycle::Trial;
                trialClockStarted_ = true;
                trialStartTick_ = clock_.Now();
                lastHealthEvaluationTick_ = trialStartTick_;
            } else if (current == record.Active.PreviousCommittedBootTarget) {
                lifecycle = UpdateLifecycle::AwaitingRestart;
            } else return RecoveryRequired(&record);
        } else if (record.Intent == DurableIntent::CommitIntent) {
            if (current != record.Active.CandidateBootTarget) return RecoveryRequired(&record);
            executingCandidate = true;
            lifecycle = UpdateLifecycle::Committing;
        } else if (record.Intent == DurableIntent::RollbackIntent) {
            if (current == record.Active.PreviousCommittedBootTarget) {
                const auto transaction = record.Active.Transaction;
                const auto finalized = control_.FinalizeRollback(transaction);
                if (finalized != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(finalized);
                if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
                return PublishTerminal(record, transaction, TerminalUpdateOutcome::RolledBack,
                                       UpdateOperation::Rollback, Result::Success()) ? Result::Success() : ProjectionFailure();
            }
            if (current != record.Active.CandidateBootTarget) return RecoveryRequired(&record);
            executingCandidate = true;
            lifecycle = UpdateLifecycle::RollingBack;
        } else if (record.Active.Point == RecoveryPoint::TrialBootEntered) {'''
new_init = '''        if (record.Intent == DurableIntent::ActivationArmed) {
            if (current == record.Active.CandidateBootTarget) {
                if (!trial_.IsCurrentBootTrial()) return RecoveryRequired(&record);
                executingCandidate = true;
            } else if (current != record.Active.PreviousCommittedBootTarget) {
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
        } else if (record.Active.Point == RecoveryPoint::TrialBootEntered) {'''
replace_once(old_init, new_init)

replace_once(
'''        if (acquisitionStarted_ || acquisition_.IsActive() || verificationStarted_ || verification_.IsActive() || staging_.IsActive()) {''',
'''        if (acquisitionStarted_ || acquisition_.IsActive() || verificationStarted_ || verification_.IsActive() ||
            staging_.IsActive() || componentLifecycle_.IsActive()) {''')

replace_once(
'''        const auto preflight = PreflightUpdatePlan(
            candidatePlan, targetProfile, record.Active.Transaction, record.Active.CandidateGeneration);
        if (!preflight) return preflight;
''',
'''        if (record.Intent == DurableIntent::None &&
            record.Active.Point < RecoveryPoint::StagingStarted) {
            const auto preflight = PreflightUpdatePlan(
                candidatePlan, targetProfile, record.Active.Transaction, record.Active.CandidateGeneration);
            if (!preflight) return preflight;
        }
''')

old_bind_publish = '''        status_.VerifiedManifestBound = true;
        status_.UpdatePlanReady = true;
        const bool executingCandidate = record.Active.Point >= RecoveryPoint::TrialBootEntered;
        if (!PublishActive(record, CoordinatorDetail::LifecycleForRecoveryPoint(record.Active.Point),
                           executingCandidate, true)) {
            return ProjectionFailure();
        }
        status_.VerifiedManifestBound = true;
        status_.UpdatePlanReady = true;
        return Result::Success();'''
new_bind_publish = '''        staging_.Reset();
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
        return Result::Success();'''
replace_once(old_bind_publish, new_bind_publish)

replace_once(
'''            if (verificationStarted_ || verification_.IsActive()) {
                verification_.Abort();
                verificationStarted_ = false;
            }
            const auto abandoned = control_.AbandonTransaction(transaction);''',
'''            if (verificationStarted_ || verification_.IsActive()) {
                verification_.Abort();
                verificationStarted_ = false;
            }
            staging_.Reset();
            componentLifecycle_.Reset();
            const auto abandoned = control_.AbandonTransaction(transaction);''')

replace_once(
'''        if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
        if (!PublishActive(record, UpdateLifecycle::Activating, false)) return ProjectionFailure();
        return {OutcomeClass::Pending, {}};
    }

    Result Advance() noexcept {''',
'''        if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
        componentLifecycle_.Reset();
        activationRestartIssued_ = false;
        if (!PublishActive(record, UpdateLifecycle::Activating, false)) return ProjectionFailure();
        return {OutcomeClass::Pending, {}};
    }

    Result Advance() noexcept {''')

old_activation = '''        if (record.Intent == DurableIntent::ActivationArmed) {
            const auto current = boot_.CurrentBootTarget();
            if (current == record.Active.CandidateBootTarget) {
                if (!trial_.IsCurrentBootTrial()) return RecoveryRequired(&record);
                const auto marked = control_.MarkTrialEntered(record.Active.Transaction);
                if (marked != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(marked);
                if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
                trialClockStarted_ = true;
                trialStartTick_ = clock_.Now();
                lastHealthEvaluationTick_ = trialStartTick_;
                healthEvaluated_ = false;
                if (!PublishActive(record, UpdateLifecycle::Trial, true, true)) return ProjectionFailure();
                return {OutcomeClass::Pending, {}};
            }
            if (current != record.Active.PreviousCommittedBootTarget) return RecoveryRequired(&record);
            if (boot_.NextBootTarget() != record.Active.CandidateBootTarget) {
                const auto mapped = CoordinatorDetail::PlatformResult(boot_.SelectNextBootTarget(record.Active.CandidateBootTarget));
                if (mapped.Outcome != OutcomeClass::Success) return mapped;
                if (!PublishActive(record, UpdateLifecycle::AwaitingRestart, false, true)) return ProjectionFailure();
                return {OutcomeClass::Pending, {}};
            }
            const auto mapped = CoordinatorDetail::PlatformResult(restart_.Restart(Platform::OTA::RestartReason::ActivateCandidate));
            if (mapped.Outcome != OutcomeClass::Success && mapped.Outcome != OutcomeClass::Pending) return mapped;
            if (!PublishActive(record, UpdateLifecycle::AwaitingRestart, false, true)) return ProjectionFailure();
            return CoordinatorDetail::CoreResult(OutcomeClass::Pending, CoordinatorCoreReason::AwaitingRestart);
        }
'''
new_activation = '''        if (record.Intent == DurableIntent::ActivationArmed) {
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
'''
replace_once(old_activation, new_activation)

old_commit = '''        if (record.Intent == DurableIntent::CommitIntent) {
            if (boot_.CurrentBootTarget() != record.Active.CandidateBootTarget) return RecoveryRequired(&record);
            const auto platform = CoordinatorDetail::PlatformResult(trial_.MarkCurrentBootValid());
            if (platform.Outcome != OutcomeClass::Success) return platform;
            const auto transaction = record.Active.Transaction;
            const auto finalized = control_.FinalizeCommit(transaction);
            if (finalized != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(finalized);
            if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
            const Result success = Result::Success();
            return PublishTerminal(record, transaction, TerminalUpdateOutcome::Completed,
                                   UpdateOperation::Commit, success) ? success : ProjectionFailure();
        }
'''
new_commit = '''        if (record.Intent == DurableIntent::CommitIntent) {
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
'''
replace_once(old_commit, new_commit)

old_rollback = '''        if (record.Intent == DurableIntent::RollbackIntent) {
            const auto current = boot_.CurrentBootTarget();
            if (current == record.Active.PreviousCommittedBootTarget) {
                const auto transaction = record.Active.Transaction;
                const auto finalized = control_.FinalizeRollback(transaction);
                if (finalized != OTADurableStatus::Success) return CoordinatorDetail::DurableResult(finalized);
                if (control_.Load(record) != OTADurableStatus::Success) return RecoveryRequired(&record);
                const Result success = Result::Success();
                return PublishTerminal(record, transaction, TerminalUpdateOutcome::RolledBack,
                                       UpdateOperation::Rollback, success) ? success : ProjectionFailure();
            }
            if (current != record.Active.CandidateBootTarget) return RecoveryRequired(&record);
            if (boot_.NextBootTarget() != record.Active.PreviousCommittedBootTarget) {
                const auto mapped = CoordinatorDetail::PlatformResult(boot_.SelectNextBootTarget(record.Active.PreviousCommittedBootTarget));
                if (mapped.Outcome != OutcomeClass::Success) return mapped;
                if (!PublishActive(record, UpdateLifecycle::RollingBack, true, true)) return ProjectionFailure();
                return {OutcomeClass::Pending, {}};
            }
            if (trial_.IsCurrentBootTrial()) {
                const auto mapped = CoordinatorDetail::PlatformResult(trial_.MarkCurrentBootInvalid());
                if (mapped.Outcome != OutcomeClass::Success && mapped.Outcome != OutcomeClass::Pending) return mapped;
                if (!PublishActive(record, UpdateLifecycle::RollingBack, true, true)) return ProjectionFailure();
                return {OutcomeClass::Pending, {}};
            }
            const auto mapped = CoordinatorDetail::PlatformResult(restart_.Restart(Platform::OTA::RestartReason::Rollback));
            if (mapped.Outcome != OutcomeClass::Success && mapped.Outcome != OutcomeClass::Pending) return mapped;
            if (!PublishActive(record, UpdateLifecycle::RollingBack, true, true)) return ProjectionFailure();
            return CoordinatorDetail::CoreResult(OutcomeClass::Pending, CoordinatorCoreReason::AwaitingRestart);
        }
'''
new_rollback = '''        if (record.Intent == DurableIntent::RollbackIntent) {
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
            if (boot_.NextBootTarget() != record.Active.PreviousCommittedBootTarget) {
                const auto mapped = CoordinatorDetail::PlatformResult(
                    boot_.SelectNextBootTarget(record.Active.PreviousCommittedBootTarget));
                if (mapped.Outcome != OutcomeClass::Success) return mapped;
                if (!PublishActive(record, UpdateLifecycle::RollingBack, true, true)) return ProjectionFailure();
                return {OutcomeClass::Pending, {}};
            }
            if (trial_.IsCurrentBootTrial()) {
                const auto mapped = CoordinatorDetail::PlatformResult(trial_.MarkCurrentBootInvalid());
                if (mapped.Outcome != OutcomeClass::Success && mapped.Outcome != OutcomeClass::Pending) return mapped;
                if (!PublishActive(record, UpdateLifecycle::RollingBack, true, true)) return ProjectionFailure();
                return {OutcomeClass::Pending, {}};
            }
            const auto mapped = CoordinatorDetail::PlatformResult(
                restart_.Restart(Platform::OTA::RestartReason::Rollback));
            if (mapped.Outcome != OutcomeClass::Success && mapped.Outcome != OutcomeClass::Pending) return mapped;
            if (!PublishActive(record, UpdateLifecycle::RollingBack, true, true)) return ProjectionFailure();
            return CoordinatorDetail::CoreResult(OutcomeClass::Pending, CoordinatorCoreReason::AwaitingRestart);
        }
'''
replace_once(old_rollback, new_rollback)

path.write_text(text)
print("patched", path)
