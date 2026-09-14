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

class FakeSha256 final : public ESPressio::Security::IStreamingDigest {
    std::array<std::uint8_t, 32> state_{};
    std::size_t offset_{0U};
    bool active_{false};
public:
    bool Supports(ESPressio::Security::DigestAlgorithmIdentifier algorithm) const noexcept override {
        return algorithm == ESPressio::Security::DigestAlgorithm::SHA256;
    }

    std::size_t DigestSize(ESPressio::Security::DigestAlgorithmIdentifier algorithm) const noexcept override {
        return Supports(algorithm) ? state_.size() : 0U;
    }

    ESPressio::Security::VerificationResult Begin(
        ESPressio::Security::DigestAlgorithmIdentifier algorithm) noexcept override {
        if (!Supports(algorithm)) {
            return {ESPressio::Security::VerificationStatus::UnsupportedAlgorithm, 0};
        }
        state_.fill(0U);
        offset_ = 0U;
        active_ = true;
        return ESPressio::Security::VerificationResult::Ok();
    }

    ESPressio::Security::VerificationResult Update(
        ESPressio::Security::ByteView bytes) noexcept override {
        if (!active_ || !bytes.IsValid()) {
            return {ESPressio::Security::VerificationStatus::InvalidArgument, 0};
        }
        for (std::size_t i = 0U; i < bytes.Size; ++i, ++offset_) {
            const std::size_t slot = offset_ % state_.size();
            state_[slot] = static_cast<std::uint8_t>(
                state_[slot] ^ static_cast<std::uint8_t>(bytes.Data[i] + static_cast<std::uint8_t>(offset_)));
        }
        return ESPressio::Security::VerificationResult::Ok();
    }

    ESPressio::Security::VerificationResult Finalize(
        ESPressio::Security::MutableByteView output,
        std::size_t& written) noexcept override {
        written = 0U;
        if (!active_ || !output.IsValid()) {
            return {ESPressio::Security::VerificationStatus::InvalidArgument, 0};
        }
        if (output.Size < state_.size()) {
            return {ESPressio::Security::VerificationStatus::CapacityUnavailable, 0};
        }
        for (std::size_t i = 0U; i < state_.size(); ++i) output.Data[i] = state_[i];
        written = state_.size();
        active_ = false;
        return ESPressio::Security::VerificationResult::Ok();
    }
};

using TestProfile = UpdateTargetProfile<ConstrainedV1CapacityProfile>;
using TestProfileWire = UpdateTargetProfileCanonicalWire<ConstrainedV1CapacityProfile>;

bool ConfigureProfile(TestProfile& profile, bool reverseComponentOrder) {
    if (profile.SetSystemIdentity(
            ESPressio::System::ProductTypeIdentifier{11U},
            ESPressio::System::HardwareFamilyIdentifier{22U},
            ESPressio::System::HardwareRevision{3U},
            ESPressio::System::ArchitectureIdentifier{44U},
            ESPressio::System::SoftwareVariantIdentifier{55U}) != TargetProfileStatus::Success) return false;
    if (profile.SetStorageLayout(
            ESPressio::Platform::OTA::StorageLayoutIdentifier{66U},
            ESPressio::Platform::OTA::StorageLayoutGeneration{2U}) != TargetProfileStatus::Success) return false;
    if (profile.SetPersistenceSchema(
            ESPressio::Persistence::SchemaIdentifier{77U},
            ESPressio::Persistence::SchemaGeneration{4U}) != TargetProfileStatus::Success) return false;
    if (profile.SetOTASupport(OTAProtocolV1, 0x5U) != TargetProfileStatus::Success) return false;

    const ComponentTypeId first{reverseComponentOrder ? 0x1002U : 0x1001U};
    const ComponentTypeId second{reverseComponentOrder ? 0x1001U : 0x1002U};
    return profile.AddSupportedComponentType(first) == TargetProfileStatus::Success &&
           profile.AddSupportedComponentType(second) == TargetProfileStatus::Success;
}

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
static_assert(ESPressio::Serializable::IsBoundedSerializable<TestProfileWire>);

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

    TestProfile profileA;
    TestProfile profileB;
    if (!ConfigureProfile(profileA, false) || !ConfigureProfile(profileB, true)) return 14;

    FakeSha256 digestA;
    FakeSha256 digestB;
    if (FinalizeUpdateTargetProfile(profileA, digestA) != TargetProfileFingerprintStatus::Success) return 15;
    if (FinalizeUpdateTargetProfile(profileB, digestB) != TargetProfileFingerprintStatus::Success) return 16;
    if (!profileA.IsFrozen() || !profileB.IsFrozen()) return 17;
    if (profileA.Fingerprint() != profileB.Fingerprint()) return 18;
    if (profileA.SupportedComponentType(0U).Value() != 0x1001U ||
        profileA.SupportedComponentType(1U).Value() != 0x1002U) return 19;
    if (profileB.SupportedComponentType(0U).Value() != 0x1001U ||
        profileB.SupportedComponentType(1U).Value() != 0x1002U) return 20;
    if (profileA.AddSupportedComponentType(ComponentTypeId{0x1003U}) != TargetProfileStatus::Frozen) return 21;

    TestProfileWire wire;
    if (BuildCanonicalProfileWire(profileA, wire) != TargetProfileFingerprintStatus::Success) return 22;
    std::array<std::uint8_t,
        ESPressio::Serializable::MaximumSerializedSize<TestProfileWire, ESPressio::Serializable::DirectBinary>> encoded{};
    const auto serialized = ESPressio::Serializable::SerializeDirectBinary(wire, encoded.data(), encoded.size());
    if (!serialized || serialized.Bytes == 0U) return 23;

    TestProfileWire restored;
    const auto deserialized = ESPressio::Serializable::DeserializeBoundedDirectBinary(
        encoded.data(), serialized.Bytes, restored);
    if (!deserialized) return 24;
    if (restored.DomainTag != UpdateTargetProfileDomainTag ||
        restored.ProductType != 11U ||
        restored.StorageLayoutGeneration != 2U ||
        restored.PersistenceSchemaGeneration != 4U ||
        restored.SupportedComponentTypes.size() != 2U ||
        restored.SupportedComponentTypes[0] != 0x1001U ||
        restored.SupportedComponentTypes[1] != 0x1002U) return 25;

    return 0;
}
