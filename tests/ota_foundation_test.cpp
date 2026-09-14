#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTA.hpp"

using namespace ESPressio::OTA;

struct ApplicationFirmwareComponent {};

namespace ESPressio::OTA {

template<>
struct ComponentTypeTraits<::ApplicationFirmwareComponent> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {
            ComponentTypeId{0x1001U},
            "ApplicationFirmware",
            ComponentKind::ApplicationFirmware,
            ComponentMultiplicity::Single,
            1U
        };
    }
};

} // namespace ESPressio::OTA

namespace {

static_assert(sizeof(ComponentTypeId) == 8U);
static_assert(sizeof(ArtifactIdentifier) == 16U);
static_assert(sizeof(UpdateTargetProfileFingerprint) == 32U);
static_assert(sizeof(UpdateTargetProfileSchemaVersion) == 2U);
static_assert(UpdateTargetProfileSchemaV1.Value() == 1U);
static_assert(SecurityGeneration{0U}.Value() == 0U);
static_assert(ReleaseChannelIdentifier{0U}.Value() == 0U);
static_assert(ConstrainedV1CapacityProfile::MaximumManifestBytes == 8192U);
static_assert(ConstrainedV1CapacityProfile::MaximumDigestBytes == 64U);
static_assert(ConstrainedV1CapacityProfile::MaximumCompatibilityClaimTokenBytes == 128U);
static_assert(DescribeComponentType<ApplicationFirmwareComponent>().Kind == ComponentKind::ApplicationFirmware);

} // namespace

int main() {
    ComponentTypeDirectory<ConstrainedV1CapacityProfile> components;
    const auto component = DescribeComponentType<ApplicationFirmwareComponent>();
    if (components.Register(component) != DirectoryStatus::Success) return 1;
    if (components.Register(component) != DirectoryStatus::DuplicateIdentifier) return 2;
    components.Freeze();
    if (!components.IsFrozen()) return 3;
    if (components.Find(component.TypeId) == nullptr) return 4;
    if (components.Register({ComponentTypeId{2U}, "Other", ComponentKind::Data, ComponentMultiplicity::Single, 1U}) != DirectoryStatus::Frozen) return 5;

    StageDirectory<ConstrainedV1CapacityProfile> stages;
    const StageDescriptor stage{
        StageTypeId{0x2001U},
        "AcquireArtifact",
        UpdateOperation::Acquire,
        ProgressMode::Determinate,
        ProgressUnit::Bytes,
        component.TypeId
    };
    if (stages.Register(stage) != DirectoryStatus::Success) return 6;
    stages.Freeze();
    if (stages.Find(stage.TypeId) == nullptr) return 7;

    const StageProgress valid{stage.TypeId, 1U, ProgressMode::Determinate, ProgressUnit::Bytes, 64U, 128U, ComponentIdentifier{1U}};
    if (!valid.IsValid() || valid.IsComplete()) return 8;
    const StageProgress complete{stage.TypeId, 1U, ProgressMode::Determinate, ProgressUnit::Bytes, 128U, 128U, ComponentIdentifier{1U}};
    if (!complete.IsValid() || !complete.IsComplete()) return 9;
    const StageProgress invalid{stage.TypeId, 1U, ProgressMode::Determinate, ProgressUnit::Bytes, 129U, 128U, ComponentIdentifier{1U}};
    if (invalid.IsValid()) return 10;

    Diagnostic diagnostic;
    if (!diagnostic.Add(DiagnosticContextKey::ComponentIdentifier, 1U)) return 11;
    if (diagnostic.ContextCount != 1U) return 12;
    if (!Result::Success()) return 13;

    UpdateTargetProfile<ConstrainedV1CapacityProfile> profile;
    if (profile.SetSystemIdentity(
            ESPressio::System::ProductTypeIdentifier{11U},
            ESPressio::System::HardwareFamilyIdentifier{22U},
            ESPressio::System::HardwareRevision{3U},
            ESPressio::System::ArchitectureIdentifier{44U},
            ESPressio::System::SoftwareVariantIdentifier{55U}) != TargetProfileStatus::Success) return 14;
    if (profile.SetStorageLayout(
            ESPressio::Platform::OTA::StorageLayoutIdentifier{66U},
            ESPressio::Platform::OTA::StorageLayoutGeneration{2U}) != TargetProfileStatus::Success) return 15;
    if (profile.SetPersistenceSchema(
            ESPressio::Persistence::SchemaIdentifier{77U},
            ESPressio::Persistence::SchemaGeneration{4U}) != TargetProfileStatus::Success) return 16;
    if (profile.SetOTASupport(OTAProtocolV1, 0x5U) != TargetProfileStatus::Success) return 17;
    if (profile.AddSupportedComponentType(ComponentTypeId{0x1002U}) != TargetProfileStatus::Success) return 18;
    if (profile.AddSupportedComponentType(ComponentTypeId{0x1001U}) != TargetProfileStatus::Success) return 19;
    if (profile.AddSupportedComponentType(ComponentTypeId{0x1001U}) != TargetProfileStatus::DuplicateComponentType) return 20;
    if (profile.Canonicalize() != TargetProfileStatus::Success) return 21;
    if (profile.SupportedComponentType(0U).Value() != 0x1001U || profile.SupportedComponentType(1U).Value() != 0x1002U) return 22;

    std::array<std::uint8_t, 32> fingerprintBytes{};
    fingerprintBytes[0] = 1U;
    if (profile.SetFingerprintAndFreeze(UpdateTargetProfileFingerprint{fingerprintBytes}) != TargetProfileStatus::Success) return 23;
    if (!profile.IsFrozen() || !profile.Fingerprint()) return 24;
    if (profile.AddSupportedComponentType(ComponentTypeId{0x1003U}) != TargetProfileStatus::Frozen) return 25;

    return 0;
}
