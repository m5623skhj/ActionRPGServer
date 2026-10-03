#include "MonsterDefinition.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameRoomServer
{
    namespace
    {
        using Json = nlohmann::json;
        using Index = std::unordered_map<std::string, const Json*>;
        constexpr std::uintmax_t MAX_DOCUMENT_BYTES = 4 * 1024 * 1024;
        constexpr std::uintmax_t MAX_CATALOG_DOCUMENT_BYTES = 64 * 1024 * 1024;

        void Require(const bool inCondition, const std::string& inMessage)
        {
            if (!inCondition) throw std::runtime_error(inMessage);
        }
        void Keys(const Json& inValue, const std::initializer_list<std::string_view> inKeys)
        {
            Require(inValue.is_object(), "Expected a JSON object.");
            for (const auto& [key, value] : inValue.items())
                Require(std::find(inKeys.begin(), inKeys.end(), key) != inKeys.end(), "Unsupported field: " + key);
        }
        std::string Text(const Json& inValue, const bool inIdentifier = false)
        {
            Require(inValue.is_string(), "Expected a string.");
            const std::string value = inValue.get<std::string>();
            Require(!value.empty() && value.size() <= 1024 && value.find_first_not_of(" \t\r\n") != std::string::npos,
                "Empty or oversized text.");
            if (inIdentifier)
            {
                static const std::regex pattern("[A-Za-z][A-Za-z0-9_.-]{0,63}");
                Require(std::regex_match(value, pattern), "Invalid identifier: " + value);
            }
            return value;
        }
        double Number(const Json& inValue, const double inMin,
            const double inMax = std::numeric_limits<float>::max())
        {
            Require(inValue.is_number(), "Expected a number.");
            const double value = inValue.get<double>();
            Require(std::isfinite(value) && value >= inMin && value <= inMax, "Number is outside its valid range.");
            return value;
        }
        std::uint64_t Integer(const Json& inValue, const std::uint64_t inMax)
        {
            Require(inValue.is_number_integer(), "Expected a nonnegative integer.");
            if (!inValue.is_number_unsigned()) Require(inValue.get<std::int64_t>() >= 0, "Negative integer.");
            const auto value = inValue.get<std::uint64_t>();
            Require(value <= inMax, "Integer is outside its valid range.");
            return value;
        }
        Index MakeIndex(const Json& inList, const std::size_t inLimit)
        {
            Require(inList.is_array() && inList.size() <= inLimit, "Invalid definition list size.");
            Index index;
            for (const auto& value : inList)
            {
                const std::string id = Text(value.at("id"), true);
                Require(index.emplace(id, &value).second, "Duplicate identifier: " + id);
            }
            return index;
        }
        void Reference(const Json& inValue, const Index& inIndex)
        {
            const std::string id = Text(inValue, true);
            Require(inIndex.contains(id), "Unknown reference: " + id);
        }
        void ValidateAction(const Json& inAction, const Index& inSkills, const Index& inMotions)
        {
            Keys(inAction, { "type", "parameters" });
            const std::string type = Text(inAction.at("type"));
            const auto& parameters = inAction.at("parameters");
            if (type == "Wait") { Keys(parameters, { "seconds" }); (void)Number(parameters.at("seconds"), 0.001); }
            else if (type == "MoveToTarget")
            {
                Keys(parameters, { "stopDistance", "run" });
                (void)Number(parameters.at("stopDistance"), 0);
                Require(parameters.at("run").is_boolean(), "MoveToTarget.run must be boolean.");
            }
            else if (type == "ReturnToSpawn")
            {
                Keys(parameters, { "arrivalDistance" }); (void)Number(parameters.at("arrivalDistance"), 0);
            }
            else if (type == "UseSkill") { Keys(parameters, { "skillId" }); Reference(parameters.at("skillId"), inSkills); }
            else if (type == "PlayMotion") { Keys(parameters, { "motionId" }); Reference(parameters.at("motionId"), inMotions); }
            else throw std::runtime_error("Unsupported action: " + type);
        }
        void ValidateCondition(const Json& inCondition, const Index& inSkills,
            const unsigned inDepth, unsigned& inTerms)
        {
            Require(inDepth <= 8 && ++inTerms <= 128, "Condition depth or term limit exceeded.");
            const std::string type = Text(inCondition.at("type"));
            if (type == "All" || type == "Any" || type == "Not")
            {
                Keys(inCondition, { "type", "children" });
                const auto& children = inCondition.at("children");
                Require(children.is_array() && !children.empty() && (type != "Not" || children.size() == 1),
                    "Invalid compound condition.");
                for (const auto& child : children) ValidateCondition(child, inSkills, inDepth + 1, inTerms);
                return;
            }
            Keys(inCondition, { "type", "parameters" });
            const auto& parameters = inCondition.at("parameters");
            if (type == "Always" || type == "HasTarget" || type == "TargetLost") Keys(parameters, {});
            else if (type == "TargetInRange" || type == "TargetOutOfRange" || type == "AtSpawn")
            { Keys(parameters, { "distance" }); (void)Number(parameters.at("distance"), 0); }
            else if (type == "StateTimeAtLeast")
            { Keys(parameters, { "seconds" }); (void)Number(parameters.at("seconds"), 0); }
            else if (type == "HealthRatioAtMost")
            { Keys(parameters, { "ratio" }); (void)Number(parameters.at("ratio"), 0, 1); }
            else if (type == "SkillReady") { Keys(parameters, { "skillId" }); Reference(parameters.at("skillId"), inSkills); }
            else throw std::runtime_error("Unsupported condition: " + type);
        }
        bool DelaysOnEntry(const Json& inCondition)
        {
            const std::string type = inCondition.at("type");
            if (type == "StateTimeAtLeast") return inCondition.at("parameters").at("seconds").get<double>() > 0;
            if (type == "All" || type == "Any")
            {
                const auto& children = inCondition.at("children");
                if (type == "All") return std::any_of(children.begin(), children.end(), DelaysOnEntry);
                return std::all_of(children.begin(), children.end(), DelaysOnEntry);
            }
            return false;
        }

        // Cycles across ticks are valid. Reject only paths that can loop without consuming time.
        void ValidateGraph(const Json& inAi, const Index& inSkills, const Index& inMotions)
        {
            Keys(inAi, { "initialNodeId", "nodes", "edges" });
            const Index nodes = MakeIndex(inAi.at("nodes"), 512);
            Require(!nodes.empty(), "AI needs an initial state.");
            Reference(inAi.at("initialNodeId"), nodes);
            const Index edges = MakeIndex(inAi.at("edges"), 2048);
            for (const auto& [id, node] : nodes)
            {
                Keys(*node, { "id", "label", "position", "action" });
                (void)Text(node->at("label"));
                Keys(node->at("position"), { "x", "y" });
                (void)Number(node->at("position").at("x"), -1000000, 1000000);
                (void)Number(node->at("position").at("y"), -1000000, 1000000);
                ValidateAction(node->at("action"), inSkills, inMotions);
            }
            std::unordered_map<std::string, std::vector<const Json*>> outgoing;
            std::unordered_map<std::string, std::vector<std::string>> immediate;
            for (const auto& [id, node] : nodes)
            {
                outgoing.emplace(id, std::vector<const Json*>{});
                immediate.emplace(id, std::vector<std::string>{});
            }
            for (const auto& [id, edge] : edges)
            {
                Keys(*edge, { "id", "from", "to", "priority", "trigger", "condition" });
                Reference(edge->at("from"), nodes); Reference(edge->at("to"), nodes);
                (void)Integer(edge->at("priority"), 9007199254740991ULL);
                const std::string trigger = Text(edge->at("trigger"));
                Require(trigger == "OnUpdate" || trigger == "AfterAction" || trigger == "Immediate", "Unsupported trigger.");
                unsigned terms = 0; ValidateCondition(edge->at("condition"), inSkills, 1, terms);
                const std::string from = edge->at("from"), to = edge->at("to");
                const auto& action = nodes.at(from)->at("action");
                const std::string actionType = action.at("type");
                if (trigger == "AfterAction" && actionType == "PlayMotion")
                    Require(!inMotions.at(action.at("parameters").at("motionId").get<std::string>())->at("loop").get<bool>(),
                        "A looping motion cannot finish.");
                outgoing[from].push_back(edge);
                const bool actionTakesTime = actionType == "Wait" || actionType == "UseSkill" || actionType == "PlayMotion";
                if (trigger != "OnUpdate" && !DelaysOnEntry(edge->at("condition"))
                    && !(trigger == "AfterAction" && actionTakesTime)) immediate[from].push_back(to);
            }
            for (auto& [id, list] : outgoing)
            {
                std::sort(list.begin(), list.end(), [](const Json* a, const Json* b)
                    { return a->at("priority").get<std::uint64_t>() < b->at("priority").get<std::uint64_t>(); });
                std::unordered_set<std::string> alwaysTriggers;
                std::unordered_set<std::uint64_t> priorities;
                for (const auto* edge : list)
                {
                    Require(priorities.insert(edge->at("priority").get<std::uint64_t>()).second, "Duplicate outgoing priority.");
                    const std::string trigger = edge->at("trigger");
                    Require(!alwaysTriggers.contains(trigger), "An Always edge masks a lower-priority edge.");
                    if (edge->at("condition").at("type") == "Always") alwaysTriggers.insert(trigger);
                }
            }
            const std::string initial = inAi.at("initialNodeId");
            std::unordered_set<std::string> reached{ initial };
            std::vector<std::string> queue{ initial };
            for (std::size_t index = 0; index < queue.size(); ++index)
                for (const auto* edge : outgoing[queue[index]])
                {
                    const std::string to = edge->at("to");
                    if (reached.insert(to).second) queue.push_back(to);
                }
            Require(reached.size() == nodes.size(), "Unreachable AI state.");
            std::unordered_map<std::string, unsigned> colors;
            std::function<void(const std::string&)> visit = [&](const std::string& id)
            {
                colors[id] = 1;
                for (const auto& to : immediate[id])
                {
                    Require(colors[to] != 1, "AI cycle can repeat without consuming time.");
                    if (colors[to] == 0) visit(to);
                }
                colors[id] = 2;
            };
            for (const auto& [id, node] : nodes) if (colors[id] == 0) visit(id);
        }
        void ValidateDocument(const Json& inDocument)
        {
            Keys(inDocument, { "schemaVersion", "documentId", "skills", "motions", "monsters", "approval" });
            Require(inDocument.at("schemaVersion") == 1, "Unsupported MonsterEditor schema.");
            (void)Text(inDocument.at("documentId"), true);
            const Index skills = MakeIndex(inDocument.at("skills"), 2048);
            const Index motions = MakeIndex(inDocument.at("motions"), 2048);
            const Index monsters = MakeIndex(inDocument.at("monsters"), 256);
            Require(!monsters.empty(), "No monster definitions.");
            for (const auto& [id, skill] : skills)
            {
                Keys(*skill, { "id", "label", "animationId", "durationSeconds", "cooldownSeconds", "minRange", "maxRange", "hitType" });
                (void)Text(skill->at("label")); (void)Text(skill->at("animationId"), true);
                (void)Number(skill->at("durationSeconds"), 0.001); (void)Number(skill->at("cooldownSeconds"), 0);
                const double minRange = Number(skill->at("minRange"), 0);
                (void)Number(skill->at("maxRange"), minRange);
                Require(skill->at("hitType") == "Normal" || skill->at("hitType") == "Airborne", "Invalid skill hit type.");
            }
            for (const auto& [id, motion] : motions)
            {
                Keys(*motion, { "id", "label", "animationId", "durationSeconds", "loop" });
                (void)Text(motion->at("label")); (void)Text(motion->at("animationId"), true);
                (void)Number(motion->at("durationSeconds"), 0.001);
                Require(motion->at("loop").is_boolean(), "Motion.loop must be boolean.");
            }
            for (const auto& [id, monster] : monsters)
            {
                Keys(*monster, { "id", "name", "maxHp", "ai" });
                (void)Text(monster->at("name"));
                Require(Integer(monster->at("maxHp"), std::numeric_limits<std::uint32_t>::max()) > 0, "HP must be positive.");
                ValidateGraph(monster->at("ai"), skills, motions);
            }
        }
        Json ReadJson(const std::filesystem::path& inPath)
        {
            Require(std::filesystem::file_size(inPath) <= MAX_DOCUMENT_BYTES, "Monster JSON exceeds 4 MB.");
            std::ifstream input(inPath, std::ios::binary);
            Require(input.good(), "Cannot read monster JSON.");
            return Json::parse(input);
        }
    }

    const nlohmann::json& MonsterDefinition::GetAi() const { return document->at("monsters").at(monsterIndex).at("ai"); }
    const nlohmann::json& MonsterDefinition::GetSkills() const { return document->at("skills"); }
    const nlohmann::json& MonsterDefinition::GetMotions() const { return document->at("motions"); }
    const nlohmann::json& MonsterDefinition::GetNode(const std::string& inId) const { return *nodes.at(inId); }
    const nlohmann::json& MonsterDefinition::GetSkill(const std::string& inId) const { return *skills.at(inId); }
    const nlohmann::json& MonsterDefinition::GetMotion(const std::string& inId) const { return *motions.at(inId); }
    const std::vector<const nlohmann::json*>& MonsterDefinition::GetOutgoing(const std::string& inId) const
    { return outgoing.at(inId); }

    MonsterDefinition::Catalog MonsterDefinition::LoadCatalog(const std::filesystem::path& inPath)
    {
        try
        {
            const auto catalog = ReadJson(inPath);
            Keys(catalog, { "version", "monsters" });
            Require(catalog.at("version") == 1, "Unsupported monster catalog version.");
            const auto& entries = catalog.at("monsters");
            Require(entries.is_array() && !entries.empty() && entries.size() <= 256, "Invalid monster catalog size.");
            Catalog definitions;
            std::unordered_map<std::string, std::shared_ptr<const Json>> documents;
            std::uintmax_t totalBytes = 0;
            for (const auto& entry : entries)
            {
                Keys(entry, { "dataId", "definitionFile", "monsterId" });
                const auto dataId = static_cast<std::uint32_t>(Integer(entry.at("dataId"), 1000000));
                Require(dataId != 0 && !definitions.contains(dataId), "Duplicate or invalid monster Data ID: " + std::to_string(dataId));
                const std::string file = Text(entry.at("definitionFile"));
                static const std::regex filePattern("[A-Za-z][A-Za-z0-9_.-]{0,127}\\.json");
                Require(std::regex_match(file, filePattern), "definitionFile must be a JSON filename within Data/Monsters.");
                const std::string monsterId = Text(entry.at("monsterId"), true);
                if (!documents.contains(file))
                {
                    const auto path = inPath.parent_path() / file;
                    totalBytes += std::filesystem::file_size(path);
                    Require(totalBytes <= MAX_CATALOG_DOCUMENT_BYTES, "Monster documents exceed 64 MB.");
                    auto document = ReadJson(path);
                    try { ValidateDocument(document); }
                    catch (const std::exception& error) { throw std::runtime_error(file + ": " + error.what()); }
                    documents.emplace(file, std::make_shared<const Json>(std::move(document)));
                }
                const auto document = documents.at(file);
                const auto& monsters = document->at("monsters");
                const auto source = std::find_if(monsters.begin(), monsters.end(), [&monsterId](const Json& monster)
                    { return monster.at("id") == monsterId; });
                Require(source != monsters.end(), "Unknown monster '" + monsterId + "' in " + file);
                auto definition = std::make_shared<MonsterDefinition>();
                definition->dataId = dataId; definition->id = monsterId;
                definition->name = source->at("name").get<std::string>();
                definition->maxHp = source->at("maxHp").get<std::uint32_t>();
                definition->document = document;
                definition->monsterIndex = static_cast<std::size_t>(std::distance(monsters.begin(), source));
                for (const auto& node : definition->GetAi().at("nodes"))
                {
                    const std::string id = node.at("id");
                    definition->nodes.emplace(id, &node);
                    definition->outgoing.emplace(id, std::vector<const Json*>{});
                }
                for (const auto& skill : definition->GetSkills()) definition->skills.emplace(skill.at("id").get<std::string>(), &skill);
                for (const auto& motion : definition->GetMotions()) definition->motions.emplace(motion.at("id").get<std::string>(), &motion);
                for (const auto& edge : definition->GetAi().at("edges"))
                    definition->outgoing.at(edge.at("from").get<std::string>()).push_back(&edge);
                for (auto& [id, edges] : definition->outgoing)
                    std::sort(edges.begin(), edges.end(), [](const Json* a, const Json* b)
                        { return a->at("priority").get<std::uint64_t>() < b->at("priority").get<std::uint64_t>(); });
                definitions.emplace(dataId, std::move(definition));
                std::cout << "Monster loaded: " << dataId << " (" << monsterId << ", " << file << ")\n";
            }
            return definitions;
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error("Monster catalog " + inPath.string() + ": " + error.what());
        }
    }
}
