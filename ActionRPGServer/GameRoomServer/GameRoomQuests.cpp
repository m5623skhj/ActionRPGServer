#include "GameRoom.h"
#include <algorithm>
#include <cmath>

namespace GameRoomServer
{
    namespace Q = ActionRPG::Quests;
    using Q::Json;

    void GameRoom::SetQuestTransport(std::string incarnation, std::function<void(std::string)> send)
    {
        asio::dispatch(strand, [self = shared_from_this(), incarnation = std::move(incarnation), send = std::move(send)]() mutable
        { self->questIncarnation = std::move(incarnation); self->questSend = std::move(send); });
    }
    std::vector<GameRoom::PlayerId> GameRoom::QuestParticipants(const std::string& mapId) const
    {
        std::vector<PlayerId> result;
        for (const auto& [id, player] : players)
            if (enteredPlayers.contains(id) && player.worldReady && player.mapId == mapId) result.push_back(id);
        std::sort(result.begin(), result.end()); return result;
    }
    std::vector<std::string> GameRoom::QuestRegions(const PlayerState& player) const
    {
        std::set<std::string> result;
        for (const auto& [id, definition] : combatDefinition->quests.definitions)
            for (const auto& stage : definition.at("steps")) for (const auto& goal : stage.at("objectives"))
            {
                if (goal.at("type") != "Reach" || goal.at("target").at("mapId") != player.mapId) continue;
                const auto& target = goal.at("target");
                if (!target.contains("region")) continue;
                const auto& region = target.at("region");
                if (std::hypot(player.position.x - region.at("x").get<float>(), player.position.y - region.at("y").get<float>())
                    <= region.at("radius").get<float>()) result.insert(region.at("id").get<std::string>());
            }
        return {result.begin(), result.end()};
    }
    bool GameRoom::EmitQuest(std::string kind, std::string target, const std::string& mapId,
        std::vector<PlayerId> credited, std::uint64_t monsterId)
    {
        if (questOutbox.size() >= 512 || questSequence == UINT64_MAX || questIncarnation.empty()) return false;
        Json ids = Json::array(); for (const auto id : credited) ids.push_back(Q::Decimal(id));
        Json fact{{"incarnation", questIncarnation}, {"sequence", Q::Decimal(++questSequence)}, {"dungeonId", dungeonId},
            {"mapId", mapId}, {"kind", std::move(kind)}, {"targetId", std::move(target)}, {"creditedPlayerIds", ids}};
        if (monsterId) fact["monsterInstanceId"] = Q::Decimal(monsterId);
        questOutbox.push_back(std::move(fact)); if (questBatch.empty()) FlushQuests(); return true;
    }
    void GameRoom::ObserveQuestLocations()
    {
        for (const auto& [id, player] : players)
        {
            if (!enteredPlayers.contains(id) || !player.worldReady) continue;
            std::unordered_set<std::string> regions;
            auto targets = QuestRegions(player); targets.push_back(player.mapId);
            for (const auto& target : targets)
            {
                const auto key = player.mapId + ":" + target;
                if (questRegions[id].contains(key)) { regions.insert(key); continue; }
                if (EmitQuest("RegionReached", target, player.mapId, {id})) regions.insert(key);
            }
            questRegions[id] = std::move(regions);
        }
    }
    /** Retransmit exactly the same bounded batch until Town confirms the committed facts. */
    void GameRoom::FlushQuests()
    {
        if (questOutbox.empty() || !questSend) return;
        if (questBatch.empty())
        {
            Json batch{{"events", Json::array()}};
            for (const auto& event : questOutbox)
            {
                auto next = batch; next["events"].push_back(event);
                if (next.dump().size() > 16384 || next.at("events").size() > 32) break;
                batch = std::move(next);
            }
            questBatch = batch.dump();
        }
        questSend(questBatch); questTimer.expires_after(std::chrono::seconds(1));
        questTimer.async_wait([self = shared_from_this()](const asio::error_code& error) { if (!error) self->FlushQuests(); });
    }
    void GameRoom::AcknowledgeQuests(std::string text)
    {
        asio::dispatch(strand, [self = shared_from_this(), text = std::move(text)]
        {
            if (text != self->questBatch || self->questBatch.empty()) return;
            const auto count = Json::parse(self->questBatch).at("events").size();
            for (std::size_t index = 0; index < count; ++index) self->questOutbox.pop_front();
            self->questBatch.clear(); self->questTimer.cancel();
            if (!self->questOutbox.empty()) { self->FlushQuests(); return; }
            auto drains = std::move(self->questDrains); self->questDrains.clear();
            for (auto& done : drains) done();
        });
    }
    void GameRoom::DrainQuests(std::function<void()> done)
    {
        if (questOutbox.empty()) done(); else questDrains.push_back(std::move(done));
    }
    void GameRoom::QuestContext(PlayerId playerId, std::function<void(std::string)> handler)
    {
        asio::dispatch(strand, [self = shared_from_this(), playerId, handler = std::move(handler)]
        {
            const auto found = self->players.find(playerId);
            if (found == self->players.end() || !self->enteredPlayers.contains(playerId) || !found->second.worldReady)
            { handler(""); return; }
            const auto& player = found->second;
            handler(Json{{"mapId", player.mapId}, {"roomId", Q::Decimal(self->roomId)},
                {"x", player.position.x}, {"y", player.position.y}, {"regions", self->QuestRegions(player)},
                {"cursor", {{"incarnation", self->questIncarnation}, {"roomId", Q::Decimal(self->roomId)},
                    {"sequence", Q::Decimal(self->questSequence)}}}}.dump());
        });
    }
}
