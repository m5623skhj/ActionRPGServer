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
        std::shared_ptr<const DungeonDefinition> inDefinition,
        std::shared_ptr<const CombatDefinition> inCombatDefinition, EmptyHandler inClearHandler,
        std::function<void(std::vector<PlayerId>)> inStartedHandler)
        : strand(asio::make_strand(inIoContext)),
          enterTimer(strand),
          tickTimer(strand),
          roomId(inRoomId),
          dungeonId(inDungeonId),
          combatSeed(inCombatSeed),
          definition(std::move(inDefinition)),
          combatDefinition(std::move(inCombatDefinition)),
          expectedPlayers(inExpectedPlayerIds.begin(), inExpectedPlayerIds.end()),
          enterTimeout(inEnterTimeout),
          emptyHandler(std::move(inEmptyHandler)),
          clearHandler(std::move(inClearHandler)),
          startedHandler(std::move(inStartedHandler))
    {
        dungeonWorld = definition->world;
        dungeonWorld["roomId"] = roomId;
        dungeonWorld["playerSkills"] = combatDefinition->playerSkills.source;
        dungeonWorld["skillTrees"] = combatDefinition->skillTrees.source;
        dungeonWorld["combatRules"] = {
            { "version", COMBAT_PROTOCOL_VERSION }, { "maxHp", combatDefinition->playerMaxHp },
            { "tickRate", TICK_RATE }, { "snapshotRate", SNAPSHOT_RATE }, { "tickIntervalSeconds", TICK_SECONDS },
            { "walkSpeed", combatDefinition->walkSpeed }, { "runSpeed", combatDefinition->runSpeed },
            { "shotPrepareSeconds", combatDefinition->shotPrepareSeconds },
            { "shotIntervalSeconds", combatDefinition->shotIntervalSeconds },
            { "shotRecoverSeconds", combatDefinition->shotRecoverSeconds },
            { "shotHitstopSeconds", combatDefinition->shotHitstopSeconds },
            { "jumpSpeed", combatDefinition->jumpSpeed }, { "gravity", combatDefinition->gravity },
            { "jumpPrepareSeconds", combatDefinition->jumpPrepareSeconds },
            { "hitStunSeconds", combatDefinition->hitStunSeconds }, { "downSeconds", combatDefinition->downSeconds },
            { "riseSeconds", combatDefinition->riseSeconds }, { "airFireLift", combatDefinition->airFireLift },
            { "airRecoilDistance", combatDefinition->airRecoilDistance }, { "muzzleHeight", combatDefinition->muzzleHeight },
            { "projectileSpeed", combatDefinition->projectileSpeed }, { "maxShots", 5 }
        };
        auto& slideDefinitions = dungeonWorld["combatRules"]["slideDefinitions"] = nlohmann::json::array();
        std::vector<std::uint32_t> characterIds;
        for (const auto& [id, character] : combatDefinition->characters) characterIds.push_back(id);
        std::sort(characterIds.begin(), characterIds.end());
        for (const auto id : characterIds)
        {
            const auto& character = combatDefinition->characters.at(id);
            slideDefinitions.push_back({ { "characterId", id }, { "attackPower", character.attackPower },
                { "durationSeconds", character.slide.durationSeconds }, { "motionId", character.slide.motionId },
                { "distancePerRunSpeedSeconds", character.slide.distancePerRunSpeedSeconds },
                { "hitRecovery", character.hitRecovery }, { "hitstopSeconds", character.slide.hitstopSeconds } });
        }
        // Create monsters once per dungeon instance; map transfers preserve their IDs and HP.
        std::uint64_t nextMonsterId = 1;
        for (auto& [mapId, map] : dungeonWorld["maps"].items())
            for (auto& monster : map["monsters"])
            {
                const auto monsterDefinition = definition->monsterDefinitions.at(monster.at("dataId").get<std::uint32_t>());
                const auto instanceId = nextMonsterId++;
                MonsterState instance;
                instance.definition = monsterDefinition;
                instance.mapId = mapId;
                instance.placementId = monster.at("id").get<std::string>();
                instance.aiNodeId = monsterDefinition->GetAi().at("initialNodeId").get<std::string>();
                instance.position = instance.spawnPosition = DungeonDefinition::Point(monster.at("position"));
                instance.facingLeft = monster.at("facingLeft").get<bool>();
                instance.actor.hp = monsterDefinition->maxHp;
                instance.actor.hitRecovery = combatDefinition->monsters.at(monsterDefinition->dataId).hitRecovery;
                InitializeMonsterBehavior(instance, instanceId);
                monsters.emplace(instanceId, std::move(instance));
                monsterIdsByMap[mapId].push_back(instanceId);
                monster["instanceId"] = instanceId;
                monster["hp"] = monsterDefinition->maxHp;
                monster["maxHp"] = monsterDefinition->maxHp;
            }
        for (const auto& room : definition->configuration.at("rooms"))
            if (room.at("kind") == "boss")
                for (const auto& [id, monster] : monsters)
                    if (monster.mapId == room.at("mapId").get<std::string>()) bosses.insert(id);
        std::size_t slot = 0;
        dungeonWorld["players"] = nlohmann::json::object();
        for (const PlayerId id : inExpectedPlayerIds)
        {
            const auto& spawn = dungeonWorld.at("playerSpawns").at(slot++);
            PlayerState player;
            player.mapId = dungeonWorld.at("entryMapId").get<std::string>();
            player.position = DungeonDefinition::Point(spawn);
            player.walkSpeed = combatDefinition->walkSpeed;
            player.runSpeed = combatDefinition->runSpeed;
            player.actor.hp = combatDefinition->playerMaxHp;
            players.emplace(id, std::move(player));
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
                || self->state == State::Stopped || self->state == State::Cleared || self->clearRequested) return;
            auto& player = iterator->second;
            if (inInput.sequence <= player.sequence) return;
            player.sequence = inInput.sequence;
            player.worldReady = true;
            player.directionX = inInput.directionX;
            player.directionY = inInput.directionY;
            // Movement may leave recovery after the last shot, never preparation/fire.
            if (player.actor.hitstopRemainingSeconds == 0 && (inInput.directionX != 0 || inInput.directionY != 0)
                && player.shotPhase == ShotPhase::Recover && player.pendingShots == 0)
                player.shotPhase = ShotPhase::None;
            if (!player.skill && !player.slide.active && player.actor.hitstopRemainingSeconds == 0
                && !self->IsShotFacingLocked(player) && player.actor.reaction == Reaction::None
                && inInput.directionX != 0) player.facingLeft = inInput.directionX < 0;
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

    // Read authoritative HP on the room strand. Empty maps are already cleared.
    bool GameRoom::IsMapCleared(const std::string& inMapId) const
    {
        const auto found = monsterIdsByMap.find(inMapId);
        if (found == monsterIdsByMap.end()) return true;
        return std::all_of(found->second.begin(), found->second.end(), [this](const auto id)
            { return monsters.at(id).actor.hp == 0; });
    }

    // Room state is owned by its strand; small movement steps cannot tunnel through walls/gates.
    void GameRoom::UpdatePlayers()
    {
        if (clearRequested) return;
        for (auto& [id, player] : players)
        {
            if (!enteredPlayers.contains(id) || !player.worldReady) continue;
            const float actionDelta = ActionDelta(player.actor);
            if (actionDelta == 0) continue;
            if (player.slide.active) { UpdateSlide(player, actionDelta); continue; }
            if (player.actor.hp == 0 || player.actor.reaction != Reaction::None
                || player.skill || (player.shotPhase != ShotPhase::None
                    && !(player.shotPhase == ShotPhase::Recover && player.pendingShots == 0))) continue;
            const auto& map = dungeonWorld.at("maps").at(player.mapId);
            float dx = static_cast<float>(player.directionX), dy = static_cast<float>(player.directionY);
            if (std::chrono::steady_clock::now() - player.lastInput > std::chrono::seconds(1)) dx = dy = 0;
            const float length = std::hypot(dx, dy);
            const float distance = (player.running ? player.runSpeed : player.walkSpeed)
                * BuffMultiplier(player, "movementMultiplier") * actionDelta;
            const int steps = std::max(1, static_cast<int>(std::ceil(distance / 4)));
            if (length > 0)
            {
                const bool mapCleared = IsMapCleared(player.mapId);
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
                    else if (player.warpArmed && mapCleared && player.actor.height == 0 && !player.jumpPreparing)
                    {
                        const auto& action = zone->at("action");
                        const std::string targetMapId = action.at("targetMapId");
                        const auto& entries = dungeonWorld.at("maps").at(targetMapId).at("entryPoints");
                        const auto entry = std::find_if(entries.begin(), entries.end(), [&action](const auto& value)
                            { return value.at("id") == action.at("targetEntryPointId"); });
                        player.mapId = targetMapId;
                        ++player.mapEpoch;
                        StopSlide(player);
                        CancelHitstop(player.actor);
                        player.bufferedActions.clear();
                        ResetShotState(player);
                        player.position = DungeonDefinition::Point(entry->at("position"));
                        player.warpArmed = false;
                        player.skill.reset();
                        player.lastSkillId.clear();
                        player.directionX = player.directionY = 0;
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

    void GameRoom::UpdatePlayerProgress(PlayerId inPlayerId, std::string inProgression)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, data = std::move(inProgression)]()
        {
            const auto found = self->players.find(inPlayerId);
            if (found == self->players.end() || !self->enteredPlayers.contains(inPlayerId)) return;
            try
            {
                auto progression = ActionRPG::PlayerSkills::CharacterProgression::Parse(nlohmann::json::parse(data));
                self->combatDefinition->skillTrees.ValidateProgression(self->combatDefinition->playerSkills,
                    progression, found->second.characterId);
                found->second.progression = std::move(progression);
            }
            catch (const std::exception&) { return; }
        });
    }

    void GameRoom::Stop()
    {
        const std::shared_ptr<GameRoom> self = shared_from_this();
        asio::dispatch(strand, [self]()
        {
            self->state = State::Stopped;
            for (auto& [playerId, player] : self->players)
            {
                self->ResetShotState(player);
                player.bufferedActions.clear();
                self->StopSlide(player);
                self->CancelHitstop(player.actor);
            }
            for (auto& [id, monster] : self->monsters) self->CancelHitstop(monster.actor);
            self->PublishRealtime();
            self->realtimeSubscribers.clear();
            asio::error_code ignoredError;
            self->enterTimer.cancel(ignoredError);
            self->tickTimer.cancel(ignoredError);
        });
    }

    void GameRoom::TryEnter(const PlayerId inPlayerId, std::uint32_t inCharacterId, std::string inProgression,
        EnterResultHandler inResultHandler)
    {
        const std::shared_ptr<GameRoom> self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, inCharacterId, data = std::move(inProgression),
            resultHandler = std::move(inResultHandler)]() mutable
        {
            ActionRPG::PlayerSkills::CharacterProgression progression;
            try
            {
                progression = ActionRPG::PlayerSkills::CharacterProgression::Parse(nlohmann::json::parse(data));
                self->combatDefinition->skillTrees.ValidateProgression(self->combatDefinition->playerSkills, progression, inCharacterId);
            }
            catch (const std::exception&)
            {
                if (resultHandler) resultHandler(false);
                return;
            }
            const bool accepted = self->state == State::WaitingForPlayers
                && inCharacterId != 0
                && self->combatDefinition->characters.contains(inCharacterId)
                && self->expectedPlayers.contains(inPlayerId)
                && self->enteredPlayers.insert(inPlayerId).second;
            if (accepted)
            {
                self->players.at(inPlayerId).characterId = inCharacterId;
                self->players.at(inPlayerId).actor.hitRecovery = self->combatDefinition->characters.at(inCharacterId).hitRecovery;
                self->players.at(inPlayerId).progression = std::move(progression);
            }
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
            // Revoke the reservation as well: an outstanding join must not re-enter after release.
            self->expectedPlayers.erase(inPlayerId);
            const auto player = self->players.find(inPlayerId);
            if (player != self->players.end())
            {
                self->ResetShotState(player->second);
                player->second.bufferedActions.clear();
                self->StopSlide(player->second);
                self->CancelHitstop(player->second.actor);
                player->second.skill.reset(); player->second.buffs.clear(); player->second.lastSkillId.clear();
            }
            self->realtimeSubscribers.erase(inPlayerId);
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
            if (const auto player = self->players.find(inPlayerId); player != self->players.end())
            {
                self->ResetShotState(player->second);
                player->second.bufferedActions.clear();
                self->StopSlide(player->second);
                self->CancelHitstop(player->second.actor);
            }
            self->realtimeSubscribers.erase(inPlayerId);
            const bool roomEmpty = self->enteredPlayers.empty()
                && self->state != State::WaitingForPlayers;
            if (resultHandler)
            {
                resultHandler(roomEmpty);
            }
        });
    }

    // Validate membership and terminal state on the room strand before a replacement is created.
    void GameRoom::ValidateCompletion(std::vector<PlayerId> inParticipants, std::function<void(bool)> inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, participants = std::move(inParticipants), handler = std::move(inHandler)]()
        {
            const std::unordered_set<PlayerId> requested(participants.begin(), participants.end());
            handler(self->state == State::Cleared && !requested.empty()
                && requested.size() == participants.size() && requested == self->enteredPlayers);
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
            for (auto& [playerId, player] : self->players)
            {
                self->ResetShotState(player);
                player.bufferedActions.clear();
                self->StopSlide(player);
                self->CancelHitstop(player.actor);
            }
            for (auto& [id, monster] : self->monsters) self->CancelHitstop(monster.actor);
            self->PublishRealtime();
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
        if (startedHandler) startedHandler(std::vector<PlayerId>(enteredPlayers.begin(), enteredPlayers.end()));
        asio::error_code ignoredError;
        enterTimer.cancel(ignoredError);
        nextTickAt = std::chrono::steady_clock::now();
        ScheduleTick();
    }

    void GameRoom::ScheduleTick()
    {
        // Anchor deadlines so processing time does not accumulate into the tick interval.
        // Under overload, skip missed deadlines rather than executing an unbounded catch-up burst.
        nextTickAt += TICK_INTERVAL;
        const auto now = std::chrono::steady_clock::now();
        if (nextTickAt <= now) nextTickAt = now + TICK_INTERVAL;
        tickTimer.expires_at(nextTickAt);
        const std::shared_ptr<GameRoom> self = shared_from_this();
        tickTimer.async_wait([self](const asio::error_code& inError)
        {
            if (!inError && self->state == State::Running)
            {
                ++self->serverTick;
                self->PrepareActorTimes(TICK_SECONDS);
                self->UpdatePlayers();
                self->UpdateCombat(TICK_SECONDS);
                if (self->serverTick % (TICK_RATE / SNAPSHOT_RATE) == 0 || self->clearRequested
                    || self->hitstopChanged) self->PublishRealtime();
                if (self->state == State::Running && !self->clearRequested) self->ScheduleTick();
            }
        });
    }
}
