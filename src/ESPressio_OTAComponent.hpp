#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ESPressio_OTACapacityProfile.hpp"
#include "ESPressio_OTATypes.hpp"

namespace ESPressio::OTA {

struct ComponentTypeDescriptor final {
    ComponentTypeId TypeId{};
    const char* CanonicalName{nullptr};
    ComponentKind Kind{ComponentKind::PlatformSpecific};
    ComponentMultiplicity Multiplicity{ComponentMultiplicity::Single};
    std::uint16_t ParameterSchemaVersion{1U};

    constexpr explicit operator bool() const noexcept {
        return bool(TypeId) && CanonicalName != nullptr && CanonicalName[0] != '\0' && ParameterSchemaVersion != 0U;
    }
};

enum class DirectoryStatus : std::uint8_t {
    Success,
    Frozen,
    CapacityUnavailable,
    InvalidDescriptor,
    DuplicateIdentifier,
    DuplicateCanonicalName
};

template<typename TCapacityProfile>
class ComponentTypeDirectory final {
    static_assert(TCapacityProfile::IsValid, "OTA ComponentTypeDirectory requires a valid capacity profile");
    std::array<ComponentTypeDescriptor, TCapacityProfile::MaximumSupportedComponentTypes> entries_{};
    std::size_t count_{0};
    bool frozen_{false};
public:
    constexpr std::size_t Size() const noexcept { return count_; }
    constexpr bool IsFrozen() const noexcept { return frozen_; }

    DirectoryStatus Register(ComponentTypeDescriptor descriptor) noexcept {
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

    const ComponentTypeDescriptor* Find(ComponentTypeId typeId) const noexcept {
        if (!frozen_ || !typeId) return nullptr;
        for (std::size_t i = 0; i < count_; ++i) if (entries_[i].TypeId == typeId) return &entries_[i];
        return nullptr;
    }
};

template<typename T>
struct ComponentTypeTraits;

template<typename T>
constexpr ComponentTypeDescriptor DescribeComponentType() noexcept {
    return ComponentTypeTraits<T>::Describe();
}

} // namespace ESPressio::OTA
