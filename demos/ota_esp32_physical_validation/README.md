# ESP32 physical OTA validation harness

This demo is the deliberately **operator-driven / destructive** companion to the read-only provider-readiness demos.

Its purpose is to make the remaining P10 hardware evidence reproducible on a real ESP32 without requiring a network server or a second firmware artifact. The harness reads the currently running application partition and stages a byte-for-byte clone into the inactive OTA slot through `ESPressio::Platform::IDF::OTAApplicationImageStaging`. Boot selection, restart, trial acceptance and rollback are then separate explicit commands.

## Safety model

Startup is read-only. The demo never writes flash, changes the boot target, marks a trial valid/invalid or restarts unless the operator enters a mutating command with the exact suffix ` NOW`.

Use a board with a recoverable serial flashing path. This is a validation harness, not an application example to deploy unattended.

## Required partition layout

Two OTA application slots are required.

The PlatformIO variant pins `min_spiffs.csv`, which provides two application slots on the normal 4 MiB `esp32dev` target. For Arduino IDE, select an equivalent **OTA-capable dual-slot partition scheme** (for example the board menu option commonly named `Minimal SPIFFS (Large APPS with OTA)`). If the board/core exposes a differently named scheme, choose one containing both `ota_0` and `ota_1`.

A single-slot / `huge_app` layout is intentionally rejected by the harness.

## Commands

At 115200 baud:

```text
help
status
clone NOW
abort NOW
arm NOW
reboot NOW
commit NOW
rollback NOW
```

The normal positive path is:

```text
status
clone NOW
# wait for STAGED
arm NOW
reboot NOW
# after reboot, status should show the candidate as current
# with bootloader rollback enabled, current boot should report trial
commit NOW
status
```

The rollback path is:

```text
clone NOW
arm NOW
reboot NOW
rollback NOW
# device restarts through the separate SystemRestart provider
status
```

## Power-loss matrix

Capture the serial transcript before and after each case.

| Injection point | Expected safety property |
| --- | --- |
| During `clone NOW` | Current boot selection remains unchanged; partially written inactive slot is not activated. |
| After `STAGED`, before `arm NOW` | Current boot remains unchanged because staging/finalization is not activation. |
| After `arm NOW`, before controlled reboot | A reset/power cut may boot the selected candidate; this deliberately exercises the activation boundary. |
| During candidate trial | Bootloader/platform trial facts must be observable after restart when rollback support is enabled. |
| Immediately after `commit NOW` | Subsequent restart must keep the candidate selected/valid. |
| Immediately after the rollback marker | Subsequent restart must return to the previous bootable slot. |

The harness stages one 4096-byte chunk per `loop()` iteration specifically to create a large, reproducible cut window while retaining caller-driven/cooperative behaviour.

## Evidence emitted by `status`

`status` prints:

- running, configured-boot and next-update partitions;
- portable `BootTargetIdentifier` values;
- trial-boot observation;
- storage-layout identity and inactive-slot capacity;
- heap total/free/minimum-free;
- sketch used/free space;
- flash-chip size;
- clone/stage/arm state for the current boot.

These measurements are useful target-specific evidence, but they do **not** replace whole-application production memory accounting.

## Rollback configuration note

Trial detection and `commit NOW` / `rollback NOW` depend on ESP-IDF bootloader rollback support. If a candidate boots successfully but `status` reports `current boot trial: no`, validate the bootloader configuration for the exact board/core build before interpreting that run as a trial/rollback result.

## Scope of what this proves

A successful physical run validates the concrete ESP-IDF Platform mechanics used by ESPressio on the target: application-image staging, boot-target selection, restart separation, trial marking/rollback where enabled, and real RAM/flash observations.

It does **not by itself** prove the higher-level Coordinator's durable-intent/checkpoint recovery across every power-cut boundary. Coordinator/Persistence power-loss evidence must be captured with the full OTA composition and its real durable Store. Compilation of this demo is never represented as physical execution evidence.
