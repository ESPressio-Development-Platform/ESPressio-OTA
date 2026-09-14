#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTAComponent.hpp"
#include "ESPressio_OTAManifest.hpp"
#include "ESPressio_OTAProfile.hpp"
#include "ESPressio_OTAProviders.hpp"
#include "ESPressio_OTAStage.hpp"

namespace ESPressio::OTA {

enum class ComponentActionStatus : std::uint8_t {
    Complete,
    Pending,
    RestartRequired,
    Failed
};

struct ComponentActionResult final {
    ComponentActionStatus Status{ComponentActionStatus::Failed};
    Result Detail{OutcomeClass::Failed, {}};

    constexpr explicit operator bool() const noexcept {
        return Status == ComponentActionStatus::Complete && Detail.Outcome == OutcomeClass::Success;
    }

    static constexpr ComponentActionResult Complete() noexcept {
        return {ComponentActionStatus::Complete, Result::Success()};
    }
    static constexpr ComponentActionResult Pending() noexcept {
        return {ComponentActionStatus::Pending, {OutcomeClass::Pending, {}}};
    }
    static constexpr ComponentActionResult RestartRequired() noexcept {
        return {ComponentActionStatus::RestartRequired, Result::Success()};
    }
    static constexpr ComponentActionResult Failed(Result detail) noexcept {
        return {ComponentActionStatus::Failed, detail};
    }
};

enum class ComponentRecoveryState : std::uint8_t {
    NotPrepared,
    PartiallyStaged,
    Staged,
    Activated,
    Committed,
    RolledBack,
    Inconsistent
};

struct ComponentRecoveryInspection final {
    ComponentRecoveryState State{ComponentRecoveryState::Inconsistent};
    Result Detail{OutcomeClass::Failed, {}};
};

template<typename TCapacityProfile>
class VerifiedComponentArtifacts final {
    struct Binding final {
        ArtifactIdentifier Identifier{};
        IVerifiedArtifactReader<TCapacityProfile>* Reader{nullptr};
    };

    std::array<Binding, TCapacityProfile::MaximumArtifactsPerComponent> bindings_{};
    std::size_t count_{0U};
public:
    std::size_t Size() const noexcept { return count_; }

    Result Add(IVerifiedArtifactReader<TCapacityProfile>& reader) noexcept {
        const auto& descriptor = reader.Descriptor();
        if (!descriptor.IsValid()) return {OutcomeClass::Invalid, {DiagnosticDomain::Component, 1U}};
        for (std::size_t i = 0U; i < count_; ++i) {
            if (bindings_[i].Identifier == descriptor.Identifier) {
                return {OutcomeClass::Invalid, {DiagnosticDomain::Component, 2U}};
            }
        }
        if (count_ == bindings_.size()) {
            return {OutcomeClass::CapacityUnavailable, {DiagnosticDomain::Component, 3U}};
        }

        std::size_t position = count_;
        while (position != 0U && descriptor.Identifier < bindings_[position - 1U].Identifier) {
            bindings_[position] = bindings_[position - 1U];
            --position;
        }
        bindings_[position] = {descriptor.Identifier, &reader};
        ++count_;
        return Result::Success();
    }

    IVerifiedArtifactReader<TCapacityProfile>* Find(ArtifactIdentifier identifier) const noexcept {
        for (std::size_t i = 0U; i < count_; ++i) {
            if (bindings_[i].Identifier == identifier) return bindings_[i].Reader;
        }
        return nullptr;
    }
};

template<typename TCapacityProfile>
struct ComponentPreflightContext final {
    const ManifestComponent<TCapacityProfile>* Component{nullptr};
    const UpdateTargetProfile<TCapacityProfile>* TargetProfile{nullptr};
    UpdateTransactionId Transaction{};
    UpdateGenerationId CandidateGeneration{};

    bool IsValid() const noexcept {
        return Component != nullptr && TargetProfile != nullptr && TargetProfile->IsFrozen() &&
               Component->Identifier != 0U && Component->TypeId != 0U &&
               bool(Transaction) && bool(CandidateGeneration);
    }
};

template<typename TCapacityProfile>
struct ComponentExecutionContext final {
    ComponentPreflightContext<TCapacityProfile> Base{};
    const VerifiedComponentArtifacts<TCapacityProfile>* Artifacts{nullptr};

    bool IsValid() const noexcept {
        return Base.IsValid() && Artifacts != nullptr;
    }
};

template<typename TCapacityProfile>
class IComponentHandler {
public:
    virtual ~IComponentHandler() = default;
    virtual ComponentTypeDescriptor Descriptor() const noexcept = 0;

    virtual Result Preflight(const ComponentPreflightContext<TCapacityProfile>&) noexcept {
        return Result::Success();
    }
    virtual ComponentActionResult Prepare(const ComponentPreflightContext<TCapacityProfile>&) noexcept {
        return ComponentActionResult::Complete();
    }
    virtual ComponentActionResult Stage(const ComponentExecutionContext<TCapacityProfile>&) noexcept {
        return ComponentActionResult::Complete();
    }
    virtual ComponentActionResult FinalizeStage(const ComponentExecutionContext<TCapacityProfile>&) noexcept {
        return ComponentActionResult::Complete();
    }
    virtual ComponentActionResult Activate(const ComponentExecutionContext<TCapacityProfile>&) noexcept {
        return ComponentActionResult::Complete();
    }
    virtual ComponentActionResult Commit(const ComponentExecutionContext<TCapacityProfile>&) noexcept {
        return ComponentActionResult::Complete();
    }
    virtual ComponentActionResult Rollback(const ComponentExecutionContext<TCapacityProfile>&) noexcept {
        return ComponentActionResult::Complete();
    }
    virtual ComponentActionResult Cleanup(const ComponentExecutionContext<TCapacityProfile>&) noexcept {
        return ComponentActionResult::Complete();
    }

    virtual ComponentRecoveryInspection InspectRecoveryState(
        const ComponentPreflightContext<TCapacityProfile>& context) noexcept = 0;

    virtual bool CurrentProgress(StageProgress&) const noexcept { return false; }
};

/** Compile-time Component Type binding over the bounded type-erased runtime bridge. */
template<typename TComponent, typename TCapacityProfile>
class ComponentHandler : public IComponentHandler<TCapacityProfile> {
public:
    using Component = TComponent;
    using CapacityProfile = TCapacityProfile;

    ComponentTypeDescriptor Descriptor() const noexcept final {
        return DescribeComponentType<TComponent>();
    }
};

enum class ComponentHandlerDirectoryStatus : std::uint8_t {
    Success,
    Frozen,
    CapacityUnavailable,
    InvalidHandler,
    DuplicateType
};

/**
 * Frozen TypeId-to-handler dispatch bridge. It is constructed explicitly from
 * composition-bound provider instances and is not a general runtime service locator.
 */
template<typename TCapacityProfile>
class ComponentHandlerDirectory final {
    struct Entry final {
        ComponentTypeId TypeId{};
        IComponentHandler<TCapacityProfile>* Handler{nullptr};
    };
    std::array<Entry, TCapacityProfile::MaximumSupportedComponentTypes> entries_{};
    std::size_t count_{0U};
    bool frozen_{false};
public:
    bool IsFrozen() const noexcept { return frozen_; }
    std::size_t Size() const noexcept { return count_; }

    template<typename TComponent>
    ComponentHandlerDirectoryStatus Register(ComponentHandler<TComponent, TCapacityProfile>& handler) noexcept {
        if (frozen_) return ComponentHandlerDirectoryStatus::Frozen;
        const auto descriptor = handler.Descriptor();
        if (!descriptor) return ComponentHandlerDirectoryStatus::InvalidHandler;
        for (std::size_t i = 0U; i < count_; ++i) {
            if (entries_[i].TypeId == descriptor.TypeId) return ComponentHandlerDirectoryStatus::DuplicateType;
        }
        if (count_ == entries_.size()) return ComponentHandlerDirectoryStatus::CapacityUnavailable;

        std::size_t position = count_;
        while (position != 0U && descriptor.TypeId < entries_[position - 1U].TypeId) {
            entries_[position] = entries_[position - 1U];
            --position;
        }
        entries_[position] = {descriptor.TypeId, &handler};
        ++count_;
        return ComponentHandlerDirectoryStatus::Success;
    }

    void Freeze() noexcept { frozen_ = true; }

    IComponentHandler<TCapacityProfile>* Find(ComponentTypeId typeId) const noexcept {
        if (!frozen_ || !typeId) return nullptr;
        for (std::size_t i = 0U; i < count_; ++i) {
            if (entries_[i].TypeId == typeId) return entries_[i].Handler;
        }
        return nullptr;
    }
};

} // namespace ESPressio::OTA
