#include <Arduino.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include <ESPressio_ESP32.hpp>
#include <ESPressio_OTA.hpp>

namespace {

using Capacity = ESPressio::ESP32Platform::ESP32OTACapacityProfile;
using ESPressio::Platform::OTA::Status;

/**
 * Read-only ESP32 OTA composition demonstration.
 *
 * The demo deliberately performs no image write, boot-target mutation, trial
 * acceptance or restart. It shows consuming developers how the concrete
 * ESP32/ESP-IDF providers satisfy portable OTA contracts and how to perform
 * safe inspection/preflight before an application constructs its Coordinator.
 */
class OTAProviderReadinessDemo final {
    ESPressio::ESP32Platform::OTAApplicationImageStaging staging_{};
    ESPressio::ESP32Platform::OTABootControl boot_{};
    ESPressio::ESP32Platform::OTATrialBoot trial_{};
    ESPressio::ESP32Platform::OTAStorageLayoutInspection layout_{};
    ESPressio::ESP32Platform::OTASHA256 sha256_{};

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
        Serial.printf("Partition bytes:       %llu\n",
                      static_cast<unsigned long long>(info.TotalBytes));
        Serial.printf("Inactive OTA capacity: %llu\n",
                      static_cast<unsigned long long>(info.AvailableBytes));
    }

    void DemonstrateSafePreflight() {
        constexpr std::uint64_t illustrativeImageBytes = 256U * 1024U;
        const auto preflight = staging_.PreflightApplicationImage(illustrativeImageBytes);
        Serial.printf("256 KiB stage preflight: %s, maximum=%llu bytes\n",
                      StatusName(preflight.Code),
                      static_cast<unsigned long long>(preflight.AvailableBytes));
        // No BeginApplicationImage() call follows. Preflight is non-mutating.
    }

    void DemonstrateSecurityProvider() {
        constexpr std::array<std::uint8_t, 13> sample{{
            'E','S','P','r','e','s','s','i','o','-','O','T','A'}};
        std::array<std::uint8_t, 32> digest{};
        std::size_t written = 0U;

        auto result = sha256_.Begin(ESPressio::Security::DigestAlgorithm::SHA256);
        if (result) result = sha256_.Update({sample.data(), sample.size()});
        if (result) result = sha256_.Finalize({digest.data(), digest.size()}, written);

        Serial.printf("PSA SHA-256 provider:  %s (%u bytes)\n",
                      result ? "Success" : "Failed", static_cast<unsigned>(written));
        if (result && written >= 4U) {
            Serial.printf("Digest prefix:         %02x%02x%02x%02x...\n",
                          digest[0], digest[1], digest[2], digest[3]);
        }
    }

public:
    void Begin() {
        Serial.begin(115200);
        delay(250);

        static_assert(ESPressio::Platform::OTA::IsApplicationImageStagingProviderV<
                      ESPressio::ESP32Platform::OTAApplicationImageStaging>);
        static_assert(ESPressio::Platform::OTA::IsBootControlProviderV<
                      ESPressio::ESP32Platform::OTABootControl>);
        static_assert(ESPressio::Platform::OTA::IsTrialBootProviderV<
                      ESPressio::ESP32Platform::OTATrialBoot>);
        static_assert(Capacity::TransferBufferBytes == 4096U);

        Serial.println();
        Serial.println("ESPressio OTA ESP32 provider readiness demo");
        Serial.printf("Manifest bound:        %u bytes\n", static_cast<unsigned>(Capacity::MaximumManifestBytes));
        Serial.printf("Transfer workspace:    %u bytes\n", static_cast<unsigned>(Capacity::TransferBufferBytes));
        PrintBootFacts();
        PrintStorageFacts();
        DemonstrateSafePreflight();
        DemonstrateSecurityProvider();
        Serial.println("No flash, boot-selection, trial-state or restart mutation was performed.");
    }

    void Poll() noexcept {
        // OTA is cooperative/caller-driven. A real application would invoke
        // Coordinator::Advance() from its own scheduling policy here.
        delay(1000);
    }
};

OTAProviderReadinessDemo demo;

} // namespace

void setup() {
    demo.Begin();
}

void loop() {
    demo.Poll();
}
