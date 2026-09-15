#pragma once

#include "ESPressio_OTACommands.hpp"

namespace ESPressio::OTA::Integration {

/**
 * Thin optional Command-family adapter over the authoritative Coordinator API.
 *
 * Command owns CommandExecutionKey, duplicate/replay authority and response
 * retention. This adapter owns no second transaction registry and never carries
 * Manifest/Artifact bytes beyond the semantic ManifestIdentifier in Start.
 */
template<class TCoordinator>
class OTACommandBridge final {
    TCoordinator& coordinator_;
    const IOTACommandAuthorizer* authorizer_{nullptr};

    bool Authorized(const Command::CommandExecutionContext& context,
                    OTACommandOperation operation) const noexcept {
        if (context.IsLocal()) return true;
        return authorizer_ != nullptr && authorizer_->AuthorizeRemote(context, operation);
    }

public:
    explicit OTACommandBridge(TCoordinator& coordinator,
                              const IOTACommandAuthorizer* authorizer = nullptr) noexcept
        : coordinator_(coordinator), authorizer_(authorizer) {}

    void SetAuthorizer(const IOTACommandAuthorizer* authorizer) noexcept {
        authorizer_ = authorizer;
    }

    OTACommandResultValue HandleStart(
        const StartUpdateCommand& command,
        const Command::CommandExecutionContext& context) noexcept {
        if (!Authorized(context, OTACommandOperation::StartUpdate)) {
            return OTACommandResultValue::Unauthorized();
        }

        CoordinatorStartRequest request;
        request.Release = ReleaseIdentifier{command.Release};
        request.Manifest = ManifestIdentifier{command.Manifest};
        request.CandidateSecurity = SecurityGeneration{command.CandidateSecurityGeneration};

        UpdateTransactionId transaction{};
        const auto result = coordinator_.Start(request, transaction);
        return OTACommandResultValue::From(result, transaction);
    }

    OTACommandResultValue HandleCancel(
        const CancelUpdateCommand& command,
        const Command::CommandExecutionContext& context) noexcept {
        if (!Authorized(context, OTACommandOperation::CancelUpdate)) {
            return OTACommandResultValue::Unauthorized();
        }

        const UpdateTransactionId transaction{command.Transaction};
        const auto result = coordinator_.Cancel(transaction);
        return OTACommandResultValue::From(result, transaction);
    }
};

} // namespace ESPressio::OTA::Integration
