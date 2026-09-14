#pragma once

#include <cstddef>

namespace ESPressio::OTA {

template<
    std::size_t TMaximumManifestBytes,
    std::size_t TMaximumComponents,
    std::size_t TMaximumArtifacts,
    std::size_t TMaximumDependencyEdges,
    std::size_t TMaximumArtifactsPerComponent,
    std::size_t TMaximumTargetClauses,
    std::size_t TMaximumSupportedComponentTypes,
    std::size_t TMaximumRequiredHealthConditions,
    std::size_t TMaximumComponentParameterBytes,
    std::size_t TMaximumManifestSignatures,
    std::size_t TMaximumSignatureBytes,
    std::size_t TMaximumDigestBytes,
    std::size_t TMaximumArtifactCheckpoints,
    std::size_t TMaximumDistributionRecipients,
    std::size_t TMaximumCatalogCandidates,
    std::size_t TMaximumDiagnosticContextEntries,
    std::size_t TMaximumPolicyProvidersPerDecisionPoint,
    std::size_t TMaximumHealthChecks,
    std::size_t TMaximumStageDescriptors,
    std::size_t TMaximumOTAControlRecordBytes,
    std::size_t TMaximumArtifactCheckpointRecordBytes,
    std::size_t TTransferBufferBytes>
struct OTACapacityProfile final {
    static constexpr std::size_t MaximumManifestBytes = TMaximumManifestBytes;
    static constexpr std::size_t MaximumComponents = TMaximumComponents;
    static constexpr std::size_t MaximumArtifacts = TMaximumArtifacts;
    static constexpr std::size_t MaximumDependencyEdges = TMaximumDependencyEdges;
    static constexpr std::size_t MaximumArtifactsPerComponent = TMaximumArtifactsPerComponent;
    static constexpr std::size_t MaximumTargetClauses = TMaximumTargetClauses;
    static constexpr std::size_t MaximumSupportedComponentTypes = TMaximumSupportedComponentTypes;
    static constexpr std::size_t MaximumRequiredHealthConditions = TMaximumRequiredHealthConditions;
    static constexpr std::size_t MaximumComponentParameterBytes = TMaximumComponentParameterBytes;
    static constexpr std::size_t MaximumManifestSignatures = TMaximumManifestSignatures;
    static constexpr std::size_t MaximumSignatureBytes = TMaximumSignatureBytes;
    static constexpr std::size_t MaximumDigestBytes = TMaximumDigestBytes;
    static constexpr std::size_t MaximumArtifactCheckpoints = TMaximumArtifactCheckpoints;
    static constexpr std::size_t MaximumDistributionRecipients = TMaximumDistributionRecipients;
    static constexpr std::size_t MaximumCatalogCandidates = TMaximumCatalogCandidates;
    static constexpr std::size_t MaximumDiagnosticContextEntries = TMaximumDiagnosticContextEntries;
    static constexpr std::size_t MaximumPolicyProvidersPerDecisionPoint = TMaximumPolicyProvidersPerDecisionPoint;
    static constexpr std::size_t MaximumHealthChecks = TMaximumHealthChecks;
    static constexpr std::size_t MaximumStageDescriptors = TMaximumStageDescriptors;
    static constexpr std::size_t MaximumOTAControlRecordBytes = TMaximumOTAControlRecordBytes;
    static constexpr std::size_t MaximumArtifactCheckpointRecordBytes = TMaximumArtifactCheckpointRecordBytes;
    static constexpr std::size_t TransferBufferBytes = TTransferBufferBytes;

    static constexpr std::size_t MaximumCompatibilityClaimTokenBytes = 128U;

    static constexpr bool IsValid =
        MaximumManifestBytes != 0U &&
        MaximumComponents != 0U &&
        MaximumArtifacts != 0U &&
        MaximumDependencyEdges != 0U &&
        MaximumArtifactsPerComponent != 0U &&
        MaximumTargetClauses != 0U &&
        MaximumSupportedComponentTypes != 0U &&
        MaximumRequiredHealthConditions != 0U &&
        MaximumComponentParameterBytes != 0U &&
        MaximumManifestSignatures != 0U &&
        MaximumSignatureBytes != 0U &&
        MaximumDigestBytes != 0U &&
        MaximumArtifactCheckpoints != 0U &&
        MaximumDistributionRecipients != 0U &&
        MaximumCatalogCandidates != 0U &&
        MaximumDiagnosticContextEntries != 0U &&
        MaximumPolicyProvidersPerDecisionPoint != 0U &&
        MaximumHealthChecks != 0U &&
        MaximumStageDescriptors != 0U &&
        MaximumOTAControlRecordBytes != 0U &&
        MaximumArtifactCheckpointRecordBytes != 0U &&
        TransferBufferBytes != 0U;
};

/// <summary>Conservative constrained-device profile used by the OTA native contract tests.</summary>
/// <remarks>Concrete ESP32 integration may alias this after whole-device accounting; OTA core does not select it implicitly.</remarks>
using ConstrainedV1CapacityProfile = OTACapacityProfile<
    8192U, // manifest bytes
    8U,    // components
    16U,   // artifacts
    32U,   // dependency edges
    4U,    // artifacts per component
    8U,    // target clauses
    16U,   // supported component Types
    8U,    // required health conditions
    256U,  // component parameter bytes
    2U,    // manifest signatures
    512U,  // signature bytes
    64U,   // digest bytes (explicit implementation-discovered bound)
    4U,    // artifact checkpoints
    32U,   // distribution recipients
    8U,    // catalog candidates
    4U,    // diagnostic context entries
    8U,    // policy providers per point
    8U,    // health checks
    32U,   // stage descriptors
    2048U, // OTA control record bytes
    256U,  // artifact checkpoint record bytes
    4096U  // transfer buffer bytes
>;

static_assert(ConstrainedV1CapacityProfile::IsValid);

} // namespace ESPressio::OTA
