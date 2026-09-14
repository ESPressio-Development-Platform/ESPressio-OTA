#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAProviders.hpp"

namespace ESPressio::OTA {

enum class CatalogCandidateSetStatus : std::uint8_t {
    Added,
    AlreadyPresent,
    Conflict,
    CapacityUnavailable,
    Invalid
};

/**
 * Bounded cross-provider candidate aggregation.
 *
 * Storage order is canonical by ManifestIdentifier then ReleaseIdentifier and
 * therefore never conveys provider priority or discovery order.
 */
template<typename TCapacityProfile>
class CatalogCandidateSet final {
    static_assert(TCapacityProfile::IsValid, "CatalogCandidateSet requires a valid OTA capacity profile");
    std::array<CatalogCandidate<TCapacityProfile>, TCapacityProfile::MaximumCatalogCandidates> candidates_{};
    std::size_t count_{0U};

    static constexpr bool Less(
        const CatalogCandidate<TCapacityProfile>& left,
        const CatalogCandidate<TCapacityProfile>& right) noexcept {
        if (left.Manifest < right.Manifest) return true;
        if (right.Manifest < left.Manifest) return false;
        return left.Release < right.Release;
    }

public:
    constexpr std::size_t Size() const noexcept { return count_; }
    constexpr bool Empty() const noexcept { return count_ == 0U; }

    constexpr const CatalogCandidate<TCapacityProfile>* At(std::size_t index) const noexcept {
        return index < count_ ? &candidates_[index] : nullptr;
    }

    CatalogCandidateSetStatus Add(const CatalogCandidate<TCapacityProfile>& candidate) noexcept {
        if (!candidate.IsValid()) return CatalogCandidateSetStatus::Invalid;

        for (std::size_t i = 0U; i < count_; ++i) {
            const auto& current = candidates_[i];
            if (current.Release == candidate.Release) {
                return current.Manifest == candidate.Manifest
                    ? CatalogCandidateSetStatus::AlreadyPresent
                    : CatalogCandidateSetStatus::Conflict;
            }
            if (current.Manifest == candidate.Manifest && current.Release != candidate.Release) {
                return CatalogCandidateSetStatus::Conflict;
            }
        }

        if (count_ == candidates_.size()) return CatalogCandidateSetStatus::CapacityUnavailable;

        std::size_t position = count_;
        while (position != 0U && Less(candidate, candidates_[position - 1U])) {
            candidates_[position] = candidates_[position - 1U];
            --position;
        }
        candidates_[position] = candidate;
        ++count_;
        return CatalogCandidateSetStatus::Added;
    }

    constexpr void Clear() noexcept {
        count_ = 0U;
    }
};

} // namespace ESPressio::OTA
