# ESPressio OTA V1 Implementation Readiness

**Status:** software implementation/toolchain tranche complete; physical-device recovery evidence outstanding  
**Recorded:** 2026-09-15 (Europe/Prague)  
**Implementation branch:** `ota_v1`  
**Software code anchor:** `9f73b988dd6aa24e58a40f6febb76bad9bad9f10`

This file is the living implementation/readiness handoff for the authorized ESPressio-OTA V1 tranche. It records the implemented state and reproducible validation baseline. It does **not** replace or weaken the authoritative OTA architecture/design document. Where this file and the architecture appear to conflict semantically, preserve the locked architecture and investigate the implementation before changing the contract.

## Current disposition

The authorized P0-P10 software implementation is complete at code anchor `9f73b988dd6aa24e58a40f6febb76bad9bad9f10` for the currently defined V1 scope. Native validation and the complete ESP32/ESP-IDF/Arduino toolchain matrix are green at that exact code anchor.

No further implementation defect is currently known from the automated validation surface. The remaining release-readiness boundary is **physical hardware evidence** for reboot, Trial, commit, rollback and injected power-loss recovery. Cross-compilation is deliberately not represented as evidence that those behaviours have occurred on hardware.

No version-number change, release/tag creation or reintegration to `main` is part of this completion record.

## Exact automated evidence

### Native

- Workflow: `OTA Native Tests`
- Run: `34988412624`
- OTA SHA: `9f73b988dd6aa24e58a40f6febb76bad9bad9f10`
- Result: **GREEN**
- Passed stages include configure, build, constrained object-footprint accounting, retained-memory accounting and the complete native test suite.

### ESP32 / toolchain matrix

- Workflow: `OTA Demo Builds`
- Run: `34988412654`
- OTA SHA: `9f73b988dd6aa24e58a40f6febb76bad9bad9f10`
- Result: **GREEN — 9/9 validation gates passed**

The nine gates are:

1. ESP32 Arduino PlatformIO provider-readiness demo.
2. Pure ESP-IDF PlatformIO provider-readiness demo.
3. Exact ESP32 Arduino IDE provider-readiness sketch source.
4. Exact ESP-IDF-provider Arduino IDE sketch source.
5. ESP32 physical-validation PlatformIO harness.
6. Exact ESP32 physical-validation Arduino IDE sketch source.
7. ESP32 Coordinator-recovery PlatformIO harness.
8. Exact ESP32 Coordinator-recovery Arduino IDE sketch source.
9. Rollback-enabled Arduino-as-ESP-IDF-component Coordinator-recovery bootloader harness, including generated-configuration assertions.

The rollback gate verifies the effective generated configuration includes:

```text
CONFIG_AUTOSTART_ARDUINO=y
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_FREERTOS_HZ=1000
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_COMPILER_CXX_EXCEPTIONS=y
```

and verifies that a non-empty rollback-environment `bootloader.bin` was produced.

## Final implementation hardening

Two late issues were resolved before freezing the software anchor.

### Hybrid C++ exception configuration

Commit `8b642d50c295e6df98049c34c402fdc51cef062f` enables `CONFIG_COMPILER_CXX_EXCEPTIONS=y` in the rollback Arduino+ESP-IDF composition and makes CI assert the generated configuration. ESP-IDF otherwise applies `-fno-exceptions`, while existing ESPressio dependencies including Threads and Units use C++ exception semantics. This is a build/composition requirement and does not alter OTA-domain semantics.

That commit passed the native suite and all nine device/toolchain gates before the subsequent durable-composition hardening.

### Complete bounded durable-key composition

Commit `9f73b988dd6aa24e58a40f6febb76bad9bad9f10` expands the Coordinator recovery laboratory's ESP32 NVS record store from the control key alone to the complete bounded durable key set for the constrained profile:

- `OTAControlStore::RecordKey()`; and
- all `Capacity::MaximumArtifactCheckpoints` authoritative `ArtifactCheckpointStore` keys.

The demo performs a compile-time assertion that every configured key is nonzero and unique. The current zero-Artifact fixture still does not write Artifact checkpoints, but the durable backend no longer requires structural rewiring before an Artifact-bearing transaction can legally use them.

The same commit adds the 1 kHz FreeRTOS generated-configuration assertion to the rollback gate. Native and all nine device/toolchain gates are green at this exact commit.

## Reproducible dependency baseline

Branches are mutable. The following SHAs record the dependency tips present when this completion baseline was captured. A successor should compare current branch tips with these values before interpreting a later failure as a regression in OTA itself.

### OTA-specific prerequisite branches (`ota_v1`)

| Repository | Branch | Captured SHA |
| --- | --- | --- |
| ESPressio-System | `ota_v1` | `691410daa9e0469362be165c5da44a1a22de4212` |
| ESPressio-Platform | `ota_v1` | `3a5194e1b8fb6bf86723f570a7f049debc394515` |
| ESPressio-Security | `ota_v1` | `db85d14abffce41fb78b70523f67aa71947f6828` |
| ESPressio-Platform-ESP-IDF | `ota_v1` | `b06d1bd49495c4467cd0c961ebd722a42a513c8e` |
| ESPressio-ESP32 | `ota_v1` | `ec29eea2e6a8ed7917ab4be88262c7f13038a17c` |
| ESPressio-Persistence | `ota_v1` | `d7636fb254f969a4d1a0f2046583650a7da8cfc3` |
| ESPressio-Serializable | `ota_v1` | `515d54911eac684fc2dc34ac47400b32eba06f38` |

### Primitive-redesign branches used directly by native CI

| Repository | Branch | Captured SHA |
| --- | --- | --- |
| ESPressio-Primitive | `primitives_redesign` | `3fdc769797d4e061ae0be92a0726009d03b97179` |
| ESPressio-Units | `primitives_redesign` | `987b500309b3345368a713d467d789f627590b7a` |
| ESPressio-Observable | `primitives_redesign` | `476fd1dd133d11949ee07ffd16b194451d891d5a` |
| ESPressio-Task | `primitives_redesign` | `c6bd4df69c074c1fcbce7b2d006771aad18c81fc` |
| ESPressio-Threads | `primitives_redesign` | `7d40e526eada8a1fed48327fee28dacbebcfe649` |
| ESPressio-Timing | `primitives_redesign` | `f94e82459ebbca42e74e210e306be0861cc8d73b` |
| ESPressio-State | `primitives_redesign` | `25637a7555e3a03f1d709bd8e37340bc4d545b6e` |
| ESPressio-Event | `primitives_redesign` | `532d04e1200b04b5734b68467018c597ae996223` |
| ESPressio-Command | `primitives_redesign` | `658a9f9064a8ef58a0016d041a29be0c8ca3da7f` |

The native workflow checks these repositories out by branch name, not immutable SHA. The table above is therefore the immutable reconstruction reference for the proven state.

## Implemented V1 boundaries that must not regress

The implementation represented by the software anchor preserves the locked V1 architecture, including:

- OTA-owned semantic contracts and lifecycle orchestration without platform-specific types leaking into portable APIs;
- bounded/fixed-capacity data structures and no hidden OTA worker task;
- one active local mutating transaction in V1;
- canonical OTA State as public current truth and Persistence as private durable recovery truth;
- signed/canonical Manifest semantics and exact-length/digest Artifact verification;
- deterministic UpdatePlan ordering and reverse-order rollback;
- Component lifecycle `Preflight -> Prepare -> Stage -> FinalizeStage -> Activate -> Commit`, with `Rollback`/`Cleanup` as required;
- durable intent before dangerous mutation;
- recovery by reconciliation (`InspectRecoveryState`/Platform facts), not blind replay or guessed progress;
- Coordinator-only restart authority;
- global health + durable commit barrier before Component Commit;
- retained verified Artifact-reader binding for component execution;
- bounded Artifact checkpoint/restart/resume semantics;
- optional Event and Command bridges that do not become lifecycle authority;
- cross-version Trial/rollback durable-schema compatibility rules;
- ESP-IDF/ESP32 concrete image-staging, boot-control, Trial, restart, storage-layout and cryptographic provider composition.

## Physical validation still required

The software tranche must **not** be described as having completed physical power-loss validation. The remaining evidence should be obtained on an appropriate ESP32 target using the checked-in laboratories.

Primary harness:

```text
demos/ota_esp32_coordinator_recovery/
```

Build/flash the rollback-capable environment documented by that lab. Retain device identity, partition layout, bootloader configuration, firmware SHA, complete Serial transcript and `status` output around every injected boundary.

The required physical evidence includes at minimum:

- normal staged -> activation -> restart -> Trial -> health -> CommitIntent -> commit path;
- cancellation/rollback after the activation boundary;
- candidate Trial state backed by the rollback-enabled ESP-IDF bootloader;
- reboot/power-loss recovery around the durable boundaries documented in the lab's power-loss matrix;
- no promotion of candidate generation/security floor before successful commit;
- restoration of previous committed target during rollback;
- explicit failure/recovery-required behaviour for inconsistent durable/platform facts rather than guessed progress.

Do **not** erase the OTA NVS namespace between a power interruption and its recovery observation.

## Successor-agent continuation procedure

If work resumes after this handoff:

1. Read the authoritative OTA architecture/design document in full before changing semantics.
2. Read this file in full.
3. Re-fetch `ESPressio-OTA/ota_v1` and all dependency branch tips before making changes.
4. Treat `9f73b988dd6aa24e58a40f6febb76bad9bad9f10` as the frozen **software code anchor**. A later documentation-only commit may be the branch tip without changing that anchor.
5. Compare any moved dependency branch against the captured SHA table before diagnosing a new build/test failure.
6. Do not duplicate already-complete P0-P10 software work merely because an older architecture handoff records an earlier frontier.
7. Preserve all locked architecture/resource/recovery/trust contracts. If implementation reveals a real semantic conflict, stop only for that architectural conflict; do not silently weaken the contract for convenience.
8. Keep version numbers, tags/releases and `main` reintegration untouched unless separately authorized.
9. Distinguish automated compile/test evidence from physical-device evidence in every status report.

## Completion statement

At software anchor `9f73b988dd6aa24e58a40f6febb76bad9bad9f10`, the ESPressio-OTA V1 P0-P10 software implementation and the defined native/ESP32 toolchain validation surface are complete and green. The remaining readiness activity is physical hardware validation and evidence capture; it is not currently an identified software implementation gap.
