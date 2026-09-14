#include <array>
#include <cstdint>

#include "ESPressio_OTA.hpp"
#include <ESPressio_RuntimeIdentity.hpp>

using namespace ESPressio;
using namespace ESPressio::OTA;

namespace {

Timing::QualifiedTime CapturedTime() {
    return {123456U, Timing::TimeReliability::Synchronized};
}

bool InstallIdentity() {
    System::DeviceIdentifier::Storage bytes{};
    bytes[0] = 0x42U;
    const System::DeviceRuntimeIdentity expected{
        System::DeviceIdentifier{bytes}, System::RuntimeIncarnationId{1U}};
    const auto status = System::RuntimeIdentity::Install(expected);
    if (status == System::RuntimeIdentity::InstallationStatus::Success) return true;
    if (status != System::RuntimeIdentity::InstallationStatus::AlreadyInstalled) return false;
    System::DeviceRuntimeIdentity installed{};
    return System::RuntimeIdentity::TryRead(installed) && installed == expected;
}

} // namespace

int main() {
    if (!InstallIdentity()) return 1;

    Primitive::TypeDirectory<9> directory;
    if (RegisterOTAStateTypes(directory) != Primitive::TypeDirectoryRegistrationStatus::Success) return 2;
    if (directory.Size() != 9U) return 3;
    if (directory.Initialize() != Primitive::TypeDirectoryInitializationStatus::Success) return 4;

    const auto view = directory.View();
    if (!view.IsFrozen() || view.Size() != 9U) return 5;
    const auto* availabilityDescriptor = view.Find({State::StateFamilyId, CoordinatorAvailabilityState::TypeId.Value()});
    if (availabilityDescriptor == nullptr ||
        availabilityDescriptor->CanonicalName != CoordinatorAvailabilityState::CanonicalName) return 6;
    const auto* stateExtension = State::GetStateTypeDescriptor(*availabilityDescriptor);
    if (stateExtension == nullptr || stateExtension->Tier != State::StateTier::Transmissible) return 7;

    OTAStateRuntime<> runtime;
    OTAStateOwners owners;
    if (!BindOTAStateOwners(runtime, owners)) return 8;
    if (runtime.Initialize(view, &CapturedTime) != State::StateRuntimeStatus::Success) return 9;
    if (runtime.Start() != State::StateRuntimeStatus::Success) return 10;

    CoordinatorAvailabilityValue availability;
    availability.Availability = CoordinatorAvailability::Ready;
    if (owners.CoordinatorAvailability.Set(availability) != State::StateSetStatus::Changed) return 11;
    if (owners.CoordinatorAvailability.Set(availability) != State::StateSetStatus::NoChange) return 12;

    ActiveUpdateTransactionValue activeTransaction;
    if (!activeTransaction.IsCanonical()) return 13;
    if (owners.ActiveTransaction.Set(activeTransaction) != State::StateSetStatus::Changed) return 14;

    ActiveUpdateLifecycleValue lifecycle;
    if (!lifecycle.IsCanonical()) return 15;
    if (owners.ActiveLifecycle.Set(lifecycle) != State::StateSetStatus::Changed) return 16;

    GenerationValue executing;
    executing.Generation = 1U;
    if (!executing.IsCanonical()) return 17;
    if (owners.ExecutingGeneration.Set(executing) != State::StateSetStatus::Changed) return 18;

    CommittedGenerationValue committed;
    committed.Identity.Generation = 1U;
    committed.SecurityGeneration = 0U;
    if (!committed.IsCanonical()) return 19;
    if (owners.CommittedGeneration.Set(committed) != State::StateSetStatus::Changed) return 20;

    CandidateGenerationValue candidate;
    if (!candidate.IsCanonical()) return 21;
    if (owners.CandidateGeneration.Set(candidate) != State::StateSetStatus::Changed) return 22;

    MinimumAcceptedSecurityLevelValue floor;
    floor.SecurityGeneration = 0U;
    if (owners.MinimumAcceptedSecurity.Set(floor) != State::StateSetStatus::Changed) return 23;

    UpdateProgressValue progress;
    if (!progress.IsCanonical()) return 24;
    if (owners.UpdateProgress.Set(progress) != State::StateSetStatus::Changed) return 25;

    LastUpdateOutcomeValue lastOutcome;
    if (!lastOutcome.IsCanonical()) return 26;
    if (owners.LastOutcome.Set(lastOutcome) != State::StateSetStatus::Changed) return 27;

    State::StateSnapshot<ExecutingGenerationState> executingSnapshot;
    State::StateSnapshot<CommittedGenerationState> committedSnapshot;
    State::StateSnapshot<CandidateGenerationState> candidateSnapshot;
    if (!runtime.TryRead(executingSnapshot) || !runtime.TryRead(committedSnapshot) ||
        !runtime.TryRead(candidateSnapshot)) return 28;
    if (executingSnapshot.Value.Generation != 1U || committedSnapshot.Value.Identity.Generation != 1U ||
        candidateSnapshot.Value.Present) return 29;

    // Trial semantics: Executing becomes candidate while Committed remains previous known-good.
    activeTransaction.Present = true;
    activeTransaction.Transaction = 1U;
    activeTransaction.CandidateGeneration = 2U;
    activeTransaction.Release = 2U;
    activeTransaction.Manifest[15] = 2U;
    activeTransaction.CandidateSecurityGeneration = 1U;
    if (!activeTransaction.IsCanonical() ||
        owners.ActiveTransaction.Set(activeTransaction) != State::StateSetStatus::Changed) return 30;

    lifecycle.Present = true;
    lifecycle.Transaction = 1U;
    lifecycle.Lifecycle = UpdateLifecycle::Trial;
    if (!lifecycle.IsCanonical() || owners.ActiveLifecycle.Set(lifecycle) != State::StateSetStatus::Changed) return 31;

    candidate.Present = true;
    candidate.Transaction = 1U;
    candidate.Generation = 2U;
    candidate.Release = 2U;
    candidate.Manifest[15] = 2U;
    candidate.SecurityGeneration = 1U;
    if (!candidate.IsCanonical() || owners.CandidateGeneration.Set(candidate) != State::StateSetStatus::Changed) return 32;

    executing.Generation = 2U;
    executing.HasRelease = true;
    executing.Release = 2U;
    executing.HasManifest = true;
    executing.Manifest[15] = 2U;
    if (!executing.IsCanonical() || owners.ExecutingGeneration.Set(executing) != State::StateSetStatus::Changed) return 33;

    if (!runtime.TryRead(executingSnapshot) || !runtime.TryRead(committedSnapshot) ||
        !runtime.TryRead(candidateSnapshot)) return 34;
    if (executingSnapshot.Value.Generation != 2U || committedSnapshot.Value.Identity.Generation != 1U ||
        !candidateSnapshot.Value.Present || candidateSnapshot.Value.Generation != 2U) return 35;

    UpdateProgressValue liveProgress;
    liveProgress.Present = true;
    liveProgress.Transaction = 1U;
    liveProgress.Operation = UpdateOperation::Stage;
    liveProgress.StageType = 0x7001U;
    liveProgress.StagePosition = 2U;
    liveProgress.Mode = ProgressMode::Determinate;
    liveProgress.Unit = ProgressUnit::Bytes;
    liveProgress.Current = 512U;
    liveProgress.Total = 1024U;
    liveProgress.HasComponent = true;
    liveProgress.Component = 1U;
    if (!liveProgress.IsCanonical() || owners.UpdateProgress.Set(liveProgress) != State::StateSetStatus::Changed) return 36;

    Result terminalResult{OutcomeClass::Success, {}};
    const auto completed = LastUpdateOutcomeValue::FromResult(
        UpdateTransactionId{1U}, TerminalUpdateOutcome::Completed, UpdateOperation::Commit, terminalResult);
    if (!completed.IsCanonical() || owners.LastOutcome.Set(completed) != State::StateSetStatus::Changed) return 37;

    if (runtime.Shutdown() != State::StateRuntimeStatus::Success) return 38;
    return 0;
}
