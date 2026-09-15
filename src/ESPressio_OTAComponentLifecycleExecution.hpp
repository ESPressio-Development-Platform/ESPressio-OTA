#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAExecution.hpp"

namespace ESPressio::OTA {

enum class ComponentLifecycleOperation : std::uint8_t {
    Activate,
    Commit,
    Rollback,
    Cleanup
};

enum class ComponentLifecycleExecutionReason : std::uint32_t {
    None = 0U,
    InvalidConfiguration,
    InvalidLifecycle,
    MissingArtifact,
    ArtifactBindingFailed,
    HandlerFailure,
    RecoveryInconsistent
};

namespace ComponentLifecycleExecutionDetail {

inline constexpr Result Failure(
    OutcomeClass outcome,
    ComponentLifecycleExecutionReason reason) noexcept {
    return {outcome, {DiagnosticDomain::Component, static_cast<std::uint32_t>(reason), 0, {}, 0U}};
}

inline constexpr Result Pending() noexcept {
    return {OutcomeClass::Pending, {DiagnosticDomain::Component, 0U, 0, {}, 0U}};
}

inline constexpr bool IsForward(ComponentLifecycleOperation operation) noexcept {
    return operation != ComponentLifecycleOperation::Rollback;
}

} // namespace ComponentLifecycleExecutionDetail

/**
 * Deterministic cooperative execution of post-staging ComponentHandler hooks.
 *
 * Activate and Commit use UpdatePlan dependency order. Rollback uses reverse
 * dependency order so dependents unwind before prerequisites. Cleanup uses
 * deterministic forward order; it is deliberately outside transaction
 * authority and never changes an already-durable terminal outcome.
 *
 * A single handler invocation is made per Advance(). Artifact readers are
 * retained across Pending re-entry for the current component. RestartRequired
 * is accumulated and reported to the Coordinator; handlers never restart the
 * system directly.
 */
template<typename TCapacityProfile>
class ComponentLifecycleExecutionSession final {
    static_assert(TCapacityProfile::IsValid,
                  "ComponentLifecycleExecutionSession requires a valid OTA capacity profile");

    IReadableArtifactStore& store_;
    RetainedArtifactReadLease lease_;
    std::array<RetainedVerifiedArtifactReader<TCapacityProfile>,
               TCapacityProfile::MaximumArtifactsPerComponent> readers_{};
    VerifiedComponentArtifacts<TCapacityProfile> artifacts_{};

    const Manifest<TCapacityProfile>* manifest_{nullptr};
    const UpdatePlan<TCapacityProfile>* plan_{nullptr};
    const UpdateTargetProfile<TCapacityProfile>* targetProfile_{nullptr};
    UpdateTransactionId transaction_{};
    UpdateGenerationId candidateGeneration_{};
    ComponentLifecycleOperation operation_{ComponentLifecycleOperation::Activate};
    std::size_t componentIndex_{0U};
    bool active_{false};
    bool complete_{false};
    bool artifactsBound_{false};
    bool restartRequired_{false};

    void ReleaseArtifactBindings() noexcept {
        for (auto& reader : readers_) lease_.Close(&reader);
        artifacts_ = {};
        artifactsBound_ = false;
    }

    Result Fail(Result result) noexcept {
        ReleaseArtifactBindings();
        active_ = false;
        complete_ = false;
        return result;
    }

    const UpdatePlanEntry<TCapacityProfile>* Entry(std::size_t index) const noexcept {
        if (plan_ == nullptr) return nullptr;
        return ComponentLifecycleExecutionDetail::IsForward(operation_)
            ? plan_->Forward(index)
            : plan_->Reverse(index);
    }

    ComponentPreflightContext<TCapacityProfile> PreflightContext(
        const UpdatePlanEntry<TCapacityProfile>& entry) const noexcept {
        ComponentPreflightContext<TCapacityProfile> context;
        context.Component = entry.ManifestComponentEntry;
        context.TargetProfile = targetProfile_;
        context.Transaction = transaction_;
        context.CandidateGeneration = candidateGeneration_;
        return context;
    }

    Result BuildArtifacts(const UpdatePlanEntry<TCapacityProfile>& entry) noexcept {
        ReleaseArtifactBindings();
        if (manifest_ == nullptr || entry.ManifestComponentEntry == nullptr) {
            return ComponentLifecycleExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentLifecycleExecutionReason::InvalidConfiguration);
        }
        const auto& component = *entry.ManifestComponentEntry;
        if (component.Artifacts.size() > readers_.size()) {
            return ComponentLifecycleExecutionDetail::Failure(
                OutcomeClass::CapacityUnavailable, ComponentLifecycleExecutionReason::ArtifactBindingFailed);
        }
        for (std::size_t i = 0U; i < component.Artifacts.size(); ++i) {
            const auto* artifact = FindManifestArtifact(
                *manifest_, ArtifactIdentifier{component.Artifacts[i]});
            if (artifact == nullptr) {
                return ComponentLifecycleExecutionDetail::Failure(
                    OutcomeClass::Invalid, ComponentLifecycleExecutionReason::MissingArtifact);
            }
            const auto configured = readers_[i].Configure(*artifact, lease_);
            if (!configured) return configured;
            const auto added = artifacts_.Add(readers_[i]);
            if (!added) return added;
        }
        artifactsBound_ = true;
        return Result::Success();
    }

    bool RecoveryStateAlreadyComplete(ComponentRecoveryState state) const noexcept {
        switch (operation_) {
            case ComponentLifecycleOperation::Activate:
                return state == ComponentRecoveryState::Activated ||
                       state == ComponentRecoveryState::Committed;
            case ComponentLifecycleOperation::Commit:
                return state == ComponentRecoveryState::Committed;
            case ComponentLifecycleOperation::Rollback:
                return state == ComponentRecoveryState::RolledBack ||
                       state == ComponentRecoveryState::NotPrepared;
            case ComponentLifecycleOperation::Cleanup:
                return false;
        }
        return false;
    }

    bool RecoveryStateCanExecute(ComponentRecoveryState state) const noexcept {
        switch (operation_) {
            case ComponentLifecycleOperation::Activate:
                return state == ComponentRecoveryState::Staged;
            case ComponentLifecycleOperation::Commit:
                return state == ComponentRecoveryState::Activated ||
                       state == ComponentRecoveryState::Staged;
            case ComponentLifecycleOperation::Rollback:
                return state == ComponentRecoveryState::Committed ||
                       state == ComponentRecoveryState::Activated ||
                       state == ComponentRecoveryState::Staged ||
                       state == ComponentRecoveryState::PartiallyStaged;
            case ComponentLifecycleOperation::Cleanup:
                return state != ComponentRecoveryState::Inconsistent;
        }
        return false;
    }

    ComponentActionResult Invoke(
        IComponentHandler<TCapacityProfile>& handler,
        const ComponentExecutionContext<TCapacityProfile>& context) noexcept {
        switch (operation_) {
            case ComponentLifecycleOperation::Activate: return handler.Activate(context);
            case ComponentLifecycleOperation::Commit: return handler.Commit(context);
            case ComponentLifecycleOperation::Rollback: return handler.Rollback(context);
            case ComponentLifecycleOperation::Cleanup: return handler.Cleanup(context);
        }
        return ComponentActionResult::Failed(ComponentLifecycleExecutionDetail::Failure(
            OutcomeClass::Invalid, ComponentLifecycleExecutionReason::InvalidLifecycle));
    }

public:
    explicit ComponentLifecycleExecutionSession(IReadableArtifactStore& store) noexcept
        : store_(store), lease_(store_) {}

    bool IsActive() const noexcept { return active_; }
    bool IsComplete() const noexcept { return complete_; }
    bool RestartRequired() const noexcept { return restartRequired_; }
    std::size_t ComponentIndex() const noexcept { return componentIndex_; }
    ComponentLifecycleOperation Operation() const noexcept { return operation_; }

    void Reset() noexcept {
        ReleaseArtifactBindings();
        manifest_ = nullptr;
        plan_ = nullptr;
        targetProfile_ = nullptr;
        transaction_ = {};
        candidateGeneration_ = {};
        componentIndex_ = 0U;
        active_ = false;
        complete_ = false;
        restartRequired_ = false;
    }

    Result Begin(
        const Manifest<TCapacityProfile>& manifest,
        const UpdatePlan<TCapacityProfile>& plan,
        const UpdateTargetProfile<TCapacityProfile>& targetProfile,
        UpdateTransactionId transaction,
        UpdateGenerationId candidateGeneration,
        ComponentLifecycleOperation operation,
        bool recover = false) noexcept {
        if (active_ || !plan.IsReady() || !targetProfile.IsFrozen() ||
            !transaction || !candidateGeneration) {
            return ComponentLifecycleExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentLifecycleExecutionReason::InvalidConfiguration);
        }
        Reset();
        manifest_ = &manifest;
        plan_ = &plan;
        targetProfile_ = &targetProfile;
        transaction_ = transaction;
        candidateGeneration_ = candidateGeneration;
        operation_ = operation;
        active_ = true;

        if (plan.Size() == 0U) {
            active_ = false;
            complete_ = true;
            return Result::Success();
        }

        if (recover && operation_ != ComponentLifecycleOperation::Cleanup) {
            while (componentIndex_ < plan.Size()) {
                const auto* entry = Entry(componentIndex_);
                if (entry == nullptr || !*entry) {
                    return Fail(ComponentLifecycleExecutionDetail::Failure(
                        OutcomeClass::Invalid, ComponentLifecycleExecutionReason::RecoveryInconsistent));
                }
                const auto context = PreflightContext(*entry);
                if (!context.IsValid()) {
                    return Fail(ComponentLifecycleExecutionDetail::Failure(
                        OutcomeClass::Invalid, ComponentLifecycleExecutionReason::RecoveryInconsistent));
                }
                const auto inspection = entry->Handler->InspectRecoveryState(context);
                if (!inspection.Detail) return Fail(inspection.Detail);
                if (RecoveryStateAlreadyComplete(inspection.State)) {
                    ++componentIndex_;
                    continue;
                }
                if (!RecoveryStateCanExecute(inspection.State)) {
                    return Fail(ComponentLifecycleExecutionDetail::Failure(
                        OutcomeClass::Failed, ComponentLifecycleExecutionReason::RecoveryInconsistent));
                }
                break;
            }
            if (componentIndex_ == plan.Size()) {
                active_ = false;
                complete_ = true;
                return Result::Success();
            }
        }
        return ComponentLifecycleExecutionDetail::Pending();
    }

    Result Advance() noexcept {
        if (!active_ || manifest_ == nullptr || plan_ == nullptr || targetProfile_ == nullptr ||
            componentIndex_ >= plan_->Size()) {
            return complete_ ? Result::Success() : ComponentLifecycleExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentLifecycleExecutionReason::InvalidLifecycle);
        }

        const auto* entry = Entry(componentIndex_);
        if (entry == nullptr || !*entry) {
            return Fail(ComponentLifecycleExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentLifecycleExecutionReason::InvalidConfiguration));
        }
        if (!artifactsBound_) {
            const auto bound = BuildArtifacts(*entry);
            if (!bound) return Fail(bound);
        }
        const auto preflight = PreflightContext(*entry);
        ComponentExecutionContext<TCapacityProfile> context{preflight, &artifacts_};
        if (!context.IsValid()) {
            return Fail(ComponentLifecycleExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentLifecycleExecutionReason::InvalidConfiguration));
        }

        const auto action = Invoke(*entry->Handler, context);
        switch (action.Status) {
            case ComponentActionStatus::Pending:
                if (action.Detail.Outcome != OutcomeClass::Pending &&
                    action.Detail.Outcome != OutcomeClass::Deferred) {
                    return Fail(ComponentLifecycleExecutionDetail::Failure(
                        OutcomeClass::Failed, ComponentLifecycleExecutionReason::HandlerFailure));
                }
                return action.Detail;
            case ComponentActionStatus::Failed:
                return Fail(action.Detail);
            case ComponentActionStatus::RestartRequired:
                if (!action.Detail) return Fail(action.Detail);
                restartRequired_ = true;
                break;
            case ComponentActionStatus::Complete:
                if (!action.Detail) return Fail(action.Detail);
                break;
        }

        ReleaseArtifactBindings();
        ++componentIndex_;
        if (componentIndex_ == plan_->Size()) {
            active_ = false;
            complete_ = true;
            return Result::Success();
        }
        return ComponentLifecycleExecutionDetail::Pending();
    }
};

} // namespace ESPressio::OTA
