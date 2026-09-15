#include <array>
#include <cassert>
#include <cstdint>

#include <ESPressio_OTAEventBridge.hpp>
#include <ESPressio_OTACommandBridge.hpp>
#include <ESPressio_SerializationTraits.hpp>

using namespace ESPressio;
using namespace ESPressio::OTA;
using namespace ESPressio::OTA::Integration;

static_assert(Serializable::IsBoundedSerializable<UpdateStartedEvent>);
static_assert(Serializable::IsBoundedSerializable<UpdateFailedEvent>);
static_assert(Serializable::IsBoundedSerializable<StartUpdateCommand>);
static_assert(Serializable::IsBoundedSerializable<CancelUpdateCommand>);
static_assert(Serializable::IsBoundedSerializable<OTACommandResultValue>);
static_assert(StartUpdateCommand::TypeId != CancelUpdateCommand::TypeId);
static_assert(UpdateStartedEvent::TypeId != UpdateCommittedEvent::TypeId);

namespace {

struct FakeCoordinator final {
    std::size_t StartCalls{0U};
    std::size_t CancelCalls{0U};
    CoordinatorStartRequest LastStart{};
    UpdateTransactionId LastCancel{};

    Result Start(const CoordinatorStartRequest& request, UpdateTransactionId& transaction) noexcept {
        ++StartCalls;
        LastStart = request;
        if (!request.IsValid()) return {OutcomeClass::Invalid, {DiagnosticDomain::OTA, 11U, 0, {}, 0U}};
        transaction = UpdateTransactionId{77U};
        return Result::Success();
    }

    Result Cancel(UpdateTransactionId transaction) noexcept {
        ++CancelCalls;
        LastCancel = transaction;
        return transaction ? Result::Success()
                           : Result{OutcomeClass::Invalid, {DiagnosticDomain::OTA, 12U, 0, {}, 0U}};
    }
};

struct AllowAuthorizer final : IOTACommandAuthorizer {
    bool AuthorizeRemote(const Command::CommandExecutionContext&,
                         OTACommandOperation) const noexcept override {
        return true;
    }
};

System::DeviceIdentifier Device(std::uint8_t first) {
    System::DeviceIdentifier::Storage bytes{};
    bytes[0] = first;
    return System::DeviceIdentifier{bytes};
}

Command::CommandExecutionContext Context(bool remote) {
    const auto origin = Device(1U);
    const auto executor = remote ? Device(2U) : origin;
    const Command::CommandExecutionKey key{
        StartUpdateCommand::TypeId,
        origin,
        System::RuntimeIncarnationId{1U},
        Command::CommandId{9U}};
    return {key, Timing::QualifiedTime{},
            System::DeviceRuntimeIdentity{executor, System::RuntimeIncarnationId{2U}}};
}

ActiveUpdateTransactionValue ActiveTransaction() {
    ActiveUpdateTransactionValue active;
    active.Present = true;
    active.Transaction = 9U;
    active.CandidateGeneration = 10U;
    active.Release = 11U;
    active.Manifest[0] = 1U;
    active.CandidateSecurityGeneration = 12U;
    return active;
}

} // namespace

int main() {
    // Attaching an Event bridge to already-established State must not replay history.
    OTAEventBridge events;
    const auto active = ActiveTransaction();
    ActiveUpdateLifecycleValue lifecycle;
    lifecycle.Present = true;
    lifecycle.Transaction = active.Transaction;
    lifecycle.Lifecycle = UpdateLifecycle::Trial;
    LastUpdateOutcomeValue outcome{};
    events.Observe(active, lifecycle, outcome);
    assert(events.UnavailableOccurrences() == 0U);

    std::array<std::uint8_t, 16U> manifest{};
    manifest[0] = 0x5AU;
    StartUpdateCommand start{5U, manifest, 7U};
    CancelUpdateCommand cancel{77U};

    // Remote mutation is deny-by-default and cannot reach the Coordinator.
    FakeCoordinator deniedCoordinator;
    OTACommandBridge<FakeCoordinator> denied{deniedCoordinator};
    const auto deniedResult = denied.HandleStart(start, Context(true));
    assert(deniedResult.Outcome == static_cast<std::uint8_t>(OutcomeClass::Rejected));
    assert(deniedCoordinator.StartCalls == 0U);

    // Explicit remote authorization permits the same authoritative Start API.
    FakeCoordinator remoteCoordinator;
    AllowAuthorizer authorizer;
    OTACommandBridge<FakeCoordinator> remote{remoteCoordinator, &authorizer};
    const auto accepted = remote.HandleStart(start, Context(true));
    assert(accepted.Accepted());
    assert(accepted.Transaction == 77U);
    assert(remoteCoordinator.StartCalls == 1U);
    assert(remoteCoordinator.LastStart.Release == ReleaseIdentifier{5U});
    assert(remoteCoordinator.LastStart.Manifest == ManifestIdentifier{manifest});
    assert(remoteCoordinator.LastStart.CandidateSecurity == SecurityGeneration{7U});

    const auto cancelled = remote.HandleCancel(cancel, Context(true));
    assert(cancelled.Accepted());
    assert(remoteCoordinator.CancelCalls == 1U);
    assert(remoteCoordinator.LastCancel == UpdateTransactionId{77U});

    // Local Command execution does not require a remote authorization object.
    FakeCoordinator localCoordinator;
    OTACommandBridge<FakeCoordinator> local{localCoordinator};
    const auto localAccepted = local.HandleStart(start, Context(false));
    assert(localAccepted.Accepted());
    assert(localCoordinator.StartCalls == 1U);

    return 0;
}
