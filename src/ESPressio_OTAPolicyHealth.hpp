#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTATypes.hpp"

namespace ESPressio::OTA {

// Stable OTA-owned policy decision-point semantic Types.
struct AcquirePolicyDecisionPoint final {
    static constexpr PolicyDecisionPointTypeId TypeId{0x4553504F54415001ULL};
};
struct DistributePolicyDecisionPoint final {
    static constexpr PolicyDecisionPointTypeId TypeId{0x4553504F54415002ULL};
};
struct StagePolicyDecisionPoint final {
    static constexpr PolicyDecisionPointTypeId TypeId{0x4553504F54415003ULL};
};
struct ActivatePolicyDecisionPoint final {
    static constexpr PolicyDecisionPointTypeId TypeId{0x4553504F54415004ULL};
};

// Stable OTA-owned health-condition semantic Types.
struct ApplicationReadyHealthCondition final {
    static constexpr HealthConditionTypeId TypeId{0x4553504F54414801ULL};
};
struct PersistenceMigrationValidHealthCondition final {
    static constexpr HealthConditionTypeId TypeId{0x4553504F54414802ULL};
};
struct RequiredHardwareReadyHealthCondition final {
    static constexpr HealthConditionTypeId TypeId{0x4553504F54414803ULL};
};
struct RadioSubsystemReadyHealthCondition final {
    static constexpr HealthConditionTypeId TypeId{0x4553504F54414804ULL};
};
struct StableRuntimeReachedHealthCondition final {
    static constexpr HealthConditionTypeId TypeId{0x4553504F54414805ULL};
};

namespace PolicyHealthDetail {

constexpr int CompareDiagnostic(const Diagnostic& left, const Diagnostic& right) noexcept {
    if (left.Domain != right.Domain)
        return static_cast<std::uint8_t>(left.Domain) < static_cast<std::uint8_t>(right.Domain) ? -1 : 1;
    if (left.Reason != right.Reason) return left.Reason < right.Reason ? -1 : 1;
    if (left.NativeCode != right.NativeCode) return left.NativeCode < right.NativeCode ? -1 : 1;
    if (left.ContextCount != right.ContextCount) return left.ContextCount < right.ContextCount ? -1 : 1;
    for (std::size_t i = 0U; i < left.ContextCount && i < left.Context.size(); ++i) {
        const auto leftKey = static_cast<std::uint16_t>(left.Context[i].Key);
        const auto rightKey = static_cast<std::uint16_t>(right.Context[i].Key);
        if (leftKey != rightKey) return leftKey < rightKey ? -1 : 1;
        if (left.Context[i].Value != right.Context[i].Value)
            return left.Context[i].Value < right.Context[i].Value ? -1 : 1;
    }
    return 0;
}

constexpr std::uint8_t PolicyRank(PolicyVerdict verdict) noexcept {
    switch (verdict) {
        case PolicyVerdict::Allow: return 0U;
        case PolicyVerdict::Defer: return 1U;
        case PolicyVerdict::Reject: return 2U;
    }
    return 2U;
}

constexpr std::uint8_t HealthRank(HealthResult result) noexcept {
    switch (result) {
        case HealthResult::Pass: return 0U;
        case HealthResult::Pending: return 1U;
        case HealthResult::Fail: return 2U;
    }
    return 2U;
}

} // namespace PolicyHealthDetail

struct PolicyCommonContext final {
    UpdateTransactionId Transaction{};
    ReleaseIdentifier Release{};
    ManifestIdentifier Manifest{};
    UpdateGenerationId CandidateGeneration{};
    SecurityGeneration CandidateSecurityGeneration{};
    UpdateGenerationId CommittedGeneration{};

    constexpr bool IsCanonical() const noexcept {
        return bool(Transaction) && bool(Release) && bool(Manifest) &&
               bool(CandidateGeneration) && bool(CommittedGeneration);
    }
};

template<typename TDecisionPoint>
struct PolicyContext;

template<>
struct PolicyContext<AcquirePolicyDecisionPoint> final {
    PolicyCommonContext Common{};
    std::size_t ArtifactCount{0U};
    std::uint64_t TotalExpectedBytes{0U};
    bool UsesMeteredTransport{false};

    constexpr bool IsCanonical() const noexcept {
        return Common.IsCanonical() && ArtifactCount != 0U && TotalExpectedBytes != 0U;
    }
};

template<>
struct PolicyContext<DistributePolicyDecisionPoint> final {
    PolicyCommonContext Common{};
    ArtifactIdentifier Artifact{};
    std::size_t RecipientCount{0U};
    std::uint64_t ExpectedTransmittedBytes{0U};

    constexpr bool IsCanonical() const noexcept {
        return Common.IsCanonical() && bool(Artifact) && RecipientCount != 0U && ExpectedTransmittedBytes != 0U;
    }
};

template<>
struct PolicyContext<StagePolicyDecisionPoint> final {
    PolicyCommonContext Common{};
    ComponentIdentifier Component{};
    ComponentTypeId ComponentType{};
    std::size_t ArtifactCount{0U};
    std::uint64_t RequiredStagingBytes{0U};

    constexpr bool IsCanonical() const noexcept {
        // A valid component may intentionally have no Artifact payload. ArtifactCount and
        // RequiredStagingBytes describe the component; they are not validity sentinels.
        return Common.IsCanonical() && bool(Component) && bool(ComponentType);
    }
};

template<>
struct PolicyContext<ActivatePolicyDecisionPoint> final {
    PolicyCommonContext Common{};
    bool RestartRequired{false};
    bool InterruptsCurrentRuntime{false};
    bool TrialBootAvailable{false};
    bool TrialBootRequired{false};

    constexpr bool IsCanonical() const noexcept {
        return Common.IsCanonical() && (!TrialBootRequired || TrialBootAvailable);
    }
};

enum class PolicyReevaluationHintKind : std::uint8_t {
    None,
    AfterDelay,
    AfterExternalSignal
};

struct PolicyReevaluationHint final {
    PolicyReevaluationHintKind Kind{PolicyReevaluationHintKind::None};
    std::uint64_t MinimumDelayNanoseconds{0U};

    constexpr bool IsCanonical() const noexcept {
        if (Kind == PolicyReevaluationHintKind::AfterDelay) return MinimumDelayNanoseconds != 0U;
        return MinimumDelayNanoseconds == 0U;
    }
};

enum class PolicyCoreReason : std::uint32_t {
    None = 0U,
    InvalidProviderDecision = 1U,
    InvalidEvaluationContext = 2U
};

struct PolicyDecision final {
    PolicyVerdict Verdict{PolicyVerdict::Allow};
    Diagnostic Detail{DiagnosticDomain::Policy, 0U, 0, {}, 0U};
    PolicyReevaluationHint Reevaluation{};

    constexpr bool IsCanonical() const noexcept {
        if (!Reevaluation.IsCanonical()) return false;
        if (Verdict == PolicyVerdict::Allow)
            return Reevaluation.Kind == PolicyReevaluationHintKind::None;
        if (Detail.Domain != DiagnosticDomain::Policy || Detail.Reason == 0U) return false;
        return Verdict != PolicyVerdict::Reject || Reevaluation.Kind == PolicyReevaluationHintKind::None;
    }

    static constexpr PolicyDecision Allow() noexcept { return {}; }

    static constexpr PolicyDecision Defer(
        std::uint32_t reason,
        PolicyReevaluationHint hint = {},
        std::int32_t nativeCode = 0) noexcept {
        PolicyDecision decision;
        decision.Verdict = PolicyVerdict::Defer;
        decision.Detail = {DiagnosticDomain::Policy, reason, nativeCode, {}, 0U};
        decision.Reevaluation = hint;
        return decision;
    }

    static constexpr PolicyDecision Reject(std::uint32_t reason, std::int32_t nativeCode = 0) noexcept {
        PolicyDecision decision;
        decision.Verdict = PolicyVerdict::Reject;
        decision.Detail = {DiagnosticDomain::Policy, reason, nativeCode, {}, 0U};
        return decision;
    }
};

namespace PolicyHealthDetail {

constexpr bool PolicyDecisionLess(const PolicyDecision& left, const PolicyDecision& right) noexcept {
    const int diagnostic = CompareDiagnostic(left.Detail, right.Detail);
    if (diagnostic != 0) return diagnostic < 0;
    if (left.Reevaluation.Kind != right.Reevaluation.Kind)
        return static_cast<std::uint8_t>(left.Reevaluation.Kind) < static_cast<std::uint8_t>(right.Reevaluation.Kind);
    return left.Reevaluation.MinimumDelayNanoseconds < right.Reevaluation.MinimumDelayNanoseconds;
}

inline constexpr PolicyDecision InvalidPolicyDecision() noexcept {
    return PolicyDecision::Defer(static_cast<std::uint32_t>(PolicyCoreReason::InvalidProviderDecision));
}

} // namespace PolicyHealthDetail

template<typename TDecisionPoint>
class IPolicyGate {
public:
    virtual ~IPolicyGate() = default;
    virtual PolicyDecision Evaluate(const PolicyContext<TDecisionPoint>& context) noexcept = 0;
};

enum class PolicyGateRegistrationStatus : std::uint8_t {
    Success,
    Invalid,
    Duplicate,
    CapacityUnavailable
};

struct PolicyEvaluation final {
    PolicyDecision Decision{};
    std::size_t EvaluatedCount{0U};
    std::size_t DeferredCount{0U};
    std::size_t RejectedCount{0U};
};

template<typename TDecisionPoint, std::size_t TMaximumProviders>
class PolicyGateSet final {
    static_assert(TMaximumProviders != 0U, "PolicyGateSet requires non-zero bounded capacity");
    std::array<IPolicyGate<TDecisionPoint>*, TMaximumProviders> gates_{};
    std::size_t count_{0U};
public:
    constexpr std::size_t Size() const noexcept { return count_; }
    constexpr std::size_t Capacity() const noexcept { return gates_.size(); }

    PolicyGateRegistrationStatus Add(IPolicyGate<TDecisionPoint>& gate) noexcept {
        for (std::size_t i = 0U; i < count_; ++i)
            if (gates_[i] == &gate) return PolicyGateRegistrationStatus::Duplicate;
        if (count_ == gates_.size()) return PolicyGateRegistrationStatus::CapacityUnavailable;
        gates_[count_++] = &gate;
        return PolicyGateRegistrationStatus::Success;
    }

    PolicyEvaluation Evaluate(const PolicyContext<TDecisionPoint>& context) noexcept {
        if (!context.IsCanonical()) {
            PolicyEvaluation invalid;
            invalid.Decision = PolicyDecision::Reject(
                static_cast<std::uint32_t>(PolicyCoreReason::InvalidEvaluationContext));
            return invalid;
        }

        PolicyEvaluation aggregate;
        for (std::size_t i = 0U; i < count_; ++i) {
            PolicyDecision current = gates_[i]->Evaluate(context);
            ++aggregate.EvaluatedCount;
            if (!current.IsCanonical()) current = PolicyHealthDetail::InvalidPolicyDecision();
            if (current.Verdict == PolicyVerdict::Defer) ++aggregate.DeferredCount;
            if (current.Verdict == PolicyVerdict::Reject) ++aggregate.RejectedCount;

            const auto currentRank = PolicyHealthDetail::PolicyRank(current.Verdict);
            const auto aggregateRank = PolicyHealthDetail::PolicyRank(aggregate.Decision.Verdict);
            if (currentRank > aggregateRank ||
                (currentRank == aggregateRank && currentRank != 0U &&
                 PolicyHealthDetail::PolicyDecisionLess(current, aggregate.Decision))) {
                aggregate.Decision = current;
            }
        }
        return aggregate;
    }
};

struct HealthCommonContext final {
    UpdateTransactionId Transaction{};
    UpdateGenerationId CandidateGeneration{};
    ReleaseIdentifier Release{};
    ManifestIdentifier Manifest{};
    UpdateGenerationId ExecutingGeneration{};
    UpdateGenerationId CommittedGeneration{};
    std::uint64_t TrialElapsedNanoseconds{0U};
    std::uint64_t TrialRemainingNanoseconds{0U};
    bool TrialDeadlineExpired{false};

    constexpr bool IsCanonical() const noexcept {
        if (!Transaction || !CandidateGeneration || !Release || !Manifest ||
            !ExecutingGeneration || !CommittedGeneration) return false;
        return TrialDeadlineExpired ? TrialRemainingNanoseconds == 0U : true;
    }
};

template<typename TCondition>
struct HealthContext final {
    HealthCommonContext Common{};
    static constexpr HealthConditionTypeId ConditionTypeId = TCondition::TypeId;

    constexpr bool IsCanonical() const noexcept {
        return bool(ConditionTypeId) && Common.IsCanonical();
    }
};

enum class HealthCoreReason : std::uint32_t {
    None = 0U,
    InvalidProviderDecision = 1U,
    InvalidEvaluationContext = 2U,
    MissingRequiredCondition = 3U,
    TrialDeadlineExpired = 4U
};

struct HealthDecision final {
    HealthResult Result{HealthResult::Pass};
    Diagnostic Detail{DiagnosticDomain::Health, 0U, 0, {}, 0U};

    constexpr bool IsCanonical() const noexcept {
        if (Result == HealthResult::Pass) return true;
        return Detail.Domain == DiagnosticDomain::Health && Detail.Reason != 0U;
    }

    static constexpr HealthDecision Pass() noexcept { return {}; }
    static constexpr HealthDecision Pending(std::uint32_t reason, std::int32_t nativeCode = 0) noexcept {
        return {HealthResult::Pending, {DiagnosticDomain::Health, reason, nativeCode, {}, 0U}};
    }
    static constexpr HealthDecision Fail(std::uint32_t reason, std::int32_t nativeCode = 0) noexcept {
        return {HealthResult::Fail, {DiagnosticDomain::Health, reason, nativeCode, {}, 0U}};
    }
};

namespace PolicyHealthDetail {

constexpr bool HealthDecisionLess(const HealthDecision& left, const HealthDecision& right) noexcept {
    return CompareDiagnostic(left.Detail, right.Detail) < 0;
}

inline constexpr HealthDecision InvalidHealthDecision() noexcept {
    return HealthDecision::Fail(static_cast<std::uint32_t>(HealthCoreReason::InvalidProviderDecision));
}

} // namespace PolicyHealthDetail

template<typename TCondition>
class IHealthCheck {
public:
    virtual ~IHealthCheck() = default;
    virtual HealthDecision Check(const HealthContext<TCondition>& context) noexcept = 0;
};

enum class HealthCheckRegistrationStatus : std::uint8_t {
    Success,
    Invalid,
    Duplicate,
    CapacityUnavailable
};

struct HealthEvaluation final {
    HealthConditionTypeId Condition{};
    HealthDecision Decision{};
    std::size_t EvaluatedCount{0U};
    std::size_t PendingCount{0U};
    std::size_t FailedCount{0U};
};

template<typename TCondition, std::size_t TMaximumChecks>
class HealthCheckSet final {
    static_assert(TMaximumChecks != 0U, "HealthCheckSet requires non-zero bounded capacity");
    std::array<IHealthCheck<TCondition>*, TMaximumChecks> checks_{};
    std::size_t count_{0U};
public:
    constexpr std::size_t Size() const noexcept { return count_; }
    constexpr std::size_t Capacity() const noexcept { return checks_.size(); }

    HealthCheckRegistrationStatus Add(IHealthCheck<TCondition>& check) noexcept {
        for (std::size_t i = 0U; i < count_; ++i)
            if (checks_[i] == &check) return HealthCheckRegistrationStatus::Duplicate;
        if (count_ == checks_.size()) return HealthCheckRegistrationStatus::CapacityUnavailable;
        checks_[count_++] = &check;
        return HealthCheckRegistrationStatus::Success;
    }

    HealthEvaluation Evaluate(const HealthContext<TCondition>& context) noexcept {
        HealthEvaluation aggregate;
        aggregate.Condition = TCondition::TypeId;
        if (!context.IsCanonical()) {
            aggregate.Decision = HealthDecision::Fail(
                static_cast<std::uint32_t>(HealthCoreReason::InvalidEvaluationContext));
            return aggregate;
        }

        for (std::size_t i = 0U; i < count_; ++i) {
            HealthDecision current = checks_[i]->Check(context);
            ++aggregate.EvaluatedCount;
            if (!current.IsCanonical()) current = PolicyHealthDetail::InvalidHealthDecision();
            if (current.Result == HealthResult::Pending) ++aggregate.PendingCount;
            if (current.Result == HealthResult::Fail) ++aggregate.FailedCount;

            const auto currentRank = PolicyHealthDetail::HealthRank(current.Result);
            const auto aggregateRank = PolicyHealthDetail::HealthRank(aggregate.Decision.Result);
            if (currentRank > aggregateRank ||
                (currentRank == aggregateRank && currentRank != 0U &&
                 PolicyHealthDetail::HealthDecisionLess(current, aggregate.Decision))) {
                aggregate.Decision = current;
            }
        }
        return aggregate;
    }
};

class IHealthConditionEvaluator {
public:
    virtual ~IHealthConditionEvaluator() = default;
    virtual HealthConditionTypeId ConditionType() const noexcept = 0;
    virtual HealthEvaluation Evaluate(const HealthCommonContext& context) noexcept = 0;
};

template<typename TCondition, std::size_t TMaximumChecks>
class HealthConditionEvaluator final : public IHealthConditionEvaluator {
    HealthCheckSet<TCondition, TMaximumChecks>& checks_;
public:
    explicit HealthConditionEvaluator(HealthCheckSet<TCondition, TMaximumChecks>& checks) noexcept : checks_(checks) {}
    HealthConditionTypeId ConditionType() const noexcept override { return TCondition::TypeId; }
    HealthEvaluation Evaluate(const HealthCommonContext& context) noexcept override {
        return checks_.Evaluate(HealthContext<TCondition>{context});
    }
};

enum class HealthRegistryStatus : std::uint8_t {
    Success,
    Invalid,
    DuplicateCondition,
    CapacityUnavailable
};

template<std::size_t TMaximumConditions>
class HealthRegistry final {
    static_assert(TMaximumConditions != 0U, "HealthRegistry requires non-zero bounded capacity");
    std::array<IHealthConditionEvaluator*, TMaximumConditions> evaluators_{};
    std::size_t count_{0U};
public:
    HealthRegistryStatus Add(IHealthConditionEvaluator& evaluator) noexcept {
        const auto id = evaluator.ConditionType();
        if (!id) return HealthRegistryStatus::Invalid;
        for (std::size_t i = 0U; i < count_; ++i)
            if (evaluators_[i]->ConditionType() == id) return HealthRegistryStatus::DuplicateCondition;
        if (count_ == evaluators_.size()) return HealthRegistryStatus::CapacityUnavailable;
        evaluators_[count_++] = &evaluator;
        return HealthRegistryStatus::Success;
    }

    std::size_t Size() const noexcept { return count_; }

    IHealthConditionEvaluator* Find(HealthConditionTypeId condition) const noexcept {
        for (std::size_t i = 0U; i < count_; ++i)
            if (evaluators_[i]->ConditionType() == condition) return evaluators_[i];
        return nullptr;
    }
};

} // namespace ESPressio::OTA
