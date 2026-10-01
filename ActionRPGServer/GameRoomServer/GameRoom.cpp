#include "GameRoom.h"

#include <algorithm>
#include <cmath>
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
        EmptyHandler inEmptyHandler,
        std::shared_ptr<const DungeonDefinition> inDefinition)
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
        dungeonWorld = inDefinition->world;
        dungeonWorld["roomId"] = roomId;
        // IDs are scoped to this dungeon instance and stay stable when changing rooms.
        std::uint64_t nextMonsterId = 1;
        for (auto& map : dungeonWorld["maps"])
            for (auto& monster : map["monsters"])
            {
                monster["instanceId"] = nextMonsterId++;
                monster["hp"] = 100;
                monster["maxHp"] = 100;
            }
        std::size_t slot = 0;
        dungeonWorld["players"] = nlohmann::json::object();
        for (const PlayerId id : inExpectedPlayerIds)
        {
            const auto& spawn = dungeonWorld.at("playerSpawns").at(slot++);
            players.emplace(id, PlayerState{ dungeonWorld.at("entryMapId").get<std::string>(),
                DungeonDefinition::Point(spawn) });
            dungeonWorld["players"][std::to_string(id)] = spawn;
        }
        const auto snapshot = std::make_shared<const std::string>(dungeonWorld.dump());
        for (const PlayerId id : inExpectedPlayerIds) initialWorlds.emplace(id, snapshot);
    }

    std::shared_ptr<const std::string> GameRoom::GetWorldFor(const PlayerId inPlayerId) const
    {
        const auto iterator = initialWorlds.find(inPlayerId);
        return iterator == initialWorlds.end() ? nullptr : iterator->second;
    }

    void GameRoom::UpdateInput(const PlayerId inPlayerId,
        ActionRPG::DungeonProtocol::DungeonMoveInput inInput,
        std::function<void(ActionRPG::DungeonProtocol::DungeonPlayerState)> inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, inInput, handler = std::move(inHandler)]()
        {
            const auto iterator = self->players.find(inPlayerId);
            if (iterator == self->players.end() || !self->enteredPlayers.contains(inPlayerId)
                || self->state == State::Stopped || self->state == State::Cleared) return;
            auto& player = iterator->second;
            if (inInput.sequence <= player.sequence) return;
            player.sequence = inInput.sequence;
            player.directionX = inInput.directionX;
            player.directionY = inInput.directionY;
            player.running = inInput.running != 0;
            player.lastInput = std::chrono::steady_clock::now();
            ActionRPG::DungeonProtocol::DungeonPlayerState packet;
            packet.sequence = player.sequence;
            packet.mapId = player.mapId;
            packet.x = player.position.x;
            packet.y = player.position.y;
            handler(std::move(packet));
        });
    }

    // Room state is owned by its strand; small movement steps cannot tunnel through walls/gates.
    void GameRoom::UpdatePlayers(const float inDeltaSeconds)
    {
        for (auto& [id, player] : players)
        {
            if (!enteredPlayers.contains(id)) continue;
            const auto& map = dungeonWorld.at("maps").at(player.mapId);
            float dx = static_cast<float>(player.directionX), dy = static_cast<float>(player.directionY);
            if (std::chrono::steady_clock::now() - player.lastInput > std::chrono::seconds(1)) dx = dy = 0;
            const float length = std::hypot(dx, dy);
            const float distance = (player.running ? player.runSpeed : player.walkSpeed) * inDeltaSeconds;
            const int steps = std::max(1, static_cast<int>(std::ceil(distance / 4)));
            if (length > 0)
            {
                dx *= distance / length / steps; dy *= distance / length / steps;
                for (int step = 0; step < steps; ++step)
                {
                    const DungeonPoint next{ player.position.x + dx, player.position.y + dy };
                    if (DungeonDefinition::Movable(map, next)) player.position = next;
                    else
                    {
                        const DungeonPoint horizontal{ player.position.x + dx, player.position.y };
                        if (DungeonDefinition::Movable(map, horizontal)) player.position = horizontal;
                        const DungeonPoint vertical{ player.position.x, player.position.y + dy };
                        if (DungeonDefinition::Movable(map, vertical)) player.position = vertical;
                    }
                    const auto& zones = map.at("transitionZones");
                    const auto zone = std::find_if(zones.begin(), zones.end(), [&player](const auto& value)
                        { return DungeonDefinition::Contains(value.at("polygon"), player.position); });
                    if (zone == zones.end()) player.warpArmed = true;
                    else if (player.warpArmed)
                    {
                        const auto& action = zone->at("action");
                        const std::string targetMapId = action.at("targetMapId");
                        const auto& entries = dungeonWorld.at("maps").at(targetMapId).at("entryPoints");
                        const auto entry = std::find_if(entries.begin(), entries.end(), [&action](const auto& value)
                            { return value.at("id") == action.at("targetEntryPointId"); });
                        player.mapId = targetMapId;
                        player.position = DungeonDefinition::Point(entry->at("position"));
                        player.warpArmed = false;
                        break;
                    }
                }
            }
        }
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
                self->UpdatePlayers(0.05f);
                self->ScheduleTick();
            }
        });
    }
}
