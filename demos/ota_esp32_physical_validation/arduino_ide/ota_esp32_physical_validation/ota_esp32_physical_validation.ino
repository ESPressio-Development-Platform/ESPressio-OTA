#include <Arduino.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <ESPressio_Platform_IDFOTA.hpp>

namespace {

using ESPressio::Platform::OTA::BootTargetIdentifier;
using ESPressio::Platform::OTA::RestartReason;
using ESPressio::Platform::OTA::Status;

/**
 * Operator-driven physical OTA validation harness.
 *
 * IMPORTANT: This demo is intentionally destructive only after explicit serial
 * commands ending in " NOW". Startup is read-only. The staged candidate is a
 * byte-for-byte copy of the currently running application partition, written
 * through ESPressio's real ESP-IDF ApplicationImageStaging provider. This keeps
 * the lab self-contained while still exercising real OTA flash/boot mechanics.
 */
class OTAPhysicalValidationHarness final {
    static constexpr std::size_t CopyChunkBytes = 4096U;
    static constexpr std::uint64_t ProgressReportInterval = 64U * 1024U;

    ESPressio::Platform::IDF::OTAApplicationImageStaging staging_{};
    ESPressio::Platform::IDF::OTABootControl boot_{};
    ESPressio::Platform::IDF::OTATrialBoot trial_{};
    ESPressio::Platform::IDF::OTASystemRestart restart_{};
    ESPressio::Platform::IDF::OTAStorageLayoutInspection layout_{};

    std::array<std::uint8_t, CopyChunkBytes> copyBuffer_{};
    const esp_partition_t* cloneSource_{nullptr};
    std::uint64_t cloneBytes_{0U};
    std::uint64_t cloneOffset_{0U};
    std::uint64_t nextProgressReport_{0U};
    BootTargetIdentifier stagedTarget_{};
    bool cloneActive_{false};
    bool stagedThisBoot_{false};
    bool armedThisBoot_{false};

    std::array<char, 64U> commandBuffer_{};
    std::size_t commandLength_{0U};

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

    static void PrintPartition(const char* heading, const esp_partition_t* partition) {
        if (partition == nullptr) {
            Serial.printf("[OTA-LAB] %-20s unavailable\n", heading);
            return;
        }
        Serial.printf("[OTA-LAB] %-20s %-16s @ 0x%08lx, %lu bytes\n",
                      heading,
                      partition->label,
                      static_cast<unsigned long>(partition->address),
                      static_cast<unsigned long>(partition->size));
    }

    static void PrintResult(const char* operation, ESPressio::Platform::OTA::Result result) {
        Serial.printf("[OTA-LAB] %-20s %s (native=%ld)\n",
                      operation,
                      StatusName(result.Code),
                      static_cast<long>(result.NativeCode));
    }

    void PrintHelp() const {
        Serial.println();
        Serial.println("[OTA-LAB] Commands (mutating commands require the literal suffix ' NOW'):");
        Serial.println("[OTA-LAB]   help          - show this command list");
        Serial.println("[OTA-LAB]   status        - print boot, partition and RAM/flash evidence");
        Serial.println("[OTA-LAB]   clone NOW     - cooperatively clone the running app into the inactive OTA slot");
        Serial.println("[OTA-LAB]   abort NOW     - abort an in-progress clone without changing boot selection");
        Serial.println("[OTA-LAB]   arm NOW       - select the successfully staged clone as next boot target");
        Serial.println("[OTA-LAB]   reboot NOW    - restart into the armed candidate");
        Serial.println("[OTA-LAB]   commit NOW    - mark the current trial boot valid (when rollback is enabled)");
        Serial.println("[OTA-LAB]   rollback NOW  - mark the current trial boot invalid, then restart");
        Serial.println();
        Serial.println("[OTA-LAB] Suggested power-cut points: during clone, after clone, after arm, during trial,");
        Serial.println("[OTA-LAB] and immediately after the commit/rollback marker has been issued.");
    }

    void PrintStatus() const {
        Serial.println();
        Serial.println("[OTA-LAB] ===== Physical OTA status =====");

        PrintPartition("running partition", esp_ota_get_running_partition());
        PrintPartition("configured boot", esp_ota_get_boot_partition());
        PrintPartition("next update slot", esp_ota_get_next_update_partition(nullptr));

        const auto current = boot_.CurrentBootTarget();
        const auto committed = boot_.CommittedBootTarget();
        const auto next = boot_.NextBootTarget();
        Serial.printf("[OTA-LAB] current target        0x%08lx\n",
                      static_cast<unsigned long>(current.Value()));
        Serial.printf("[OTA-LAB] provider committed    0x%08lx\n",
                      static_cast<unsigned long>(committed.Value()));
        Serial.printf("[OTA-LAB] next boot target      0x%08lx\n",
                      static_cast<unsigned long>(next.Value()));
        Serial.printf("[OTA-LAB] current boot trial    %s\n",
                      trial_.IsCurrentBootTrial() ? "YES" : "no");

        ESPressio::Platform::OTA::StorageLayoutInfo storage{};
        const auto storageResult = layout_.InspectStorageLayout(storage);
        Serial.printf("[OTA-LAB] storage inspection    %s (native=%ld)\n",
                      StatusName(storageResult.Code),
                      static_cast<long>(storageResult.NativeCode));
        if (storageResult) {
            Serial.printf("[OTA-LAB] storage layout id     0x%016llx\n",
                          static_cast<unsigned long long>(storage.Layout.Value()));
            Serial.printf("[OTA-LAB] inactive OTA bytes    %llu\n",
                          static_cast<unsigned long long>(storage.AvailableBytes));
        }

        Serial.printf("[OTA-LAB] heap total/free/min   %lu / %lu / %lu bytes\n",
                      static_cast<unsigned long>(ESP.getHeapSize()),
                      static_cast<unsigned long>(ESP.getFreeHeap()),
                      static_cast<unsigned long>(ESP.getMinFreeHeap()));
        Serial.printf("[OTA-LAB] sketch used/free      %lu / %lu bytes\n",
                      static_cast<unsigned long>(ESP.getSketchSize()),
                      static_cast<unsigned long>(ESP.getFreeSketchSpace()));
        Serial.printf("[OTA-LAB] flash chip size       %lu bytes\n",
                      static_cast<unsigned long>(ESP.getFlashChipSize()));

        Serial.printf("[OTA-LAB] clone active          %s\n", cloneActive_ ? "YES" : "no");
        if (cloneActive_) {
            Serial.printf("[OTA-LAB] clone progress        %llu / %llu bytes\n",
                          static_cast<unsigned long long>(cloneOffset_),
                          static_cast<unsigned long long>(cloneBytes_));
        }
        Serial.printf("[OTA-LAB] staged this boot      %s\n", stagedThisBoot_ ? "YES" : "no");
        Serial.printf("[OTA-LAB] armed this boot       %s\n", armedThisBoot_ ? "YES" : "no");
        Serial.println("[OTA-LAB] ===============================");
    }

    void ResetCloneState() noexcept {
        cloneSource_ = nullptr;
        cloneBytes_ = 0U;
        cloneOffset_ = 0U;
        nextProgressReport_ = 0U;
        cloneActive_ = false;
    }

    void FailClone(const char* reason) {
        Serial.printf("[OTA-LAB] clone FAILED: %s\n", reason);
        const auto abortResult = staging_.AbortApplicationImage();
        PrintResult("stage abort", abortResult);
        stagedTarget_ = {};
        stagedThisBoot_ = false;
        armedThisBoot_ = false;
        ResetCloneState();
    }

    void BeginClone() {
        if (cloneActive_) {
            Serial.println("[OTA-LAB] clone already active; use 'abort NOW' first.");
            return;
        }
        if (trial_.IsCurrentBootTrial()) {
            Serial.println("[OTA-LAB] refusing to stage while the current boot is a trial; commit or rollback first.");
            return;
        }

        const auto* running = esp_ota_get_running_partition();
        const auto* candidate = esp_ota_get_next_update_partition(nullptr);
        if (running == nullptr || candidate == nullptr || running == candidate) {
            Serial.println("[OTA-LAB] no distinct inactive OTA application partition is available.");
            Serial.println("[OTA-LAB] Use an OTA-capable dual-slot partition table (PlatformIO uses min_spiffs.csv here).");
            return;
        }

        const auto exactBytes = static_cast<std::uint64_t>(running->size);
        const auto preflight = staging_.PreflightApplicationImage(exactBytes);
        Serial.printf("[OTA-LAB] clone preflight       %s, required=%llu, maximum=%llu, native=%ld\n",
                      StatusName(preflight.Code),
                      static_cast<unsigned long long>(preflight.RequiredBytes),
                      static_cast<unsigned long long>(preflight.MaximumBytes),
                      static_cast<long>(preflight.NativeCode));
        if (!preflight) return;

        const auto begin = staging_.BeginApplicationImage(exactBytes);
        PrintResult("stage begin", begin);
        if (!begin) return;

        cloneSource_ = running;
        cloneBytes_ = exactBytes;
        cloneOffset_ = 0U;
        nextProgressReport_ = ProgressReportInterval;
        stagedTarget_ = staging_.ApplicationImageTarget();
        cloneActive_ = true;
        stagedThisBoot_ = false;
        armedThisBoot_ = false;

        Serial.printf("[OTA-LAB] cloning %llu bytes from 0x%08lx to 0x%08lx in %u-byte cooperative chunks.\n",
                      static_cast<unsigned long long>(cloneBytes_),
                      static_cast<unsigned long>(running->address),
                      static_cast<unsigned long>(stagedTarget_.Value()),
                      static_cast<unsigned>(CopyChunkBytes));
        Serial.println("[OTA-LAB] Cutting power now or during clone must leave the current boot target unchanged.");
    }

    void AdvanceCloneOneChunk() {
        if (!cloneActive_ || cloneSource_ == nullptr) return;

        const auto remaining = cloneBytes_ - cloneOffset_;
        const auto chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(copyBuffer_.size())));
        if (chunk == 0U) {
            FailClone("internal zero-length copy chunk");
            return;
        }

        const auto readResult = esp_partition_read(
            cloneSource_,
            static_cast<std::size_t>(cloneOffset_),
            copyBuffer_.data(),
            chunk);
        if (readResult != ESP_OK) {
            FailClone("esp_partition_read failed");
            return;
        }

        const auto write = staging_.WriteApplicationImage(copyBuffer_.data(), chunk);
        if (!write || write.ConsumedBytes != chunk) {
            FailClone("ESPressio staging write failed or consumed a partial chunk");
            return;
        }

        cloneOffset_ += static_cast<std::uint64_t>(write.ConsumedBytes);
        if (cloneOffset_ >= nextProgressReport_ || cloneOffset_ == cloneBytes_) {
            Serial.printf("[OTA-LAB] clone progress        %llu / %llu bytes\n",
                          static_cast<unsigned long long>(cloneOffset_),
                          static_cast<unsigned long long>(cloneBytes_));
            while (nextProgressReport_ <= cloneOffset_) {
                nextProgressReport_ += ProgressReportInterval;
            }
        }

        if (cloneOffset_ != cloneBytes_) return;

        const auto finalize = staging_.FinalizeApplicationImage();
        PrintResult("stage finalize", finalize);
        if (!finalize) {
            stagedTarget_ = {};
            stagedThisBoot_ = false;
            armedThisBoot_ = false;
            ResetCloneState();
            return;
        }

        stagedTarget_ = staging_.ApplicationImageTarget();
        stagedThisBoot_ = bool(stagedTarget_);
        armedThisBoot_ = false;
        ResetCloneState();

        Serial.printf("[OTA-LAB] STAGED target 0x%08lx. Boot selection is still unchanged.\n",
                      static_cast<unsigned long>(stagedTarget_.Value()));
        Serial.println("[OTA-LAB] Use 'arm NOW' only after capturing this successful staging evidence.");
    }

    void AbortClone() {
        if (!cloneActive_) {
            Serial.println("[OTA-LAB] no clone is active.");
            return;
        }
        const auto result = staging_.AbortApplicationImage();
        PrintResult("stage abort", result);
        stagedTarget_ = {};
        stagedThisBoot_ = false;
        armedThisBoot_ = false;
        ResetCloneState();
    }

    void ArmCandidate() {
        if (cloneActive_) {
            Serial.println("[OTA-LAB] clone is still active; wait for finalize or abort it.");
            return;
        }
        if (!stagedThisBoot_ || !stagedTarget_) {
            Serial.println("[OTA-LAB] no successfully staged candidate from this boot; run 'clone NOW' first.");
            return;
        }

        const auto result = boot_.SelectNextBootTarget(stagedTarget_);
        PrintResult("select boot target", result);
        if (!result) return;

        armedThisBoot_ = true;
        Serial.printf("[OTA-LAB] ARMED target 0x%08lx. A reset/power loss from this point may boot the candidate.\n",
                      static_cast<unsigned long>(stagedTarget_.Value()));
        Serial.println("[OTA-LAB] Use 'reboot NOW' for a controlled activation restart.");
    }

    void RebootCandidate() {
        if (!armedThisBoot_) {
            Serial.println("[OTA-LAB] refusing controlled activation restart: no candidate was armed in this boot.");
            return;
        }
        Serial.println("[OTA-LAB] restarting for candidate activation...");
        Serial.flush();
        delay(50);
        const auto result = restart_.Restart(RestartReason::ActivateCandidate);
        PrintResult("activation restart", result); // normally unreachable on hardware
    }

    void CommitTrial() {
        if (!trial_.IsCurrentBootTrial()) {
            Serial.println("[OTA-LAB] current boot is not reported as a trial.");
            Serial.println("[OTA-LAB] If candidate boot succeeded but this remains false, verify bootloader rollback support.");
            return;
        }
        const auto result = trial_.MarkCurrentBootValid();
        PrintResult("mark trial valid", result);
        if (result) {
            Serial.println("[OTA-LAB] COMMIT marker accepted. Capture status, then reset to confirm the candidate remains selected.");
        }
    }

    void RollbackTrial() {
        if (!trial_.IsCurrentBootTrial()) {
            Serial.println("[OTA-LAB] current boot is not reported as a trial; rollback marker was not issued.");
            return;
        }

        const auto invalid = trial_.MarkCurrentBootInvalid();
        PrintResult("mark trial invalid", invalid);
        if (!invalid) return;

        Serial.println("[OTA-LAB] ROLLBACK marker accepted; restarting through the separate SystemRestart provider...");
        Serial.flush();
        delay(50);
        const auto restart = restart_.Restart(RestartReason::Rollback);
        PrintResult("rollback restart", restart); // normally unreachable on hardware
    }

    void HandleCommand(const char* command) {
        if (std::strcmp(command, "help") == 0) {
            PrintHelp();
        } else if (std::strcmp(command, "status") == 0) {
            PrintStatus();
        } else if (std::strcmp(command, "clone NOW") == 0) {
            BeginClone();
        } else if (std::strcmp(command, "abort NOW") == 0) {
            AbortClone();
        } else if (std::strcmp(command, "arm NOW") == 0) {
            ArmCandidate();
        } else if (std::strcmp(command, "reboot NOW") == 0) {
            RebootCandidate();
        } else if (std::strcmp(command, "commit NOW") == 0) {
            CommitTrial();
        } else if (std::strcmp(command, "rollback NOW") == 0) {
            RollbackTrial();
        } else {
            Serial.printf("[OTA-LAB] unknown command: '%s' (type 'help')\n", command);
        }
    }

    void PollSerial() {
        while (Serial.available() > 0) {
            const int raw = Serial.read();
            if (raw < 0) return;
            const char value = static_cast<char>(raw);

            if (value == '\r') continue;
            if (value == '\n') {
                if (commandLength_ != 0U) {
                    commandBuffer_[commandLength_] = '\0';
                    HandleCommand(commandBuffer_.data());
                    commandLength_ = 0U;
                }
                continue;
            }

            if (commandLength_ + 1U >= commandBuffer_.size()) {
                commandLength_ = 0U;
                Serial.println("[OTA-LAB] command discarded: line exceeds fixed 63-byte command buffer.");
                continue;
            }
            commandBuffer_[commandLength_++] = value;
        }
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
        static_assert(ESPressio::Platform::OTA::IsSystemRestartProviderV<
                      ESPressio::Platform::IDF::OTASystemRestart>);

        Serial.println();
        Serial.println("[OTA-LAB] ESPressio OTA ESP32 physical validation harness");
        Serial.println("[OTA-LAB] Startup is READ-ONLY. Nothing is staged or boot-selected automatically.");
        Serial.println("[OTA-LAB] Mutating commands require the exact literal suffix ' NOW'.");
        PrintStatus();
        PrintHelp();

        if (trial_.IsCurrentBootTrial()) {
            Serial.println("[OTA-LAB] IMPORTANT: trial boot detected. Use 'commit NOW' or 'rollback NOW' deliberately.");
        }
    }

    void Poll() {
        PollSerial();
        // One flash chunk per loop keeps staging cooperative and creates a
        // reproducible power-cut window instead of hiding the whole copy in setup().
        AdvanceCloneOneChunk();
        delay(1);
    }
};

OTAPhysicalValidationHarness harness;

} // namespace

void setup() {
    harness.Begin();
}

void loop() {
    harness.Poll();
}
