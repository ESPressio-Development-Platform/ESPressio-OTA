#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAComponentHandler.hpp"
#include "ESPressio_OTADurable.hpp"
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
    RecoveryInconsistent
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

/**
 * Serializes access to a retained Artifact store whose read interface exposes a
 * single cursor. A handler may retain multiple verified reader objects, but V1
 * deliberately permits only one of them to own the underlying store cursor at
 * a time. This prevents accidental cross-Artifact cursor corruption without
 * allocating a reader per store backend.
 */
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

/**
 * Read-only verified Artifact view backed by the immutable retained Artifact
 * store. Verification metadata is copied from the already trusted Manifest;
 * Reset() always reopens the immutable object from byte zero.
 */
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

/**
 * Runs the non-mutating handler Preflight surface in deterministic plan order.
 * This is intentionally separate from ComponentStagingSession so the Coordinator
 * can reject/defer before Artifact acquisition begins.
 */
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

/**
 * Cooperative, allocation-free execution of Prepare -> Stage -> FinalizeStage
 * over an immutable UpdatePlan. One handler method is invoked per Advance().
 *
 * Durable StagingStarted/Staged transitions remain Coordinator-owned. The
 * session therefore requires the caller to persist StagingStarted before Begin
 * and to persist Staged only after IsComplete() becomes true.
 */
template<typename TCapacityProfile>
class ComponentStagingSession final {
    static_assert(TCapacityProfile::IsValid,
                  "ComponentStagingSession requires a valid OTA capacity profile");

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
    std::size_t componentIndex_{0U};
    ComponentStagingPhase phase_{ComponentStagingPhase::Idle};
    bool active_{false};
    bool artifactsBound_{false};

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
    explicit ComponentStagingSession(IReadableArtifactStore& store) noexcept
        : store_(store), lease_(store_) {}

    ComponentStagingPhase Phase() const noexcept { return phase_; }
    bool IsActive() const noexcept { return active_; }
    bool IsComplete() const noexcept { return phase_ == ComponentStagingPhase::Complete; }
    std::size_t ComponentIndex() const noexcept { return componentIndex_; }

    Result Begin(
        const Manifest<TCapacityProfile>& manifest,
        const UpdatePlan<TCapacityProfile>& plan,
        const UpdateTargetProfile<TCapacityProfile>& targetProfile,
        UpdateTransactionId transaction,
        UpdateGenerationId candidateGeneration,
        bool recover = false) noexcept {
        if (active_ || !plan.IsReady() || !targetProfile.IsFrozen() || !transaction || !candidateGeneration) {
            return ComponentExecutionDetail::Failure(
                OutcomeClass::Invalid, ComponentExecutionReason::InvalidConfiguration);
        }
        ReleaseArtifactBindings();
        manifest_ = &manifest;
        plan_ = &plan;
        targetProfile_ = &targetProfile;
        transaction_ = transaction;
        candidateGeneration_ = candidateGeneration;
        componentIndex_ = 0U;
        phase_ = ComponentStagingPhase::Preparing;
        active_ = true;

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
                foundIncomplete = true;
                continue;
            }
            if (!foundIncomplete && inspection.State == ComponentRecoveryState::PartiallyStaged) {
                componentIndex_ = index;
                phase_ = ComponentStagingPhase::Staging;
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
