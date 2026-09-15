#define main espressio_platform_recovery_original_main
#include "ota_coordinator_platform_recovery_test.cpp"
#undef main

#include <cstdio>

int main() {
    if (!InstallIdentity()) return 101;
    Fixture fixture;
    if (!fixture.InitializeRuntime()) return 102;
    ActiveTransactionRecord active;
    if (!fixture.CreateStaged(10U, active)) return 103;
    const auto manifest = CandidateManifest(10U);
    if (fixture.Control.ArmActivation(active.Transaction, Platform::OTA::BootTargetIdentifier{2U},
                                      Platform::OTA::BootTargetIdentifier{1U}) != OTADurableStatus::Success) return 104;
    fixture.Boot.SelectStatus = Platform::OTA::Status::Failed;
    auto coordinator = fixture.MakeCoordinator();
    const auto initialized = coordinator.Initialize();
    const auto bound = coordinator.BindVerifiedManifest(manifest, fixture.TargetProfile);
    const auto advanced = coordinator.Advance();
    OTAControlRecord<Capacity> record;
    const auto loaded = fixture.Control.Load(record);
    std::printf("init=%u/%u bind=%u/%u advance=%u/%u/%u/%d\n",
                static_cast<unsigned>(initialized.Outcome), static_cast<unsigned>(initialized.Detail.Reason),
                static_cast<unsigned>(bound.Outcome), static_cast<unsigned>(bound.Detail.Reason),
                static_cast<unsigned>(advanced.Outcome), static_cast<unsigned>(advanced.Detail.Domain),
                static_cast<unsigned>(advanced.Detail.Reason), static_cast<int>(advanced.Detail.NativeCode));
    std::printf("load=%u active=%u intent=%u point=%u tx=%llu cand=%u prev=%u\n",
                static_cast<unsigned>(loaded), record.HasActiveTransaction ? 1U : 0U,
                static_cast<unsigned>(record.Intent), static_cast<unsigned>(record.Active.Point),
                static_cast<unsigned long long>(record.Active.Transaction.Value()),
                static_cast<unsigned>(record.Active.CandidateBootTarget.Value()),
                static_cast<unsigned>(record.Active.PreviousCommittedBootTarget.Value()));
    std::printf("recovery=%u activate=%zu current=%u next=%u selects=%zu\n",
                static_cast<unsigned>(fixture.Handler.Recovery), fixture.Handler.ActivateCalls,
                static_cast<unsigned>(fixture.Boot.Current.Value()), static_cast<unsigned>(fixture.Boot.Next.Value()),
                fixture.Boot.SelectCalls);
    return 0;
}
