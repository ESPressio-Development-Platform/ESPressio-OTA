#include <array>
#include <cstddef>
#include <cstdint>

#include "ESPressio_OTA.hpp"

using namespace ESPressio::OTA;

namespace {

using Capacity = ConstrainedV1CapacityProfile;

struct TestComponent {};

} // namespace

namespace ESPressio::OTA {

template<>
struct ComponentTypeTraits<TestComponent> {
    static constexpr ComponentTypeDescriptor Describe() noexcept {
        return {
            ComponentTypeId{0x5101U},
            "test.component",
            ComponentKind::Data,
            ComponentMultiplicity::Single,
            1U
        };
    }
};

} // namespace ESPressio::OTA

namespace {

constexpr std::array<std::uint8_t, 16> Id(std::uint8_t value) noexcept {
    std::array<std::uint8_t, 16> result{};
    result[15] = value;
    return result;
}

class FakeVerifiedReader final : public IVerifiedArtifactReader<Capacity> {
    VerifiedArtifactDescriptor<Capacity> descriptor_{};
public:
    explicit FakeVerifiedReader(std::uint8_t id) noexcept {
        descriptor_.Identifier = ArtifactIdentifier{Id(id)};
        descriptor_.Length = 4U;
        descriptor_.DigestAlgorithm = ESPressio::Security::DigestAlgorithm::SHA256;
        for (std::size_t i = 0U; i < 32U; ++i) {
            (void)descriptor_.Digest.push_back(static_cast<std::uint8_t>(i + id));
        }
    }

    const VerifiedArtifactDescriptor<Capacity>& Descriptor() const noexcept override {
        return descriptor_;
    }

    Result Reset() noexcept override { return Result::Success(); }

    StreamReadResult Read(std::uint8_t* output, std::size_t capacity) noexcept override {
        if (output == nullptr || capacity == 0U) {
            return StreamReadResult::Failed({OutcomeClass::Invalid, {DiagnosticDomain::Component, 1U}});
        }
        output[0] = 0x5AU;
        return StreamReadResult::Data(1U);
    }
};

class TestHandler final : public ComponentHandler<TestComponent, Capacity> {
public:
    ComponentActionResult Stage(const ComponentExecutionContext<Capacity>&) noexcept override {
        return ComponentActionResult::Pending();
    }

    ComponentActionResult Activate(const ComponentExecutionContext<Capacity>&) noexcept override {
        return ComponentActionResult::RestartRequired();
    }

    ComponentRecoveryInspection InspectRecoveryState(
        const ComponentPreflightContext<Capacity>&) noexcept override {
        return {ComponentRecoveryState::Staged, Result::Success()};
    }
};

} // namespace

int main() {
    TestHandler handler;
    const auto descriptor = handler.Descriptor();
    if (!descriptor || descriptor.TypeId != ComponentTypeId{0x5101U}) return 1;
    if (descriptor.Kind != ComponentKind::Data || descriptor.Multiplicity != ComponentMultiplicity::Single) return 2;

    ComponentHandlerDirectory<Capacity> directory;
    if (directory.Register(handler) != ComponentHandlerDirectoryStatus::Success) return 3;
    if (directory.Register(handler) != ComponentHandlerDirectoryStatus::DuplicateType) return 4;
    if (directory.Find(ComponentTypeId{0x5101U}) != nullptr) return 5;
    directory.Freeze();
    if (!directory.IsFrozen() || directory.Size() != 1U) return 6;
    if (directory.Find(ComponentTypeId{0x5101U}) != &handler) return 7;
    if (directory.Register(handler) != ComponentHandlerDirectoryStatus::Frozen) return 8;

    FakeVerifiedReader first{1U};
    FakeVerifiedReader second{2U};
    VerifiedComponentArtifacts<Capacity> artifacts;
    if (!artifacts.Add(second) || !artifacts.Add(first)) return 9;
    if (artifacts.Size() != 2U) return 10;
    if (artifacts.Find(ArtifactIdentifier{Id(1U)}) != &first) return 11;
    if (artifacts.Find(ArtifactIdentifier{Id(2U)}) != &second) return 12;
    if (artifacts.Add(first).Outcome != OutcomeClass::Invalid) return 13;

    ComponentPreflightContext<Capacity> preflight;
    if (handler.Preflight(preflight).Outcome != OutcomeClass::Success) return 14;
    if (handler.Prepare(preflight).Status != ComponentActionStatus::Complete) return 15;

    ComponentExecutionContext<Capacity> execution;
    if (handler.Stage(execution).Status != ComponentActionStatus::Pending) return 16;
    if (handler.Activate(execution).Status != ComponentActionStatus::RestartRequired) return 17;
    if (handler.FinalizeStage(execution).Status != ComponentActionStatus::Complete) return 18;
    if (handler.Commit(execution).Status != ComponentActionStatus::Complete) return 19;
    if (handler.Rollback(execution).Status != ComponentActionStatus::Complete) return 20;
    if (handler.Cleanup(execution).Status != ComponentActionStatus::Complete) return 21;

    const auto recovery = handler.InspectRecoveryState(preflight);
    if (recovery.State != ComponentRecoveryState::Staged || recovery.Detail.Outcome != OutcomeClass::Success) return 22;

    return 0;
}
