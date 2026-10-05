#pragma once

#include "Player.h"
#include "PartyManager.h"
#include "../Shared/RoomControlProtocol.h"
#include "DungeonCatalog.h"
#include "TownMap.h"
#include "../Shared/SkillTreeCatalog.h"
#include "Database/OdbcDatabase.h"

#include <asio.hpp>

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <map>
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
            DungeonCatalog inDungeonCatalog, const std::filesystem::path& inDataDirectory,
            std::shared_ptr<Database::OdbcDatabase> inDatabase);

        /// Handler runs on the town strand. Re-find players by ID and validate their current session/state.
        template <typename TProcedure, typename THandler>
        void RunStoreProcedure(std::unique_ptr<TProcedure> inProcedure, THandler inHandler)
        {
            database->Run(std::move(inProcedure), strand,
                [weakTown = weak_from_this(), handler = std::move(inHandler)](auto inResult) mutable
                {
                    if (const auto town = weakTown.lock())
                        std::invoke(handler, *town, std::move(inResult));
                });
        }

        using ProgressionChangedHandler = std::function<void(ActionRPG::RoomControlProtocol::RoomId,
            PlayerId, std::string)>;
        void SetProgressionChangedHandler(ProgressionChangedHandler inHandler);
        void RequestSkillState(PlayerId inPlayerId);
        void LearnSkill(PlayerId inPlayerId, std::string inSkillId, std::uint32_t inExpectedSkillLevel);
        void GetProgression(PlayerId inPlayerId, std::function<void(std::string)> inHandler);
        void AdvancePlayerLevel(PlayerId inPlayerId, std::uint32_t inLevel, std::function<void(bool)> inHandler);

        void Start();
        void Stop();
        void Enter(std::shared_ptr<Network::PlayerSession> inSession, std::string inPlayerName,
            std::uint32_t inCharacterId);
        void Leave(std::uint64_t inSessionId,
            std::function<void(ActionRPG::RoomControlProtocol::RoomId, PlayerId)> inDungeonLeaveHandler);
        void ApplyMovementInput(std::uint64_t inSessionId, TownProtocol::MoveInput inInput);
        void ValidateDungeonRequest(std::uint64_t inSessionId, std::string inZoneId,
            std::uint32_t inDungeonId, DungeonRequestHandler inHandler);
        void CompleteDungeonRequest(std::vector<PlayerId> inParticipantPlayerIds,
            ActionRPG::RoomControlProtocol::CreateRoomResult inResult);
        void ValidateDungeonJoin(PlayerId inPlayerId, ActionRPG::RoomControlProtocol::RoomId inRoomId,
            std::function<void(bool)> inHandler);
        void ValidateDungeonCompletion(std::uint64_t inSessionId,
            ActionRPG::RoomControlProtocol::RoomId inRoomId, DungeonRequestHandler inHandler);
        void CompleteDungeonCompletion(std::vector<PlayerId> inParticipants, bool inRetry,
            ActionRPG::RoomControlProtocol::FinishRoomResult inResult);
        void InviteToParty(std::uint64_t inSessionId, PlayerId inTargetPlayerId);
        void CreateParty(std::uint64_t inSessionId, std::string inTitle, bool inIsPublic);
        void AnswerPartyInvitation(std::uint64_t inSessionId, std::uint64_t inInvitationId,
            bool inAccepted);
        void LeaveParty(std::uint64_t inSessionId);
        void KickPartyMember(std::uint64_t inSessionId, PlayerId inTargetPlayerId);
        void UpdatePartySettings(std::uint64_t inSessionId, std::string inTitle, bool inIsPublic);
        void RequestPartyDirectoryPage(std::uint64_t inSessionId, std::uint32_t inPage);
        void RequestPartyDetail(std::uint64_t inSessionId, PartyManager::PartyId inPartyId);
        void RequestPartyJoin(std::uint64_t inSessionId, PartyManager::PartyId inPartyId);
        void AnswerPartyJoin(std::uint64_t inSessionId, std::uint64_t inRequestId, bool inAccepted);
        void UnsubscribePartyDirectory(std::uint64_t inSessionId);
        void EnterDungeon(PlayerId inPlayerId, ActionRPG::RoomControlProtocol::RoomId inRoomId,
            std::function<void(bool)> inHandler);
        void LeaveDungeon(PlayerId inPlayerId, ActionRPG::RoomControlProtocol::RoomId inRoomId,
            bool inNotify = true);
        void HandleRoomStarted(ActionRPG::RoomControlProtocol::RoomStarted inRoomStarted);
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
            ActionRPG::RoomControlProtocol::RoomId reservedDungeonRoomId{};
            std::string activeTransitionZoneId;
            ActionRPG::PlayerSkills::CharacterProgression progression;
            std::chrono::steady_clock::time_point lastSimulationTime{ std::chrono::steady_clock::now() };
        };

        void ScheduleTick();
        void Tick();
        bool SimulateMovement(PlayerId inPlayerId, PlayerEntry& inEntry,
            std::chrono::steady_clock::time_point inNow);
        void EnterOnStrand(std::shared_ptr<Network::PlayerSession> inSession, std::string inPlayerName,
            std::uint32_t inCharacterId);
        void LeaveOnStrand(std::uint64_t inSessionId);
        bool EnterDungeonOnStrand(PlayerId inPlayerId, ActionRPG::RoomControlProtocol::RoomId inRoomId);
        void HideFromTown(PlayerId inPlayerId, PlayerEntry& inEntry);
        void SendSkillState(PlayerEntry& inEntry, const std::string& inResult);
        void NotifyProgression(PlayerId inPlayerId, const PlayerEntry& inEntry);
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
        void RefreshDungeonLeader(ActionRPG::RoomControlProtocol::RoomId inRoomId);
        void SendPartyDirectoryPage(PlayerId inPlayerId, std::uint32_t inPage);
        void NotifyPartyDirectoryChanged();
        struct PendingPartyJoin
        {
            std::uint64_t requestId{};
            PartyManager::PartyId partyId{};
            PlayerId leaderPlayerId{};
            PlayerId requesterPlayerId{};
            std::string requesterName;
        };
        [[nodiscard]] bool IsPlayerPartyBusy(PlayerId inPlayerId) const;
        [[nodiscard]] TownProtocol::PartyResultCode ValidatePartyJoin(const PendingPartyJoin& inRequest) const;
        void SendPartyJoinUpdate(const PendingPartyJoin& inRequest,
            TownProtocol::PartyJoinRequestState inState, TownProtocol::PartyResultCode inResult);
        void PrunePartyJoinRequests();
        [[nodiscard]] bool IsPartyBusy(PartyManager::PartyId inPartyId) const;
        [[nodiscard]] SectorCoordinate GetSector(std::string_view inMapId,
            TownProtocol::Vector2 inPosition) const;
        [[nodiscard]] std::unordered_set<PlayerId> FindVisiblePlayers(PlayerId inPlayerId) const;

        asio::strand<asio::io_context::executor_type> strand;
        std::shared_ptr<Database::OdbcDatabase> database;
        asio::steady_timer tickTimer;
        std::chrono::steady_clock::time_point nextTickTime{};
        std::unordered_map<std::string, TownMap> maps;
        std::string defaultMapId;
        DungeonCatalog dungeonCatalog;
        const ActionRPG::PlayerSkills::ProgressionPolicy progressionPolicy;
        const ActionRPG::PlayerSkills::Catalog playerSkills;
        const ActionRPG::PlayerSkills::SkillTreeCatalog skillTrees;
        ProgressionChangedHandler progressionChangedHandler;
        std::unordered_map<PlayerId, PlayerEntry> players;
        std::unordered_map<std::uint64_t, PlayerId> sessionToPlayer;
        std::unordered_map<SectorCoordinate, std::unordered_set<PlayerId>, SectorHash> sectors;
        PartyManager partyManager;
        std::map<std::uint64_t, PendingPartyJoin> partyJoinRequests;
        std::uint64_t nextPartyJoinRequestId = 1;
        std::unordered_set<PlayerId> pendingDungeonPlayers;
        std::unordered_set<PlayerId> partyDirectorySubscribers;
        std::uint64_t partyDirectoryRevision{};
        PlayerId nextPlayerId = 1;
        std::uint32_t serverTick = 0;
        bool running = false;
    };
}
