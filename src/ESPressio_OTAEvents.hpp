#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ESPressio_OTAState.hpp"

#include <ESPressio_PrimitivePolicy.hpp>
#include <ESPressio_TransmissibleEvent.hpp>
#include <ESPressio_SerializationMacros.hpp>

namespace ESPressio::OTA::Integration {

/**
 * Finite best-effort policy for optional OTA diagnostic/lifecycle Events.
 *
 * These Events are projections of already-established canonical OTA State. They
 * are never transaction authority, so exhausting the bounded delivery budget is
 * diagnostic-only and must never fail or roll back the OTA operation itself.
 */
struct OTAEventDeliveryPolicy final {
    using PolicyCategory = Primitive::OccurrenceDeliveryPolicyTag;
    using RequiredEvidence = Primitive::DestinationPrimitiveAdmission;
    using TerminalDisposition = Primitive::DiagnosticOnlyAfterBudget;
    static constexpr std::uint64_t MaximumResidenceNanoseconds = 5'000'000'000ULL;
    static constexpr std::uint64_t MaximumAdapterAdmissionWaitNanoseconds = 0U;
    static constexpr std::uint16_t MaximumAttempts = 2U;
    static constexpr std::uint64_t MinimumRetrySpacingNanoseconds = 1'000'000ULL;
    static constexpr std::uint64_t MaximumRetrySpacingNanoseconds = 100'000'000ULL;
};

/** Bounded identity snapshot carried by meaningful OTA lifecycle Events. */
struct OTAEventTransactionContext final
    : Serializable::SerializableBase<OTAEventTransactionContext> {
    std::uint64_t Transaction{0U};
    std::uint64_t CandidateGeneration{0U};
    std::uint64_t Release{0U};
    std::array<std::uint8_t, 16U> Manifest{};

    constexpr OTAEventTransactionContext() noexcept = default;
    constexpr OTAEventTransactionContext(
        std::uint64_t transaction,
        std::uint64_t candidateGeneration,
        std::uint64_t release,
        const std::array<std::uint8_t, 16U>& manifest) noexcept
        : Transaction(transaction), CandidateGeneration(candidateGeneration),
          Release(release), Manifest(manifest) {}

    constexpr bool IsValid() const noexcept {
        return Transaction != 0U && CandidateGeneration != 0U && Release != 0U &&
               bool(ManifestIdentifier{Manifest});
    }

    constexpr bool operator==(const OTAEventTransactionContext& other) const noexcept {
        return Transaction == other.Transaction && CandidateGeneration == other.CandidateGeneration &&
               Release == other.Release && Detail::FixedBytesEqual(Manifest, other.Manifest);
    }

    static constexpr OTAEventTransactionContext From(
        const ActiveUpdateTransactionValue& active) noexcept {
        return OTAEventTransactionContext{
            active.Transaction, active.CandidateGeneration, active.Release, active.Manifest};
    }

    ESPRESSIO_SERIALIZABLE_TYPE(OTAEventTransactionContext)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("transaction", Transaction),
        ESPRESSIO_PROPERTY_REQUIRED("candidateGeneration", CandidateGeneration),
        ESPRESSIO_PROPERTY_REQUIRED("release", Release),
        ESPRESSIO_PROPERTY_REQUIRED("manifest", Manifest))
};

#define ESPRESSIO_OTA_DECLARE_LIFECYCLE_EVENT(TName, TId, TCanonicalName) \
    struct TName final : Event::TransmissibleEvent<TName> { \
        static constexpr Event::EventTypeId TypeId{TId}; \
        static constexpr std::string_view CanonicalName{TCanonicalName}; \
        static constexpr std::size_t MaximumLiveInstances = 4U; \
        static constexpr std::size_t MaximumPendingInstances = 1U; \
        using DeliveryPolicy = OTAEventDeliveryPolicy; \
        OTAEventTransactionContext Context{}; \
        TName() noexcept = default; \
        explicit TName(const OTAEventTransactionContext& context) noexcept : Context(context) {} \
        ESPRESSIO_SERIALIZABLE_TYPE(TName) \
        ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1) \
        ESPRESSIO_SERIALIZABLE_PROPERTIES(ESPRESSIO_PROPERTY_REQUIRED("context", Context)) \
    }

ESPRESSIO_OTA_DECLARE_LIFECYCLE_EVENT(UpdateStartedEvent,
    0x45534F5441450001ULL, "ESPressio.OTA.Event.UpdateStarted");
ESPRESSIO_OTA_DECLARE_LIFECYCLE_EVENT(CandidateSelectedEvent,
    0x45534F5441450002ULL, "ESPressio.OTA.Event.CandidateSelected");
ESPRESSIO_OTA_DECLARE_LIFECYCLE_EVENT(CandidateStagedEvent,
    0x45534F5441450003ULL, "ESPressio.OTA.Event.CandidateStaged");
ESPRESSIO_OTA_DECLARE_LIFECYCLE_EVENT(TrialStartedEvent,
    0x45534F5441450004ULL, "ESPressio.OTA.Event.TrialStarted");
ESPRESSIO_OTA_DECLARE_LIFECYCLE_EVENT(UpdateCommittedEvent,
    0x45534F5441450005ULL, "ESPressio.OTA.Event.UpdateCommitted");
ESPRESSIO_OTA_DECLARE_LIFECYCLE_EVENT(UpdateRolledBackEvent,
    0x45534F5441450006ULL, "ESPressio.OTA.Event.UpdateRolledBack");
ESPRESSIO_OTA_DECLARE_LIFECYCLE_EVENT(UpdateCancelledEvent,
    0x45534F5441450007ULL, "ESPressio.OTA.Event.UpdateCancelled");
ESPRESSIO_OTA_DECLARE_LIFECYCLE_EVENT(RecoveryRequiredEvent,
    0x45534F5441450008ULL, "ESPressio.OTA.Event.RecoveryRequired");

#undef ESPRESSIO_OTA_DECLARE_LIFECYCLE_EVENT

/** Terminal failure projection. Diagnostic payload is copied only after State owns it. */
struct UpdateFailedEvent final : Event::TransmissibleEvent<UpdateFailedEvent> {
    static constexpr Event::EventTypeId TypeId{0x45534F5441450009ULL};
    static constexpr std::string_view CanonicalName{"ESPressio.OTA.Event.UpdateFailed"};
    static constexpr std::size_t MaximumLiveInstances = 4U;
    static constexpr std::size_t MaximumPendingInstances = 1U;
    using DeliveryPolicy = OTAEventDeliveryPolicy;

    OTAEventTransactionContext Context{};
    LastUpdateOutcomeValue Outcome{};

    UpdateFailedEvent() noexcept = default;
    UpdateFailedEvent(const OTAEventTransactionContext& context,
                      const LastUpdateOutcomeValue& outcome) noexcept
        : Context(context), Outcome(outcome) {}

    ESPRESSIO_SERIALIZABLE_TYPE(UpdateFailedEvent)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("context", Context),
        ESPRESSIO_PROPERTY_REQUIRED("outcome", Outcome))
};

struct RollbackFailedEvent final : Event::TransmissibleEvent<RollbackFailedEvent> {
    static constexpr Event::EventTypeId TypeId{0x45534F544145000AULL};
    static constexpr std::string_view CanonicalName{"ESPressio.OTA.Event.RollbackFailed"};
    static constexpr std::size_t MaximumLiveInstances = 4U;
    static constexpr std::size_t MaximumPendingInstances = 1U;
    using DeliveryPolicy = OTAEventDeliveryPolicy;

    OTAEventTransactionContext Context{};
    LastUpdateOutcomeValue Outcome{};

    RollbackFailedEvent() noexcept = default;
    RollbackFailedEvent(const OTAEventTransactionContext& context,
                        const LastUpdateOutcomeValue& outcome) noexcept
        : Context(context), Outcome(outcome) {}

    ESPRESSIO_SERIALIZABLE_TYPE(RollbackFailedEvent)
    ESPRESSIO_SERIALIZABLE_SCHEMA_VERSION(1)
    ESPRESSIO_SERIALIZABLE_PROPERTIES(
        ESPRESSIO_PROPERTY_REQUIRED("context", Context),
        ESPRESSIO_PROPERTY_REQUIRED("outcome", Outcome))
};

} // namespace ESPressio::OTA::Integration
