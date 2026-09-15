#pragma once

#include <cstdint>

#include "ESPressio_OTAEvents.hpp"

namespace ESPressio::OTA::Integration {

/**
 * Optional caller-owned projector from canonical OTA State to meaningful Events.
 *
 * The first observation only establishes a baseline: attaching a bridge must not
 * replay historical lifecycle occurrences. Subsequent observations may emit a
 * bounded set of meaningful lifecycle/terminal occurrences using TryDispatch.
 * Dispatch failure is diagnostic-only and never changes OTA State or outcome.
 */
class OTAEventBridge final {
    bool primed_{false};
    OTAEventTransactionContext context_{};
    std::uint64_t lifecycleTransaction_{0U};
    UpdateLifecycle lifecycle_{UpdateLifecycle::Idle};
    bool hasLifecycle_{false};
    std::uint64_t outcomeTransaction_{0U};
    TerminalUpdateOutcome outcome_{TerminalUpdateOutcome::None};
    std::uint64_t unavailableOccurrences_{0U};

    template<class TEvent, class... TArgs>
    void Emit(TArgs&&... args) noexcept {
        try {
            if (!TEvent::TryDispatch(static_cast<TArgs&&>(args)...)) ++unavailableOccurrences_;
        } catch (...) {
            ++unavailableOccurrences_;
        }
    }

    void CaptureContext(const ActiveUpdateTransactionValue& active) noexcept {
        if (active.Present && active.IsCanonical()) {
            context_ = OTAEventTransactionContext::From(active);
        }
    }

    void EmitLifecycle(UpdateLifecycle lifecycle) noexcept {
        if (!context_.IsValid()) return;
        switch (lifecycle) {
            case UpdateLifecycle::Checking:
                Emit<UpdateStartedEvent>(context_);
                break;
            case UpdateLifecycle::CandidateSelected:
                Emit<CandidateSelectedEvent>(context_);
                break;
            case UpdateLifecycle::Staged:
                Emit<CandidateStagedEvent>(context_);
                break;
            case UpdateLifecycle::Trial:
                Emit<TrialStartedEvent>(context_);
                break;
            default:
                break;
        }
    }

    void EmitTerminal(const LastUpdateOutcomeValue& value) noexcept {
        if (!context_.IsValid()) return;
        switch (value.Outcome) {
            case TerminalUpdateOutcome::Completed:
                Emit<UpdateCommittedEvent>(context_);
                break;
            case TerminalUpdateOutcome::RolledBack:
                Emit<UpdateRolledBackEvent>(context_);
                break;
            case TerminalUpdateOutcome::Cancelled:
                Emit<UpdateCancelledEvent>(context_);
                break;
            case TerminalUpdateOutcome::RollbackFailed:
                Emit<RollbackFailedEvent>(context_, value);
                break;
            case TerminalUpdateOutcome::RecoveryRequired:
                Emit<RecoveryRequiredEvent>(context_);
                break;
            case TerminalUpdateOutcome::Failed:
            case TerminalUpdateOutcome::Unsupported:
            case TerminalUpdateOutcome::Rejected:
            case TerminalUpdateOutcome::CapacityUnavailable:
                Emit<UpdateFailedEvent>(context_, value);
                break;
            case TerminalUpdateOutcome::None:
                break;
        }
    }

public:
    OTAEventBridge() noexcept = default;
    OTAEventBridge(const OTAEventBridge&) = delete;
    OTAEventBridge& operator=(const OTAEventBridge&) = delete;

    /** Forget projection history without emitting an occurrence. */
    void Reset() noexcept {
        primed_ = false;
        context_ = {};
        lifecycleTransaction_ = 0U;
        lifecycle_ = UpdateLifecycle::Idle;
        hasLifecycle_ = false;
        outcomeTransaction_ = 0U;
        outcome_ = TerminalUpdateOutcome::None;
        unavailableOccurrences_ = 0U;
    }

    /** Count Event occurrences that could not be admitted to the bounded Event family. */
    std::uint64_t UnavailableOccurrences() const noexcept { return unavailableOccurrences_; }

    /**
     * Project one coherent read of the three canonical State values needed by
     * the lifecycle Event bridge. The caller should read these snapshots only
     * after State reports a change and then call Observe from its observer lane.
     */
    void Observe(const ActiveUpdateTransactionValue& active,
                 const ActiveUpdateLifecycleValue& lifecycle,
                 const LastUpdateOutcomeValue& lastOutcome) noexcept {
        CaptureContext(active);

        if (!primed_) {
            primed_ = true;
            hasLifecycle_ = lifecycle.Present;
            lifecycleTransaction_ = lifecycle.Transaction;
            lifecycle_ = lifecycle.Lifecycle;
            outcomeTransaction_ = lastOutcome.Present ? lastOutcome.Transaction : 0U;
            outcome_ = lastOutcome.Present ? lastOutcome.Outcome : TerminalUpdateOutcome::None;
            return;
        }

        if (lifecycle.Present &&
            (!hasLifecycle_ || lifecycle.Transaction != lifecycleTransaction_ || lifecycle.Lifecycle != lifecycle_)) {
            lifecycleTransaction_ = lifecycle.Transaction;
            lifecycle_ = lifecycle.Lifecycle;
            hasLifecycle_ = true;
            EmitLifecycle(lifecycle.Lifecycle);
        } else if (!lifecycle.Present) {
            hasLifecycle_ = false;
            lifecycleTransaction_ = 0U;
            lifecycle_ = UpdateLifecycle::Idle;
        }

        if (lastOutcome.Present &&
            (lastOutcome.Transaction != outcomeTransaction_ || lastOutcome.Outcome != outcome_)) {
            outcomeTransaction_ = lastOutcome.Transaction;
            outcome_ = lastOutcome.Outcome;
            EmitTerminal(lastOutcome);
        } else if (!lastOutcome.Present) {
            outcomeTransaction_ = 0U;
            outcome_ = TerminalUpdateOutcome::None;
        }
    }
};

} // namespace ESPressio::OTA::Integration
