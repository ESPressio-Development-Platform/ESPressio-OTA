from pathlib import Path

path = Path("tests/ota_coordinator_test.cpp")
text = path.read_text()


def replace_once(old: str, new: str) -> None:
    global text
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"expected exactly one match, found {count}: {old[:120]!r}")
    text = text.replace(old, new, 1)

replace_once(
'''    std::size_t PrepareCalls{0U};
    std::size_t StageCalls{0U};
    std::size_t FinalizeCalls{0U};
''',
'''    std::size_t PrepareCalls{0U};
    std::size_t StageCalls{0U};
    std::size_t FinalizeCalls{0U};
    std::size_t ActivateCalls{0U};
    std::size_t CommitCalls{0U};
    std::size_t RollbackCalls{0U};
''')

replace_once(
'''    ComponentRecoveryInspection InspectRecoveryState(
        const ComponentPreflightContext<Capacity>&) noexcept override {
        return {Recovery, Result::Success()};
    }
};''',
'''    ComponentActionResult Activate(const ComponentExecutionContext<Capacity>& context) noexcept override {
        ++ActivateCalls;
        if (!context.IsValid()) {
            return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 4U}});
        }
        Recovery = ComponentRecoveryState::Activated;
        return ComponentActionResult::Complete();
    }

    ComponentActionResult Commit(const ComponentExecutionContext<Capacity>& context) noexcept override {
        ++CommitCalls;
        if (!context.IsValid()) {
            return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 5U}});
        }
        Recovery = ComponentRecoveryState::Committed;
        return ComponentActionResult::Complete();
    }

    ComponentActionResult Rollback(const ComponentExecutionContext<Capacity>& context) noexcept override {
        ++RollbackCalls;
        if (!context.IsValid()) {
            return ComponentActionResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 6U}});
        }
        Recovery = ComponentRecoveryState::RolledBack;
        return ComponentActionResult::Complete();
    }

    ComponentRecoveryInspection InspectRecoveryState(
        const ComponentPreflightContext<Capacity>&) noexcept override {
        return {Recovery, Result::Success()};
    }
};''')

replace_once(
'''        if (coordinator.Advance().Outcome != OutcomeClass::Pending || fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{2U}) return 31;
''',
'''        if (coordinator.Advance().Outcome != OutcomeClass::Pending ||
            fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{2U} ||
            fixture.ComponentHandler.ActivateCalls != 1U) return 31;
''')

replace_once(
'''        if (!coordinator.Advance()) return 36;
        if (fixture.Trial.MarkValidCalls != 1U) return 37;
''',
'''        if (!coordinator.Advance()) return 36;
        if (fixture.Trial.MarkValidCalls != 1U || fixture.ComponentHandler.CommitCalls != 1U) return 37;
''')

# Scenario 3 uses a no-health manifest. Verify activation and reverse rollback hooks are actually driven.
replace_once(
'''        if (coordinator.Advance().Outcome != OutcomeClass::Pending) return 58;
        fixture.Boot.Current = Platform::OTA::BootTargetIdentifier{2U};''',
'''        if (coordinator.Advance().Outcome != OutcomeClass::Pending ||
            fixture.ComponentHandler.ActivateCalls != 1U) return 58;
        fixture.Boot.Current = Platform::OTA::BootTargetIdentifier{2U};''')

replace_once(
'''        if (coordinator.Advance().Outcome != OutcomeClass::Pending || fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{1U}) return 61;
''',
'''        if (coordinator.Advance().Outcome != OutcomeClass::Pending ||
            fixture.Boot.Next != Platform::OTA::BootTargetIdentifier{1U} ||
            fixture.ComponentHandler.RollbackCalls != 1U) return 61;
''')

scenario5 = r'''
#elif ESPRESSIO_OTA_COORDINATOR_SCENARIO == 5
    {
        Fixture fixture;
        if (!fixture.InitializeState()) return 90;
        if (fixture.Control.ProvisionBaseline(FactoryBaseline(), SecurityGeneration{0U}) != OTADurableStatus::Success) return 91;
        const auto manifest = CandidateManifest(50U, 50U, 1U, false);
        if (ValidateManifest(manifest) != ManifestStatus::Success) return 92;
        UpdateTransactionId transaction;

        {
            auto first = fixture.MakeCoordinator(5'000'000'000ULL);
            if (!first.Initialize()) return 93;
            if (!first.Start({ReleaseIdentifier{50U}, ManifestIdentifier{Id(50U)}, SecurityGeneration{1U}}, transaction)) return 94;
            if (!first.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 95;

            // Zero-Artifact acquisition and verification still cross their durable boundaries.
            if (first.Advance().Outcome != OutcomeClass::Pending) return 96;
            if (first.Advance().Outcome != OutcomeClass::Pending) return 97;
            // Persist StagingStarted before any component mutation.
            if (first.Advance().Outcome != OutcomeClass::Pending) return 98;
            // Execute Prepare only, then simulate power loss while PartiallyStaged.
            if (first.Advance().Outcome != OutcomeClass::Pending) return 99;
            OTAControlRecord<Capacity> interrupted;
            if (fixture.Control.Load(interrupted) != OTADurableStatus::Success ||
                interrupted.Active.Point != RecoveryPoint::StagingStarted ||
                fixture.ComponentHandler.Recovery != ComponentRecoveryState::PartiallyStaged ||
                fixture.ComponentHandler.PrepareCalls != 1U ||
                fixture.ComponentHandler.StageCalls != 0U ||
                fixture.ComponentHandler.FinalizeCalls != 0U) return 100;
        }

        // Reconstruct a fresh Coordinator over the same durable/platform/component facts.
        auto recovered = fixture.MakeCoordinator(5'000'000'000ULL);
        if (!recovered.Initialize() || recovered.Status().Lifecycle != UpdateLifecycle::Staging) return 101;
        if (!recovered.BindVerifiedManifest(manifest, fixture.TargetProfile)) return 102;
        if (recovered.Advance().Outcome != OutcomeClass::Pending) return 103;
        if (fixture.ComponentHandler.PrepareCalls != 1U ||
            fixture.ComponentHandler.StageCalls != 1U ||
            fixture.ComponentHandler.FinalizeCalls != 0U) return 104;
        if (recovered.Advance().Outcome != OutcomeClass::Pending) return 105;

        OTAControlRecord<Capacity> staged;
        if (fixture.Control.Load(staged) != OTADurableStatus::Success ||
            staged.Active.Point != RecoveryPoint::Staged ||
            recovered.Status().Lifecycle != UpdateLifecycle::Staged ||
            fixture.ComponentHandler.PrepareCalls != 1U ||
            fixture.ComponentHandler.StageCalls != 1U ||
            fixture.ComponentHandler.FinalizeCalls != 1U ||
            fixture.ComponentHandler.Recovery != ComponentRecoveryState::Staged) return 106;
    }
'''
replace_once('\n#else\n#error Unsupported ESPRESSIO_OTA_COORDINATOR_SCENARIO\n', scenario5 + '\n#else\n#error Unsupported ESPRESSIO_OTA_COORDINATOR_SCENARIO\n')

path.write_text(text)
print("patched", path)
