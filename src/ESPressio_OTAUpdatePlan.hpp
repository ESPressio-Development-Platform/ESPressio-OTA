#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAComponentHandler.hpp"
#include "ESPressio_OTAManifest.hpp"

namespace ESPressio::OTA {

enum class UpdatePlanStatus : std::uint8_t {
    Success,
    InvalidManifest,
    HandlerDirectoryNotFrozen,
    HandlerUnavailable,
    ParameterSchemaUnsupported,
    MultiplicityViolation,
    DependencyCycle,
    CapacityUnavailable
};

template<typename TCapacityProfile>
struct UpdatePlanEntry final {
    ComponentIdentifier Identifier{};
    ComponentTypeId TypeId{};
    const ManifestComponent<TCapacityProfile>* ManifestComponentEntry{nullptr};
    IComponentHandler<TCapacityProfile>* Handler{nullptr};

    constexpr explicit operator bool() const noexcept {
        return bool(Identifier) && bool(TypeId) && ManifestComponentEntry != nullptr && Handler != nullptr;
    }
};

/**
 * Immutable deterministic execution plan.
 *
 * The source Manifest MUST outlive this plan. The plan never copies component
 * parameter payloads or Artifact associations; it retains read-only pointers into
 * the already trusted, canonical Manifest and resolved handler instances.
 */
template<typename TCapacityProfile>
class UpdatePlan final {
    std::array<UpdatePlanEntry<TCapacityProfile>, TCapacityProfile::MaximumComponents> entries_{};
    std::size_t count_{0U};
    bool ready_{false};

    template<typename TProfile>
    friend UpdatePlanStatus BuildUpdatePlan(
        const Manifest<TProfile>&,
        const ComponentHandlerDirectory<TProfile>&,
        UpdatePlan<TProfile>&) noexcept;

public:
    constexpr bool IsReady() const noexcept { return ready_; }
    constexpr std::size_t Size() const noexcept { return ready_ ? count_ : 0U; }

    constexpr const UpdatePlanEntry<TCapacityProfile>* Forward(std::size_t index) const noexcept {
        return ready_ && index < count_ ? &entries_[index] : nullptr;
    }

    constexpr const UpdatePlanEntry<TCapacityProfile>* Reverse(std::size_t index) const noexcept {
        return ready_ && index < count_ ? &entries_[count_ - 1U - index] : nullptr;
    }
};

namespace UpdatePlanDetail {

template<typename TCapacityProfile>
std::size_t FindManifestComponentIndex(
    const Manifest<TCapacityProfile>& manifest,
    std::uint32_t identifier) noexcept {
    for (std::size_t i = 0U; i < manifest.Components.size(); ++i) {
        if (manifest.Components[i].Identifier == identifier) return i;
    }
    return manifest.Components.size();
}

} // namespace UpdatePlanDetail

template<typename TCapacityProfile>
UpdatePlanStatus BuildUpdatePlan(
    const Manifest<TCapacityProfile>& manifest,
    const ComponentHandlerDirectory<TCapacityProfile>& handlers,
    UpdatePlan<TCapacityProfile>& output) noexcept {
    if (ValidateManifest(manifest) != ManifestStatus::Success) {
        return UpdatePlanStatus::InvalidManifest;
    }
    if (!handlers.IsFrozen()) return UpdatePlanStatus::HandlerDirectoryNotFrozen;
    if (manifest.Components.size() > output.entries_.size()) return UpdatePlanStatus::CapacityUnavailable;

    // Resolve every exact semantic Component Type before any execution starts.
    std::array<IComponentHandler<TCapacityProfile>*, TCapacityProfile::MaximumComponents> resolved{};
    for (std::size_t i = 0U; i < manifest.Components.size(); ++i) {
        const auto& component = manifest.Components[i];
        auto* handler = handlers.Find(ComponentTypeId{component.TypeId});
        if (handler == nullptr) return UpdatePlanStatus::HandlerUnavailable;
        const auto descriptor = handler->Descriptor();
        if (!descriptor || descriptor.TypeId.Value() != component.TypeId) {
            return UpdatePlanStatus::HandlerUnavailable;
        }
        if (descriptor.ParameterSchemaVersion != component.ParameterSchemaVersion) {
            return UpdatePlanStatus::ParameterSchemaUnsupported;
        }
        resolved[i] = handler;
    }

    // Enforce intrinsic single-instance component semantics.
    for (std::size_t i = 0U; i < manifest.Components.size(); ++i) {
        const auto descriptor = resolved[i]->Descriptor();
        if (descriptor.Multiplicity != ComponentMultiplicity::Single) continue;
        for (std::size_t j = i + 1U; j < manifest.Components.size(); ++j) {
            if (manifest.Components[j].TypeId == manifest.Components[i].TypeId) {
                return UpdatePlanStatus::MultiplicityViolation;
            }
        }
    }

    std::array<std::uint16_t, TCapacityProfile::MaximumComponents> indegree{};
    std::array<bool, TCapacityProfile::MaximumComponents> selected{};
    for (const auto& dependency : manifest.Dependencies) {
        const auto index = UpdatePlanDetail::FindManifestComponentIndex(manifest, dependency.Component);
        if (index == manifest.Components.size()) return UpdatePlanStatus::InvalidManifest;
        ++indegree[index];
    }

    output.count_ = 0U;
    output.ready_ = false;

    while (output.count_ < manifest.Components.size()) {
        std::size_t best = manifest.Components.size();
        for (std::size_t i = 0U; i < manifest.Components.size(); ++i) {
            if (selected[i] || indegree[i] != 0U) continue;
            if (best == manifest.Components.size() ||
                manifest.Components[i].Identifier < manifest.Components[best].Identifier) {
                best = i;
            }
        }
        if (best == manifest.Components.size()) {
            output.count_ = 0U;
            return UpdatePlanStatus::DependencyCycle;
        }

        const auto& component = manifest.Components[best];
        output.entries_[output.count_++] = {
            ComponentIdentifier{component.Identifier},
            ComponentTypeId{component.TypeId},
            &component,
            resolved[best]
        };
        selected[best] = true;

        for (const auto& dependency : manifest.Dependencies) {
            if (dependency.DependsOn != component.Identifier) continue;
            const auto dependent = UpdatePlanDetail::FindManifestComponentIndex(manifest, dependency.Component);
            if (dependent == manifest.Components.size() || indegree[dependent] == 0U) {
                output.count_ = 0U;
                return UpdatePlanStatus::InvalidManifest;
            }
            --indegree[dependent];
        }
    }

    output.ready_ = true;
    return UpdatePlanStatus::Success;
}

} // namespace ESPressio::OTA
