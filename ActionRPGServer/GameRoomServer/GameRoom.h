#pragma once

#include "../Shared/RoomControlProtocol.h"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_set>
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
            std::chrono::milliseconds inEnterTimeout, EmptyHandler inEmptyHandler);

        void Start();
        void Stop();
        void TryEnter(PlayerId inPlayerId, EnterResultHandler inResultHandler);
        void Leave(PlayerId inPlayerId, LeaveResultHandler inResultHandler);
        void RemoveUnannouncedPlayer(PlayerId inPlayerId, std::function<void(bool)> inResultHandler);
        void CompleteDungeon(std::function<void(std::vector<PlayerId>)> inResultHandler);

        [[nodiscard]] RoomId GetRoomId() const noexcept;
        [[nodiscard]] std::uint64_t GetCombatSeed() const noexcept;

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

        asio::strand<asio::io_context::executor_type> strand;
        asio::steady_timer enterTimer;
        asio::steady_timer tickTimer;
        RoomId roomId;
        std::uint32_t dungeonId;
        std::uint64_t combatSeed;
        std::unordered_set<PlayerId> expectedPlayers;
        std::unordered_set<PlayerId> enteredPlayers;
        std::chrono::milliseconds enterTimeout;
        EmptyHandler emptyHandler;
        State state = State::WaitingForPlayers;
        std::uint64_t serverTick{};
    };
}
