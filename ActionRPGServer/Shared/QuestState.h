#pragma once

#include "QuestCatalog.h"

namespace ActionRPG::Quests
{
    struct State
    {
        // Durable instance shape is shared with V7. Summary rows use cycle_key="" and the separate shape below.
        std::map<std::string, Json> instances, summaries;
        std::vector<std::string> tracked;

        static std::string Key(const std::string& id, const std::string& cycle) { return id + ":" + cycle; }
        static bool Live(const Json& state)
        { return state.at("state") == "Active" || state.at("state") == "ReadyToReport" || state.at("state") == "RewardPending"; }
        static void ValidateInstance(const Json& value)
        {
            Rules::Keys(value, {"kind", "questId", "cycleKey", "definitionVersion", "acceptGeneration", "state", "stageIndex",
                "counters", "definition", "resetPolicy", "acceptedAtMs", "updatedAtMs", "roomCursor"});
            Rules::Require(value.at("kind") == "Instance" && value.dump().size() <= 8192, "Invalid quest state shape/size.");
            Id(value.at("questId")); Rules::Require(value.at("cycleKey").is_string(), "Invalid cycle key.");
            const auto cycle = value.at("cycleKey").get<std::string>();
            Rules::Require(cycle == "once" || cycle.size() == 10, "Invalid cycle date.");
            if (cycle != "once") static_cast<void>(ParseUtc(cycle + "T00:00:00Z"));
            Decimal(value.at("acceptGeneration"), false); Decimal(value.at("acceptedAtMs")); Decimal(value.at("updatedAtMs"));
            const auto& definition = value.at("definition"); Catalog::ValidateDefinition(definition);
            const auto& policy = value.at("resetPolicy"); Rules::Keys(policy, {"timezone", "dailyHour", "weeklyDay", "weeklyHour"});
            Rules::Require(policy.at("timezone") == "UTC" && policy.at("weeklyDay") == "Monday", "Invalid stored reset calendar.");
            Rules::Integer(policy.at("dailyHour"), 0, 23); Rules::Integer(policy.at("weeklyHour"), 0, 23);
            Rules::Require(definition.at("id") == value.at("questId") && definition.at("version") == value.at("definitionVersion"), "Stored quest definition mismatch.");
            const auto phase = value.at("state").get<std::string>();
            Rules::Require(phase == "Active" || phase == "ReadyToReport" || phase == "RewardPending" || phase == "Completed"
                || phase == "Abandoned" || phase == "Expired", "Invalid quest state.");
            Rules::Integer(value.at("stageIndex"), 0, static_cast<std::uint32_t>(definition.at("steps").size()));
            const auto stage = value.at("stageIndex").get<std::size_t>();
            Rules::Require(phase != "Active" || stage < definition.at("steps").size(), "Invalid active quest stage.");
            Rules::Require((phase != "ReadyToReport" && phase != "RewardPending" && phase != "Completed")
                || stage == definition.at("steps").size(), "Invalid completed quest stage.");
            const auto& counters = value.at("counters");
            Rules::Require(counters.is_array() && counters.size() == definition.at("steps").size(), "Invalid quest counters.");
            for (std::size_t step = 0; step < counters.size(); ++step)
            {
                const auto& goals = definition.at("steps").at(step).at("objectives");
                Rules::Require(counters.at(step).is_array() && counters.at(step).size() == goals.size(), "Invalid objective counters.");
                for (std::size_t goal = 0; goal < goals.size(); ++goal)
                    Rules::Integer(counters.at(step).at(goal), 0, goals.at(goal).at("count").get<std::uint32_t>());
            }
            if (!value.at("roomCursor").is_null())
            {
                const auto& cursor = value.at("roomCursor"); Rules::Keys(cursor, {"incarnation", "roomId", "sequence"});
                Rules::Require(Items::IsHexId(cursor.at("incarnation").get<std::string>(), 32), "Invalid quest incarnation.");
                Decimal(cursor.at("roomId"), false); Decimal(cursor.at("sequence"));
            }
        }

        void Add(const std::string& id, const std::string& cycle, const Json& value)
        {
            Id(id);
            if (cycle.empty())
            {
                Rules::Keys(value, {"kind", "completedEver", "lastCompletedCycle", "lastAcceptGeneration", "lastState", "lastCycleKey"});
                Rules::Require(value.at("kind") == "Summary" && value.at("completedEver").is_boolean()
                    && value.at("lastCompletedCycle").is_string() && value.at("lastState").is_string()
                    && value.at("lastCycleKey").is_string(), "Invalid quest summary.");
                Decimal(value.at("lastAcceptGeneration"));
                Rules::Require(summaries.emplace(id, value).second, "Duplicate quest summary.");
            }
            else
            {
                ValidateInstance(value);
                Rules::Require(value.at("questId") == id && value.at("cycleKey") == cycle
                    && instances.emplace(Key(id, cycle), value).second, "Duplicate/mismatched quest instance.");
            }
        }

        bool Completed(const std::string& id) const
        {
            const auto summary = summaries.find(id);
            if (summary != summaries.end() && summary->second.at("completedEver") == true) return true;
            for (const auto& [key, row] : instances) if (row.at("questId") == id && row.at("state") == "Completed") return true;
            return false;
        }
        static std::uint32_t Held(const Items::Inventory& inventory, const std::string& id)
        {
            std::uint64_t count{};
            for (const auto& item : inventory.items) if (item.definitionId == id && item.container < Items::EQUIPPED_CONTAINER) count += item.count;
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(count, std::numeric_limits<std::uint32_t>::max()));
        }
        static Json TurnIn(const Json& definition)
        {
            std::map<std::string, std::uint32_t> totals;
            for (const auto& step : definition.at("steps")) for (const auto& goal : step.at("objectives"))
                if (goal.at("type") == "HoldItem") totals[goal.at("target").at("itemDefinitionId").get<std::string>()] += goal.at("count").get<std::uint32_t>();
            auto rows = Json::array();
            for (const auto& [id, count] : totals) rows.push_back({{"definitionId", id}, {"count", count}});
            return rows;
        }

        bool Eligible(const Catalog& catalog, const Json& quest, std::uint32_t level, const std::string& map,
            std::uint64_t now) const
        {
            if (!catalog.Within(quest, "acceptance", now) || level < quest.at("accept").at("minLevel").get<std::uint32_t>()) return false;
            const auto& maps = quest.at("accept").at("mapIds");
            if (!maps.empty() && std::find(maps.begin(), maps.end(), map) == maps.end()) return false;
            for (const auto& id : quest.at("accept").at("prerequisites")) if (!Completed(id.get<std::string>())) return false;
            const auto id = quest.at("id").get<std::string>(), cycle = catalog.Cycle(quest, now);
            if (quest.at("repeat") == "Once" && Completed(id)) return false;
            const auto found = instances.find(Key(id, cycle));
            if (found != instances.end() && (Live(found->second) || found->second.at("state") == "Completed")) return false;
            const auto summary = summaries.find(id);
            return summary == summaries.end() || summary->second.at("lastCompletedCycle") != cycle;
        }

        std::string Accept(const Catalog& catalog, const Json& quest, std::uint32_t level,
            const std::string& map, std::uint64_t now, const Json& cursor)
        {
            if (!Eligible(catalog, quest, level, map, now)) return "RequirementsNotMet";
            std::size_t active{}; for (const auto& [key, row] : instances) if (Live(row)) ++active;
            if (active >= MAX_ACTIVE) return "QuestLimit";
            const auto id = quest.at("id").get<std::string>(); std::uint64_t generation{};
            if (summaries.contains(id)) generation = Decimal(summaries.at(id).at("lastAcceptGeneration"));
            for (const auto& [key, row] : instances) if (row.at("questId") == id) generation = std::max(generation, Decimal(row.at("acceptGeneration")));
            if (generation == std::numeric_limits<std::uint64_t>::max()) return "QuestLimit";
            auto counters = Json::array();
            for (const auto& step : quest.at("steps")) counters.push_back(std::vector<std::uint32_t>(step.at("objectives").size(), 0));
            const auto cycle = catalog.Cycle(quest, now);
            Json row{{"kind", "Instance"}, {"questId", id}, {"cycleKey", cycle}, {"definitionVersion", quest.at("version")},
                {"acceptGeneration", Decimal(generation + 1)}, {"state", "Active"}, {"stageIndex", 0}, {"counters", counters},
                {"definition", quest}, {"resetPolicy", catalog.source.at("resetPolicy")},
                {"acceptedAtMs", Decimal(now)}, {"updatedAtMs", Decimal(now)}, {"roomCursor", cursor}};
            ValidateInstance(row); instances[Key(id, cycle)] = std::move(row); return "Succeeded";
        }

        /** Update one current stage only. An event cannot leak through into the next stage. */
        bool Evaluate(const Catalog& catalog, Json& row, const Items::Inventory& inventory, std::uint64_t now, const Json& event)
        {
            if (row.at("state") != "Active") return false;
            const auto& definition = row.at("definition");
            if (!catalog.Within(definition, "progress", now)) return false;
            const auto before = row;
            const auto step = row.at("stageIndex").get<std::size_t>();
            if (step >= definition.at("steps").size()) return false;
            const auto& goals = definition.at("steps").at(step).at("objectives");
            bool newEvent = !event.is_null();
            if (newEvent && event.contains("cursor"))
            {
                const auto& previous = row.at("roomCursor"); const auto& cursor = event.at("cursor");
                if (!previous.is_null() && previous.at("incarnation") == cursor.at("incarnation")
                    && previous.at("roomId") == cursor.at("roomId")
                    && Decimal(cursor.at("sequence")) <= Decimal(previous.at("sequence"))) newEvent = false;
            }
            bool done = true;
            for (std::size_t index = 0; index < goals.size(); ++index)
            {
                const auto& goal = goals.at(index); auto count = row["counters"][step][index].get<std::uint32_t>();
                const auto required = goal.at("count").get<std::uint32_t>(); const auto type = goal.at("type").get<std::string>();
                const auto& target = goal.at("target");
                if (type == "HoldItem") count = std::min(required, Held(inventory, target.at("itemDefinitionId")));
                else if (newEvent)
                {
                    const auto kind = event.at("kind").get<std::string>(), id = event.at("targetId").get<std::string>();
                    bool match = (type == "KillMonster" && kind == "MonsterKilled" && id == std::to_string(target.value("monsterDataId", 0u)))
                        || (type == "AcquireItem" && kind == "AcquireItem" && id == target.value("itemDefinitionId", std::string{}))
                        || (type == "TalkNpc" && kind == "TalkNpc" && id == target.value("npcId", std::string{}))
                        || (type == "ClearDungeon" && kind == "DungeonCleared" && id == std::to_string(target.value("dungeonId", 0u)));
                    if (type == "Reach" && kind == "RegionReached" && event.value("mapId", std::string{}) == target.at("mapId").get<std::string>())
                        match = id == (target.contains("region") ? target.at("region").at("id").get<std::string>() : target.at("mapId").get<std::string>());
                    if (match) count = static_cast<std::uint32_t>(std::min<std::uint64_t>(required,
                        static_cast<std::uint64_t>(count) + event.value("amount", 1u)));
                }
                row["counters"][step][index] = count; if (count < required) done = false;
            }
            if (done)
            {
                row["stageIndex"] = step + 1;
                if (step + 1 == definition.at("steps").size()) row["state"] = definition.at("complete").at("mode") == "NpcReport" ? "ReadyToReport" : "RewardPending";
            }
            if (row != before && newEvent && event.contains("cursor")) row["roomCursor"] = event.at("cursor");
            if (row != before) { row["updatedAtMs"] = Decimal(now); return true; }
            return false;
        }

        void Refresh(const Catalog& catalog, const Items::Inventory& inventory, std::uint32_t level,
            const std::string& map, std::uint64_t now, const Json& cursor, bool evaluateInventory = true)
        {
            for (auto& [key, row] : instances)
            {
                if (!Live(row)) continue;
                const auto& definition = row.at("definition");
                bool expired = false;
                if (row.at("state") == "Active" && definition.at("repeat") != "Once"
                    && row.at("cycleKey") != catalog.Cycle(definition, now, &row.at("resetPolicy"))) expired = true;
                if (!definition.at("event").is_null())
                {
                    const auto window = row.at("state") == "Active" ? "progress" : "reward";
                    expired = expired || now >= ParseUtc(definition.at("event").at(window).at("endUtc"));
                }
                if (expired) { row["state"] = "Expired"; row["updatedAtMs"] = Decimal(now); }
                else if (evaluateInventory)
                {
                    const auto stage = row.at("stageIndex");
                    Evaluate(catalog, row, inventory, now, nullptr);
                    if (row.at("stageIndex") != stage && !cursor.is_null()) row["roomCursor"] = cursor;
                }
            }
            if (evaluateInventory) for (const auto& [id, quest] : catalog.definitions)
                if (quest.at("accept").at("mode") == "Automatic" && Eligible(catalog, quest, level, map, now))
                    static_cast<void>(Accept(catalog, quest, level, map, now, cursor));
            std::erase_if(tracked, [this](const auto& id)
            {
                return std::none_of(instances.begin(), instances.end(), [&id](const auto& pair) { return pair.second.at("questId") == id && Live(pair.second); });
            });
        }

        Json Changes(const State& previous) const
        {
            auto rows = Json::array();
            for (const auto& [key, row] : instances)
                if (!previous.instances.contains(key) || previous.instances.at(key) != row)
                    rows.push_back({{"questId", row.at("questId")}, {"cycleKey", row.at("cycleKey")}, {"state", row}});
            Json result{{"upserts", rows}};
            if (tracked != previous.tracked) result["trackedQuestIds"] = tracked;
            return result;
        }
    };
}
