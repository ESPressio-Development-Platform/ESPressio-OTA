#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ESPressio_OTACapacityProfile.hpp"
#include "ESPressio_OTAComponent.hpp"
#include "ESPressio_OTATypes.hpp"

namespace ESPressio::OTA {

struct StageDescriptor final {
    StageTypeId TypeId{};
    const char* CanonicalName{nullptr};
    UpdateOperation Operation{UpdateOperation::Check};
    ProgressMode Mode{ProgressMode::Indeterminate};
    ProgressUnit Unit{ProgressUnit::None};
    ComponentTypeId ComponentType{};

    constexpr explicit operator bool() const noexcept {
        return bool(TypeId) && CanonicalName != nullptr && CanonicalName[0] != '\0';
    }
};

struct StageProgress final {
    StageTypeId Stage{};
    std::uint16_t StagePosition{0};
    ProgressMode Mode{ProgressMode::Indeterminate};
    ProgressUnit Unit{ProgressUnit::None};
    std::uint64_t Current{0};
    std::uint64_t Total{0};
    ComponentIdentifier Component{};

    constexpr bool IsValid() const noexcept {
        if (!Stage) return false;
        if (Mode == ProgressMode::Determinate && Current > Total) return false;
        if (Mode == ProgressMode::Indeterminate && (Current != 0U || Total != 0U)) return false;
        return true;
    }

    constexpr bool IsComplete() const noexcept {
        return Mode == ProgressMode::Determinate && Total != 0U && Current == Total;
    }
};

template<typename TCapacityProfile>
class StageDirectory final {
    static_assert(TCapacityProfile::IsValid, "OTA StageDirectory requires a valid capacity profile");
    std::array<StageDescriptor, TCapacityProfile::MaximumStageDescriptors> entries_{};
    std::size_t count_{0};
    bool frozen_{false};
public:
    constexpr std::size_t Size() const noexcept { return count_; }
    constexpr bool IsFrozen() const noexcept { return frozen_; }

    DirectoryStatus Register(StageDescriptor descriptor) noexcept {
        if (frozen_) return DirectoryStatus::Frozen;
        if (!descriptor) return DirectoryStatus::InvalidDescriptor;
        if (count_ == entries_.size()) return DirectoryStatus::CapacityUnavailable;
        const std::string_view candidateName{descriptor.CanonicalName};
        for (std::size_t i = 0; i < count_; ++i) {
            if (entries_[i].TypeId == descriptor.TypeId) return DirectoryStatus::DuplicateIdentifier;
            if (std::string_view{entries_[i].CanonicalName} == candidateName) return DirectoryStatus::DuplicateCanonicalName;
        }
        entries_[count_++] = descriptor;
        return DirectoryStatus::Success;
    }

    void Freeze() noexcept { frozen_ = true; }

    const StageDescriptor* Find(StageTypeId typeId) const noexcept {
        if (!frozen_ || !typeId) return nullptr;
        for (std::size_t i = 0; i < count_; ++i) if (entries_[i].TypeId == typeId) return &entries_[i];
        return nullptr;
    }
};

} // namespace ESPressio::OTA
