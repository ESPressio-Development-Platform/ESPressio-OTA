# ESPressio-OTA

`ESPressio-OTA` is the OTA-domain library for the ESPressio Development Platform. It owns portable OTA semantics, lifecycle orchestration, signed release metadata, bounded update planning, crash-consistent recovery, canonical OTA State, policy and health integration, and optional Event/Command projections.

This `ota_v1` branch is the active implementation line for the V1 architecture. It is not a release tag and this document does not change any ESPressio library version.

## Design goals

ESPressio-OTA is designed for constrained embedded targets without making the OTA domain depend on one MCU, framework, transport, storage backend, or bootloader implementation.

The core rules are:

- OTA owns OTA-domain contracts and behaviour.
- Platform, Security, Persistence, State and other domains own the lower-level contracts OTA consumes.
- concrete ESP-IDF/Arduino mechanisms do not leak into portable OTA APIs;
- all V1 resource growth is explicitly bounded;
- the Coordinator is cooperative and caller-driven: OTA creates no hidden worker task;
- there is one active local mutating OTA transaction in V1;
- State is canonical public current truth;
- Persistence is private durable recovery truth;
- Event is an optional projection of meaningful occurrences, not lifecycle authority;
- Command is an optional control adapter, not lifecycle authority;
- dangerous platform mutation is preceded by durable intent;
- recovery reconciles durable truth with actual Platform and ComponentHandler facts rather than guessing.

## Namespace

Portable OTA APIs are under:

```cpp
ESPressio::OTA
```

Concrete ESP32 providers supplied by the ESPressio ESP32 integration are under:

```cpp
ESPressio::ESP32Platform
```

## Architectural ownership

The portable dependency direction is intentionally one-way:

```text
Application
    |
    v
ESPressio-OTA
    |-- ESPressio-System          generic system identity/runtime contracts
    |-- ESPressio-Platform        portable platform OTA capabilities
    |-- ESPressio-Security        digest, signature and trust semantics
    |-- ESPressio-Persistence     atomic durable record contract
    |-- ESPressio-Serializable    bounded/canonical wire encoding
    |-- ESPressio-State           canonical current OTA truth
    `-- supporting Primitive / Observable / Timing contracts

optional integrations:
    ESPressio-Event
    ESPressio-Command

concrete implementations:
    ESPressio-Platform-ESP-IDF
    ESPressio-ESP32
```

An application using OTA depends on semantic contract owners. It should not make portable OTA code depend directly on ESP-IDF or Arduino merely because those frameworks supply the concrete implementation on an ESP32 target.

## Core model

### Signed Manifest

The V1 Manifest is a bounded immutable release-authoritative object. Its canonical signed representation is DirectBinary; JSON/CBOR projections are not alternative signed canonical encodings.

The signed content includes, among other data:

- `ManifestIdentifier` and `ReleaseIdentifier`;
- `SecurityGeneration`;
- bounded target clauses;
- bounded Components and Artifacts;
- dependency edges;
- required health conditions;
- required OTA protocol/features;
- signature-selection metadata;
- signed candidate runtime Manifest/durable/protocol compatibility declarations.

Manifest authenticity is established before its semantics are authoritative. Artifact payloads are then accepted only when exact length and cryptographic digest match the trusted Manifest declaration. A partially verified payload can never become activatable.

### Update target profile

`UpdateTargetProfile<TCapacityProfile>` captures a frozen, bounded description of the current device/runtime compatibility surface. Planning and policy consume the frozen profile; the semantic plan is not silently rebuilt around changing provider order or object addresses.

Compatibility Claim Tokens are advisory negative filters only. Authoritative applicability/admission still requires the trusted Manifest and local target facts.

### Component handlers and deterministic plan

A `ComponentHandler<TComponent>` participates in the OTA-owned lifecycle:

```text
Preflight
Prepare
Stage
FinalizeStage
Activate
Commit
Rollback
Cleanup
```

Handlers do not own release discovery, Artifact transport, acquisition retry/failover, or Manifest trust verification. OTA supplies verified Artifact reader semantics.

`UpdatePlan` is fixed-capacity and deterministic. Stage, Activate and Commit follow topological dependency order; Rollback follows reverse topological order. Stable semantic identifiers are used for deterministic tie breaking.

### Coordinator

`Coordinator` is the authoritative OTA lifecycle orchestrator. It is advanced cooperatively by the application:

```cpp
const auto result = coordinator.Advance();
```

No hidden OTA task is required. Each call performs bounded work and may return `Pending` or `Deferred` when more caller-driven progress is needed.

Only the Coordinator requests system restart. A ComponentHandler can report that restart is required, but does not reboot the system itself.

## Durable recovery and power loss

OTA uses `Persistence::IAtomicRecordStore` rather than owning a private storage backend.

The V1 durable control record contains bounded semantic recovery facts including:

- explicit `OTADurableSchemaVersion`;
- next transaction/generation counters;
- security anti-downgrade floor;
- committed baseline;
- optional active transaction;
- durable recovery point;
- durable intent.

It deliberately does **not** persist provider pointers, runtime session handles, high-frequency progress or opaque cryptographic internal state.

Critical ordering is:

```text
persist durable intent/truth
THEN mutate or expose corresponding runtime/platform truth
```

The V1 committed-baseline promotion, security-floor advancement and active-transaction clearing occur in one atomic control-record replacement. Therefore a crash at that boundary reconstructs either the complete old CommitIntent state or the complete new committed state; OTA never exposes an intermediate baseline/floor combination.

Artifact checkpoints are separately bounded and atomic. When safe resumability cannot be proven, V1 may restart acquisition from offset zero rather than inventing unsafe partial-resume semantics.

## Trial, commit and rollback

Activation is intentionally distinct from staging and commit.

A typical application-firmware transition is:

```text
verified + staged
    -> durable ActivationArmed
    -> select candidate boot target
    -> restart
    -> candidate Trial runtime
    -> health validation
    -> durable CommitIntent
    -> mark boot valid
    -> component Commit
    -> atomic committed-baseline promotion
```

If health fails, activation is cancelled after the activation boundary, or recovery determines rollback is required, rollback is driven in reverse dependency order and retains the previous committed baseline/security floor until rollback completes.

`ExecutingGenerationState` and `CommittedGenerationState` are deliberately different during Trial. A candidate does not become committed merely because it booted.

## Cross-version recovery compatibility

An OTA transaction can cross a firmware reboot, so the updater and the candidate may contain different OTA implementations.

Before activation is armed, the current runtime verifies signed candidate capability declarations. The candidate must be able to read/reconcile the durable OTA schema it will inherit and must support the Manifest schema needed to recover the transaction.

During Trial, OTA control data must remain readable by the previous committed runtime so rollback remains safe. Irreversible OTA durable-schema migration is therefore not allowed during Trial; a future migration to an old-runtime-incompatible schema belongs behind successful Commit and must itself be crash-consistent.

Recovery depends on stable semantic IDs and versioned schemas, never C++ object identity or private backend layout.

## Canonical OTA State

The public current truth is expressed through nine State Types:

- `CoordinatorAvailabilityState`
- `ActiveUpdateTransactionState`
- `ActiveUpdateLifecycleState`
- `ExecutingGenerationState`
- `CommittedGenerationState`
- `CandidateGenerationState`
- `MinimumAcceptedSecurityLevelState`
- `UpdateProgressState`
- `LastUpdateOutcomeState`

Persistence is not a second public status API. The Coordinator reconstructs/publishes State only after durable and platform facts have been established.

Consumers can use the normal State/Observable mechanisms to react to current truth without introducing an OTA-private callback engine.

## Optional Event bridge

`ESPressio_OTAEventBridge.hpp` provides an opt-in projection layer. It can publish meaningful OTA lifecycle/terminal occurrences from canonical State observations through Event's non-blocking dispatch path.

Event delivery is never required for OTA correctness. A dropped Event does not change the transaction, and a remote Event is observational rather than local mutation authority.

The core umbrella header does not make Event a mandatory dependency.

## Optional Command bridge

`ESPressio_OTACommandBridge.hpp` provides the opt-in mutation adapter for OTA operations that have a real direct Coordinator operation.

V1 exposes Start/Cancel through the bridge. It reuses Command's `CommandExecutionKey` for remote idempotency/replay rather than inventing an OTA request identity. Remote mutation is denied by default unless an explicit authorizer approves it.

The bridge does not invent a `CheckForUpdate` command because the current Coordinator does not expose a corresponding authoritative direct discovery operation. Discovery/catalog policy remains separate from lifecycle mutation.

The core umbrella header does not make Command a mandatory dependency.

## Capacity and memory guidance

OTA does not have one universal capacity profile. Applications/backends select a compile-time `OTACapacityProfile` appropriate to the target.

The conservative constrained V1 profile used for WROOM-class validation includes:

| Quantity | Bound |
| --- | ---: |
| Manifest bytes | 8192 |
| Components | 8 |
| Artifacts | 16 |
| dependency edges | 32 |
| Artifacts per Component | 4 |
| target clauses | 8 |
| supported Component Types | 16 |
| required health conditions | 8 |
| Component parameter bytes | 256 |
| Manifest signatures | 2 |
| signature bytes | 512 |
| Artifact checkpoints | 4 |
| distribution recipients | 32 |
| catalog candidates | 8 |
| policy providers / decision point | 8 |
| stage descriptors | 32 |
| OTA control record bytes | 2048 |
| Artifact checkpoint record bytes | 256 |
| transfer workspace bytes | 4096 |

Current native host-ABI retained-object accounting for that profile reports:

```text
Coordinator:                  2488 bytes
State runtime:                1112 bytes
State owner bindings:          144 bytes
Signed Manifest object:       8384 bytes
Manifest wire workspace:      8192 bytes
Artifact transfer workspace:  4096 bytes
Frozen target profile:         256 bytes
----------------------------------------
Retained core envelope:      24672 bytes
Dedicated OTA task stack:        0 bytes
```

These values are an object-accounting guardrail, not a claim about complete ESP32 application RAM. The real Arduino/ESP32 integration build includes the framework, networking/radio and backend static allocations as well.

## ESP32 / ESP-IDF concrete providers

The current ESP32 implementation line supplies portable OTA capabilities using ESP-IDF primitives beneath the Arduino integration where appropriate:

```cpp
ESPressio::ESP32Platform::OTAApplicationImageStaging
ESPressio::ESP32Platform::OTABootControl
ESPressio::ESP32Platform::OTATrialBoot
ESPressio::ESP32Platform::OTASystemRestart
ESPressio::ESP32Platform::OTAStorageLayoutInspection
ESPressio::ESP32Platform::OTASHA256
ESPressio::ESP32Platform::OTAECDSAP256SHA256SignatureVerifier
```

`ESPressio::ESP32Platform::ESP32OTACapacityProfile` is the target convenience alias for the constrained ESP32 profile.

The concrete integration intentionally prefers ESP-IDF OTA/boot primitives rather than creating a separate Arduino-specific OTA semantic model.

## Minimal portable integration shape

The exact providers and policies are application composition choices, but the high-level flow is:

```cpp
#include <ESPressio_OTA.hpp>

using Capacity = ESPressio::OTA::ConstrainedV1CapacityProfile;

// 1. Bind canonical OTA State owners to the application's State runtime.
// 2. Provide Persistence::IAtomicRecordStore.
// 3. Provide Platform BootControl / TrialBoot / SystemRestart / Clock.
// 4. Register ComponentHandlers, policies and health conditions.
// 5. Provide Artifact source/store and Security digest verification.
// 6. Construct Coordinator<Capacity, ...>.
// 7. Initialize(), Start(), bind the verified Manifest/target profile,
//    then call Advance() cooperatively until the current operation settles.
```

A trusted Manifest must be verified before `BindVerifiedManifest`; naming a method "BindVerifiedManifest" is not a substitute for performing Security verification.

## Distributed OTA

Transport/distribution is intentionally outside the correctness authority of the local lifecycle Coordinator.

A remote catalog, Mesh node, socket endpoint or other transport can carry offers, Manifests and Artifacts, but:

- remote compatibility claims are advisory until locally verified;
- remote OTA State is observational;
- remote Event projection is observational;
- remote mutation through Command requires explicit authorization;
- local signed-Manifest verification, local policy, durable intent, local Platform facts and local health remain authoritative.

This allows OTA to compose with future transport/distribution systems without making any one transport part of the OTA trust root.

## Testing and validation

The native suite covers bounded schemas, canonical Manifest encoding, providers/catalogs, deterministic UpdatePlan construction, Component lifecycle execution, durable control/recovery, State, policy/health, acquisition/verification, Coordinator activation/commit/rollback, staging recovery, cross-version candidate admission, optional bridges, memory accounting, durable fault injection and platform-boundary recovery.

The ESP32 implementation line is separately built against a real Arduino/ESP32 PlatformIO toolchain using the ESP-IDF-backed OTA providers.

Physical-device validation remains a distinct release-readiness activity: a successful host or cross-compile is not represented as proof that power-cut/reboot behaviour has been exercised on physical hardware.

## CMake development build

For a source checkout containing the required dependency trees under `deps/`:

```bash
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Tests are enabled by default and can be disabled for a consuming super-project with:

```bash
-DESPRESSIO_OTA_BUILD_TESTS=OFF
```

## Demos

P10 demos live under `demos/` and follow the ESPressio demo policy: each logical Arduino-capable demo provides matching `arduino_ide/` and `platformio/` variants. Device examples demonstrate concrete provider composition without silently turning demo startup into a flash/boot mutation.

## License

Apache License 2.0. See `LICENSE`.
