#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "ESPressio_OTA.hpp"

using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;
using TestManifest = Manifest<Capacity>;
using TestSignedManifest = SignedManifest<Capacity>;
using TestOffer = UpdateOffer<Capacity>;
using TestWorkspace = ManifestWireWorkspace<Capacity>;

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

bool BuildEnvelope(TestSignedManifest& envelope) {
    auto& manifest = envelope.Content;
    manifest.Identifier = Id(9U);
    manifest.Release = 42U;
    manifest.ReleaseChannel = 0U;
    manifest.SecurityGeneration = 0U;
    manifest.RequiredOTAProtocol = OTAProtocolV1.Value();
    manifest.RequiredOTAFeatures = 0x1U;

    ManifestTargetClause<Capacity> clause;
    clause.ProductTypeMode = static_cast<std::uint8_t>(TargetMatchMode::Exact);
    clause.ProductType = 11U;
    clause.MinimumProfileSchema = UpdateTargetProfileSchemaV1.Value();
    clause.MinimumOTAProtocol = OTAProtocolV1.Value();
    if (!clause.RequiredComponentTypes.push_back(0x1001U) ||
        !manifest.TargetClauses.push_back(clause)) return false;

    ManifestArtifact<Capacity> artifact;
    artifact.Identifier = Id(1U);
    artifact.ExpectedLength = 1024U;
    artifact.DigestAlgorithm = ESPressio::Security::DigestAlgorithm::SHA256.Value();
    for (std::size_t i = 0U; i < 32U; ++i) {
        if (!artifact.Digest.push_back(static_cast<std::uint8_t>(i + 1U))) return false;
    }
    if (!manifest.Artifacts.push_back(artifact)) return false;

    ManifestComponent<Capacity> component;
    component.Identifier = 1U;
    component.TypeId = 0x1001U;
    component.ParameterSchemaVersion = 1U;
    if (!component.Artifacts.push_back(artifact.Identifier) ||
        !manifest.Components.push_back(component)) return false;

    ManifestSignatureDescriptor descriptor;
    descriptor.Algorithm = 1U;
    descriptor.TrustAnchor = 7U;
    descriptor.TrustPolicy = 8U;
    if (!manifest.SignatureDescriptors.push_back(descriptor)) return false;

    if (PrepareManifestForSigning(manifest) != ManifestStatus::Success) return false;

    ManifestSignatureValue<Capacity> signature;
    if (!signature.Bytes.push_back(0xAAU) || !signature.Bytes.push_back(0xBBU) ||
        !signature.Bytes.push_back(0xCCU) || !envelope.Signatures.push_back(signature)) return false;
    return ValidateSignedManifestEnvelope(envelope) == ManifestStatus::Success;
}

static_assert(ESPressio::Serializable::IsBoundedSerializable<TestOffer>);
static_assert(sizeof(TestWorkspace) == Capacity::MaximumManifestBytes);

} // namespace

int main() {
    std::cout << "sizeof Manifest=" << sizeof(TestManifest) << '\n';
    std::cout << "sizeof SignedManifest=" << sizeof(TestSignedManifest) << '\n';
    std::cout << "sizeof UpdateOffer=" << sizeof(TestOffer) << '\n';

    TestSignedManifest envelope;
    if (!BuildEnvelope(envelope)) return 1;
    const ManifestIdentifier identifier{envelope.Content.Identifier};

    CandidateCompatibilityClaim<Capacity> claim;
    if (claim.SetProductType(ESPressio::System::ProductTypeIdentifier{11U}) != CompatibilityClaimStatus::Success) return 2;
    if (claim.AddRequiredComponentType(ComponentTypeId{0x1001U}) != CompatibilityClaimStatus::Success) return 3;
    CompatibilityClaimToken<Capacity> claimToken;
    if (BuildCandidateCompatibilityClaimToken(claim, claimToken) != CompatibilityClaimStatus::Success) return 4;

    UpdateOfferAdvisorySummary summary;
    summary.Release = envelope.Content.Release;
    summary.ReleaseChannel = envelope.Content.ReleaseChannel;
    summary.SecurityGeneration = envelope.Content.SecurityGeneration;
    summary.RequiredOTAProtocol = envelope.Content.RequiredOTAProtocol;
    summary.RequiredOTAFeatures = envelope.Content.RequiredOTAFeatures;

    TestSignedManifest scratch;
    TestWorkspace workspace;

    TestOffer reference;
    if (BuildManifestReferenceOffer(identifier, &claimToken, &summary, reference) != UpdateOfferStatus::Success) return 5;
    if (ValidateUpdateOffer(reference, scratch) != UpdateOfferStatus::Success) return 6;
    if (!reference.EmbeddedManifest.empty()) return 7;
    if (reference.CompatibilityClaimSchema != CompatibilityClaimTokenSchemaV1 ||
        reference.CompatibilityClaim.size() != claimToken.TokenLength) return 8;
    if (ValidateUpdateOfferAgainstVerifiedManifest(reference, envelope.Content, scratch) != UpdateOfferStatus::Success) return 9;

    TestOffer embedded;
    if (BuildEmbeddedManifestOffer(envelope, &claimToken, &summary, workspace, embedded) != UpdateOfferStatus::Success) return 10;
    if (ValidateUpdateOffer(embedded, scratch) != UpdateOfferStatus::Success) return 11;
    if (embedded.EmbeddedManifest.empty() || embedded.EmbeddedManifest.size() > Capacity::MaximumManifestBytes) return 12;
    if (ValidateUpdateOfferAgainstVerifiedManifest(embedded, envelope.Content, scratch) != UpdateOfferStatus::Success) return 13;

    TestSignedManifest decoded;
    if (DeserializeSignedManifestIntoScratch(
            embedded.EmbeddedManifest.data(), embedded.EmbeddedManifest.size(), decoded) != ManifestStatus::Success) return 14;
    if (decoded.Content.Identifier != envelope.Content.Identifier) return 15;

    TestOffer advisoryMismatch = reference;
    advisoryMismatch.AdvisorySummary.Release += 1U;
    if (ValidateUpdateOfferAgainstVerifiedManifest(advisoryMismatch, envelope.Content, scratch) != UpdateOfferStatus::AdvisoryMismatch) return 16;

    TestOffer manifestMismatch = reference;
    manifestMismatch.Manifest = Id(99U);
    if (ValidateUpdateOfferAgainstVerifiedManifest(manifestMismatch, envelope.Content, scratch) != UpdateOfferStatus::ManifestMismatch) return 17;

    TestOffer strayEmbedded = reference;
    if (!strayEmbedded.EmbeddedManifest.push_back(0x01U)) return 18;
    if (ValidateUpdateOffer(strayEmbedded, scratch) != UpdateOfferStatus::Invalid) return 19;

    TestOffer malformedClaim = reference;
    malformedClaim.CompatibilityClaimSchema = 0U;
    if (ValidateUpdateOffer(malformedClaim, scratch) != UpdateOfferStatus::Invalid) return 20;

    TestOffer absentSummary = reference;
    absentSummary.HasAdvisorySummary = 0U;
    absentSummary.AdvisorySummary = {};
    if (ValidateUpdateOffer(absentSummary, scratch) != UpdateOfferStatus::Success) return 21;

    TestSignedManifest badEnvelope = envelope;
    badEnvelope.Signatures.clear();
    TestOffer rejected;
    const auto* noClaim = static_cast<const CompatibilityClaimToken<Capacity>*>(nullptr);
    const auto* noSummary = static_cast<const UpdateOfferAdvisorySummary*>(nullptr);
    if (BuildEmbeddedManifestOffer(badEnvelope, noClaim, noSummary, workspace, rejected) != UpdateOfferStatus::Invalid) return 22;

    return 0;
}
