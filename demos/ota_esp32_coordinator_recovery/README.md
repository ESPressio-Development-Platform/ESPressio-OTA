# ESP32 Coordinator Recovery Physical Lab

This laboratory exercises the **real ESPressio-OTA Coordinator durable state machine** on an ESP32 while keeping every dangerous transition operator-controlled.

It complements `ota_esp32_physical_validation`: that earlier harness validates concrete image-staging/boot providers directly. This harness validates Coordinator ownership of durable intent, boot selection, trial reconciliation, commit, rollback and reboot/power-loss recovery.

## Scope and trust boundary

The lab deliberately uses one stateless zero-Artifact Component. The inactive application image is prepared separately by cloning the currently-running valid image through `OTAApplicationImageStaging`. The Component itself owns no external mutable resource, so its recovery state is truthfully and permanently `Staged`; its Activate/Commit/Rollback hooks are idempotent no-ops.

The Manifest is a fixed build-time laboratory fixture and is supplied directly to `BindVerifiedManifest()`. That is intentional isolation of the Coordinator/recovery path. **A successful run is not evidence that Manifest signature/trust verification was physically exercised.** Security/trust validation remains covered by its dedicated tests/providers.

## Critical bootloader requirement

Physical Trial/rollback validation requires the **flashed ESP-IDF bootloader** to be built with:

```text
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

Defining that symbol only for application compilation is not sufficient. The harness detects the application-side configuration and prints a warning when rollback support is absent, but the decisive evidence is actual bootloader behaviour: after candidate selection/reboot, the running candidate must report the ESP-IDF `PENDING_VERIFY` trial state.

The PlatformIO project therefore has two deliberately different environments:

```text
esp32dev           Arduino/Arduino-IDE-equivalent compile path; uses the prebuilt Arduino bootloader
esp32dev-rollback  Arduino as an ESP-IDF component; rebuilds bootloader from sdkconfig.defaults
```

The checked-in `sdkconfig.defaults` contains both:

```text
CONFIG_AUTOSTART_ARDUINO=y
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

Build or flash the physical rollback laboratory with:

```bash
pio run -e esp32dev-rollback --project-dir demos/ota_esp32_coordinator_recovery/platformio
pio run -e esp32dev-rollback -t upload --project-dir demos/ota_esp32_coordinator_recovery/platformio
```

CI additionally checks the merged environment-specific sdkconfig and requires a non-empty rollback-environment `bootloader.bin`; this protects against accidentally compiling only the application while ignoring the bootloader setting.

CI compilation therefore proves source/toolchain/configuration integration only. It must never be recorded as physical Trial/power-loss evidence.

## Safety model

Startup does not select a boot target, stage an image, mark a trial valid/invalid, restart the device or begin an OTA transaction. The only first-use mutation is provisioning the dedicated `ota_coord_lab` NVS control record if it does not yet exist.

All operator mutations require the literal ` NOW` suffix:

```text
clone NOW
abort NOW
start NOW
step NOW
activate NOW
cancel NOW
```

`status` and `help` are read-only.

The durable backend is the ESP32 `NVSAtomicRecordStore` bound only to `OTAControlStore::RecordKey()`. This zero-Artifact lab never invokes Artifact checkpoint persistence. A subsequent Artifact-bearing laboratory will register the authoritative checkpoint record keys as well.

## Normal commit path

1. Flash the harness with the `esp32dev-rollback` environment and the dual-OTA `min_spiffs.csv` partition layout.
2. Open Serial at 115200 baud and run `status`.
3. Run `clone NOW` and allow the cooperative clone to reach `complete`.
4. Run `start NOW`. This creates the durable transaction and binds the fixed lab Manifest/profile.
5. Run `step NOW` repeatedly, checking `status` between steps, until the durable recovery point is `Staged`.
6. Run `activate NOW`. This persists `ActivationArmed`; it does **not** itself select/reboot.
7. Run `step NOW` exactly once at a time. One step executes the Component activation hook, a later step selects the candidate boot target, and a subsequent step requests restart.
8. After reboot, inspect `status`. The candidate must be reported as a Trial runtime.
9. Run `step NOW` once to evaluate the empty required-health set and persist `CommitIntent`.
10. Inspect `status`, then run `step NOW` again to mark the trial valid, execute Component Commit and atomically promote the committed baseline.
11. Verify that no active transaction remains and committed generation advanced.

## Rollback path

Drive through activation/reboot until the candidate is in Trial, but **before** the health/commit step run:

```text
cancel NOW
```

At/after the activation boundary this requests durable rollback rather than abandoning the transaction. Continue with one `step NOW` at a time. The Coordinator must restore/select the previous committed target, request restart as required, and finally clear the active transaction without promoting candidate generation/security.

## Power-loss matrix

For each case, remove power only after the preceding command has printed its completed result, reboot, run `status`, then continue. Capture the Serial transcript for every case.

| Boundary | Expected recovery invariant |
| --- | --- |
| during `clone NOW` | no OTA transaction exists; active slot remains unchanged; clone can restart from zero |
| after `start NOW` / ManifestAccepted | transaction reconstructs; Manifest can be rebound; no boot mutation occurred |
| after ArtifactsAcquired/ArtifactsVerified | zero-Artifact phases reconstruct deterministically |
| after `StagingStarted` | stateless Component inspection reconciles safely to Staged |
| after `Staged` | candidate is not selected until `activate NOW` |
| immediately after `activate NOW` | `ActivationArmed` is durable while previous image is still running |
| after candidate boot target selection but before restart | reboot/recovery sees durable ActivationArmed and platform selection consistently |
| immediately after candidate reboot | current target must equal durable candidate and be ESP-IDF Trial/PENDING_VERIFY |
| after Trial entered but before health evaluation | previous committed baseline/security floor remain authoritative |
| immediately after `CommitIntent` | reboot must resume commit; candidate is not silently demoted or re-planned |
| during rollback intent | previous committed baseline/security floor remain authoritative |
| after previous target selected but before rollback restart | reboot/recovery must continue rollback, not commit candidate |

Unexpected target identity, missing Trial state where required, a torn/corrupt NVS control frame, or a mismatch between durable intent and Platform facts must lead to explicit failure/recovery-required behaviour rather than guessed progress.

## Evidence to retain

For release-readiness evidence, retain the device/board identity, flash/partition layout, bootloader configuration, firmware commit SHA, full Serial transcript and the `status` output before/after every injected power cut. `status` also reports heap/free/min-free heap, sketch size/free sketch space, total flash, running/boot/next targets and current Coordinator durable state.

Do not erase the device/NVS namespace between a power cut and the recovery observation. Erasing durable state invalidates the test.
