#include "GameRoom.h"

#include <bit>
#include <utility>

namespace GameRoomServer
{
    namespace
    {
        constexpr std::size_t MAX_REALTIME_BYTES = 48 * 1024;

        // Wire records have no struct padding and always use little-endian binary32 values.
        class FrameWriter
        {
        public:
            void Byte(std::uint8_t inValue) { value.push_back(static_cast<char>(inValue)); }
            void Integer(std::uint64_t inValue, unsigned inBytes)
            {
                for (unsigned index = 0; index < inBytes; ++index)
                    Byte(static_cast<std::uint8_t>(inValue >> (index * 8)));
            }
            void Float(float inValue) { Integer(std::bit_cast<std::uint32_t>(inValue), 4); }
            void Text(const std::string& inValue)
            {
                Integer(inValue.size(), 2);
                value.append(inValue);
            }
            std::string value;
        };
    }

    void GameRoom::SubscribeRealtime(PlayerId inPlayerId, std::weak_ptr<const std::uint8_t> inLease,
        RealtimeHandler inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, lease = std::move(inLease), handler = std::move(inHandler)]() mutable
        {
            const auto player = self->players.find(inPlayerId);
            if (lease.expired() || !handler || player == self->players.end()
                || !self->enteredPlayers.contains(inPlayerId) || self->state == State::Stopped) return;
            player->second.worldReady = true;
            self->realtimeSubscribers.insert_or_assign(inPlayerId, RealtimeSubscriber{ lease, std::move(handler) });
            self->PublishRealtime();
        });
    }

    // A complete map frame makes loss recoverable without deltas or per-actor retransmission.
    // Called only on the room strand; subscribers receive immutable shared data.
    void GameRoom::PublishRealtime()
    {
        if (realtimeSubscribers.empty()) return;
        ++realtimeSequence;
        const auto timeMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        std::unordered_map<std::string, std::shared_ptr<const RealtimeFrame>> frames;
        for (auto iterator = realtimeSubscribers.begin(); iterator != realtimeSubscribers.end();)
        {
            const auto player = players.find(iterator->first);
            if (iterator->second.lease.expired() || player == players.end()
                || !enteredPlayers.contains(iterator->first))
            {
                iterator = realtimeSubscribers.erase(iterator);
                continue;
            }
            const auto& mapId = player->second.mapId;
            const auto [frame, inserted] = frames.try_emplace(mapId);
            if (inserted) frame->second = BuildRealtimeFrame(mapId, timeMs);
            if (frame->second) iterator->second.handler(player->second.mapEpoch, frame->second);
            ++iterator;
        }
    }

    std::shared_ptr<const GameRoom::RealtimeFrame> GameRoom::BuildRealtimeFrame(
        const std::string& inMapId, std::uint64_t inTimeMs) const
    {
        std::uint16_t playerCount{}, monsterCount{}, projectileCount{};
        for (const auto& [id, player] : players)
            if (player.mapId == inMapId && enteredPlayers.contains(id) && player.worldReady) ++playerCount;
        const auto mapMonsters = monsterIdsByMap.find(inMapId);
        if (mapMonsters != monsterIdsByMap.end()) monsterCount = static_cast<std::uint16_t>(mapMonsters->second.size());
        for (const auto& projectile : projectiles) if (projectile.mapId == inMapId) ++projectileCount;

        FrameWriter writer;
        writer.Integer(playerCount, 2);
        writer.Integer(monsterCount, 2);
        writer.Integer(projectileCount, 2);
        const auto writeActor = [&writer](std::uint64_t id, std::uint32_t dataId, const DungeonPoint& position,
            const ActorState& actor, std::uint32_t maxHp, bool facingLeft)
        {
            writer.Integer(id, 8); writer.Integer(dataId, 4);
            writer.Float(position.x); writer.Float(position.y);
            writer.Float(actor.height); writer.Float(actor.verticalSpeed); writer.Float(actor.reactionSeconds);
            writer.Integer(actor.hp, 4); writer.Integer(maxHp, 4);
            writer.Byte(facingLeft); writer.Byte(static_cast<std::uint8_t>(actor.reaction));
            writer.Integer(actor.reactionSequence, 4);
        };
        for (const auto& [id, player] : players)
        {
            if (player.mapId != inMapId || !enteredPlayers.contains(id) || !player.worldReady) continue;
            writeActor(id, 0, player.position, player.actor, combatDefinition->playerMaxHp, player.facingLeft);
            writer.Integer(player.sequence, 4); writer.Integer(player.actionSequence, 4);
            writer.Integer(player.shotSequence, 4); writer.Integer(player.jumpSequence, 4);
            writer.Byte(static_cast<std::uint8_t>(player.shotPhase)); writer.Byte(player.airAttack);
            writer.Float(player.shotSeconds); writer.Byte(static_cast<std::uint8_t>(player.shotCount));
            writer.Byte(static_cast<std::uint8_t>(player.airShotCount));
            writer.Byte(player.jumpPreparing ? 1 : player.actor.height > 0 ? 2 : 0);
            writer.Float(player.jumpSeconds); writer.Byte(player.running);
        }
        if (mapMonsters != monsterIdsByMap.end()) for (const auto id : mapMonsters->second)
        {
            const auto& monster = monsters.at(id);
            const auto& action = monster.definition->GetNode(monster.aiNodeId).at("action");
            const std::string type = action.at("type");
            std::string animationId;
            if (type == "UseSkill") animationId = monster.definition->GetSkill(
                action.at("parameters").at("skillId").get<std::string>()).at("animationId");
            if (type == "PlayMotion") animationId = monster.definition->GetMotion(
                action.at("parameters").at("motionId").get<std::string>()).at("animationId");
            writeActor(id, monster.definition->dataId, monster.position, monster.actor,
                monster.definition->maxHp, monster.facingLeft);
            writer.Integer(monster.actionSequence, 4);
            writer.Byte(type == "Wait" ? 0 : type == "MoveToTarget" ? 1 : type == "ReturnToSpawn" ? 2
                : type == "UseSkill" ? 3 : 4);
            writer.Byte(monster.actionStarted); writer.Byte(monster.actionComplete); writer.Float(monster.actionSeconds);
            writer.Text(monster.aiNodeId); writer.Text(animationId);
            if (writer.value.size() > MAX_REALTIME_BYTES) return nullptr;
        }
        for (const auto& projectile : projectiles)
        {
            if (projectile.mapId != inMapId) continue;
            writer.Integer(projectile.id, 8); writer.Integer(projectile.ownerId, 8);
            writer.Float(projectile.position.x); writer.Float(projectile.position.y); writer.Float(projectile.height);
            writer.Float(projectile.direction); writer.Float(projectile.heightDirection);
            if (writer.value.size() > MAX_REALTIME_BYTES) return nullptr;
        }
        if (!combatDefinition->playerSkills.skills.empty())
        {
            writer.value.append("SKL1", 4); writer.Integer(playerCount, 2);
            for (const auto& [id, player] : players)
            {
                if (player.mapId != inMapId || !enteredPlayers.contains(id) || !player.worldReady) continue;
                writer.Integer(id, 8); writer.Integer(player.characterId, 4); writer.Integer(player.skillSequence, 4);
                writer.Text(player.lastSkillId); writer.Byte(player.skill.has_value()); writer.Byte(player.lastSkillAirborne);
                writer.Float(player.lastSkillSeconds); writer.Float(BuffMultiplier(player, "movementMultiplier"));
                writer.Byte(static_cast<std::uint8_t>(player.buffs.size()));
                for (const auto& buff : player.buffs) { writer.Text(buff.id); writer.Float(buff.remainingSeconds); }
            }
            writer.Integer(projectileCount, 2);
            for (const auto& projectile : projectiles)
            {
                if (projectile.mapId != inMapId) continue;
                writer.Integer(projectile.id, 8); writer.Text(projectile.skillId); writer.Float(projectile.directionY);
                writer.Float(projectile.speed); writer.Float(projectile.radius); writer.Float(projectile.ageSeconds);
            }
        }
        if (writer.value.size() > MAX_REALTIME_BYTES) return nullptr;
        auto frame = std::make_shared<RealtimeFrame>();
        frame->mapId = inMapId; frame->payload = std::move(writer.value);
        frame->sequence = realtimeSequence; frame->tick = serverTick; frame->timeMs = inTimeMs;
        frame->state = static_cast<std::uint8_t>(clearRequested && state == State::Running ? State::Cleared : state);
        return frame;
    }
}
