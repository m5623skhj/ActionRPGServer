#include "GameRoom.h"

#include <utility>

namespace GameRoomServer
{
    GameRoom::GameRoom(
        asio::io_context& inIoContext,
        const RoomId inRoomId,
        const std::uint32_t inDungeonId,
        const std::uint64_t inCombatSeed,
        std::vector<PlayerId> inExpectedPlayerIds,
        const std::chrono::milliseconds inEnterTimeout,
        EmptyHandler inEmptyHandler)
        : strand(asio::make_strand(inIoContext)),
          enterTimer(strand),
          tickTimer(strand),
          roomId(inRoomId),
          dungeonId(inDungeonId),
          combatSeed(inCombatSeed),
          expectedPlayers(inExpectedPlayerIds.begin(), inExpectedPlayerIds.end()),
          enterTimeout(inEnterTimeout),
          emptyHandler(std::move(inEmptyHandler))
    {
    }

    void GameRoom::Start()
    {
        const std::shared_ptr<GameRoom> self = shared_from_this();
        asio::dispatch(strand, [self]()
        {
            self->enterTimer.expires_after(self->enterTimeout);
            self->enterTimer.async_wait([self](const asio::error_code& inError)
            {
                if (!inError)
                {
                    self->HandleEnterTimeout();
                }
            });
        });
    }

    void GameRoom::Stop()
    {
        const std::shared_ptr<GameRoom> self = shared_from_this();
        asio::dispatch(strand, [self]()
        {
            self->state = State::Stopped;
            asio::error_code ignoredError;
            self->enterTimer.cancel(ignoredError);
            self->tickTimer.cancel(ignoredError);
        });
    }

    void GameRoom::TryEnter(const PlayerId inPlayerId, EnterResultHandler inResultHandler)
    {
        const std::shared_ptr<GameRoom> self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, resultHandler = std::move(inResultHandler)]() mutable
        {
            const bool accepted = self->state == State::WaitingForPlayers
                && self->expectedPlayers.contains(inPlayerId)
                && self->enteredPlayers.insert(inPlayerId).second;
            if (accepted && self->enteredPlayers.size() == self->expectedPlayers.size())
            {
                self->StartDungeon();
            }
            if (resultHandler)
            {
                resultHandler(accepted);
            }
        });
    }

    void GameRoom::Leave(const PlayerId inPlayerId, LeaveResultHandler inResultHandler)
    {
        const std::shared_ptr<GameRoom> self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, resultHandler = std::move(inResultHandler)]() mutable
        {
            const bool removed = self->enteredPlayers.erase(inPlayerId) > 0;
            const bool roomEmpty = removed && self->enteredPlayers.empty()
                && self->state != State::WaitingForPlayers;
            if (resultHandler)
            {
                resultHandler(removed, roomEmpty);
            }
        });
    }

    void GameRoom::RemoveUnannouncedPlayer(
        const PlayerId inPlayerId,
        std::function<void(bool)> inResultHandler)
    {
        const std::shared_ptr<GameRoom> self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, resultHandler = std::move(inResultHandler)]() mutable
        {
            self->enteredPlayers.erase(inPlayerId);
            const bool roomEmpty = self->enteredPlayers.empty()
                && self->state != State::WaitingForPlayers;
            if (resultHandler)
            {
                resultHandler(roomEmpty);
            }
        });
    }

    void GameRoom::CompleteDungeon(std::function<void(std::vector<PlayerId>)> inResultHandler)
    {
        const std::shared_ptr<GameRoom> self = shared_from_this();
        asio::dispatch(strand, [self, resultHandler = std::move(inResultHandler)]() mutable
        {
            if (self->state != State::Running)
            {
                return;
            }
            self->state = State::Cleared;
            asio::error_code ignoredError;
            self->tickTimer.cancel(ignoredError);
            if (resultHandler)
            {
                resultHandler(std::vector<PlayerId>(
                    self->enteredPlayers.begin(), self->enteredPlayers.end()));
            }
        });
    }

    GameRoom::RoomId GameRoom::GetRoomId() const noexcept
    {
        return roomId;
    }

    std::uint64_t GameRoom::GetCombatSeed() const noexcept
    {
        return combatSeed;
    }

    void GameRoom::HandleEnterTimeout()
    {
        if (state != State::WaitingForPlayers)
        {
            return;
        }

        expectedPlayers = enteredPlayers;
        if (enteredPlayers.empty())
        {
            state = State::Stopped;
            if (emptyHandler)
            {
                emptyHandler(roomId);
            }
            return;
        }
        StartDungeon();
    }

    void GameRoom::StartDungeon()
    {
        if (state != State::WaitingForPlayers)
        {
            return;
        }
        state = State::Running;
        asio::error_code ignoredError;
        enterTimer.cancel(ignoredError);
        ScheduleTick();
    }

    void GameRoom::ScheduleTick()
    {
        tickTimer.expires_after(std::chrono::milliseconds(50));
        const std::shared_ptr<GameRoom> self = shared_from_this();
        tickTimer.async_wait([self](const asio::error_code& inError)
        {
            if (!inError && self->state == State::Running)
            {
                ++self->serverTick;
                self->ScheduleTick();
            }
        });
    }
}
