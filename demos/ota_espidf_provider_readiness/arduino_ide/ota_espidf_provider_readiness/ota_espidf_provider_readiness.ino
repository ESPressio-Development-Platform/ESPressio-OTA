#include <Arduino.h>

#include <cstdint>

#include <ESPressio_OTA.hpp>
#include <ESPressio_Platform_IDFOTA.hpp>

namespace {

using Capacity = ESPressio::OTA::ConstrainedV1CapacityProfile;
using ESPressio::Platform::OTA::Status;

/**
 * Arduino IDE counterpart to the ESP-IDF provider-readiness demo.
 *
 * ESP32 Arduino is itself built on ESP-IDF, so the same concrete
 * ESPressio-Platform-ESP-IDF OTA providers can be consumed directly here. The
 * demo remains read-only: it inspects boot/storage facts and preflights an
 * illustrative image size without beginning staging or changing boot state.
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

    void PrintBootFacts() const {
        const auto current = boot_.CurrentBootTarget();
        const auto committed = boot_.CommittedBootTarget();
        const auto next = boot_.NextBootTarget();
        Serial.printf("Current boot target:   0x%08lx\n", static_cast<unsigned long>(current.Value()));
        Serial.printf("Committed boot target: 0x%08lx\n", static_cast<unsigned long>(committed.Value()));
        Serial.printf("Next boot target:      0x%08lx\n", static_cast<unsigned long>(next.Value()));
        Serial.printf("Current boot is trial: %s\n", trial_.IsCurrentBootTrial() ? "yes" : "no");
    }

    void PrintStorageFacts() const {
        ESPressio::Platform::OTA::StorageLayoutInfo info{};
        const auto result = layout_.InspectStorageLayout(info);
        Serial.printf("Storage inspection:    %s (native=%ld)\n",
                      StatusName(result.Code), static_cast<long>(result.NativeCode));
        if (!result) return;
        Serial.printf("Storage layout ID:     0x%016llx\n",
                      static_cast<unsigned long long>(info.Layout.Value()));
        Serial.printf("Partition bytes:       %llu\n", static_cast<unsigned long long>(info.TotalBytes));
        Serial.printf("Inactive OTA capacity: %llu\n", static_cast<unsigned long long>(info.AvailableBytes));
    }

    void DemonstrateSafePreflight() {
        constexpr std::uint64_t illustrativeImageBytes = 256U * 1024U;
        const auto preflight = staging_.PreflightApplicationImage(illustrativeImageBytes);
        Serial.printf("256 KiB stage preflight: %s, required=%llu, maximum=%llu bytes\n",
                      StatusName(preflight.Code),
                      static_cast<unsigned long long>(preflight.RequiredBytes),
                      static_cast<unsigned long long>(preflight.MaximumBytes));
        // Preflight is non-mutating. No BeginApplicationImage() follows.
    }

public:
    void Begin() {
        Serial.begin(115200);
        delay(250);

        static_assert(ESPressio::Platform::OTA::IsApplicationImageStagingProviderV<
                      ESPressio::Platform::IDF::OTAApplicationImageStaging>);
        static_assert(ESPressio::Platform::OTA::IsBootControlProviderV<
                      ESPressio::Platform::IDF::OTABootControl>);
        static_assert(ESPressio::Platform::OTA::IsTrialBootProviderV<
                      ESPressio::Platform::IDF::OTATrialBoot>);
        static_assert(Capacity::MaximumManifestBytes == 8192U);
        static_assert(Capacity::TransferBufferBytes == 4096U);

        Serial.println();
        Serial.println("ESPressio OTA ESP-IDF provider readiness demo");
        Serial.printf("Manifest bound:        %u bytes\n", static_cast<unsigned>(Capacity::MaximumManifestBytes));
        Serial.printf("Transfer workspace:    %u bytes\n", static_cast<unsigned>(Capacity::TransferBufferBytes));
        PrintBootFacts();
        PrintStorageFacts();
        DemonstrateSafePreflight();
        Serial.println("No flash, boot-selection, trial-state or restart mutation was performed.");
    }

    void Poll() noexcept {
        // A complete application would invoke Coordinator::Advance() from its
        // own cooperative scheduling policy. ESPressio-OTA creates no hidden task.
        delay(1000);
    }
};

OTAIDFReadinessDemo demo;

} // namespace

void setup() {
    demo.Begin();
}

void loop() {
    demo.Poll();
}
