#include "GameRoom.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace GameRoomServer
{
    namespace
    {
        constexpr std::uint32_t MAX_SHOTS = 5;
        constexpr std::size_t MAX_PROJECTILES = 256;
        constexpr unsigned MAX_TRANSITIONS_PER_TICK = 64;
        constexpr std::size_t MAX_SNAPSHOT_BYTES = 512 * 1024;
        float Distance(DungeonPoint a, DungeonPoint b) { return std::hypot(a.x - b.x, a.y - b.y); }
    }

    const char* GameRoom::ReactionName(Reaction inReaction)
    {
        switch (inReaction)
        {
        case Reaction::Hit: return "Hit";
        case Reaction::Falling: return "Falling";
        case Reaction::Down: return "Down";
        case Reaction::Rising: return "Rising";
        case Reaction::Dead: return "Dead";
        default: return "None";
        }
    }

    // Inputs express intent only. Position, HP, attack timing and hit targets belong to the strand.
    void GameRoom::SubmitAction(PlayerId inPlayerId, ActionRPG::DungeonProtocol::DungeonActionInput inInput,
        std::function<void(ActionRPG::DungeonProtocol::DungeonActionResult)> inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, inInput, handler = std::move(inHandler)]()
        {
            ActionRPG::DungeonProtocol::DungeonActionResult result;
            result.sequence = inInput.sequence;
            result.serverTick = self->serverTick;
            const auto found = self->players.find(inPlayerId);
            if (found != self->players.end() && self->enteredPlayers.contains(inPlayerId))
            {
                auto& player = found->second;
                player.worldReady = true;
                if (inInput.sequence > player.actionSequence)
                {
                    player.actionSequence = inInput.sequence;
                    if (self->state == State::Running && !self->clearRequested && player.actor.hp > 0
                        && player.actor.reaction == Reaction::None)
                    {
                        if (inInput.action == 1 && inInput.facingLeft <= 1
                            && player.shotCount + player.pendingShots < MAX_SHOTS
                            && (player.actor.height == 0 || player.airShotCount + player.pendingShots < MAX_SHOTS))
                        {
                            if (player.shotPhase == ShotPhase::None)
                            {
                                player.facingLeft = inInput.facingLeft != 0;
                                player.airAttack = player.jumpPreparing || player.actor.height > 0;
                                player.shotCount = 0;
                                player.shotPhase = ShotPhase::Prepare;
                                player.shotSeconds = 0;
                            }
                            ++player.pendingShots;
                            player.directionX = player.directionY = 0;
                            result.accepted = 1;
                        }
                        else if (inInput.action == 2 && player.actor.height == 0 && !player.jumpPreparing
                            && player.shotPhase == ShotPhase::None)
                        {
                            player.jumpPreparing = true;
                            player.jumpSeconds = 0;
                            player.airShotCount = 0;
                            player.directionX = player.directionY = 0;
                            result.accepted = 1;
                        }
                    }
                }
            }
            if (handler) handler(std::move(result));
        });
    }

    DungeonPoint GameRoom::MoveOnMap(const std::string& inMapId, DungeonPoint inPosition,
        DungeonPoint inTarget, float inDistance) const
    {
        const float length = Distance(inPosition, inTarget);
        if (length <= 0 || inDistance <= 0) return inPosition;
        const float distance = std::min(length, inDistance);
        const int steps = std::max(1, static_cast<int>(std::ceil(distance / 4)));
        const float dx = (inTarget.x - inPosition.x) / length * distance / steps;
        const float dy = (inTarget.y - inPosition.y) / length * distance / steps;
        const auto& map = dungeonWorld.at("maps").at(inMapId);
        for (int step = 0; step < steps; ++step)
        {
            const DungeonPoint next{ inPosition.x + dx, inPosition.y + dy };
            if (DungeonDefinition::Movable(map, next)) inPosition = next;
            else
            {
                const DungeonPoint horizontal{ inPosition.x + dx, inPosition.y };
                if (DungeonDefinition::Movable(map, horizontal)) inPosition = horizontal;
                const DungeonPoint vertical{ inPosition.x, inPosition.y + dy };
                if (DungeonDefinition::Movable(map, vertical)) inPosition = vertical;
            }
        }
        if (distance == length && Distance(inPosition, inTarget) <= 0.01f && DungeonDefinition::Movable(map, inTarget))
            return inTarget;
        return inPosition;
    }

    void GameRoom::ApplyDamage(ActorState& inActor, std::uint32_t inDamage, bool inAirborne)
    {
        if (inActor.hp == 0) return;
        inActor.hp -= std::min(inActor.hp, inDamage);
        if (inActor.hp == 0)
        {
            inActor.reaction = Reaction::Dead;
            inActor.height = inActor.verticalSpeed = 0;
        }
        else if (inAirborne || inActor.height > 0)
        {
            // An already airborne victim falls immediately instead of receiving another launch.
            if (inActor.height > 0) inActor.verticalSpeed = std::min(inActor.verticalSpeed, 0.0f);
            else { inActor.height = 0.001f; inActor.verticalSpeed = combatDefinition->jumpSpeed; }
            inActor.reaction = Reaction::Falling;
        }
        else if (inActor.reaction == Reaction::None || inActor.reaction == Reaction::Hit)
        {
            inActor.reaction = Reaction::Hit;
            inActor.reactionSeconds = combatDefinition->hitStunSeconds;
        }
    }

    void GameRoom::UpdateActor(ActorState& inActor, float inDeltaSeconds)
    {
        if (inActor.hp == 0) return;
        if (inActor.height > 0)
        {
            inActor.verticalSpeed -= combatDefinition->gravity * inDeltaSeconds;
            inActor.height = std::max(0.0f, inActor.height + inActor.verticalSpeed * inDeltaSeconds);
            if (inActor.height == 0)
            {
                inActor.verticalSpeed = 0;
                if (inActor.reaction == Reaction::Falling)
                {
                    inActor.reaction = Reaction::Down;
                    inActor.reactionSeconds = combatDefinition->downSeconds;
                }
            }
        }
        else if (inActor.reaction != Reaction::None)
        {
            inActor.reactionSeconds = std::max(0.0f, inActor.reactionSeconds - inDeltaSeconds);
            if (inActor.reactionSeconds == 0)
            {
                if (inActor.reaction == Reaction::Down)
                {
                    inActor.reaction = Reaction::Rising;
                    inActor.reactionSeconds = combatDefinition->riseSeconds;
                }
                else inActor.reaction = Reaction::None;
            }
        }
    }

    void GameRoom::EnterNode(MonsterState& inMonster, const std::string& inNodeId)
    {
        inMonster.aiNodeId = inNodeId;
        inMonster.stateSeconds = inMonster.actionSeconds = 0;
        inMonster.actionStarted = inMonster.actionComplete = inMonster.hitApplied = false;
        inMonster.skillTargetId = 0;
    }

    bool GameRoom::EvaluateCondition(const MonsterState& inMonster, const nlohmann::json& inCondition) const
    {
        const std::string type = inCondition.at("type");
        if (type == "All" || type == "Any" || type == "Not")
        {
            const auto& children = inCondition.at("children");
            if (type == "Not") return !EvaluateCondition(inMonster, children.at(0));
            const auto predicate = [this, &inMonster](const auto& child) { return EvaluateCondition(inMonster, child); };
            return type == "All" ? std::all_of(children.begin(), children.end(), predicate)
                : std::any_of(children.begin(), children.end(), predicate);
        }
        const auto& parameters = inCondition.at("parameters");
        const auto target = players.find(inMonster.targetId);
        const bool hasTarget = target != players.end() && enteredPlayers.contains(target->first)
            && target->second.worldReady && target->second.actor.hp > 0 && target->second.mapId == inMonster.mapId;
        if (type == "Always") return true;
        if (type == "HasTarget") return hasTarget;
        if (type == "TargetLost") return !hasTarget;
        if (type == "AtSpawn") return Distance(inMonster.position, inMonster.spawnPosition) <= parameters.at("distance").get<float>();
        if (type == "StateTimeAtLeast") return inMonster.stateSeconds >= parameters.at("seconds").get<float>();
        if (type == "HealthRatioAtMost") return static_cast<double>(inMonster.actor.hp) / inMonster.definition->maxHp
            <= parameters.at("ratio").get<double>();
        if (type == "SkillReady")
        {
            const auto skill = inMonster.cooldowns.find(parameters.at("skillId").get<std::string>());
            return skill == inMonster.cooldowns.end() || skill->second <= 0;
        }
        if (!hasTarget) return false;
        const float distance = Distance(inMonster.position, target->second.position);
        if (type == "TargetInRange") return distance <= parameters.at("distance").get<float>();
        if (type == "TargetOutOfRange") return distance > parameters.at("distance").get<float>();
        return false;
    }

    // Edges are compiled once and sorted by priority. OnUpdate is eligible only once per tick.
    bool GameRoom::AdvanceNode(MonsterState& inMonster, const std::string& inTrigger)
    {
        for (const auto* edge : inMonster.definition->GetOutgoing(inMonster.aiNodeId))
        {
            const std::string trigger = edge->at("trigger");
            const bool eligible = trigger == "Immediate" || (trigger == "AfterAction" && inMonster.actionComplete)
                || (trigger == "OnUpdate" && inTrigger == "OnUpdate");
            if (eligible && EvaluateCondition(inMonster, edge->at("condition")))
            {
                EnterNode(inMonster, edge->at("to").get<std::string>());
                return true;
            }
        }
        return false;
    }

    /**
     * Execute a monster graph on the room strand, bounding zero-time transitions.
     * @param inMonster Mutable instance; the underlying definition stays immutable.
     * @param inDeltaSeconds Fixed simulation time consumed at most once by actions.
     */
    void GameRoom::UpdateMonster(MonsterState& inMonster, float inDeltaSeconds)
    {
        const auto& profile = combatDefinition->monsters.at(inMonster.definition->dataId);
        for (auto& [id, cooldown] : inMonster.cooldowns) cooldown = std::max(0.0f, cooldown - inDeltaSeconds);
        inMonster.targetId = 0;
        float nearest = profile.detectionRange;
        for (const auto& [id, player] : players)
        {
            if (!enteredPlayers.contains(id) || !player.worldReady || player.actor.hp == 0 || player.mapId != inMonster.mapId) continue;
            const float distance = Distance(player.position, inMonster.position);
            if (distance <= nearest && (inMonster.targetId == 0 || distance < nearest || id < inMonster.targetId))
            { nearest = distance; inMonster.targetId = id; }
        }
        if (inMonster.actor.hp == 0 || inMonster.actor.reaction != Reaction::None) return;
        inMonster.stateSeconds += inDeltaSeconds;
        unsigned transitions = AdvanceNode(inMonster, "OnUpdate") ? 1 : 0;
        float elapsed = inDeltaSeconds;
        while (transitions < MAX_TRANSITIONS_PER_TICK)
        {
            if (AdvanceNode(inMonster, "Immediate")) { ++transitions; continue; }
            const auto& action = inMonster.definition->GetNode(inMonster.aiNodeId).at("action");
            const std::string type = action.at("type");
            const auto& parameters = action.at("parameters");
            if (inMonster.stateSeconds == 0) inMonster.stateSeconds += elapsed;
            if (!inMonster.actionComplete)
            {
                if (type == "Wait")
                {
                    inMonster.actionStarted = true;
                    inMonster.actionSeconds += elapsed;
                    inMonster.actionComplete = inMonster.actionSeconds >= parameters.at("seconds").get<float>();
                }
                else if (type == "MoveToTarget" || type == "ReturnToSpawn")
                {
                    const auto target = players.find(inMonster.targetId);
                    if (type == "ReturnToSpawn" || target != players.end())
                    {
                        const DungeonPoint destination = type == "ReturnToSpawn" ? inMonster.spawnPosition : target->second.position;
                        const float tolerance = parameters.at(type == "ReturnToSpawn" ? "arrivalDistance" : "stopDistance").get<float>();
                        const float speed = type == "MoveToTarget" && parameters.at("run").get<bool>() ? profile.runSpeed : profile.walkSpeed;
                        if (destination.x != inMonster.position.x) inMonster.facingLeft = destination.x < inMonster.position.x;
                        inMonster.position = MoveOnMap(inMonster.mapId, inMonster.position, destination,
                            std::min(speed * elapsed, std::max(0.0f, Distance(inMonster.position, destination) - tolerance)));
                        inMonster.actionStarted = true;
                        inMonster.actionComplete = Distance(inMonster.position, destination) <= tolerance + 0.01f;
                    }
                }
                else if (type == "PlayMotion")
                {
                    const auto& motion = inMonster.definition->GetMotion(parameters.at("motionId").get<std::string>());
                    inMonster.actionStarted = true;
                    inMonster.actionSeconds += elapsed;
                    const float duration = motion.at("durationSeconds").get<float>();
                    if (motion.at("loop").get<bool>()) inMonster.actionSeconds = std::fmod(inMonster.actionSeconds, duration);
                    else inMonster.actionComplete = inMonster.actionSeconds >= duration;
                }
                else if (type == "UseSkill")
                {
                    const std::string skillId = parameters.at("skillId");
                    const auto& skill = inMonster.definition->GetSkill(skillId);
                    const auto& effect = profile.skills.at(skillId);
                    const auto target = players.find(inMonster.actionStarted ? inMonster.skillTargetId : inMonster.targetId);
                    if (!inMonster.actionStarted && target != players.end() && inMonster.cooldowns[skillId] <= 0)
                    {
                        const float range = Distance(inMonster.position, target->second.position);
                        if (range >= skill.at("minRange").get<float>() && range <= skill.at("maxRange").get<float>())
                        {
                            inMonster.actionStarted = true;
                            inMonster.skillTargetId = target->first;
                            inMonster.cooldowns[skillId] = skill.at("cooldownSeconds").get<float>();
                            inMonster.facingLeft = target->second.position.x < inMonster.position.x;
                        }
                    }
                    if (inMonster.actionStarted)
                    {
                        inMonster.actionSeconds += elapsed;
                        if (!inMonster.hitApplied && inMonster.actionSeconds >= effect.hitSeconds)
                        {
                            inMonster.hitApplied = true;
                            if (target != players.end() && enteredPlayers.contains(target->first) && target->second.worldReady
                                && target->second.mapId == inMonster.mapId && target->second.actor.hp > 0)
                            {
                                auto& victim = target->second;
                                const float range = Distance(inMonster.position, victim.position);
                                if (range + combatDefinition->hitRadius >= skill.at("minRange").get<float>()
                                    && range - combatDefinition->hitRadius <= skill.at("maxRange").get<float>()
                                    && victim.actor.height <= effect.reachHeight
                                    && (victim.position.x - inMonster.position.x) * (inMonster.facingLeft ? -1 : 1) >= 0)
                                {
                                    ApplyDamage(victim.actor, effect.damage, skill.at("hitType") == "Airborne");
                                    victim.shotPhase = ShotPhase::None;
                                    victim.pendingShots = victim.shotCount = 0;
                                    victim.directionX = victim.directionY = 0;
                                    victim.jumpPreparing = false;
                                }
                            }
                        }
                        inMonster.actionComplete = inMonster.actionSeconds >= skill.at("durationSeconds").get<float>();
                    }
                }
            }
            elapsed = 0; // A chain never consumes the same simulation time more than once.
            if (!AdvanceNode(inMonster, "AfterAction")) break;
            ++transitions;
        }
    }

    void GameRoom::UpdateShots(PlayerId inPlayerId, PlayerState& inPlayer, float inDeltaSeconds)
    {
        if (inPlayer.shotPhase == ShotPhase::None) return;
        if (inPlayer.actor.hp == 0 || inPlayer.actor.reaction != Reaction::None
            || (inPlayer.airAttack && !inPlayer.jumpPreparing && inPlayer.actor.height == 0))
        {
            inPlayer.shotPhase = ShotPhase::None;
            inPlayer.pendingShots = inPlayer.shotCount = 0;
            return;
        }
        if (inPlayer.jumpPreparing) return;
        inPlayer.shotSeconds += inDeltaSeconds;
        const float duration = inPlayer.shotPhase == ShotPhase::Prepare ? combatDefinition->shotPrepareSeconds
            : inPlayer.shotPhase == ShotPhase::Fire ? combatDefinition->shotIntervalSeconds : combatDefinition->shotRecoverSeconds;
        if (inPlayer.shotSeconds < duration) return;
        inPlayer.shotSeconds -= duration;
        if (inPlayer.shotPhase == ShotPhase::Recover)
        {
            if (inPlayer.pendingShots > 0) inPlayer.shotPhase = ShotPhase::Prepare;
            else { inPlayer.shotPhase = ShotPhase::None; inPlayer.shotCount = 0; }
            return;
        }
        if (inPlayer.pendingShots > 0 && projectiles.size() < MAX_PROJECTILES)
        {
            const float direction = inPlayer.facingLeft ? -1.0f : 1.0f;
            const float axisScale = inPlayer.airAttack ? 0.70710678f : 1.0f;
            projectiles.push_back({ nextProjectileId++, inPlayerId, inPlayer.mapId, inPlayer.position,
                inPlayer.actor.height + combatDefinition->muzzleHeight, direction * axisScale,
                inPlayer.airAttack ? -axisScale : 0.0f, combatDefinition->projectileRange });
            --inPlayer.pendingShots;
            ++inPlayer.shotCount;
            if (inPlayer.airAttack)
            {
                ++inPlayer.airShotCount;
                inPlayer.actor.verticalSpeed += combatDefinition->airFireLift;
                const DungeonPoint recoil{ inPlayer.position.x - direction * combatDefinition->airRecoilDistance, inPlayer.position.y };
                inPlayer.position = MoveOnMap(inPlayer.mapId, inPlayer.position, recoil, combatDefinition->airRecoilDistance);
            }
        }
        inPlayer.shotPhase = inPlayer.pendingShots > 0 ? ShotPhase::Fire : ShotPhase::Recover;
    }

    // Use the room's four-unit geometry checks and a swept horizontal hit segment between ticks.
    void GameRoom::UpdateProjectiles(float inDeltaSeconds)
    {
        for (auto& projectile : projectiles)
        {
            const float distance = std::min(projectile.remainingDistance, combatDefinition->projectileSpeed * inDeltaSeconds);
            const int steps = std::max(1, static_cast<int>(std::ceil(distance / 4)));
            const float stepDistance = distance / steps;
            const auto& map = dungeonWorld.at("maps").at(projectile.mapId);
            for (int step = 0; step < steps && projectile.remainingDistance > 0; ++step)
            {
                const float startX = projectile.position.x;
                projectile.position.x += projectile.direction * stepDistance;
                projectile.height += projectile.heightDirection * stepDistance;
                projectile.remainingDistance = std::max(0.0f, projectile.remainingDistance - stepDistance);
                if (projectile.height < 0 || !DungeonDefinition::Movable(map, projectile.position))
                { projectile.remainingDistance = 0; break; }
                std::uint64_t nearestId{};
                float nearest = std::numeric_limits<float>::max();
                const auto mapMonsters = monsterIdsByMap.find(projectile.mapId);
                if (mapMonsters == monsterIdsByMap.end()) continue;
                for (const auto id : mapMonsters->second)
                {
                    const auto& monster = monsters.at(id);
                    if (monster.actor.hp == 0) continue;
                    const auto& profile = combatDefinition->monsters.at(monster.definition->dataId);
                    const float radius = profile.hitRadius + combatDefinition->projectileRadius;
                    const float dx = monster.position.x - std::clamp(monster.position.x,
                        std::min(startX, projectile.position.x), std::max(startX, projectile.position.x));
                    const float dy = monster.position.y - projectile.position.y;
                    if (dx * dx + dy * dy > radius * radius || projectile.height < monster.actor.height - combatDefinition->projectileRadius
                        || projectile.height > monster.actor.height + profile.bodyHeight + combatDefinition->projectileRadius) continue;
                    const float along = std::abs(monster.position.x - startX);
                    if (along < nearest || (along == nearest && id < nearestId)) { nearest = along; nearestId = id; }
                }
                if (nearestId != 0)
                {
                    auto& victim = monsters.at(nearestId);
                    ApplyDamage(victim.actor, combatDefinition->shotDamage, false);
                    victim.actionStarted = victim.actionComplete = false;
                    projectile.remainingDistance = 0;
                }
            }
        }
        std::erase_if(projectiles, [](const auto& projectile) { return projectile.remainingDistance <= 0; });
    }

    void GameRoom::CheckClear()
    {
        if (clearRequested || bosses.empty() || state != State::Running) return;
        if (std::all_of(bosses.begin(), bosses.end(), [this](auto id) { return monsters.at(id).actor.hp == 0; }))
        {
            clearRequested = true;
            if (clearHandler) clearHandler(roomId);
        }
    }

    void GameRoom::UpdateCombat(float inDeltaSeconds)
    {
        if (clearRequested) return;
        for (auto& [id, player] : players)
        {
            if (!enteredPlayers.contains(id) || !player.worldReady) continue;
            if (player.jumpPreparing)
            {
                if (player.actor.hp == 0 || player.actor.reaction != Reaction::None) player.jumpPreparing = false;
                else
                {
                    player.jumpSeconds += inDeltaSeconds;
                    if (player.jumpSeconds >= combatDefinition->jumpPrepareSeconds)
                    {
                        player.jumpPreparing = false;
                        player.actor.height = 0.001f;
                        player.actor.verticalSpeed = combatDefinition->jumpSpeed;
                    }
                }
            }
            const float height = player.actor.height;
            UpdateActor(player.actor, inDeltaSeconds);
            if (height > 0 && player.actor.height == 0) player.airShotCount = 0;
            UpdateShots(id, player, inDeltaSeconds);
        }
        UpdateProjectiles(inDeltaSeconds);
        for (auto& [id, monster] : monsters)
        {
            const Reaction before = monster.actor.reaction;
            UpdateActor(monster.actor, inDeltaSeconds);
            if (before != Reaction::None && monster.actor.reaction == Reaction::None)
                EnterNode(monster, monster.definition->GetAi().at("initialNodeId").get<std::string>());
            UpdateMonster(monster, inDeltaSeconds);
        }
        CheckClear();
    }

    // Capture a single map atomically on its owning strand; the session streams this immutable value.
    void GameRoom::GetCombatSnapshot(PlayerId inPlayerId, SnapshotHandler inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, handler = std::move(inHandler)]()
        {
            const auto found = self->players.find(inPlayerId);
            if (found == self->players.end() || !self->enteredPlayers.contains(inPlayerId)) { handler(nullptr); return; }
            found->second.worldReady = true;
            const auto& mapId = found->second.mapId;
            nlohmann::json snapshot{ { "version", 1 }, { "serverTick", self->serverTick }, { "mapId", mapId },
                { "roomId", self->roomId },
                { "state", self->state == State::WaitingForPlayers ? "WaitingForPlayers"
                    : self->state == State::Running ? "Running" : self->state == State::Cleared ? "Cleared" : "Stopped" },
                { "cleared", self->state == State::Cleared || self->clearRequested },
                { "players", nlohmann::json::array() }, { "monsters", nlohmann::json::array() },
                { "projectiles", nlohmann::json::array() } };
            for (const auto& [id, player] : self->players)
            {
                if (player.mapId != mapId || !self->enteredPlayers.contains(id) || !player.worldReady) continue;
                const char* shot = player.shotPhase == ShotPhase::Prepare ? "Prepare" : player.shotPhase == ShotPhase::Fire ? "Fire"
                    : player.shotPhase == ShotPhase::Recover ? "Recover" : "None";
                snapshot["players"].push_back({ { "playerId", id }, { "x", player.position.x }, { "y", player.position.y },
                    { "hp", player.actor.hp }, { "maxHp", self->combatDefinition->playerMaxHp }, { "height", player.actor.height },
                    { "verticalSpeed", player.actor.verticalSpeed }, { "reactionSeconds", player.actor.reactionSeconds },
                    { "facingLeft", player.facingLeft }, { "reaction", ReactionName(player.actor.reaction) },
                    { "shotPhase", shot }, { "airAttack", player.airAttack }, { "actionSequence", player.actionSequence },
                    { "shotSeconds", player.shotSeconds }, { "shotCount", player.shotCount }, { "airShotCount", player.airShotCount },
                    { "jumpPhase", player.jumpPreparing ? "Prepare" : player.actor.height > 0 ? "Airborne" : "Grounded" },
                    { "jumpSeconds", player.jumpSeconds },
                    { "moveSequence", player.sequence } });
            }
            for (const auto& [id, monster] : self->monsters)
            {
                if (monster.mapId != mapId) continue;
                const auto& action = monster.definition->GetNode(monster.aiNodeId).at("action");
                const std::string type = action.at("type");
                std::string animationId;
                if (type == "UseSkill") animationId = monster.definition->GetSkill(action.at("parameters").at("skillId").get<std::string>()).at("animationId");
                if (type == "PlayMotion") animationId = monster.definition->GetMotion(action.at("parameters").at("motionId").get<std::string>()).at("animationId");
                snapshot["monsters"].push_back({ { "instanceId", id }, { "dataId", monster.definition->dataId },
                    { "x", monster.position.x }, { "y", monster.position.y }, { "hp", monster.actor.hp },
                    { "maxHp", monster.definition->maxHp }, { "height", monster.actor.height }, { "facingLeft", monster.facingLeft },
                    { "verticalSpeed", monster.actor.verticalSpeed }, { "reactionSeconds", monster.actor.reactionSeconds },
                    { "reaction", ReactionName(monster.actor.reaction) }, { "aiNodeId", monster.aiNodeId },
                    { "actionType", type }, { "actionStarted", monster.actionStarted }, { "actionComplete", monster.actionComplete },
                    { "actionSeconds", monster.actionSeconds }, { "animationId", animationId } });
            }
            for (const auto& projectile : self->projectiles)
                if (projectile.mapId == mapId) snapshot["projectiles"].push_back({ { "id", projectile.id }, { "ownerId", projectile.ownerId },
                    { "x", projectile.position.x }, { "y", projectile.position.y }, { "height", projectile.height },
                    { "direction", projectile.direction }, { "heightDirection", projectile.heightDirection } });
            auto value = std::make_shared<const std::string>(snapshot.dump());
            handler(value->size() <= MAX_SNAPSHOT_BYTES ? std::move(value) : nullptr);
        });
    }
}
