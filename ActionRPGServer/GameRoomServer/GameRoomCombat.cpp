#include "GameRoom.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace GameRoomServer
{
    namespace
    {
        constexpr std::uint32_t MAX_SHOTS = 5;
        constexpr float SHOT_INPUT_SECONDS = 0.4f;
        constexpr float ACTION_BUFFER_SECONDS = 0.25f;
        constexpr float AIRBORNE_HIT_SPEED = 420.0f;
        constexpr std::size_t MAX_PROJECTILES = 256;
        constexpr unsigned MAX_TRANSITIONS_PER_TICK = 64;
        constexpr std::size_t MAX_SNAPSHOT_BYTES = 512 * 1024;
        float Distance(DungeonPoint a, DungeonPoint b) { return std::hypot(a.x - b.x, a.y - b.y); }

        /**
         * Return the first fraction where a segment overlaps both a ground circle and a height interval.
         * The caller expands the cylinder by the projectile radius; a segment starting inside hits at zero.
         */
        std::optional<float> ProjectileHitFraction(DungeonPoint inStart, float inStartHeight,
            DungeonPoint inEnd, float inEndHeight, DungeonPoint inTarget, float inRadius,
            float inLower, float inUpper)
        {
            const float dx = inEnd.x - inStart.x, dy = inEnd.y - inStart.y;
            const float ox = inStart.x - inTarget.x, oy = inStart.y - inTarget.y;
            const double a = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
            const double b = 2 * (static_cast<double>(ox) * dx + static_cast<double>(oy) * dy);
            const double c = static_cast<double>(ox) * ox + static_cast<double>(oy) * oy
                - static_cast<double>(inRadius) * inRadius;
            double first = 0, last = 1;
            if (a <= 0.000001f) { if (c > 0) return std::nullopt; }
            else
            {
                const double discriminant = b * b - 4 * a * c;
                if (discriminant < 0) return std::nullopt;
                const double root = std::sqrt(discriminant);
                first = std::max(first, (-b - root) / (2 * a));
                last = std::min(last, (-b + root) / (2 * a));
            }
            const float dz = inEndHeight - inStartHeight;
            if (std::abs(dz) <= 0.000001f)
            {
                if (inStartHeight < inLower || inStartHeight > inUpper) return std::nullopt;
            }
            else
            {
                const double t1 = (static_cast<double>(inLower) - inStartHeight) / dz;
                const double t2 = (static_cast<double>(inUpper) - inStartHeight) / dz;
                first = std::max(first, std::min(t1, t2));
                last = std::min(last, std::max(t1, t2));
            }
            if (first > last) return std::nullopt;
            return static_cast<float>(first);
        }
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

    // Recovery may allow movement while the same unfinished combo still owns its facing.
    bool GameRoom::IsShotFacingLocked(const PlayerState& inPlayer)
    {
        return inPlayer.shotPhase != ShotPhase::None
            || (inPlayer.shotCount > 0 && inPlayer.shotCount < MAX_SHOTS && inPlayer.shotInputRemainingSeconds > 0);
    }

    // Reset the combo without clearing queued actions or the per-jump shot limit.
    void GameRoom::ResetShotState(PlayerState& inPlayer)
    {
        inPlayer.shotPhase = ShotPhase::None;
        inPlayer.pendingShots = inPlayer.shotCount = 0;
        inPlayer.shotSeconds = inPlayer.shotInputRemainingSeconds = 0;
        inPlayer.airAttack = false;
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
                        && player.actor.reaction == Reaction::None && inInput.facingLeft <= 1
                        && (inInput.action == 1 || inInput.action == 2))
                    {
                        const auto reservedShots = std::count_if(player.bufferedActions.begin(), player.bufferedActions.end(),
                            [](const auto& action) { return action.action == 1; });
                        const bool airborne = player.jumpPreparing || player.actor.height > 0;
                        const auto comboShots = player.airAttack && !airborne ? 0 : player.shotCount;
                        if (player.bufferedActions.empty()
                            && self->TryQueueAction(player, inInput.action, inInput.facingLeft != 0))
                        {
                            result.accepted = 1;
                        }
                        else if (player.bufferedActions.size() < MAX_SHOTS + 1
                            && (inInput.action == 2 || (comboShots + player.pendingShots + reservedShots < MAX_SHOTS
                                && (!airborne || player.airShotCount + player.pendingShots + reservedShots < MAX_SHOTS))))
                        {
                            player.bufferedActions.push_back({ inInput.action, inInput.facingLeft != 0, ACTION_BUFFER_SECONDS });
                            result.accepted = 1;
                        }
                    }
                }
            }
            if (handler) handler(std::move(result));
        });
    }

    // Execute immediately when legal. Combo grace never holds a movement/animation phase open.
    bool GameRoom::TryQueueAction(PlayerState& inPlayer, std::uint8_t inAction, bool inFacingLeft)
    {
        if (inPlayer.actor.hp == 0 || inPlayer.actor.reaction != Reaction::None || inPlayer.skill) return false;
        if (inAction == 2 && inPlayer.shotPhase == ShotPhase::Recover && inPlayer.pendingShots == 0)
            inPlayer.shotPhase = ShotPhase::None;
        if (inAction == 1)
        {
            // A just-landed air attack is cancelled by UpdateShots before buffered ground input runs.
            if (inPlayer.shotPhase != ShotPhase::None && inPlayer.airAttack
                && !inPlayer.jumpPreparing && inPlayer.actor.height == 0) return false;
            const bool airborne = inPlayer.jumpPreparing || inPlayer.actor.height > 0;
            const bool continuing = inPlayer.shotInputRemainingSeconds > 0 && inPlayer.airAttack == airborne;
            if (inPlayer.shotPhase == ShotPhase::None && !continuing) inPlayer.shotCount = 0;
            if (inPlayer.shotCount + inPlayer.pendingShots >= MAX_SHOTS
                || (airborne && inPlayer.airShotCount + inPlayer.pendingShots >= MAX_SHOTS)) return false;
            if (inPlayer.shotPhase == ShotPhase::None)
            {
                ++inPlayer.shotSequence;
                if (!continuing) inPlayer.facingLeft = inFacingLeft;
                inPlayer.airAttack = airborne;
                inPlayer.shotPhase = continuing ? ShotPhase::Fire : ShotPhase::Prepare;
                inPlayer.shotSeconds = 0;
            }
            else if (inPlayer.shotPhase == ShotPhase::Recover)
            {
                inPlayer.shotPhase = ShotPhase::Fire;
                inPlayer.shotSeconds = 0;
            }
            ++inPlayer.pendingShots;
        }
        else if (inAction == 2 && inPlayer.actor.height == 0 && !inPlayer.jumpPreparing
            && inPlayer.shotPhase == ShotPhase::None)
        {
            inPlayer.jumpPreparing = true;
            ++inPlayer.jumpSequence;
            inPlayer.jumpSeconds = 0;
            ResetShotState(inPlayer);
            inPlayer.airShotCount = 0;
        }
        else return false;
        // Keep held movement intent; the active phase decides when it may move again.
        return true;
    }

    // The room strand owns this bounded FIFO; each key expires independently after 250 ms.
    void GameRoom::UpdateBufferedActions(PlayerState& inPlayer, float inDeltaSeconds)
    {
        if (inPlayer.actor.hp == 0 || inPlayer.actor.reaction != Reaction::None)
        {
            inPlayer.bufferedActions.clear();
            inPlayer.shotInputRemainingSeconds = 0;
            return;
        }
        for (auto& action : inPlayer.bufferedActions) action.remainingSeconds -= inDeltaSeconds;
        while (!inPlayer.bufferedActions.empty())
        {
            const auto& action = inPlayer.bufferedActions.front();
            if (action.remainingSeconds > 0 && !TryQueueAction(inPlayer, action.action, action.facingLeft)) break;
            inPlayer.bufferedActions.pop_front();
        }
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
        ++inActor.reactionSequence;
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
            else { inActor.height = 0.001f; inActor.verticalSpeed = AIRBORNE_HIT_SPEED; }
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
        ++inMonster.actionSequence;
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
                                    ResetShotState(victim);
                                    victim.bufferedActions.clear();
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
        if (inPlayer.actor.hp == 0 || inPlayer.actor.reaction != Reaction::None
            || (inPlayer.airAttack && !inPlayer.jumpPreparing && inPlayer.actor.height == 0))
        {
            ResetShotState(inPlayer);
            return;
        }
        if (inPlayer.shotPhase == ShotPhase::None) return;
        if (inPlayer.jumpPreparing) return;
        inPlayer.shotSeconds += inDeltaSeconds;
        const float duration = inPlayer.shotPhase == ShotPhase::Prepare ? combatDefinition->shotPrepareSeconds
            : inPlayer.shotPhase == ShotPhase::Fire ? combatDefinition->shotIntervalSeconds : combatDefinition->shotRecoverSeconds;
        if (inPlayer.shotSeconds < duration) return;
        inPlayer.shotSeconds -= duration;
        if (inPlayer.shotPhase == ShotPhase::Recover)
        {
            if (inPlayer.pendingShots > 0) inPlayer.shotPhase = ShotPhase::Fire;
            else
            {
                inPlayer.shotPhase = ShotPhase::None;
                if (inPlayer.shotCount >= MAX_SHOTS)
                {
                    ResetShotState(inPlayer);
                }
            }
            return;
        }
        if (inPlayer.pendingShots > 0 && projectiles.size() < MAX_PROJECTILES)
        {
            const float direction = inPlayer.facingLeft ? -1.0f : 1.0f;
            const float axisScale = inPlayer.airAttack ? 0.70710678f : 1.0f;
            const float muzzleForward = inPlayer.airAttack
                ? combatDefinition->airMuzzleForward : combatDefinition->muzzleForward;
            const float muzzleHeight = inPlayer.airAttack
                ? combatDefinition->airMuzzleHeight : combatDefinition->muzzleHeight;
            const DungeonPoint muzzlePosition{ inPlayer.position.x + direction * muzzleForward, inPlayer.position.y };

            // Check the whole ground segment so the muzzle offset cannot spawn a shot beyond a wall.
            const auto& map = dungeonWorld.at("maps").at(inPlayer.mapId);
            const int steps = std::max(1, static_cast<int>(std::ceil(muzzleForward / 4)));
            bool validSpawn = true;
            for (int step = 0; step <= steps && validSpawn; ++step)
            {
                const float ratio = static_cast<float>(step) / steps;
                validSpawn = DungeonDefinition::Movable(map,
                    { inPlayer.position.x + direction * muzzleForward * ratio, inPlayer.position.y });
            }
            if (validSpawn)
            {
                projectiles.push_back({ nextProjectileId++, inPlayerId, inPlayer.mapId, muzzlePosition,
                    inPlayer.actor.height + muzzleHeight, direction * axisScale,
                    inPlayer.airAttack ? -axisScale : 0.0f, combatDefinition->projectileRange });
                auto& spawned = projectiles.back();
                spawned.speed = combatDefinition->projectileSpeed; spawned.radius = combatDefinition->projectileRadius;
                spawned.damage = static_cast<std::uint32_t>(std::max(1.0f,
                    combatDefinition->shotDamage * BuffMultiplier(inPlayer, "damageMultiplier")));

                // Ground shots also cover the forward body-to-muzzle segment, after the wall check above.
                if (!inPlayer.airAttack)
                {
                    const auto mapMonsters = monsterIdsByMap.find(inPlayer.mapId);
                    std::uint64_t nearestId{};
                    float nearest = std::numeric_limits<float>::max();
                    if (mapMonsters != monsterIdsByMap.end()) for (const auto id : mapMonsters->second)
                    {
                        const auto& monster = monsters.at(id);
                        if (monster.actor.hp == 0 || (monster.position.x - inPlayer.position.x) * direction < 0) continue;
                        const auto& profile = combatDefinition->monsters.at(monster.definition->dataId);
                        const auto along = ProjectileHitFraction(inPlayer.position, spawned.height,
                            spawned.position, spawned.height, monster.position, profile.hitRadius + spawned.radius,
                            monster.actor.height - spawned.radius, monster.actor.height + profile.bodyHeight + spawned.radius);
                        if (along && (*along < nearest || (*along == nearest && id < nearestId)))
                        { nearest = *along; nearestId = id; }
                    }
                    if (nearestId != 0)
                    {
                        auto& victim = monsters.at(nearestId);
                        ApplyDamage(victim.actor, spawned.damage, false);
                        victim.actionStarted = victim.actionComplete = false;
                        spawned.remainingDistance = 0;
                    }
                }
            }
            --inPlayer.pendingShots;
            ++inPlayer.shotCount;
            inPlayer.shotInputRemainingSeconds = SHOT_INPUT_SECONDS;
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

    // Sweep against vertical cylinders in ground X/Y and height, with four-unit map geometry checks.
    void GameRoom::UpdateProjectiles(float inDeltaSeconds)
    {
        for (auto& projectile : projectiles)
        {
            projectile.ageSeconds += inDeltaSeconds;
            const float distance = std::min(projectile.remainingDistance, projectile.speed * inDeltaSeconds);
            const int steps = std::max(1, static_cast<int>(std::ceil(distance / 4)));
            const float stepDistance = distance / steps;
            const auto& map = dungeonWorld.at("maps").at(projectile.mapId);
            for (int step = 0; step < steps && projectile.remainingDistance > 0; ++step)
            {
                const DungeonPoint start = projectile.position;
                const float startHeight = projectile.height;
                projectile.position.x += projectile.direction * stepDistance;
                projectile.position.y += projectile.directionY * stepDistance;
                projectile.height += projectile.heightDirection * stepDistance;
                projectile.remainingDistance = std::max(0.0f, projectile.remainingDistance - stepDistance);
                const bool hitGround = projectile.height < 0;
                if (hitGround)
                {
                    const float ratio = startHeight / (startHeight - projectile.height);
                    projectile.position.x = start.x + (projectile.position.x - start.x) * ratio;
                    projectile.position.y = start.y + (projectile.position.y - start.y) * ratio;
                    projectile.height = 0;
                }
                if (!DungeonDefinition::Movable(map, projectile.position))
                { projectile.remainingDistance = 0; break; }
                std::uint64_t nearestId{};
                float nearest = std::numeric_limits<float>::max();
                const auto mapMonsters = monsterIdsByMap.find(projectile.mapId);
                if (mapMonsters == monsterIdsByMap.end())
                { if (hitGround) projectile.remainingDistance = 0; continue; }
                for (const auto id : mapMonsters->second)
                {
                    const auto& monster = monsters.at(id);
                    if (monster.actor.hp == 0) continue;
                    const auto& profile = combatDefinition->monsters.at(monster.definition->dataId);
                    const auto along = ProjectileHitFraction(start, startHeight, projectile.position, projectile.height,
                        monster.position, profile.hitRadius + projectile.radius, monster.actor.height - projectile.radius,
                        monster.actor.height + profile.bodyHeight + projectile.radius);
                    if (along && (*along < nearest || (*along == nearest && id < nearestId)))
                    { nearest = *along; nearestId = id; }
                }
                if (nearestId != 0)
                {
                    auto& victim = monsters.at(nearestId);
                    ApplyDamage(victim.actor, projectile.damage, false);
                    victim.actionStarted = victim.actionComplete = false;
                    projectile.remainingDistance = 0;
                }
                if (hitGround) projectile.remainingDistance = 0;
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
        const auto now = std::chrono::steady_clock::now();
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
            player.shotInputRemainingSeconds = std::max(0.0f, player.shotInputRemainingSeconds - inDeltaSeconds);
            if (player.shotPhase == ShotPhase::None && player.shotInputRemainingSeconds == 0) ResetShotState(player);
            UpdateActor(player.actor, inDeltaSeconds);
            if (height > 0 && player.actor.height == 0) player.airShotCount = 0;
            UpdateShots(id, player, inDeltaSeconds);
            UpdateSkills(id, player, inDeltaSeconds);
            UpdateBufferedActions(player, inDeltaSeconds);
            if (!player.skill && !IsShotFacingLocked(player) && player.actor.hp > 0
                && player.actor.reaction == Reaction::None && player.directionX != 0
                && now - player.lastInput <= std::chrono::seconds(1))
                player.facingLeft = player.directionX < 0;
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
                { "tickRate", TICK_RATE }, { "snapshotRate", SNAPSHOT_RATE }, { "tickIntervalSeconds", TICK_SECONDS },
                { "serverTimeMs", static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()) },
                { "mapEpoch", found->second.mapEpoch },
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
                    { "reactionSequence", player.actor.reactionSequence },
                    { "shotPhase", shot }, { "airAttack", player.airAttack }, { "actionSequence", player.actionSequence },
                    { "shotSequence", player.shotSequence }, { "jumpSequence", player.jumpSequence },
                    { "shotSeconds", player.shotSeconds }, { "shotCount", player.shotCount }, { "airShotCount", player.airShotCount },
                    { "jumpPhase", player.jumpPreparing ? "Prepare" : player.actor.height > 0 ? "Airborne" : "Grounded" },
                    { "jumpSeconds", player.jumpSeconds },
                    { "moveSequence", player.sequence } });
                auto& record = snapshot["players"].back();
                record["characterId"] = player.characterId; record["skillSequence"] = player.skillSequence;
                record["skillId"] = player.lastSkillId;
                record["skillActive"] = player.skill.has_value();
                record["skillAirborne"] = player.lastSkillAirborne;
                record["skillSeconds"] = player.lastSkillSeconds;
                record["level"] = player.progression.level;
                record["skillPoints"] = player.progression.skillPoints;
                record["skillLevels"] = player.progression.skillLevels;
                record["skillCooldowns"] = player.skillCooldowns;
                record["movementMultiplier"] = BuffMultiplier(player, "movementMultiplier");
                record["buffs"] = nlohmann::json::array();
                for (const auto& buff : player.buffs) record["buffs"].push_back({ { "skillId", buff.id }, { "remainingSeconds", buff.remainingSeconds } });
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
                    { "reactionSequence", monster.actor.reactionSequence }, { "actionSequence", monster.actionSequence },
                    { "actionType", type }, { "actionStarted", monster.actionStarted }, { "actionComplete", monster.actionComplete },
                    { "actionSeconds", monster.actionSeconds }, { "animationId", animationId } });
            }
            for (const auto& projectile : self->projectiles)
                if (projectile.mapId == mapId) snapshot["projectiles"].push_back({ { "id", projectile.id }, { "ownerId", projectile.ownerId },
                    { "x", projectile.position.x }, { "y", projectile.position.y }, { "height", projectile.height },
                    { "direction", projectile.direction }, { "heightDirection", projectile.heightDirection },
                    { "directionY", projectile.directionY }, { "speed", projectile.speed }, { "radius", projectile.radius },
                    { "skillId", projectile.skillId }, { "ageSeconds", projectile.ageSeconds } });
            auto value = std::make_shared<const std::string>(snapshot.dump());
            handler(value->size() <= MAX_SNAPSHOT_BYTES ? std::move(value) : nullptr);
        });
    }
}
