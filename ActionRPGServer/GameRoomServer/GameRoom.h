#pragma once

#include "../Shared/RoomControlProtocol.h"
#include "DungeonDefinition.h"
#include "DungeonProtocol.h"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace GameRoomServer
{
    class GameRoom final : public std::enable_shared_from_this<GameRoom>
    {
    public:
        using PlayerId = ActionRPG::RoomControlProtocol::PlayerId;
        using RoomId = ActionRPG::RoomControlProtocol::RoomId;
        using EnterResultHandler = std::function<void(bool)>;
        using LeaveResultHandler = std::function<void(bool, bool)>;
        using EmptyHandler = std::function<void(RoomId)>;

        GameRoom(asio::io_context& inIoContext, RoomId inRoomId, std::uint32_t inDungeonId,
            std::uint64_t inCombatSeed, std::vector<PlayerId> inExpectedPlayerIds,
            std::chrono::milliseconds inEnterTimeout, EmptyHandler inEmptyHandler,
            std::shared_ptr<const DungeonDefinition> inDefinition);

        void Start();
        void Stop();
        void TryEnter(PlayerId inPlayerId, EnterResultHandler inResultHandler);
        void Leave(PlayerId inPlayerId, LeaveResultHandler inResultHandler);
        void RemoveUnannouncedPlayer(PlayerId inPlayerId, std::function<void(bool)> inResultHandler);
        void CompleteDungeon(std::function<void(std::vector<PlayerId>)> inResultHandler);
        [[nodiscard]] std::shared_ptr<const std::string> GetWorldFor(PlayerId inPlayerId) const;
        void UpdateInput(PlayerId inPlayerId, ActionRPG::DungeonProtocol::DungeonMoveInput inInput,
            std::function<void(ActionRPG::DungeonProtocol::DungeonPlayerState)> inHandler);

        [[nodiscard]] RoomId GetRoomId() const noexcept;
        [[nodiscard]] std::uint64_t GetCombatSeed() const noexcept;
        [[nodiscard]] const DungeonDefinition& GetDefinition() const noexcept { return *definition; }

    private:
        enum class State
        {
            WaitingForPlayers,
            Running,
            Cleared,
            Stopped
        };

        void HandleEnterTimeout();
        void StartDungeon();
        void ScheduleTick();
        void UpdatePlayers(float inDeltaSeconds);

        struct PlayerState
        {
            std::string mapId;
            DungeonPoint position;
            std::int8_t directionX{};
            std::int8_t directionY{};
            bool running{};
            std::uint32_t sequence{};
            float walkSpeed{ 280.0f };
            float runSpeed{ 480.0f };
            bool warpArmed{ true };
            std::chrono::steady_clock::time_point lastInput{};
        };

        // Definitions are shared read-only; each room owns HP and the current AI node.
        // Future updates to these instances must run on this room's strand.
        struct MonsterState
        {
            std::shared_ptr<const MonsterDefinition> definition;
            std::string mapId;
            std::string placementId;
            std::string aiNodeId;
            DungeonPoint position;
            bool facingLeft{};
            std::uint32_t hp{};
        };

        asio::strand<asio::io_context::executor_type> strand;
        asio::steady_timer enterTimer;
        asio::steady_timer tickTimer;
        RoomId roomId;
        std::uint32_t dungeonId;
        std::uint64_t combatSeed;
        const std::shared_ptr<const DungeonDefinition> definition;
        nlohmann::json dungeonWorld;
        std::unordered_map<std::uint64_t, MonsterState> monsters;
        std::unordered_map<PlayerId, PlayerState> players;
        std::unordered_map<PlayerId, std::shared_ptr<const std::string>> initialWorlds;
        std::unordered_set<PlayerId> expectedPlayers;
        std::unordered_set<PlayerId> enteredPlayers;
        std::chrono::milliseconds enterTimeout;
        EmptyHandler emptyHandler;
        State state = State::WaitingForPlayers;
        std::uint64_t serverTick{};
    };
}
