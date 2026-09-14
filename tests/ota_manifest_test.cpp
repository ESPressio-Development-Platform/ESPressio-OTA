#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTA.hpp"

using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;
using TestManifest = Manifest<Capacity>;
using TestSignedManifest = SignedManifest<Capacity>;
using TestClause = ManifestTargetClause<Capacity>;
using TestComponent = ManifestComponent<Capacity>;
using TestArtifact = ManifestArtifact<Capacity>;
using TestSignature = ManifestSignatureValue<Capacity>;

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

bool AddDigest(TestArtifact& artifact, std::uint8_t seed) {
    for (std::size_t i = 0U; i < 32U; ++i) {
        if (!artifact.Digest.push_back(static_cast<std::uint8_t>(seed + i))) return false;
    }
    return true;
}

bool ConfigureManifest(TestManifest& manifest, bool reverse) {
    manifest.Identifier = Id(9U);
    manifest.Release = 42U;
    manifest.ReleaseChannel = 0U;       // locked default/unspecified channel semantics
    manifest.SecurityGeneration = 0U;  // locked valid factory security generation
    manifest.RequiredOTAProtocol = OTAProtocolV1.Value();
    manifest.RequiredOTAFeatures = 0x5U;

    TestClause clause;
    clause.ProductTypeMode = static_cast<std::uint8_t>(TargetMatchMode::Exact);
    clause.ProductType = 11U;
    clause.HardwareFamilyMode = static_cast<std::uint8_t>(TargetMatchMode::Exact);
    clause.HardwareFamily = 22U;
    clause.HardwareRevisionMode = static_cast<std::uint8_t>(TargetRevisionMode::InclusiveRange);
    clause.HardwareRevisionMinimum = 2U;
    clause.HardwareRevisionMaximum = 4U;
    clause.ArchitectureMode = static_cast<std::uint8_t>(TargetMatchMode::Exact);
    clause.Architecture = 44U;
    clause.SoftwareVariantMode = static_cast<std::uint8_t>(TargetMatchMode::Exact);
    clause.SoftwareVariant = 55U;
    clause.CurrentStorageLayoutMode = static_cast<std::uint8_t>(TargetMatchMode::Exact);
    clause.CurrentStorageLayout = 66U;
    clause.CurrentStorageLayoutGeneration = 2U;
    clause.TargetStorageLayoutMode = static_cast<std::uint8_t>(TargetMatchMode::Exact);
    clause.TargetStorageLayout = 67U;
    clause.TargetStorageLayoutGeneration = 3U;
    clause.CurrentPersistenceSchemaMode = static_cast<std::uint8_t>(TargetMatchMode::Exact);
    clause.CurrentPersistenceSchema = 77U;
    clause.CurrentPersistenceSchemaGeneration = 4U;
    clause.TargetPersistenceSchemaMode = static_cast<std::uint8_t>(TargetMatchMode::Exact);
    clause.TargetPersistenceSchema = 78U;
    clause.TargetPersistenceSchemaGeneration = 5U;
    clause.MinimumProfileSchema = UpdateTargetProfileSchemaV1.Value();
    clause.MinimumOTAProtocol = OTAProtocolV1.Value();
    clause.RequiredOTAFeatures = 0x1U;
    if (!clause.RequiredComponentTypes.push_back(reverse ? 0x1002U : 0x1001U) ||
        !clause.RequiredComponentTypes.push_back(reverse ? 0x1001U : 0x1002U) ||
        !manifest.TargetClauses.push_back(clause)) return false;

    TestArtifact artifactA;
    artifactA.Identifier = Id(1U);
    artifactA.ExpectedLength = 1024U;
    artifactA.DigestAlgorithm = ESPressio::Security::DigestAlgorithm::SHA256.Value();
    if (!AddDigest(artifactA, 1U)) return false;

    TestArtifact artifactB;
    artifactB.Identifier = Id(2U);
    artifactB.ExpectedLength = 2048U;
    artifactB.DigestAlgorithm = ESPressio::Security::DigestAlgorithm::SHA256.Value();
    if (!AddDigest(artifactB, 2U)) return false;

    if (!(reverse ? manifest.Artifacts.push_back(artifactB) : manifest.Artifacts.push_back(artifactA))) return false;
    if (!(reverse ? manifest.Artifacts.push_back(artifactA) : manifest.Artifacts.push_back(artifactB))) return false;

    TestComponent componentA;
    componentA.Identifier = 1U;
    componentA.TypeId = 0x1001U;
    componentA.ParameterSchemaVersion = 1U;
    if (!componentA.Parameters.push_back(0xA1U)) return false;
    if (!componentA.Artifacts.push_back(reverse ? Id(2U) : Id(1U)) ||
        !componentA.Artifacts.push_back(reverse ? Id(1U) : Id(2U))) return false;

    TestComponent componentB;
    componentB.Identifier = 2U;
    componentB.TypeId = 0x1002U;
    componentB.ParameterSchemaVersion = 1U;
    if (!componentB.Parameters.push_back(0xB2U)) return false;

    if (!(reverse ? manifest.Components.push_back(componentB) : manifest.Components.push_back(componentA))) return false;
    if (!(reverse ? manifest.Components.push_back(componentA) : manifest.Components.push_back(componentB))) return false;

    ManifestDependency dependency;
    dependency.Component = 2U;
    dependency.DependsOn = 1U;
    if (!manifest.Dependencies.push_back(dependency)) return false;
    if (!manifest.RequiredHealthConditions.push_back(reverse ? 0x3002U : 0x3001U) ||
        !manifest.RequiredHealthConditions.push_back(reverse ? 0x3001U : 0x3002U)) return false;

    ManifestSignatureDescriptor signatureDescriptor;
    signatureDescriptor.Algorithm = 1U;
    signatureDescriptor.TrustAnchor = 7U;
    signatureDescriptor.TrustPolicy = 8U;
    if (!manifest.SignatureDescriptors.push_back(signatureDescriptor)) return false;
    return true;
}

class FakeAnchorProvider final : public ESPressio::Security::ITrustAnchorProvider {
    std::array<std::uint8_t, 4> key_{{1U, 2U, 3U, 4U}};
public:
    ESPressio::Security::VerificationResult Resolve(
        ESPressio::Security::TrustAnchorIdentifier identifier,
        ESPressio::Security::TrustAnchorView& anchor) const noexcept override {
        if (identifier != ESPressio::Security::TrustAnchorIdentifier{7U}) {
            return {ESPressio::Security::VerificationStatus::TrustAnchorUnavailable, 0};
        }
        anchor = {identifier, {key_.data(), key_.size()}};
        return ESPressio::Security::VerificationResult::Ok();
    }
};

class FakePolicy final : public ESPressio::Security::ITrustPolicy {
public:
    ESPressio::Security::VerificationResult Authorize(
        ESPressio::Security::TrustPolicyIdentifier policy,
        ESPressio::Security::TrustPurpose purpose,
        ESPressio::Security::TrustAnchorIdentifier anchor) const noexcept override {
        return policy == ESPressio::Security::TrustPolicyIdentifier{8U} &&
               purpose == ESPressio::Security::TrustPurpose::SoftwareUpdateManifest &&
               anchor == ESPressio::Security::TrustAnchorIdentifier{7U}
            ? ESPressio::Security::VerificationResult::Ok()
            : ESPressio::Security::VerificationResult{ESPressio::Security::VerificationStatus::UntrustedSigner, 0};
    }
};

class FakeSignatureVerifier final : public ESPressio::Security::ISignatureVerifier {
    bool accept_{true};
public:
    explicit FakeSignatureVerifier(bool accept = true) noexcept : accept_(accept) {}
    bool Supports(ESPressio::Security::SignatureAlgorithmIdentifier algorithm) const noexcept override {
        return algorithm == ESPressio::Security::SignatureAlgorithmIdentifier{1U};
    }
    ESPressio::Security::VerificationResult Verify(
        ESPressio::Security::SignatureAlgorithmIdentifier algorithm,
        ESPressio::Security::ByteView canonicalContent,
        ESPressio::Security::ByteView signature,
        const ESPressio::Security::TrustAnchorView& anchor) noexcept override {
        if (!Supports(algorithm) || !canonicalContent.IsValid() || canonicalContent.Size == 0U ||
            !signature.IsValid() || signature.Size != 3U || !anchor) {
            return {ESPressio::Security::VerificationStatus::InvalidArgument, 0};
        }
        return accept_
            ? ESPressio::Security::VerificationResult::Ok()
            : ESPressio::Security::VerificationResult{ESPressio::Security::VerificationStatus::InvalidSignature, 0};
    }
};

static_assert(ESPressio::Serializable::IsBoundedSerializable<TestClause>);
static_assert(ESPressio::Serializable::IsBoundedSerializable<TestManifest>);
static_assert(ESPressio::Serializable::IsBoundedSerializable<TestSignedManifest>);
static_assert(ESPressio::Serializable::MaximumSerializedSize<TestSignedManifest, ESPressio::Serializable::DirectBinary> > 0U);

} // namespace

int main() {
    TestManifest first;
    TestManifest second;
    if (!ConfigureManifest(first, true) || !ConfigureManifest(second, false)) return 1;

    if (ValidateManifest(first) != ManifestStatus::NonCanonical) return 2;
    if (PrepareManifestForSigning(first) != ManifestStatus::Success) return 3;
    if (PrepareManifestForSigning(second) != ManifestStatus::Success) return 4;
    if (first.SecurityGeneration != 0U || first.ReleaseChannel != 0U) return 5;

    std::array<std::uint8_t, Capacity::MaximumManifestBytes> firstBytes{};
    std::array<std::uint8_t, Capacity::MaximumManifestBytes> secondBytes{};
    std::size_t firstSize = 0U;
    std::size_t secondSize = 0U;
    if (SerializeCanonicalManifest(first, firstBytes.data(), firstBytes.size(), firstSize) != ManifestStatus::Success) return 6;
    if (SerializeCanonicalManifest(second, secondBytes.data(), secondBytes.size(), secondSize) != ManifestStatus::Success) return 7;
    if (firstSize != secondSize) return 8;
    for (std::size_t i = 0U; i < firstSize; ++i) if (firstBytes[i] != secondBytes[i]) return 9;

    TestManifest restored;
    const auto decoded = ESPressio::Serializable::DeserializeBoundedDirectBinaryIntoScratch(
        firstBytes.data(), firstSize, restored);
    if (!decoded || ValidateManifest(restored) != ManifestStatus::Success) return 10;
    if (restored.SecurityGeneration != 0U || restored.ReleaseChannel != 0U) return 11;

    TestManifest duplicate = first;
    if (!duplicate.Components.push_back(duplicate.Components[0])) return 12;
    if (CanonicalizeManifest(duplicate) != ManifestStatus::DuplicateIdentifier) return 13;

    TestManifest missing = first;
    missing.Components[0].Artifacts.clear();
    if (!missing.Components[0].Artifacts.push_back(Id(99U))) return 14;
    if (ValidateManifest(missing) != ManifestStatus::MissingReference) return 15;

    TestManifest cyclic = first;
    cyclic.Dependencies.clear();
    ManifestDependency edgeA;
    edgeA.Component = 1U;
    edgeA.DependsOn = 2U;
    ManifestDependency edgeB;
    edgeB.Component = 2U;
    edgeB.DependsOn = 1U;
    if (!cyclic.Dependencies.push_back(edgeA) || !cyclic.Dependencies.push_back(edgeB)) return 16;
    if (ValidateManifest(cyclic) != ManifestStatus::DependencyCycle) return 17;

    TestSignedManifest envelope;
    envelope.Content = first;
    TestSignature signature;
    if (!signature.Bytes.push_back(0xAAU) || !signature.Bytes.push_back(0xBBU) || !signature.Bytes.push_back(0xCCU) ||
        !envelope.Signatures.push_back(signature)) return 18;
    if (ValidateSignedManifestEnvelope(envelope) != ManifestStatus::Success) return 19;

    ManifestWireWorkspace<Capacity> workspace;
    std::size_t encodedEnvelopeBytes = 0U;
    if (SerializeSignedManifest(
            envelope, workspace.Bytes.data(), workspace.Bytes.size(), encodedEnvelopeBytes) != ManifestStatus::Success ||
        encodedEnvelopeBytes > Capacity::MaximumManifestBytes) return 20;

    FakeAnchorProvider anchors;
    FakePolicy policy;
    FakeSignatureVerifier validVerifier{true};
    if (VerifySignedManifestWithWorkspace(envelope, validVerifier, anchors, policy, workspace) != ManifestStatus::Success) return 21;
    FakeSignatureVerifier invalidVerifier{false};
    if (VerifySignedManifestWithWorkspace(envelope, invalidVerifier, anchors, policy, workspace) != ManifestStatus::NoTrustedSignature) return 22;

    TestSignedManifest mismatch = envelope;
    mismatch.Signatures.clear();
    if (ValidateSignedManifestEnvelope(mismatch) != ManifestStatus::SignatureCountMismatch) return 23;

    TestManifest invalidDigest = first;
    invalidDigest.Artifacts[0].DigestAlgorithm = 0U;
    if (ValidateManifest(invalidDigest) != ManifestStatus::Invalid) return 24;

    return 0;
}
