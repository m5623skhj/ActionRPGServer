#pragma once
#include "Database/ItemUseStoreProcedure.h"
#include <memory>
#include <asio.hpp>

namespace TownServer::Network { class PlayerSession; }
namespace TownServer::Domain
{
    class TownInstance;
    /// Town-strand coordinator. DB commits and room receipts form a saga, never a cross-process atomic transaction.
    class ItemUseService final : public std::enable_shared_from_this<ItemUseService>
    {
    public:
        using Json = ActionRPG::Items::Json;
        ItemUseService(std::shared_ptr<TownInstance> inTown, std::shared_ptr<Network::PlayerSession> inSession,
            Persistence::ItemUseRequest inIdentity, Persistence::Response inSnapshot = {});
        void Use(Json inCommand);
        void Query(std::string inRequestId, bool inInventoryState = false);
        void RecoverClaim(std::string inSelectionId);
        void ReconcileRelease();
        void ClientDisconnected();
        bool IsPending() const noexcept { return !ownershipLost && (pending || inFlight); }
    private:
        void Call(Persistence::ItemUseRequest inRequest, std::function<void(std::optional<Persistence::Response>)> inHandler);
        void Room(Json inRequest, std::function<void(Json)> inHandler);
        void Prepare();
        void Resolve(bool inAllowExecute);
        void Finalize(const std::string& inRoomStatus, const std::string& inReason);
        void SettleTerminal();
        void CancelUnreserved(const std::string& inResult);
        void FenceClaim();
        void Publish(const std::string& inResult = {});
        void SendBusy(const std::string& inRequestId, const char* inAction);
        void ScheduleClaimRecovery();
        bool ApplySnapshot(Persistence::Response inSnapshot);
        Json Control(const char* inKind) const;
        std::weak_ptr<TownInstance> town;
        std::weak_ptr<Network::PlayerSession> session;
        std::uint64_t sessionId{}, playerId{};
        Persistence::ItemUseRequest identity;
        Persistence::Response snapshot;
        Json command, definition, execution, stagedPrepare;
        std::string requestId, replyRequestId, stagedRequestId, action{"Use"}, selectionId;
        std::optional<Persistence::ItemUseRow> claimOrigin;
        bool pending{}, inFlight{}, stateQuery{}, recoveringClaim{}, claimFenced{}, ownershipLost{};
        std::shared_ptr<asio::steady_timer> recoveryTimer;
    };
}
