#include "GameRoom.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace GameRoomServer
{
    float GameRoom::BuffMultiplier(const PlayerState& inPlayer, const char* inStat)
    {
        float multiplier{};
        for (const auto& buff : inPlayer.buffs)
            if (buff.stat == inStat && buff.remainingSeconds > 0) multiplier = std::max(multiplier, buff.multiplier);
        return multiplier > 0 ? multiplier : 1.0f;
    }

    // The session supplies identity; the catalog, cooldown and actor state are checked on the room strand.
    void GameRoom::SubmitSkill(PlayerId inPlayerId, ActionRPG::DungeonProtocol::DungeonSkillInput inInput,
        std::function<void(ActionRPG::DungeonProtocol::DungeonActionResult)> inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, input = std::move(inInput), handler = std::move(inHandler)]()
        {
            ActionRPG::DungeonProtocol::DungeonActionResult result;
            result.version = COMBAT_PROTOCOL_VERSION;
            result.sequence = input.sequence; result.serverTick = self->serverTick;
            const auto found = self->players.find(inPlayerId);
            if (found != self->players.end() && self->enteredPlayers.contains(inPlayerId))
            {
                auto& player = found->second;
                if (input.sequence > player.actionSequence)
                {
                    player.actionSequence = input.sequence; player.worldReady = true;
                    const auto definition = self->combatDefinition->playerSkills.skills.find(input.skillId);
                    if (self->state == State::Running && !self->clearRequested && player.actor.hp > 0
                        && player.actor.reaction == Reaction::None && player.actor.hitstopRemainingSeconds == 0
                        && !player.skill && !player.jumpPreparing && !player.slide.active
                        && (player.shotPhase == ShotPhase::None
                            || (player.shotPhase == ShotPhase::Recover && player.pendingShots == 0)) && input.facingLeft <= 1
                        && definition != self->combatDefinition->playerSkills.skills.end())
                    {
                        const auto& skill = definition->second;
                        const auto character = skill.at("characterId").get<std::string>();
                        const bool airborne = player.actor.height > 0;
                        const auto cooldown = player.skillCooldowns.find(input.skillId);
                        if (self->combatDefinition->playerSkills.characterIds.at(character) == player.characterId
                            && player.progression.GetSkillLevel(input.skillId) > 0
                            && !skill.at(airborne ? "air" : "ground").is_null()
                            && (cooldown == player.skillCooldowns.end() || cooldown->second <= 0)
                            && (skill.at("type") != "buff" || player.buffs.size() < 16
                                || std::any_of(player.buffs.begin(), player.buffs.end(), [&input](const auto& buff) { return buff.id == input.skillId; })))
                        {
                            if (!self->IsShotFacingLocked(player)) player.facingLeft = input.facingLeft != 0;
                            player.skill = ActiveSkill{ input.skillId, airborne, player.facingLeft, false, 0, {},
                                player.progression.GetSkillLevel(input.skillId) };
                            player.lastSkillId = input.skillId; player.lastSkillAirborne = airborne; player.lastSkillSeconds = 0;
                            player.skillCooldowns[input.skillId] = skill.at("cooldownSeconds").get<float>();
                            ++player.skillSequence;
                            self->ResetShotState(player);
                            player.bufferedActions.clear();
                            self->UpdateSkills(inPlayerId, player, 0.0f, true);
                            result.accepted = 1;
                        }
                    }
                }
            }
            if (handler) handler(std::move(result));
        });
    }

    // Cooldowns/buff lifetimes use room time even when the caster's action clock is stopped.
    void GameRoom::UpdateSkillTimers(PlayerState& inPlayer, float inDeltaSeconds)
    {
        for (auto& [id, seconds] : inPlayer.skillCooldowns) seconds = std::max(0.0f, seconds - inDeltaSeconds);
        for (auto& buff : inPlayer.buffs) buff.remainingSeconds = std::max(0.0f, buff.remainingSeconds - inDeltaSeconds);
        std::erase_if(inPlayer.buffs, [](const auto& buff) { return buff.remainingSeconds <= 0; });
    }

    /**
     * Process every active frame overlapping the actor's action time, including short frames.
     * Each cast hits a monster once. Zero-time startup is allowed only when explicitly requested.
     */
    void GameRoom::UpdateSkills(PlayerId inPlayerId, PlayerState& inPlayer, float inDeltaSeconds, bool inStarting)
    {
        if (!inPlayer.lastSkillId.empty()) inPlayer.lastSkillSeconds = std::min(600.0f, inPlayer.lastSkillSeconds + inDeltaSeconds);
        if (inPlayer.actor.hp == 0) { inPlayer.buffs.clear(); inPlayer.skill.reset(); inPlayer.lastSkillId.clear(); return; }
        if (!inPlayer.skill) return;
        if (inPlayer.actor.reaction != Reaction::None || (inPlayer.skill->airborne && inPlayer.actor.height == 0))
        { inPlayer.skill.reset(); inPlayer.lastSkillId.clear(); return; }
        if (inPlayer.actor.hitstopRemainingSeconds > 0 || (inDeltaSeconds == 0 && !inStarting)) return;
        auto& cast = *inPlayer.skill;
        const auto& skill = combatDefinition->playerSkills.skills.at(cast.id);
        const auto& variant = skill.at(cast.airborne ? "air" : "ground");
        const auto& execution = skill.at("execution");
        const auto type = skill.at("type").get<std::string>();
        const float before = cast.seconds;
        cast.seconds += inDeltaSeconds;
        const float fps = variant.at("fps").get<float>();
        const float sign = cast.facingLeft ? -1.0f : 1.0f;
        if (!cast.eventApplied && cast.seconds >= variant.at("eventFrame").get<float>() / fps)
        {
            cast.eventApplied = true;
            if (type == "buff")
            {
                const auto existing = std::find_if(inPlayer.buffs.begin(), inPlayer.buffs.end(), [&cast](const auto& buff) { return buff.id == cast.id; });
                ActiveBuff buff{ cast.id, execution.at("stat").get<std::string>(), execution.at("multiplier").get<float>(), execution.at("durationSeconds").get<float>() };
                if (existing != inPlayer.buffs.end()) *existing = std::move(buff);
                else inPlayer.buffs.push_back(std::move(buff));
            }
            else if (type == "projectile" && projectiles.size() < 256)
            {
                constexpr float DEGREES_TO_RADIANS = 0.017453292519943295f;
                const float yaw = variant.at("yawDegrees").get<float>() * DEGREES_TO_RADIANS;
                const float pitch = variant.at("pitchDegrees").get<float>() * DEGREES_TO_RADIANS;
                const auto& spawn = variant.at("spawn");
                ProjectileState projectile;
                projectile.id = nextProjectileId++; projectile.ownerId = inPlayerId; projectile.mapId = inPlayer.mapId;
                projectile.position = { inPlayer.position.x + sign * spawn.at("x").get<float>(), inPlayer.position.y + spawn.at("y").get<float>() };
                projectile.height = inPlayer.actor.height + spawn.at("height").get<float>();
                projectile.direction = sign * std::cos(yaw) * std::cos(pitch);
                projectile.directionY = std::sin(yaw) * std::cos(pitch); projectile.heightDirection = std::sin(pitch);
                projectile.speed = execution.at("speed").get<float>(); projectile.radius = execution.at("radius").get<float>();
                projectile.remainingDistance = execution.at("range").get<float>(); projectile.skillId = cast.id;
                projectile.hitstopSeconds = execution.at("hitstopSeconds").get<float>();
                projectile.damage = static_cast<std::uint32_t>(std::clamp(
                    static_cast<double>(combatDefinition->skillTrees.Damage(combatDefinition->playerSkills, cast.id, cast.skillLevel))
                    * BuffMultiplier(inPlayer, "damageMultiplier"), 1.0,
                    static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
                // A spawn offset cannot shoot through a wall; traverse the same four-unit ground checks.
                const float length = std::hypot(projectile.position.x - inPlayer.position.x, projectile.position.y - inPlayer.position.y);
                const auto& map = dungeonWorld.at("maps").at(inPlayer.mapId);
                bool valid = projectile.height >= 0;
                const int steps = std::max(1, static_cast<int>(std::ceil(length / 4)));
                for (int index = 0; index <= steps && valid; ++index)
                {
                    const float t = static_cast<float>(index) / steps;
                    valid = DungeonDefinition::Movable(map, { inPlayer.position.x + (projectile.position.x - inPlayer.position.x) * t,
                        inPlayer.position.y + (projectile.position.y - inPlayer.position.y) * t });
                }
                if (valid) projectiles.push_back(std::move(projectile));
            }
        }
        if (type == "direct")
        {
            const auto found = monsterIdsByMap.find(inPlayer.mapId);
            if (found != monsterIdsByMap.end()) for (const auto& frame : variant.at("attackRects"))
            {
                const float first = frame.at("index").get<float>() / fps, last = first + 1.0f / fps;
                if (cast.seconds < first || before >= last) continue;
                const auto& rectangle = frame.at("rect");
                const float width = rectangle.at("width").get<float>();
                const float left = inPlayer.position.x + (cast.facingLeft ? -rectangle.at("x").get<float>() - width : rectangle.at("x").get<float>());
                const float bottom = inPlayer.actor.height + rectangle.at("y").get<float>();
                const float top = bottom + rectangle.at("height").get<float>();
                for (const auto id : found->second)
                {
                    auto& monster = monsters.at(id);
                    if (monster.actor.hp == 0 || cast.hitIds.contains(id)) continue;
                    const auto& profile = combatDefinition->monsters.at(monster.definition->dataId);
                    if (monster.position.x + profile.hitRadius < left || monster.position.x - profile.hitRadius > left + width
                        || std::abs(monster.position.y - inPlayer.position.y) > execution.at("depthRadius").get<float>() + profile.hitRadius
                        || monster.actor.height + profile.bodyHeight < bottom || monster.actor.height > top) continue;
                    const float length = std::hypot(monster.position.x - inPlayer.position.x, monster.position.y - inPlayer.position.y);
                    const int steps = std::max(1, static_cast<int>(std::ceil(length / 4)));
                    const auto& map = dungeonWorld.at("maps").at(inPlayer.mapId); bool visible = true;
                    for (int index = 1; index <= steps && visible; ++index)
                    {
                        const float t = static_cast<float>(index) / steps;
                        visible = DungeonDefinition::Movable(map, { inPlayer.position.x + (monster.position.x - inPlayer.position.x) * t,
                            inPlayer.position.y + (monster.position.y - inPlayer.position.y) * t });
                    }
                    if (!visible) continue;
                    cast.hitIds.insert(id);
                    const auto damage = static_cast<std::uint32_t>(std::clamp(
                        static_cast<double>(combatDefinition->skillTrees.Damage(combatDefinition->playerSkills, cast.id, cast.skillLevel))
                        * BuffMultiplier(inPlayer, "damageMultiplier"), 1.0,
                        static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
                    ApplyDamage(monster.actor, damage, false, &inPlayer.actor, execution.at("hitstopSeconds").get<float>());
                    monster.actionStarted = monster.actionComplete = false;
                }
            }
        }
        if (cast.seconds >= variant.at("durationSeconds").get<float>() && inPlayer.actor.hitstopRemainingSeconds == 0)
            inPlayer.skill.reset();
    }
}
