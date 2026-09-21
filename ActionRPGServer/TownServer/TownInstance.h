#pragma once

#include "Player.h"
#include "TownMap.h"

#include <asio.hpp>

#include <cstdint>
#include <memory>
#include <string>
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
        TownInstance(asio::io_context& inIoContext, TownMap inMap);

        void Start();
        void Stop();
        void Enter(std::shared_ptr<Network::PlayerSession> inSession, std::string inPlayerName);
        void Leave(std::uint64_t inSessionId);
        void ApplyMovementInput(std::uint64_t inSessionId, TownProtocol::MoveInput inInput);

    private:
        struct SectorCoordinate
        {
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
            SectorCoordinate sector;
            std::unordered_set<PlayerId> visiblePlayers;
            TownProtocol::Vector2 lastBroadcastPosition;
            bool wasMovingOnLastBroadcast{};
        };

        void ScheduleTick();
        void Tick();
        void EnterOnStrand(std::shared_ptr<Network::PlayerSession> inSession, std::string inPlayerName);
        void LeaveOnStrand(std::uint64_t inSessionId);
        void RefreshVisibility(PlayerId inPlayerId);
        void BroadcastMovement();
        void AddToSector(PlayerId inPlayerId, SectorCoordinate inSector);
        void RemoveFromSector(PlayerId inPlayerId, SectorCoordinate inSector);
        void SendAppear(PlayerEntry& inReceiver, const PlayerEntry& inSubject);
        void SendDisappear(PlayerEntry& inReceiver, PlayerId inSubjectId);
        [[nodiscard]] SectorCoordinate GetSector(TownProtocol::Vector2 inPosition) const noexcept;
        [[nodiscard]] std::unordered_set<PlayerId> FindVisiblePlayers(PlayerId inPlayerId) const;

        asio::strand<asio::io_context::executor_type> strand;
        asio::steady_timer tickTimer;
        TownMap map;
        std::unordered_map<PlayerId, PlayerEntry> players;
        std::unordered_map<std::uint64_t, PlayerId> sessionToPlayer;
        std::unordered_map<SectorCoordinate, std::unordered_set<PlayerId>, SectorHash> sectors;
        PlayerId nextPlayerId = 1;
        std::uint32_t serverTick = 0;
        bool running = false;
    };
}
