#pragma once

#include "CharacterRuntimeState.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace ActionRPG::Quests
{
    using Json = Items::Json;
    using Rules = PlayerSkills::Catalog;
    inline constexpr std::size_t MAX_ACTIVE = 16;
    inline constexpr std::size_t MAX_QUEUE = 64;
    inline constexpr std::size_t MAX_PAGE_BYTES = 14 * 1024;

    inline void Id(const Json& value) { Rules::Require(value.is_string() && Rules::IsId(value.get<std::string>()), "Invalid quest identifier."); }
    inline std::uint64_t Decimal(const Json& value, bool zero = true) { return Items::DecimalId(value, zero); }
    inline std::string Decimal(std::uint64_t value) { return std::to_string(value); }
    inline std::uint64_t UtcNowMs()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    // UTC calendar rules are independent of combat time and local Windows timezone.
    inline std::uint64_t ParseUtc(const std::string& value)
    {
        Rules::Require(value.size() == 20 && value[4] == '-' && value[7] == '-' && value[10] == 'T'
            && value[13] == ':' && value[16] == ':' && value[19] == 'Z', "Expected UTC timestamp.");
        const auto number = [&value](std::size_t at, std::size_t size)
        {
            Rules::Require(std::all_of(value.begin() + at, value.begin() + at + size,
                [](char digit) { return digit >= '0' && digit <= '9'; }), "UTC fields require digits.");
            int result{};
            const auto [end, error] = std::from_chars(value.data() + at, value.data() + at + size, result);
            Rules::Require(error == std::errc{} && end == value.data() + at + size, "Invalid UTC digits.");
            return result;
        };
        const std::chrono::year_month_day date{std::chrono::year(number(0, 4)),
            std::chrono::month(static_cast<unsigned>(number(5, 2))), std::chrono::day(static_cast<unsigned>(number(8, 2)))};
        const int hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
        Rules::Require(date.ok() && int(date.year()) >= 1970 && hour <= 23 && minute <= 59 && second <= 59,
            "Invalid UTC date/time.");
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::sys_days(date).time_since_epoch() + std::chrono::hours(hour)
            + std::chrono::minutes(minute) + std::chrono::seconds(second)).count());
    }

    struct Catalog
    {
        Json source;
        std::map<std::string, Json> definitions, npcs;
        unsigned dailyHour{}, weeklyHour{};

        static void ValidateDefinition(const Json& quest)
        {
            Rules::Keys(quest, {"id", "version", "title", "description", "category", "repeat", "accept",
                "complete", "event", "steps", "fixedRewards", "choiceRewards"});
            Id(quest.at("id")); Rules::Integer(quest.at("version"), 1, 1000000);
            Rules::Require(quest.at("version").is_number_integer(), "Quest version must be an integer.");
            for (const auto key : {"title", "description"})
                Rules::Require(quest.at(key).is_string() && quest.at(key).get<std::string>().size() <= 1024, "Invalid quest text.");
            Rules::Require(quest.dump().size() <= 4096, "Quest definition exceeds 4 KiB.");
            Rules::Require(quest.at("category") == "Scenario" || quest.at("category") == "Normal"
                || quest.at("category") == "Event", "Invalid quest category.");
            Rules::Require(quest.at("repeat") == "Once" || quest.at("repeat") == "Daily" || quest.at("repeat") == "Weekly", "Invalid repeat.");
            const auto& accept = quest.at("accept");
            Rules::Keys(accept, {"mode", "minLevel", "prerequisites", "mapIds", "npcId"});
            Rules::Require(accept.at("mode") == "Automatic" || accept.at("mode") == "List" || accept.at("mode") == "Npc", "Invalid accept mode.");
            Rules::Integer(accept.at("minLevel"), 1, 1000000);
            for (const auto key : {"prerequisites", "mapIds"})
            {
                Rules::Require(accept.at(key).is_array() && accept.at(key).size() <= 8, "Invalid quest condition list.");
                std::set<std::string> ids;
                for (const auto& id : accept.at(key)) { Id(id); Rules::Require(ids.insert(id.get<std::string>()).second, "Duplicate quest condition."); }
            }
            const auto& complete = quest.at("complete");
            Rules::Keys(complete, {"mode", "npcId"});
            Rules::Require(complete.at("mode") == "Automatic" || complete.at("mode") == "NpcReport", "Invalid completion mode.");
            for (const auto* rule : {&accept, &complete})
            {
                if (!rule->at("npcId").is_null()) Id(rule->at("npcId"));
                const bool needsNpc = rule->at("mode") == "Npc" || rule->at("mode") == "NpcReport";
                Rules::Require(needsNpc != rule->at("npcId").is_null(), "Invalid NPC condition.");
            }
            if (!quest.at("event").is_null())
            {
                const auto& event = quest.at("event");
                Rules::Keys(event, {"id", "visibility", "acceptance", "progress", "reward"}); Id(event.at("id"));
                for (const auto key : {"visibility", "acceptance", "progress", "reward"})
                {
                    Rules::Keys(event.at(key), {"startUtc", "endUtc"});
                    Rules::Require(ParseUtc(event.at(key).at("startUtc").get<std::string>())
                        < ParseUtc(event.at(key).at("endUtc").get<std::string>()), "Empty event interval.");
                }
                Rules::Require(ParseUtc(event.at("acceptance").at("endUtc")) <= ParseUtc(event.at("progress").at("endUtc"))
                    && ParseUtc(event.at("progress").at("endUtc")) <= ParseUtc(event.at("reward").at("endUtc")), "Invalid event deadlines.");
            }
            Rules::Require(quest.at("category") != "Event" || !quest.at("event").is_null(), "Event quest needs intervals.");
            const auto& steps = quest.at("steps");
            Rules::Require(steps.is_array() && !steps.empty() && steps.size() <= 8, "Invalid quest steps.");
            std::set<std::string> stepIds, objectiveIds;
            for (const auto& step : steps)
            {
                Rules::Keys(step, {"id", "objectives"}); Id(step.at("id"));
                Rules::Require(stepIds.insert(step.at("id")).second && step.at("objectives").is_array()
                    && !step.at("objectives").empty() && step.at("objectives").size() <= 8, "Invalid objective list.");
                for (const auto& objective : step.at("objectives"))
                {
                    Rules::Keys(objective, {"id", "type", "target", "count"}, {"description"}); Id(objective.at("id"));
                    if (objective.contains("description")) Rules::Require(objective.at("description").is_string()
                        && objective.at("description").get<std::string>().size() <= 256, "Invalid objective description.");
                    Rules::Require(objectiveIds.insert(objective.at("id")).second, "Duplicate objective ID.");
                    Rules::Integer(objective.at("count"), 1, 1000000);
                    const auto type = objective.at("type").get<std::string>(); const auto& target = objective.at("target");
                    if (type == "KillMonster") { Rules::Keys(target, {"monsterDataId"}); Rules::Integer(target.at("monsterDataId"), 1, 1000000); }
                    else if (type == "HoldItem" || type == "AcquireItem") { Rules::Keys(target, {"itemDefinitionId"}); Id(target.at("itemDefinitionId")); }
                    else if (type == "ClearDungeon") { Rules::Keys(target, {"dungeonId"}); Rules::Integer(target.at("dungeonId"), 1, 1000000); }
                    else if (type == "TalkNpc") { Rules::Keys(target, {"npcId"}); Id(target.at("npcId")); }
                    else if (type == "Reach")
                    {
                        Rules::Require(objective.at("count") == 1, "Reach is a single arrival objective.");
                        Rules::Keys(target, {"mapId"}, {"region"}); Id(target.at("mapId"));
                        if (target.contains("region"))
                        {
                            const auto& region = target.at("region"); Rules::Keys(region, {"id", "x", "y", "radius"}); Id(region.at("id"));
                            Rules::Number(region.at("x"), -1000000, 1000000); Rules::Number(region.at("y"), -1000000, 1000000);
                            Rules::Number(region.at("radius"), 1, 1000);
                        }
                    }
                    else Rules::Require(false, "Unsupported quest objective.");
                }
            }
            const auto rewards = [](const Json& rows)
            {
                Rules::Require(rows.is_array() && rows.size() <= 8, "Invalid rewards.");
                for (const auto& row : rows)
                {
                    if (row.at("type") == "Item") { Rules::Keys(row, {"type", "definitionId", "count"}); Id(row.at("definitionId")); Rules::Integer(row.at("count"), 1, 1000000); }
                    else { Rules::Require(row.at("type") == "SkillPoints", "Unsupported reward economy."); Rules::Keys(row, {"type", "amount"}); Rules::Integer(row.at("amount"), 1, 1000000); }
                }
            };
            rewards(quest.at("fixedRewards"));
            Rules::Require(quest.at("choiceRewards").is_array() && quest.at("choiceRewards").size() <= 8, "Invalid reward choices.");
            std::set<std::string> choices;
            for (const auto& choice : quest.at("choiceRewards"))
            {
                Rules::Keys(choice, {"id", "rewards"}); Id(choice.at("id"));
                Rules::Require(choices.insert(choice.at("id")).second, "Duplicate reward choice."); rewards(choice.at("rewards"));
            }
        }

        static Catalog Load(const std::filesystem::path& path)
        {
            Rules::Require(std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) <= 512 * 1024, "Missing/oversized Quests.json.");
            std::ifstream input(path, std::ios::binary); Json value; input >> value;
            Rules::Keys(value, {"format", "schemaVersion", "resetPolicy", "npcs", "quests"});
            Rules::Require(value.at("format") == "Quests" && value.at("schemaVersion") == 1, "Invalid quest catalog.");
            const auto& policy = value.at("resetPolicy"); Rules::Keys(policy, {"timezone", "dailyHour", "weeklyDay", "weeklyHour"});
            Rules::Require(policy.at("timezone") == "UTC" && policy.at("weeklyDay") == "Monday", "Unsupported reset calendar.");
            Catalog result; result.dailyHour = Rules::Integer(policy.at("dailyHour"), 0, 23); result.weeklyHour = Rules::Integer(policy.at("weeklyHour"), 0, 23);
            Rules::Require(value.at("npcs").is_array() && value.at("npcs").size() <= 32
                && value.at("quests").is_array() && value.at("quests").size() <= 64, "Quest catalog capacity exceeded.");
            for (const auto& npc : value.at("npcs"))
            {
                Rules::Keys(npc, {"id", "name", "mapId", "x", "y"}); Id(npc.at("id")); Id(npc.at("mapId"));
                Rules::Require(npc.at("name").is_string() && npc.at("name").get<std::string>().size() <= 128, "Invalid NPC name.");
                Rules::Number(npc.at("x"), -1000000, 1000000); Rules::Number(npc.at("y"), -1000000, 1000000);
                Rules::Require(result.npcs.emplace(npc.at("id"), npc).second, "Duplicate quest NPC.");
            }
            for (const auto& quest : value.at("quests")) { ValidateDefinition(quest); Rules::Require(result.definitions.emplace(quest.at("id"), quest).second, "Duplicate quest."); }
            std::set<std::string> visiting, visited;
            const std::function<void(const std::string&)> visit = [&](const auto& id)
            {
                Rules::Require(!visiting.contains(id) && result.definitions.contains(id), "Quest prerequisite cycle/reference.");
                if (visited.contains(id)) return; visiting.insert(id);
                for (const auto& prerequisite : result.definitions.at(id).at("accept").at("prerequisites")) visit(prerequisite.get<std::string>());
                visiting.erase(id); visited.insert(id);
            };
            for (const auto& [id, quest] : result.definitions)
            {
                visit(id);
                for (const auto key : {"accept", "complete"}) if (!quest.at(key).at("npcId").is_null())
                    Rules::Require(result.npcs.contains(quest.at(key).at("npcId")), "Missing quest NPC.");
                for (const auto& step : quest.at("steps")) for (const auto& objective : step.at("objectives"))
                    if (objective.at("type") == "TalkNpc") Rules::Require(result.npcs.contains(objective.at("target").at("npcId")), "Missing dialogue NPC.");
            }
            result.source = std::move(value); return result;
        }

        void ValidateItems(const Items::Catalog& items) const
        {
            for (const auto& [id, quest] : definitions)
            {
                for (const auto& step : quest.at("steps")) for (const auto& goal : step.at("objectives"))
                    if (goal.at("type") == "HoldItem" || goal.at("type") == "AcquireItem")
                        Rules::Require(items.definitions.contains(goal.at("target").at("itemDefinitionId")), "Unknown quest collection item.");
                auto rewards = quest.at("fixedRewards");
                for (const auto& choice : quest.at("choiceRewards")) for (const auto& reward : choice.at("rewards")) rewards.push_back(reward);
                for (const auto& reward : rewards) if (reward.at("type") == "Item")
                    Rules::Require(items.definitions.contains(reward.at("definitionId")), "Unknown quest reward item.");
            }
        }

        bool Within(const Json& quest, const char* window, std::uint64_t now) const
        {
            if (quest.at("event").is_null()) return true;
            const auto& interval = quest.at("event").at(window);
            return now >= ParseUtc(interval.at("startUtc")) && now < ParseUtc(interval.at("endUtc"));
        }

        std::string Cycle(const Json& quest, std::uint64_t now, const Json* policy = nullptr) const
        {
            if (quest.at("repeat") == "Once") return "once";
            const auto hour = quest.at("repeat") == "Daily" ? (policy ? policy->at("dailyHour").get<unsigned>() : dailyHour)
                : (policy ? policy->at("weeklyHour").get<unsigned>() : weeklyHour);
            const auto point = std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(now)) - std::chrono::hours(hour);
            auto day = std::chrono::floor<std::chrono::days>(point);
            if (quest.at("repeat") == "Weekly") day -= std::chrono::days((std::chrono::weekday(day).iso_encoding() + 6) % 7);
            const std::chrono::year_month_day date(day);
            const auto pad = [](unsigned v) { return (v < 10 ? "0" : "") + std::to_string(v); };
            return std::to_string(int(date.year())) + "-" + pad(unsigned(date.month())) + "-" + pad(unsigned(date.day()));
        }
    };
}
