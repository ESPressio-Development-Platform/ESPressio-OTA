#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ESPressio_OTACoordinator.hpp"

#include <ESPressio_PrimitivePolicy.hpp>
#include <ESPressio_SerializableBase.hpp>
#include <ESPressio_SerializationMacros.hpp>
#include <ESPressio_TransmissibleCommand.hpp>

namespace ESPressio::OTA::Integration {

/** Separate control-plane authorization classification; this is not an OTA PolicyGate. */
enum class OTACommandOperation : std::uint8_t {
    StartUpdate = 1U,
    CancelUpdate = 2U
};

/**
 * Remote mutating OTA Commands are denied unless an application supplies this
 * explicit authorization boundary and it approves the Command execution facts.
 */
class IOTACommandAuthorizer {
public:
    virtual ~IOTACommandAuthorizer() = default;
    virtual bool AuthorizeRemote(const Command::CommandExecutionContext& context,
                                 OTACommandOperation operation) const noexcept = 0;
};

/** Finite semantic delivery policy for OTA control requests/responses. */
struct OTACommandDeliveryPolicy final {
    using PolicyCategory = Primitive::OccurrenceDeliveryPolicyTag;
    using RequiredEvidence = Primitive::DestinationPrimitiveAdmission;
    using TerminalDisposition = Primitive::DiagnosticOnlyAfterBudget;
    static constexpr std::uint64_t MaximumResidenceNanoseconds = 10'000'000'000ULL;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds = 1'000'000ULL;
    static constexpr std::uint16_t MaximumAttempts = 3U;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds = 1'000'000ULL;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds = 250'000'000ULL;
};

/**
 * Keep a small durable response budget so duplicate Start requests can replay
 * their accepted UpdateTransactionId without invoking the Coordinator again.
 */
struct OTACommandCompletionRetention final {
    static constexpr std::size_t MaximumTrackedOrigins = 4U;
    static constexpr std::size_t ReplayWindowEntries = 8U;
    using ResultRetention = Command::PersistentResults<4U, 512U>;
};

/** Bounded immediate Command result; long-running completion remains in OTA State/Event. */
struct OTACommandResultValue final : Serializable::SerializableBase<OTACommandResultValue> {
    std::uint8_t Outcome{static_cast<std::uint8_t>(OutcomeClass::Failed)};
    std::uint8_t Domain{static_cast<std::uint8_t>(DiagnosticDomain::OTA)};
    std::uint32_t Reason{0U};
    std::int32_t NativeCode{0};
    std::uint64_t Transaction{0U};

    constexpr bool Accepted() const noexcept {
        return Outcome == static_cast<std::uint8_t>(OutcomeClass::Success) && Transaction != 0U;
    }

    static OTACommandResultValue From(const Result& result,
                                      UpdateTransactionId transaction = {}) noexcept {
        OTACommandResultValue value;
        value.Outcome = static_cast<std::uint8_t>(result.Outcome);
        value.Domain = static_cast<std::uint8_t>(result.Detail.Domain);
        value.Reason = result.Detail.Reason;
        value.NativeCode = result.Detail.NativeCode;
        value.Transaction = transaction.Value();
        return value;
    }

    static OTACommandResultValue Unauthorized() noexcept {
        Result rejected{OutcomeClass::Rejected,
            {DiagnosticDomain::OTA, 0x434D4401U, 0, {}, 0U}};
        return From(rejected);
    }

    ESPRESSIO_SERIALIZABLE_TYPE(OTACommandResultValue)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("outcome", Outcome),
        ESPRESSIO_PROPERTY_REQUIRED("domain", Domain),
        ESPRESSIO_PROPERTY_REQUIRED("reason", Reason),
        ESPRESSIO_PROPERTY_REQUIRED("nativeCode", NativeCode),
        ESPRESSIO_PROPERTY_REQUIRED("transaction", Transaction))
};

struct StartUpdateCommand final
    : Command::TransmissibleCommand<StartUpdateCommand, OTACommandResultValue> {
    static constexpr Command::CommandTypeId TypeId{0x45534F5441430001ULL};
    static constexpr std::string_view CanonicalName{"ESPressio.OTA.Command.StartUpdate"};
    static constexpr std::size_t MaximumLiveInstances = 2U;
    static constexpr std::size_t MaximumPendingExecutions = 1U;
    static constexpr std::size_t MaximumPendingResponses = 2U;
    using ExecutionAdmissionPolicy = Command::RequiredExecution;
    using RequestDeliveryPolicy = OTACommandDeliveryPolicy;
    using ResponseDeliveryPolicy = OTACommandDeliveryPolicy;
    using CompletionRetentionPolicy = OTACommandCompletionRetention;

    std::uint64_t Release{0U};
    std::array<std::uint8_t, 16U> Manifest{};
    std::uint64_t CandidateSecurityGeneration{0U};

    StartUpdateCommand() noexcept = default;
    StartUpdateCommand(std::uint64_t release,
                       const std::array<std::uint8_t, 16U>& manifest,
                       std::uint64_t candidateSecurityGeneration) noexcept
        : Release(release), Manifest(manifest),
          CandidateSecurityGeneration(candidateSecurityGeneration) {}

    ESPRESSIO_SERIALIZABLE_TYPE(StartUpdateCommand)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("release", Release),
        ESPRESSIO_PROPERTY_REQUIRED("manifest", Manifest),
        ESPRESSIO_PROPERTY_REQUIRED("candidateSecurityGeneration", CandidateSecurityGeneration))
};

struct CancelUpdateCommand final
    : Command::TransmissibleCommand<CancelUpdateCommand, OTACommandResultValue> {
    static constexpr Command::CommandTypeId TypeId{0x45534F5441430002ULL};
    static constexpr std::string_view CanonicalName{"ESPressio.OTA.Command.CancelUpdate"};
    static constexpr std::size_t MaximumLiveInstances = 2U;
    static constexpr std::size_t MaximumPendingExecutions = 1U;
    static constexpr std::size_t MaximumPendingResponses = 2U;
    using ExecutionAdmissionPolicy = Command::RequiredExecution;
    using RequestDeliveryPolicy = OTACommandDeliveryPolicy;
    using ResponseDeliveryPolicy = OTACommandDeliveryPolicy;
    using CompletionRetentionPolicy = OTACommandCompletionRetention;

    std::uint64_t Transaction{0U};

    CancelUpdateCommand() noexcept = default;
    explicit CancelUpdateCommand(std::uint64_t transaction) noexcept : Transaction(transaction) {}

    ESPRESSIO_SERIALIZABLE_TYPE(CancelUpdateCommand)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("transaction", Transaction))
};

} // namespace ESPressio::OTA::Integration
