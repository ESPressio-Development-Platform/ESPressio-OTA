#include <cstddef>
#include <cstdio>

#include "ESPressio_OTA.hpp"

using namespace ESPressio;
using namespace ESPressio::OTA;

namespace {
using Capacity = ConstrainedV1CapacityProfile;

struct BootBackend final : Platform::Backend {};
struct TrialBackend final : Platform::Backend {};
struct RestartBackend final : Platform::Backend {};
struct ClockBackend final : Platform::Backend {};

class SizeBootControl final : public Platform::ProviderDeclaration<
    BootBackend, Platform::CapabilitySet<Platform::Capability::BootControl>> {
public:
    Platform::OTA::BootTargetIdentifier CurrentBootTarget() const noexcept {
        return Platform::OTA::BootTargetIdentifier{1U};
    }
    Platform::OTA::BootTargetIdentifier CommittedBootTarget() const noexcept {
        return Platform::OTA::BootTargetIdentifier{1U};
    }
    Platform::OTA::BootTargetIdentifier NextBootTarget() const noexcept {
        return Platform::OTA::BootTargetIdentifier{1U};
    }
    Platform::OTA::Result SelectNextBootTarget(Platform::OTA::BootTargetIdentifier) noexcept {
        return {Platform::OTA::Status::Success, 0};
    }
};

class SizeTrialBoot final : public Platform::ProviderDeclaration<
    TrialBackend, Platform::CapabilitySet<Platform::Capability::TrialBoot>> {
public:
    bool IsCurrentBootTrial() const noexcept { return false; }
    Platform::OTA::Result MarkCurrentBootValid() noexcept { return {Platform::OTA::Status::Success, 0}; }
    Platform::OTA::Result MarkCurrentBootInvalid() noexcept { return {Platform::OTA::Status::Success, 0}; }
};

class SizeRestart final : public Platform::ProviderDeclaration<
    RestartBackend, Platform::CapabilitySet<Platform::Capability::SystemRestart>> {
public:
    Platform::OTA::Result Restart(Platform::OTA::RestartReason) noexcept {
        return {Platform::OTA::Status::Success, 0};
    }
};

class SizeClock final : public Platform::Clock::MonotonicProviderDeclaration<ClockBackend, 1'000'000ULL, 64U> {
public:
    Platform::Clock::Tick Now() const noexcept { return 0U; }
};

using SizeCoordinator = Coordinator<Capacity, SizeBootControl, SizeTrialBoot, SizeRestart, SizeClock>;

constexpr std::size_t RetainedCoordinatorBytes = sizeof(SizeCoordinator);
constexpr std::size_t RetainedStateRuntimeBytes = sizeof(OTAStateRuntime<>);
constexpr std::size_t RetainedStateOwnerBindingBytes = sizeof(OTAStateOwners);
constexpr std::size_t RetainedManifestBytes = sizeof(SignedManifest<Capacity>);
constexpr std::size_t RetainedManifestWorkspaceBytes = sizeof(ManifestWireWorkspace<Capacity>);
constexpr std::size_t RetainedTransferWorkspaceBytes = sizeof(ArtifactTransferWorkspace<Capacity>);
constexpr std::size_t RetainedTargetProfileBytes = sizeof(UpdateTargetProfile<Capacity>);
constexpr std::size_t RetainedCoreEnvelopeBytes =
    RetainedCoordinatorBytes +
    RetainedStateRuntimeBytes +
    RetainedStateOwnerBindingBytes +
    RetainedManifestBytes +
    RetainedManifestWorkspaceBytes +
    RetainedTransferWorkspaceBytes +
    RetainedTargetProfileBytes;

constexpr std::size_t TransientControlRecordBytes = sizeof(OTAControlRecord<Capacity>);
constexpr std::size_t TransientCheckpointBytes = sizeof(ArtifactCheckpoint<Capacity>);
constexpr std::size_t DurableControlBufferCeiling = Capacity::MaximumOTAControlRecordBytes;
constexpr std::size_t DurableCheckpointBufferCeiling = Capacity::MaximumArtifactCheckpointRecordBytes;
constexpr std::size_t DedicatedOTATaskStackBytes = 0U;

static_assert(Capacity::TransferBufferBytes == 4096U);
static_assert(Capacity::MaximumManifestBytes == 8192U);
static_assert(DedicatedOTATaskStackBytes == 0U,
              "OTA core must remain caller-driven and retain no dedicated Task stack");
// This is a retained-object guardrail, not a claim about total application RAM.
// Whole-device PlatformIO evidence is recorded separately because framework,
// radio, networking and backend statics dominate the final ESP32 image.
static_assert(RetainedCoreEnvelopeBytes <= 64U * 1024U,
              "Constrained V1 OTA retained envelope exceeded the 64 KiB review guardrail");
} // namespace

int main() {
    std::printf("OTA constrained retained-memory accounting (host ABI)\n");
    std::printf("  Coordinator:                 %zu bytes\n", RetainedCoordinatorBytes);
    std::printf("  State runtime:               %zu bytes\n", RetainedStateRuntimeBytes);
    std::printf("  State owner bindings:        %zu bytes\n", RetainedStateOwnerBindingBytes);
    std::printf("  Signed Manifest object:      %zu bytes\n", RetainedManifestBytes);
    std::printf("  Manifest wire workspace:     %zu bytes\n", RetainedManifestWorkspaceBytes);
    std::printf("  Artifact transfer workspace: %zu bytes\n", RetainedTransferWorkspaceBytes);
    std::printf("  Frozen target profile:       %zu bytes\n", RetainedTargetProfileBytes);
    std::printf("  Retained core envelope:      %zu bytes\n", RetainedCoreEnvelopeBytes);
    std::printf("  Transient control record:    %zu bytes\n", TransientControlRecordBytes);
    std::printf("  Transient checkpoint:        %zu bytes\n", TransientCheckpointBytes);
    std::printf("  Durable control max record:  %zu bytes\n", DurableControlBufferCeiling);
    std::printf("  Durable checkpoint max:      %zu bytes\n", DurableCheckpointBufferCeiling);
    std::printf("  Dedicated OTA task stack:    %zu bytes\n", DedicatedOTATaskStackBytes);
    return 0;
}
