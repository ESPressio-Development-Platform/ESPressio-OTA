#include <cstdio>
#include <cstdint>

#include <ESPressio_OTA.hpp>
#include <ESPressio_Platform_IDFOTA.hpp>

namespace {

using Capacity = ESPressio::OTA::ConstrainedV1CapacityProfile;
using ESPressio::Platform::OTA::Status;

/**
 * Read-only ESP-IDF OTA provider readiness demonstration.
 *
 * The example deliberately inspects and preflights only. It does not begin an
 * image write, change the selected boot partition, mark a trial image, or
 * restart the device. Those mutating operations remain Coordinator-owned in a
 * complete OTA application.
 */
class OTAIDFReadinessDemo final {
    ESPressio::Platform::IDF::OTAApplicationImageStaging staging_{};
    ESPressio::Platform::IDF::OTABootControl boot_{};
    ESPressio::Platform::IDF::OTATrialBoot trial_{};
    ESPressio::Platform::IDF::OTAStorageLayoutInspection layout_{};

    static const char* StatusName(Status status) noexcept {
        switch (status) {
            case Status::Success: return "Success";
            case Status::Pending: return "Pending";
            case Status::Unsupported: return "Unsupported";
            case Status::Invalid: return "Invalid";
            case Status::Busy: return "Busy";
            case Status::CapacityUnavailable: return "CapacityUnavailable";
            case Status::Failed: return "Failed";
        }
        return "Unknown";
    }

    void PrintBootFacts() const noexcept {
        const auto current = boot_.CurrentBootTarget();
        const auto committed = boot_.CommittedBootTarget();
        const auto next = boot_.NextBootTarget();
        std::printf("Current boot target:   0x%08lx\n", static_cast<unsigned long>(current.Value()));
        std::printf("Committed boot target: 0x%08lx\n", static_cast<unsigned long>(committed.Value()));
        std::printf("Next boot target:      0x%08lx\n", static_cast<unsigned long>(next.Value()));
        std::printf("Current boot is trial: %s\n", trial_.IsCurrentBootTrial() ? "yes" : "no");
    }

    void PrintStorageFacts() const noexcept {
        ESPressio::Platform::OTA::StorageLayoutInfo info{};
        const auto result = layout_.InspectStorageLayout(info);
        std::printf("Storage inspection:    %s (native=%ld)\n",
                    StatusName(result.Code), static_cast<long>(result.NativeCode));
        if (!result) return;
        std::printf("Storage layout ID:     0x%016llx\n",
                    static_cast<unsigned long long>(info.Layout.Value()));
        std::printf("Partition bytes:       %llu\n", static_cast<unsigned long long>(info.TotalBytes));
        std::printf("Inactive OTA capacity: %llu\n", static_cast<unsigned long long>(info.AvailableBytes));
    }

    void DemonstrateSafePreflight() noexcept {
        constexpr std::uint64_t illustrativeImageBytes = 256U * 1024U;
        const auto preflight = staging_.PreflightApplicationImage(illustrativeImageBytes);
        std::printf("256 KiB stage preflight: %s, required=%llu, maximum=%llu bytes\n",
                    StatusName(preflight.Code),
                    static_cast<unsigned long long>(preflight.RequiredBytes),
                    static_cast<unsigned long long>(preflight.MaximumBytes));
        // Preflight is non-mutating. No BeginApplicationImage() follows.
    }

public:
    void Run() noexcept {
        static_assert(ESPressio::Platform::OTA::IsApplicationImageStagingProviderV<
                      ESPressio::Platform::IDF::OTAApplicationImageStaging>);
        static_assert(ESPressio::Platform::OTA::IsBootControlProviderV<
                      ESPressio::Platform::IDF::OTABootControl>);
        static_assert(ESPressio::Platform::OTA::IsTrialBootProviderV<
                      ESPressio::Platform::IDF::OTATrialBoot>);
        static_assert(Capacity::MaximumManifestBytes == 8192U);
        static_assert(Capacity::TransferBufferBytes == 4096U);

        std::printf("\nESPressio OTA ESP-IDF provider readiness demo\n");
        std::printf("Manifest bound:        %u bytes\n", static_cast<unsigned>(Capacity::MaximumManifestBytes));
        std::printf("Transfer workspace:    %u bytes\n", static_cast<unsigned>(Capacity::TransferBufferBytes));
        PrintBootFacts();
        PrintStorageFacts();
        DemonstrateSafePreflight();
        std::printf("No flash, boot-selection, trial-state or restart mutation was performed.\n");
    }
};

} // namespace

extern "C" void app_main() {
    OTAIDFReadinessDemo demo;
    demo.Run();
}
