#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "ESPressio_OTAComponentHandler.hpp"
#include "ESPressio_OTADurable.hpp"
#include "ESPressio_OTAPolicyHealth.hpp"
#include "ESPressio_OTAUpdatePlan.hpp"
#include "ESPressio_OTAVerification.hpp"

namespace ESPressio::OTA {

enum class ComponentExecutionReason : std::uint32_t {
    None = 0U,
    InvalidConfiguration,
    InvalidLifecycle,
    MissingArtifact,
    ArtifactReaderBusy,
    ArtifactReadNotOpen,
    ArtifactReadInvalidResult,
    ArtifactTruncated,
    ArtifactOverrun,
    ArtifactBindingFailed,
    HandlerFailure,
    UnexpectedRestartRequired,
    RecoveryInconsistent,
    StagingByteCountOverflow
};

namespace ComponentExecutionDetail {

inline constexpr Result Failure(
    OutcomeClass outcome,
    ComponentExecutionReason reason) noexcept {
    return {outcome, {DiagnosticDomain::Component, static_cast<std::uint32_t>(reason), 0, {}, 0U}};
}

inline constexpr Result Pending() noexcept {
    return {OutcomeClass::Pending, {DiagnosticDomain::Component, 0U, 0, {}, 0U}};
}

inline constexpr bool Transient(const Result& result) noexcept {
    return result.Outcome == OutcomeClass::Pending || result.Outcome == OutcomeClass::Deferred;
}

} // namespace ComponentExecutionDetail

class RetainedArtifactReadLease final {
    IReadableArtifactStore& store_;
    const void* owner_{nullptr};
    bool open_{false};

public:
    explicit RetainedArtifactReadLease(IReadableArtifactStore& store) noexcept : store_(store) {}

    Result Open(const void* owner, const ArtifactStoreReadRequest& request) noexcept {
        if (owner == nullptr || !request.IsValid()) {
            return ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration);
        }
        if (open_ && owner_ != owner) {
            return ComponentExecutionDetail::Failure(
                OutcomeClass::Unavailable, ComponentExecutionReason::ArtifactReaderBusy);
        }
        if (open_) {
            store_.CloseRead();
            open_ = false;
            owner_ = nullptr;
        }
        const auto opened = store_.OpenRead(request);
        if (!opened) return opened;
        owner_ = owner;
        open_ = true;
        return Result::Success();
    }

    StreamReadResult Read(const void* owner, std::uint8_t* output, std::size_t capacity) noexcept {
        if (!open_ || owner_ != owner) {
            return StreamReadResult::Failed(ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::ArtifactReadNotOpen));
        }
        return store_.ReadStored(output, capacity);
    }

    void Close(const void* owner) noexcept {
        if (!open_ || owner_ != owner) return;
        store_.CloseRead();
        open_ = false;
        owner_ = nullptr;
    }

    bool IsOwnedBy(const void* owner) const noexcept {
        return open_ && owner_ == owner;
    }
};

template<typename TCapacityProfile>
class RetainedVerifiedArtifactReader final : public IVerifiedArtifactReader<TCapacityProfile> {
    static_assert(TCapacityProfile::IsValid,
                  "RetainedVerifiedArtifactReader requires a valid OTA capacity profile");

    VerifiedArtifactDescriptor<TCapacityProfile> descriptor_{};
    RetainedArtifactReadLease* lease_{nullptr};
    ExactLengthReadTracker tracker_{0U};
    bool configured_{false};
    bool active_{false};

    void Close() noexcept {
        if (lease_ != nullptr) lease_->Close(this);
        active_ = false;
    }

public:
    Result Configure(
        const ManifestArtifact<TCapacityProfile>& artifact,
        RetainedArtifactReadLease& lease) noexcept {
        Close();
        descriptor_ = {};
        descriptor_.Identifier = ArtifactIdentifier{artifact.Identifier};
        descriptor_.Length = artifact.ExpectedLength;
        descriptor_.DigestAlgorithm = Security::DigestAlgorithmIdentifier{artifact.DigestAlgorithm};
        for (const auto byte : artifact.Digest) {
            if (!descriptor_.Digest.push_back(byte)) {
                configured_ = false;
                lease_ = nullptr;
                return ComponentExecutionDetail::Failure(
                    OutcomeClass::CapacityUnavailable, ComponentExecutionReason::InvalidConfiguration);
            }
        }
        if (!descriptor_.IsValid()) {
            configured_ = false;
            lease_ = nullptr;
            return ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration);
        }
        lease_ = &lease;
        tracker_ = ExactLengthReadTracker{descriptor_.Length};
        configured_ = true;
        active_ = false;
        return Result::Success();
    }

    const VerifiedArtifactDescriptor<TCapacityProfile>& Descriptor() const noexcept override {
        return descriptor_;
    }

    Result Reset() noexcept override {
        if (!configured_ || lease_ == nullptr) {
            return ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration);
        }
        tracker_ = ExactLengthReadTracker{descriptor_.Length};
        const auto opened = lease_->Open(this, {descriptor_.Identifier, descriptor_.Length});
        active_ = bool(opened);
        return opened;
    }

    StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (!active_ || lease_ == nullptr || !lease_->IsOwnedBy(this)) {
            return StreamReadResult::Failed(ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::ArtifactReadNotOpen));
        }
        const auto read = lease_->Read(this, output, capacity);
        if (!read.IsValidFor(capacity)) {
            Close();
            return StreamReadResult::Failed(ComponentExecutionDetail::Failure(
                OutcomeClass::Failed, ComponentExecutionReason::ArtifactReadInvalidResult));
        }
        const auto observed = tracker_.Observe(read, capacity);
        switch (observed) {
            case ExactLengthReadStatus::Continue:
            case ExactLengthReadStatus::Pending:
            case ExactLengthReadStatus::AwaitingEnd:
                return read;
            case ExactLengthReadStatus::Complete:
                Close();
                return read;
            case ExactLengthReadStatus::Truncated:
                Close();
                return StreamReadResult::Failed(ComponentExecutionDetail::Failure(
                    OutcomeClass::VerificationFailed, ComponentExecutionReason::ArtifactTruncated));
            case ExactLengthReadStatus::Overrun:
                Close();
                return StreamReadResult::Failed(ComponentExecutionDetail::Failure(
                    OutcomeClass::VerificationFailed, ComponentExecutionReason::ArtifactOverrun));
            case ExactLengthReadStatus::Failed:
                Close();
                return read;
            case ExactLengthReadStatus::InvalidResult:
                Close();
                return StreamReadResult::Failed(ComponentExecutionDetail::Failure(
                    OutcomeClass::Failed, ComponentExecutionReason::ArtifactReadInvalidResult));
        }
        Close();
        return StreamReadResult::Failed(ComponentExecutionDetail::Failure(
            OutcomeClass::Failed, ComponentExecutionReason::ArtifactReadInvalidResult));
    }
};

template<typename TCapacityProfile>
const ManifestArtifact<TCapacityProfile>* FindManifestArtifact(
    const Manifest<TCapacityProfile>& manifest,
    ArtifactIdentifier identifier) noexcept {
    if (!identifier) return nullptr;
    for (const auto& artifact : manifest.Artifacts) {
        if (ArtifactIdentifier{artifact.Identifier} == identifier) return &artifact;
    }
    return nullptr;
}

template<typename TCapacityProfile>
Result PreflightUpdatePlan(
    const UpdatePlan<TCapacityProfile>& plan,
    const UpdateTargetProfile<TCapacityProfile>& targetProfile,
    UpdateTransactionId transaction,
    UpdateGenerationId candidateGeneration) noexcept {
    if (!plan.IsReady() || !targetProfile.IsFrozen() || !transaction || !candidateGeneration) {
        return ComponentExecutionDetail::Failure(
            OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration);
    }
    for (std::size_t index = 0U; index < plan.Size(); ++index) {
        const auto* entry = plan.Forward(index);
        if (entry == nullptr || !*entry) {
            return ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration);
        }
        ComponentPreflightContext<TCapacityProfile> context;
        context.Component = entry->ManifestComponentEntry;
        context.TargetProfile = &targetProfile;
        context.Transaction = transaction;
        context.CandidateGeneration = candidateGeneration;
        if (!context.IsValid()) {
            return ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration);
        }
        const auto result = entry->Handler->Preflight(context);
        if (!result) return result;
    }
    return Result::Success();
}

enum class ComponentStagingPhase : std::uint8_t {
    Idle,
    Preparing,
    Staging,
    Finalizing,
    Complete,
    Failed
};

template<typename TCapacityProfile>
class ComponentStagingSession final {
    static_assert(TCapacityProfile::IsValid,
                  "ComponentStagingSession requires a valid OTA capacity profile");

public:
    using StagePolicies = PolicyGateSet<StagePolicyDecisionPoint,
        TCapacityProfile::MaximumPolicyProvidersPerDecisionPoint>;

private:
    IReadableArtifactStore& store_;
    StagePolicies& stagePolicies_;
    RetainedArtifactReadLease lease_;
    std::array<RetainedVerifiedArtifactReader<TCapacityProfile>,
               TCapacityProfile::MaximumArtifactsPerComponent> readers_{};
    VerifiedComponentArtifacts<TCapacityProfile> artifacts_{};

    const Manifest<TCapacityProfile>* manifest_{nullptr};
    const UpdatePlan<TCapacityProfile>* plan_{nullptr};
    const UpdateTargetProfile<TCapacityProfile>* targetProfile_{nullptr};
    PolicyCommonContext policyCommon_{};
    UpdateTransactionId transaction_{};
    UpdateGenerationId candidateGeneration_{};
    std::size_t componentIndex_{0U};
    ComponentStagingPhase phase_{ComponentStagingPhase::Idle};
    bool active_{false};
    bool artifactsBound_{false};
    bool stagePolicyAllowed_{false};

    void ReleaseArtifactBindings() noexcept {
        for (auto& reader : readers_) lease_.Close(&reader);
        artifacts_ = {};
        artifactsBound_ = false;
    }

    Result Fail(Result result) noexcept {
        ReleaseArtifactBindings();
        active_ = false;
        phase_ = ComponentStagingPhase::Failed;
        return result;
    }

    Result BuildArtifacts(const UpdatePlanEntry<TCapacityProfile>& entry) noexcept {
        ReleaseArtifactBindings();
        if (entry.ManifestComponentEntry == nullptr || manifest_ == nullptr) {
            return ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration);
        }
        const auto& component = *entry.ManifestComponentEntry;
        if (component.Artifacts.size() > readers_.size()) {
            return ComponentExecutionDetail::Failure(
                OutcomeClass::CapacityUnavailable, ComponentExecutionReason::ArtifactBindingFailed);
        }
        for (std::size_t i = 0U; i < component.Artifacts.size(); ++i) {
            const ArtifactIdentifier identifier{component.Artifacts[i]};
            const auto* manifestArtifact = FindManifestArtifact(*manifest_, identifier);
            if (manifestArtifact == nullptr) {
                return ComponentExecutionDetail::Failure(
                    OutcomeClass::Invalid, ComponentExecutionReason::MissingArtifact);
            }
            const auto configured = readers_[i].Configure(*manifestArtifact, lease_);
            if (!configured) return configured;
            const auto added = artifacts_.Add(readers_[i]);
            if (!added) return added;
        }
        artifactsBound_ = true;
        return Result::Success();
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

    Result EvaluateStagePolicy(const UpdatePlanEntry<TCapacityProfile>& entry) noexcept {
        if (entry.ManifestComponentEntry == nullptr || manifest_ == nullptr) {
            return ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration);
        }

        const auto& component = *entry.ManifestComponentEntry;
        PolicyContext<StagePolicyDecisionPoint> context;
        context.Common = policyCommon_;
        context.Component = ComponentIdentifier{component.Identifier};
        context.ComponentType = ComponentTypeId{component.TypeId};
        context.ArtifactCount = component.Artifacts.size();

        std::uint64_t requiredBytes = 0U;
        for (const auto rawIdentifier : component.Artifacts) {
            const auto* artifact = FindManifestArtifact(*manifest_, ArtifactIdentifier{rawIdentifier});
            if (artifact == nullptr) {
                return ComponentExecutionDetail::Failure(
                    OutcomeClass::Invalid, ComponentExecutionReason::MissingArtifact);
            }
            if (artifact->ExpectedLength > std::numeric_limits<std::uint64_t>::max() - requiredBytes) {
                return ComponentExecutionDetail::Failure(
                    OutcomeClass::Invalid, ComponentExecutionReason::StagingByteCountOverflow);
            }
            requiredBytes += artifact->ExpectedLength;
        }
        context.RequiredStagingBytes = requiredBytes;

        const auto evaluation = stagePolicies_.Evaluate(context);
        if (evaluation.Decision.Verdict == PolicyVerdict::Defer) {
            return {OutcomeClass::Deferred, evaluation.Decision.Detail};
        }
        if (evaluation.Decision.Verdict == PolicyVerdict::Reject) {
            return {OutcomeClass::Rejected, evaluation.Decision.Detail};
        }
        stagePolicyAllowed_ = true;
        return Result::Success();
    }

    Result ObserveAction(
        const ComponentActionResult& action,
        ComponentStagingPhase next) noexcept {
        switch (action.Status) {
            case ComponentActionStatus::Pending:
                return ComponentExecutionDetail::Transient(action.Detail)
                    ? action.Detail
                    : Fail(ComponentExecutionDetail::Failure(
                        OutcomeClass::Failed, ComponentExecutionReason::HandlerFailure));
            case ComponentActionStatus::RestartRequired:
                return Fail(ComponentExecutionDetail::Failure(
                    OutcomeClass::Invalid, ComponentExecutionReason::UnexpectedRestartRequired));
            case ComponentActionStatus::Failed:
                return Fail(action.Detail);
            case ComponentActionStatus::Complete:
                if (!action.Detail) return Fail(action.Detail);
                phase_ = next;
                return next == ComponentStagingPhase::Complete
                    ? Result::Success()
                    : ComponentExecutionDetail::Pending();
        }
        return Fail(ComponentExecutionDetail::Failure(
            OutcomeClass::Failed, ComponentExecutionReason::HandlerFailure));
    }

public:
    ComponentStagingSession(IReadableArtifactStore& store, StagePolicies& stagePolicies) noexcept
        : store_(store), stagePolicies_(stagePolicies), lease_(store_) {}

    ComponentStagingPhase Phase() const noexcept { return phase_; }
    bool IsActive() const noexcept { return active_; }
    bool IsComplete() const noexcept { return phase_ == ComponentStagingPhase::Complete; }
    std::size_t ComponentIndex() const noexcept { return componentIndex_; }

    void Reset() noexcept {
        ReleaseArtifactBindings();
        manifest_ = nullptr;
        plan_ = nullptr;
        targetProfile_ = nullptr;
        policyCommon_ = {};
        transaction_ = {};
        candidateGeneration_ = {};
        componentIndex_ = 0U;
        phase_ = ComponentStagingPhase::Idle;
        active_ = false;
        stagePolicyAllowed_ = false;
    }

    Result Begin(
        const Manifest<TCapacityProfile>& manifest,
        const UpdatePlan<TCapacityProfile>& plan,
        const UpdateTargetProfile<TCapacityProfile>& targetProfile,
        UpdateTransactionId transaction,
        UpdateGenerationId candidateGeneration,
        UpdateGenerationId committedGeneration,
        bool recover = false) noexcept {
        if (active_ || !plan.IsReady() || !targetProfile.IsFrozen() || !transaction ||
            !candidateGeneration || !committedGeneration) {
            return ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration);
        }
        Reset();
        manifest_ = &manifest;
        plan_ = &plan;
        targetProfile_ = &targetProfile;
        transaction_ = transaction;
        candidateGeneration_ = candidateGeneration;
        componentIndex_ = 0U;
        phase_ = ComponentStagingPhase::Preparing;
        active_ = true;
        stagePolicyAllowed_ = false;

        policyCommon_ = {};
        policyCommon_.Transaction = transaction;
        policyCommon_.Release = ReleaseIdentifier{manifest.Release};
        policyCommon_.Manifest = ManifestIdentifier{manifest.Identifier};
        policyCommon_.CandidateGeneration = candidateGeneration;
        policyCommon_.CandidateSecurityGeneration = SecurityGeneration{manifest.SecurityGeneration};
        policyCommon_.CommittedGeneration = committedGeneration;
        if (!policyCommon_.IsCanonical()) {
            return Fail(ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration));
        }

        if (!recover) {
            if (plan.Size() == 0U) {
                active_ = false;
                phase_ = ComponentStagingPhase::Complete;
                return Result::Success();
            }
            return ComponentExecutionDetail::Pending();
        }

        bool foundIncomplete = false;
        for (std::size_t index = 0U; index < plan.Size(); ++index) {
            const auto* entry = plan.Forward(index);
            if (entry == nullptr || !*entry) {
                return Fail(ComponentExecutionDetail::Failure(
                    OutcomeClass::Invalid, ComponentExecutionReason::RecoveryInconsistent));
            }
            const auto context = PreflightContext(*entry);
            const auto inspection = entry->Handler->InspectRecoveryState(context);
            if (!inspection.Detail) return Fail(inspection.Detail);

            if (!foundIncomplete && inspection.State == ComponentRecoveryState::Staged) {
                componentIndex_ = index + 1U;
                continue;
            }
            if (!foundIncomplete && inspection.State == ComponentRecoveryState::NotPrepared) {
                componentIndex_ = index;
                phase_ = ComponentStagingPhase::Preparing;
                stagePolicyAllowed_ = false;
                foundIncomplete = true;
                continue;
            }
            if (!foundIncomplete && inspection.State == ComponentRecoveryState::PartiallyStaged) {
                componentIndex_ = index;
                phase_ = ComponentStagingPhase::Staging;
                stagePolicyAllowed_ = true;
                foundIncomplete = true;
                continue;
            }
            if (foundIncomplete && inspection.State == ComponentRecoveryState::NotPrepared) continue;

            return Fail(ComponentExecutionDetail::Failure(
                OutcomeClass::Failed, ComponentExecutionReason::RecoveryInconsistent));
        }

        if (!foundIncomplete) {
            active_ = false;
            phase_ = ComponentStagingPhase::Complete;
            return Result::Success();
        }
        return ComponentExecutionDetail::Pending();
    }

    Result Advance() noexcept {
        if (!active_ || manifest_ == nullptr || plan_ == nullptr || targetProfile_ == nullptr ||
            componentIndex_ >= plan_->Size()) {
            return IsComplete() ? Result::Success() : ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidLifecycle);
        }

        const auto* entry = plan_->Forward(componentIndex_);
        if (entry == nullptr || !*entry) {
            return Fail(ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration));
        }
        const auto preflight = PreflightContext(*entry);
        if (!preflight.IsValid()) {
            return Fail(ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration));
        }

        if (phase_ == ComponentStagingPhase::Preparing) {
            if (!stagePolicyAllowed_) {
                const auto policy = EvaluateStagePolicy(*entry);
                if (policy.Outcome == OutcomeClass::Deferred) return policy;
                if (policy.Outcome == OutcomeClass::Rejected) return Fail(policy);
                if (!policy) return Fail(policy);
            }
            const auto action = entry->Handler->Prepare(preflight);
            return ObserveAction(action, ComponentStagingPhase::Staging);
        }

        if (phase_ == ComponentStagingPhase::Staging) {
            if (!artifactsBound_) {
                const auto bindings = BuildArtifacts(*entry);
                if (!bindings) return Fail(bindings);
            }
            ComponentExecutionContext<TCapacityProfile> context{preflight, &artifacts_};
            const auto action = entry->Handler->Stage(context);
            return ObserveAction(action, ComponentStagingPhase::Finalizing);
        }

        if (phase_ == ComponentStagingPhase::Finalizing) {
            if (!artifactsBound_) {
                return Fail(ComponentExecutionDetail::Failure(
                    OutcomeClass::Invalid, ComponentExecutionReason::ArtifactBindingFailed));
            }
            ComponentExecutionContext<TCapacityProfile> context{preflight, &artifacts_};
            const auto action = entry->Handler->FinalizeStage(context);
            if (action.Status == ComponentActionStatus::Complete && action.Detail) {
                ReleaseArtifactBindings();
                ++componentIndex_;
                stagePolicyAllowed_ = false;
                if (componentIndex_ == plan_->Size()) {
                    active_ = false;
                    phase_ = ComponentStagingPhase::Complete;
                    return Result::Success();
                }
                phase_ = ComponentStagingPhase::Preparing;
                return ComponentExecutionDetail::Pending();
            }
            return ObserveAction(action, ComponentStagingPhase::Finalizing);
        }

        return Fail(ComponentExecutionDetail::Failure(
            OutcomeClass::Invalid, ComponentExecutionReason::InvalidLifecycle));
    }
};

} // namespace ESPressio::OTA
