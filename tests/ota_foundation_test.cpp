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
    return 0;
}
