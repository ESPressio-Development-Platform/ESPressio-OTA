#include "ESPressio_OTA.hpp"

using namespace ESPressio::OTA;

namespace {

PolicyCommonContext CommonPolicy() {
    std::array<std::uint8_t, 16> manifest{};
    manifest[15] = 1U;
    PolicyCommonContext context;
    context.Transaction = UpdateTransactionId{1U};
    context.Release = ReleaseIdentifier{2U};
    context.Manifest = ManifestIdentifier{manifest};
    context.CandidateGeneration = UpdateGenerationId{2U};
    context.CandidateSecurityGeneration = SecurityGeneration{1U};
    context.CommittedGeneration = UpdateGenerationId{1U};
    return context;
}

HealthCommonContext CommonHealth() {
    std::array<std::uint8_t, 16> manifest{};
    manifest[15] = 1U;
    HealthCommonContext context;
    context.Transaction = UpdateTransactionId{1U};
    context.CandidateGeneration = UpdateGenerationId{2U};
    context.Release = ReleaseIdentifier{2U};
    context.Manifest = ManifestIdentifier{manifest};
    context.ExecutingGeneration = UpdateGenerationId{2U};
    context.CommittedGeneration = UpdateGenerationId{1U};
    context.TrialElapsedNanoseconds = 100U;
    context.TrialRemainingNanoseconds = 900U;
    return context;
}

class AllowGate final : public IPolicyGate<AcquirePolicyDecisionPoint> {
public:
    PolicyDecision Evaluate(const PolicyContext<AcquirePolicyDecisionPoint>&) noexcept override {
        return PolicyDecision::Allow();
    }
};

class DeferredGate final : public IPolicyGate<AcquirePolicyDecisionPoint> {
    std::uint32_t reason_;
    std::uint64_t delay_;
public:
    DeferredGate(std::uint32_t reason, std::uint64_t delay) : reason_(reason), delay_(delay) {}
    PolicyDecision Evaluate(const PolicyContext<AcquirePolicyDecisionPoint>&) noexcept override {
        return PolicyDecision::Defer(reason_, {PolicyReevaluationHintKind::AfterDelay, delay_});
    }
};

class RejectGate final : public IPolicyGate<AcquirePolicyDecisionPoint> {
    std::uint32_t reason_;
public:
    explicit RejectGate(std::uint32_t reason) : reason_(reason) {}
    PolicyDecision Evaluate(const PolicyContext<AcquirePolicyDecisionPoint>&) noexcept override {
        return PolicyDecision::Reject(reason_);
    }
};

class InvalidGate final : public IPolicyGate<AcquirePolicyDecisionPoint> {
public:
    PolicyDecision Evaluate(const PolicyContext<AcquirePolicyDecisionPoint>&) noexcept override {
        PolicyDecision invalid;
        invalid.Verdict = PolicyVerdict::Reject;
        return invalid;
    }
};

class ReadyCheck final : public IHealthCheck<ApplicationReadyHealthCondition> {
public:
    HealthDecision Check(const HealthContext<ApplicationReadyHealthCondition>&) noexcept override {
        return HealthDecision::Pass();
    }
};

class PendingCheck final : public IHealthCheck<ApplicationReadyHealthCondition> {
    std::uint32_t reason_;
public:
    explicit PendingCheck(std::uint32_t reason) : reason_(reason) {}
    HealthDecision Check(const HealthContext<ApplicationReadyHealthCondition>&) noexcept override {
        return HealthDecision::Pending(reason_);
    }
};

class FailingCheck final : public IHealthCheck<ApplicationReadyHealthCondition> {
    std::uint32_t reason_;
public:
    explicit FailingCheck(std::uint32_t reason) : reason_(reason) {}
    HealthDecision Check(const HealthContext<ApplicationReadyHealthCondition>&) noexcept override {
        return HealthDecision::Fail(reason_);
    }
};

class InvalidHealthCheck final : public IHealthCheck<StableRuntimeReachedHealthCondition> {
public:
    HealthDecision Check(const HealthContext<StableRuntimeReachedHealthCondition>&) noexcept override {
        HealthDecision invalid;
        invalid.Result = HealthResult::Pending;
        return invalid;
    }
};

} // namespace

int main() {
    PolicyContext<AcquirePolicyDecisionPoint> acquire;
    acquire.Common = CommonPolicy();
    acquire.ArtifactCount = 2U;
    acquire.TotalExpectedBytes = 4096U;
    if (!acquire.IsCanonical()) return 1;

    PolicyGateSet<AcquirePolicyDecisionPoint, 4> emptyPolicy;
    const auto emptyPolicyResult = emptyPolicy.Evaluate(acquire);
    if (emptyPolicyResult.Decision.Verdict != PolicyVerdict::Allow || emptyPolicyResult.EvaluatedCount != 0U) return 2;

    AllowGate allow;
    DeferredGate defer20{20U, 200U};
    RejectGate reject30{30U};
    RejectGate reject10{10U};

    PolicyGateSet<AcquirePolicyDecisionPoint, 4> firstOrder;
    if (firstOrder.Add(allow) != PolicyGateRegistrationStatus::Success) return 3;
    if (firstOrder.Add(defer20) != PolicyGateRegistrationStatus::Success) return 4;
    if (firstOrder.Add(reject30) != PolicyGateRegistrationStatus::Success) return 5;
    if (firstOrder.Add(reject10) != PolicyGateRegistrationStatus::Success) return 6;
    if (firstOrder.Add(allow) != PolicyGateRegistrationStatus::Duplicate) return 7;

    PolicyGateSet<AcquirePolicyDecisionPoint, 4> secondOrder;
    if (secondOrder.Add(reject10) != PolicyGateRegistrationStatus::Success) return 8;
    if (secondOrder.Add(reject30) != PolicyGateRegistrationStatus::Success) return 9;
    if (secondOrder.Add(defer20) != PolicyGateRegistrationStatus::Success) return 10;
    if (secondOrder.Add(allow) != PolicyGateRegistrationStatus::Success) return 11;

    const auto first = firstOrder.Evaluate(acquire);
    const auto second = secondOrder.Evaluate(acquire);
    if (first.Decision.Verdict != PolicyVerdict::Reject || second.Decision.Verdict != PolicyVerdict::Reject) return 12;
    if (first.Decision.Detail.Reason != 10U || second.Decision.Detail.Reason != 10U) return 13;
    if (first.RejectedCount != 2U || first.DeferredCount != 1U || first.EvaluatedCount != 4U) return 14;

    InvalidGate invalidGate;
    PolicyGateSet<AcquirePolicyDecisionPoint, 1> invalidPolicy;
    if (invalidPolicy.Add(invalidGate) != PolicyGateRegistrationStatus::Success) return 15;
    const auto invalidPolicyResult = invalidPolicy.Evaluate(acquire);
    if (invalidPolicyResult.Decision.Verdict != PolicyVerdict::Defer ||
        invalidPolicyResult.Decision.Detail.Reason != static_cast<std::uint32_t>(PolicyCoreReason::InvalidProviderDecision)) return 16;

    auto invalidAcquire = acquire;
    invalidAcquire.Common.Transaction = UpdateTransactionId{};
    const auto invalidContextResult = emptyPolicy.Evaluate(invalidAcquire);
    if (invalidContextResult.Decision.Verdict != PolicyVerdict::Reject ||
        invalidContextResult.Decision.Detail.Reason != static_cast<std::uint32_t>(PolicyCoreReason::InvalidEvaluationContext)) return 17;

    ReadyCheck ready;
    PendingCheck pending40{40U};
    FailingCheck fail50{50U};
    FailingCheck fail30{30U};
    HealthCheckSet<ApplicationReadyHealthCondition, 4> health;
    if (health.Add(ready) != HealthCheckRegistrationStatus::Success) return 18;
    if (health.Add(pending40) != HealthCheckRegistrationStatus::Success) return 19;
    if (health.Add(fail50) != HealthCheckRegistrationStatus::Success) return 20;
    if (health.Add(fail30) != HealthCheckRegistrationStatus::Success) return 21;

    const HealthContext<ApplicationReadyHealthCondition> healthContext{CommonHealth()};
    const auto healthResult = health.Evaluate(healthContext);
    if (healthResult.Condition != ApplicationReadyHealthCondition::TypeId) return 22;
    if (healthResult.Decision.Result != HealthResult::Fail || healthResult.Decision.Detail.Reason != 30U) return 23;
    if (healthResult.FailedCount != 2U || healthResult.PendingCount != 1U || healthResult.EvaluatedCount != 4U) return 24;

    HealthConditionEvaluator<ApplicationReadyHealthCondition, 4> appEvaluator{health};
    HealthRegistry<2> registry;
    if (registry.Add(appEvaluator) != HealthRegistryStatus::Success) return 25;
    if (registry.Add(appEvaluator) != HealthRegistryStatus::DuplicateCondition) return 26;
    if (registry.Find(ApplicationReadyHealthCondition::TypeId) != &appEvaluator) return 27;
    if (registry.Find(RadioSubsystemReadyHealthCondition::TypeId) != nullptr) return 28;

    HealthCheckSet<StableRuntimeReachedHealthCondition, 1> invalidHealthSet;
    InvalidHealthCheck invalidHealthCheck;
    if (invalidHealthSet.Add(invalidHealthCheck) != HealthCheckRegistrationStatus::Success) return 29;
    const auto invalidHealth = invalidHealthSet.Evaluate(HealthContext<StableRuntimeReachedHealthCondition>{CommonHealth()});
    if (invalidHealth.Decision.Result != HealthResult::Fail ||
        invalidHealth.Decision.Detail.Reason != static_cast<std::uint32_t>(HealthCoreReason::InvalidProviderDecision)) return 30;

    return 0;
}
