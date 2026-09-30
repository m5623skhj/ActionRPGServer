#pragma once

#include "Player.h"
#include "PartyManager.h"
#include "../Shared/RoomControlProtocol.h"
#include "DungeonCatalog.h"
#include "TownMap.h"

#include <asio.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TownServer::Network
{
    class PlayerSession;
}

namespace TownServer::Domain
{
    class TownInstance final : public std::enable_shared_from_this<TownInstance>
    {
    public:
        using DungeonRequestHandler = std::function<void(bool, std::vector<PlayerId>)>;

        TownInstance(asio::io_context& inIoContext, std::vector<TownMap> inMaps,
            DungeonCatalog inDungeonCatalog);

        void Start();
        void Stop();
        void Enter(std::shared_ptr<Network::PlayerSession> inSession, std::string inPlayerName,
            std::uint32_t inCharacterId);
        void Leave(std::uint64_t inSessionId);
        void ApplyMovementInput(std::uint64_t inSessionId, TownProtocol::MoveInput inInput);
        void ValidateDungeonRequest(std::uint64_t inSessionId, std::string inZoneId,
            std::uint32_t inDungeonId, DungeonRequestHandler inHandler);
        void CompleteDungeonRequest(std::vector<PlayerId> inParticipantPlayerIds,
            ActionRPG::RoomControlProtocol::CreateRoomResult inResult);
        void InviteToParty(std::uint64_t inSessionId, PlayerId inTargetPlayerId);
        void CreateParty(std::uint64_t inSessionId, std::string inTitle, bool inIsPublic);
        void AnswerPartyInvitation(std::uint64_t inSessionId, std::uint64_t inInvitationId,
            bool inAccepted);
        void LeaveParty(std::uint64_t inSessionId);
        void KickPartyMember(std::uint64_t inSessionId, PlayerId inTargetPlayerId);
        void UpdatePartySettings(std::uint64_t inSessionId, std::string inTitle, bool inIsPublic);
        void RequestPartyDirectoryPage(std::uint64_t inSessionId, std::uint32_t inPage);
        void UnsubscribePartyDirectory(std::uint64_t inSessionId);
        void EnterDungeon(PlayerId inPlayerId, ActionRPG::RoomControlProtocol::RoomId inRoomId);
        void LeaveDungeon(PlayerId inPlayerId, ActionRPG::RoomControlProtocol::RoomId inRoomId);
        void HandleRoomEnded(ActionRPG::RoomControlProtocol::RoomEnded inRoomEnded);

    private:
        struct SectorCoordinate
        {
            std::string mapId;
            int x{};
            int y{};

            bool operator==(const SectorCoordinate&) const = default;
        };

        struct SectorHash
        {
            [[nodiscard]] std::size_t operator()(const SectorCoordinate& inCoordinate) const noexcept;
        };

        struct PlayerEntry
        {
            Player player;
            std::shared_ptr<Network::PlayerSession> session;
            std::string mapId;
            SectorCoordinate sector;
            std::unordered_set<PlayerId> visiblePlayers;
            TownProtocol::Vector2 lastBroadcastPosition;
            bool wasMovingOnLastBroadcast{};
            ActionRPG::RoomControlProtocol::RoomId dungeonRoomId{};
            std::string activeTransitionZoneId;
        };

        void ScheduleTick();
        void Tick();
        void EnterOnStrand(std::shared_ptr<Network::PlayerSession> inSession, std::string inPlayerName,
            std::uint32_t inCharacterId);
        void LeaveOnStrand(std::uint64_t inSessionId);
        void HideFromTown(PlayerId inPlayerId, PlayerEntry& inEntry);
        void ProcessTransition(PlayerId inPlayerId, PlayerEntry& inEntry);
        bool TransferMap(PlayerId inPlayerId, PlayerEntry& inEntry,
            const TownProtocol::TransitionZone& inZone);
        void RefreshVisibility(PlayerId inPlayerId);
        void BroadcastMovement();
        void AddToSector(PlayerId inPlayerId, SectorCoordinate inSector);
        void RemoveFromSector(PlayerId inPlayerId, SectorCoordinate inSector);
        void SendAppear(PlayerEntry& inReceiver, const PlayerEntry& inSubject);
        void SendDisappear(PlayerEntry& inReceiver, PlayerId inSubjectId);
        void SendPartyResult(PlayerId inPlayerId, TownProtocol::PartyOperationType inOperation,
            TownProtocol::PartyResultCode inResult);
        void SendEmptyPartySnapshot(PlayerId inPlayerId);
        void BroadcastPartySnapshot(PartyManager::PartyId inPartyId);
        void SendPartyDirectoryPage(PlayerId inPlayerId, std::uint32_t inPage);
        void NotifyPartyDirectoryChanged();
        [[nodiscard]] bool IsPartyBusy(PartyManager::PartyId inPartyId) const;
        [[nodiscard]] SectorCoordinate GetSector(std::string_view inMapId,
            TownProtocol::Vector2 inPosition) const;
        [[nodiscard]] std::unordered_set<PlayerId> FindVisiblePlayers(PlayerId inPlayerId) const;

        asio::strand<asio::io_context::executor_type> strand;
        asio::steady_timer tickTimer;
        std::unordered_map<std::string, TownMap> maps;
        std::string defaultMapId;
        DungeonCatalog dungeonCatalog;
        std::unordered_map<PlayerId, PlayerEntry> players;
        std::unordered_map<std::uint64_t, PlayerId> sessionToPlayer;
        std::unordered_map<SectorCoordinate, std::unordered_set<PlayerId>, SectorHash> sectors;
        PartyManager partyManager;
        std::unordered_set<PlayerId> pendingDungeonPlayers;
        std::unordered_set<PlayerId> partyDirectorySubscribers;
        std::uint64_t partyDirectoryRevision{};
        PlayerId nextPlayerId = 1;
        std::uint32_t serverTick = 0;
        bool running = false;
    };
}
