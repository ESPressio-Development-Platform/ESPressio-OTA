#include <array>
#include <cstdint>

#include "ESPressio_OTA.hpp"

using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

CatalogCandidate<Capacity> Candidate(std::uint64_t release, std::uint8_t manifest) {
    CatalogCandidate<Capacity> candidate;
    candidate.Release = ReleaseIdentifier{release};
    candidate.Manifest = ManifestIdentifier{Id(manifest)};
    candidate.RequiredOTAProtocol = OTAProtocolV1;
    return candidate;
}

} // namespace

int main() {
    CatalogCandidateSet<Capacity> candidates;

    const auto first = Candidate(1U, 2U);
    const auto second = Candidate(2U, 1U);
    if (candidates.Add(first) != CatalogCandidateSetStatus::Added) return 1;
    if (candidates.Add(second) != CatalogCandidateSetStatus::Added) return 2;
    if (candidates.Size() != 2U) return 3;

    // Enumeration is canonical by ManifestIdentifier, not provider/discovery order.
    if (candidates.At(0U) == nullptr || candidates.At(0U)->Manifest != second.Manifest) return 4;
    if (candidates.At(1U) == nullptr || candidates.At(1U)->Manifest != first.Manifest) return 5;

    if (candidates.Add(first) != CatalogCandidateSetStatus::AlreadyPresent) return 6;
    if (candidates.Size() != 2U) return 7;

    // Same Release may never resolve to two different immutable Manifests.
    if (candidates.Add(Candidate(1U, 3U)) != CatalogCandidateSetStatus::Conflict) return 8;
    // One immutable Manifest may not advertise two different Release identities.
    if (candidates.Add(Candidate(3U, 2U)) != CatalogCandidateSetStatus::Conflict) return 9;

    for (std::uint64_t release = 3U; release <= 8U; ++release) {
        if (candidates.Add(Candidate(release, static_cast<std::uint8_t>(release)))
            != CatalogCandidateSetStatus::Added) return 10;
    }
    if (candidates.Size() != Capacity::MaximumCatalogCandidates) return 11;
    if (candidates.Add(Candidate(9U, 9U)) != CatalogCandidateSetStatus::CapacityUnavailable) return 12;

    CatalogCandidate<Capacity> invalid;
    if (candidates.Add(invalid) != CatalogCandidateSetStatus::Invalid) return 13;

    candidates.Clear();
    if (!candidates.Empty() || candidates.Size() != 0U) return 14;

    return 0;
}
