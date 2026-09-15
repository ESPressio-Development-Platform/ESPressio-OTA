#define ESPRESSIO_OTA_COORDINATOR_SCENARIO 1
#define main ESPRESSIO_OTA_COORDINATOR_BASE_MAIN
#include "ota_coordinator_test.cpp"
#undef main
#undef ESPRESSIO_OTA_COORDINATOR_SCENARIO

namespace {

class FixedSourceSelector final : public IArtifactSourceSelector {
    IArtifactSource& source_;
public:
    std::size_t Calls{0U};
    ArtifactSourceSelectionContext Last{};

    explicit FixedSourceSelector(IArtifactSource& source) noexcept : source_(source) {}

    Result Select(const ArtifactSourceSelectionContext& context,
                  ArtifactSourceSelection& selection) noexcept override {
        ++Calls;
        Last = context;
        if (!context.IsValid()) {
            return {OutcomeClass::Invalid, {DiagnosticDomain::Source, 10U}};
        }
        selection.Source = &source_;
        selection.OffsetRead = false;
        return Result::Success();
    }
};

[[maybe_unused]] bool DriveToArtifactsAcquired(TestCoordinator& coordinator,
                                                OTAControlStore<Capacity>& control,
                                                UpdateTransactionId transaction,
                                                std::size_t maximumSteps = 64U) {
    for (std::size_t step = 0U; step < maximumSteps; ++step) {
        const auto result = coordinator.Advance();
        if (result.Outcome != OutcomeClass::Pending && result.Outcome != OutcomeClass::Deferred) return false;
        OTAControlRecord<Capacity> record;
        if (control.Load(record) != OTADurableStatus::Success || !record.HasActiveTransaction ||
            record.Active.Transaction != transaction) return false;
        if (record.Active.Point == RecoveryPoint::ArtifactsAcquired) return true;
    }
    return false;
}

} // namespace

#ifndef ESPRESSIO_OTA_SOURCE_SELECTION_SCENARIO
#define ESPRESSIO_OTA_SOURCE_SELECTION_SCENARIO 1
#endif

int main() {
    if (!InstallIdentity()) return 1;

#if ESPRESSIO_OTA_SOURCE_SELECTION_SCENARIO == 1
    Fixture fixture;
    if (!fixture.InitializeState()) return 2;
    if (fixture.Control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 3;

    // The initial Source cannot satisfy the requested Artifact length, so its
    // Open is a deterministic source-attempt failure before any Store mutation.
    fixture.Source.Size = 5U;

    FakeArtifactSource replacement;
    replacement.Size = 6U;
    replacement.Chunk = 2U;
    for (std::size_t i = 0U; i < replacement.Size; ++i) {
        replacement.Data[i] = static_cast<std::uint8_t>(0x80U + i);
    }
    FixedSourceSelector selector{replacement};

    auto coordinator = fixture.MakeCoordinator(5'000'000'000ULL);
    if (!coordinator.Initialize()) return 4;
    if (!coordinator.ConfigureArtifactSourceSelection(selector, 2U)) return 5;

    const auto manifest = CandidateManifest(70U, 70U, 1U, false, true);
    if (ValidateManifest(manifest) != ManifestStatus::Success) return 6;
    UpdateTransactionId transaction;
    if (!coordinator.Start({ReleaseIdentifier{70U}, ManifestIdentifier{Id(70U)}, SecurityGeneration{1U}}, transaction)) return 7;
    if (!coordinator.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 8;
    if (!DriveToArtifactsAcquired(coordinator, fixture.Control, transaction)) return 9;

    if (selector.Calls != 1U || selector.Last.Attempt != 2U ||
        !selector.Last.HasPreviousFailure || selector.Last.AcceptedCheckpointPrefix != 0U) return 10;
    if (fixture.Source.OpenCalls != 1U || replacement.OpenCalls != 1U || replacement.CloseCalls != 1U) return 11;
    if (fixture.ArtifactStore.Size != replacement.Size || fixture.ArtifactStore.FinalizeCalls != 1U) return 12;
    for (std::size_t i = 0U; i < replacement.Size; ++i) {
        if (fixture.ArtifactStore.Bytes[i] != replacement.Data[i]) return 13;
    }
    if (!coordinator.Cancel(transaction) || coordinator.Status().HasActiveTransaction) return 14;

#elif ESPRESSIO_OTA_SOURCE_SELECTION_SCENARIO == 2
    Fixture fixture;
    if (!fixture.InitializeState()) return 20;
    if (fixture.Control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 21;

    // Every attempt fails the same explicit Open precondition. The selector is
    // still authoritative over attempt 2; the Coordinator must stop before 3.
    fixture.Source.Size = 5U;
    FixedSourceSelector selector{fixture.Source};

    auto coordinator = fixture.MakeCoordinator(5'000'000'000ULL);
    if (!coordinator.Initialize()) return 22;
    if (!coordinator.ConfigureArtifactSourceSelection(selector, 2U)) return 23;

    const auto manifest = CandidateManifest(80U, 80U, 1U, false, true);
    if (ValidateManifest(manifest) != ManifestStatus::Success) return 24;
    UpdateTransactionId transaction;
    if (!coordinator.Start({ReleaseIdentifier{80U}, ManifestIdentifier{Id(80U)}, SecurityGeneration{1U}}, transaction)) return 25;
    if (!coordinator.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 26;

    Result terminal{OutcomeClass::Pending, {}};
    bool terminated = false;
    for (std::size_t step = 0U; step < 32U; ++step) {
        terminal = coordinator.Advance();
        if (!coordinator.Status().HasActiveTransaction) {
            terminated = true;
            break;
        }
        if (terminal.Outcome != OutcomeClass::Pending && terminal.Outcome != OutcomeClass::Deferred) return 27;
    }
    if (!terminated) return 28;
    if (terminal.Outcome != OutcomeClass::Unavailable ||
        terminal.Detail.Domain != DiagnosticDomain::OTA ||
        terminal.Detail.Reason != static_cast<std::uint32_t>(CoordinatorCoreReason::ArtifactSourceRetryExhausted)) return 29;
    if (selector.Calls != 1U || selector.Last.Attempt != 2U || fixture.Source.OpenCalls != 2U) return 30;
    if (coordinator.Status().Availability != CoordinatorAvailability::Ready || coordinator.Status().HasActiveTransaction) return 31;

#else
#error Unsupported ESPRESSIO_OTA_SOURCE_SELECTION_SCENARIO
#endif

    return 0;
}
