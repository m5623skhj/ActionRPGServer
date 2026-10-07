#pragma once

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ActionRPG::PlayerSkills
{
    // Identical contract reader is distributed to the client. No image/renderer dependency.
    class Catalog final
    {
    public:
        using Json = nlohmann::json;
        Json source{ { "format", "PlayerSkills" }, { "schemaVersion", 1 },
            { "coordinates", "groundXY-height-facingRight-worldUnits" },
            { "characters", Json::array() }, { "skills", Json::array() } };
        std::unordered_map<std::string, Json> skills;
        std::unordered_map<std::string, std::uint32_t> characterIds;

        static void Require(bool inCondition, const std::string& inMessage)
        {
            if (!inCondition) throw std::runtime_error("PlayerSkills: " + inMessage);
        }
        static double Number(const Json& inValue, double inMin, double inMax)
        {
            Require(inValue.is_number(), "Expected number.");
            const double value = inValue.get<double>();
            Require(std::isfinite(value) && value >= inMin && value <= inMax, "Number out of range.");
            return value;
        }
        static std::uint32_t Integer(const Json& inValue, std::uint32_t inMin, std::uint32_t inMax)
        {
            const double value = Number(inValue, inMin, inMax);
            Require(std::floor(value) == value, "Expected integer.");
            return static_cast<std::uint32_t>(value);
        }
        static float NonnegativeFloat(const Json& inValue)
        {
            const double value = Number(inValue, 0, std::numeric_limits<float>::max());
            const float result = static_cast<float>(value);
            Require(value == 0 || result > 0, "Attack time underflows float.");
            return result;
        }
        static bool IsId(const std::string& inValue)
        {
            if (inValue.empty() || inValue.size() > 64 || inValue == "__proto__"
                || inValue == "prototype" || inValue == "constructor") return false;
            const auto letter = [](char value) { return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z'); };
            if (!letter(inValue.front())) return false;
            for (const char value : inValue)
                if (!letter(value) && !(value >= '0' && value <= '9') && value != '_' && value != '.' && value != '-') return false;
            return true;
        }
        static void Keys(const Json& inValue, std::initializer_list<const char*> inNames)
        {
            Require(inValue.is_object() && inValue.size() == inNames.size(), "Unexpected fields.");
            for (const auto name : inNames) Require(inValue.contains(name), std::string("Missing field: ") + name);
        }
        static void Point(const Json& inValue)
        {
            Keys(inValue, { "x", "y", "height" });
            for (const auto name : { "x", "y", "height" }) Number(inValue.at(name), -1000, 1000);
        }
        static void Rect(const Json& inValue)
        {
            Keys(inValue, { "x", "y", "width", "height" });
            Number(inValue.at("x"), -1000, 1000); Number(inValue.at("y"), -1000, 1000);
            Number(inValue.at("width"), 0.1, 2000); Number(inValue.at("height"), 0.1, 2000);
        }
        static void Variant(const Json& inValue, const std::string& inType)
        {
            if (inValue.is_null()) return;
            if (inType == "direct") Keys(inValue, { "motionId", "frameCount", "fps", "durationSeconds", "eventFrame", "endFrame", "attackRects" });
            else if (inType == "projectile") Keys(inValue, { "motionId", "frameCount", "fps", "durationSeconds", "eventFrame", "endFrame", "spawn", "yawDegrees", "pitchDegrees" });
            else Keys(inValue, { "motionId", "frameCount", "fps", "durationSeconds", "eventFrame", "endFrame" });
            Require(IsId(inValue.at("motionId").get<std::string>()), "Invalid motion ID.");
            const auto frames = Integer(inValue.at("frameCount"), 1, 512);
            const double fps = Number(inValue.at("fps"), 0.001, 240);
            const double duration = Number(inValue.at("durationSeconds"), 0.001, 60);
            Require(std::abs(duration - frames / fps) <= 0.0001, "Motion timing differs from FPS.");
            const auto first = Integer(inValue.at("eventFrame"), 0, frames - 1);
            const auto last = Integer(inValue.at("endFrame"), first, frames - 1);
            if (inType == "direct")
            {
                const auto& rectangles = inValue.at("attackRects");
                Require(rectangles.is_array() && rectangles.size() == last - first + 1, "Missing attack frames.");
                std::unordered_set<std::uint32_t> indexes;
                for (const auto& rectangle : rectangles)
                {
                    Keys(rectangle, { "index", "rect" });
                    Require(indexes.insert(Integer(rectangle.at("index"), first, last)).second, "Duplicate attack frame.");
                    Rect(rectangle.at("rect"));
                }
            }
            else if (inType == "projectile")
            {
                Point(inValue.at("spawn")); Number(inValue.at("yawDegrees"), -180, 180);
                Number(inValue.at("pitchDegrees"), -90, 90);
            }
        }
        [[nodiscard]] static Catalog Parse(Json inSource)
        {
            Keys(inSource, { "format", "schemaVersion", "coordinates", "characters", "skills" });
            Require(inSource.at("format") == "PlayerSkills" && inSource.at("schemaVersion") == 1
                && inSource.at("coordinates") == "groundXY-height-facingRight-worldUnits", "Unsupported contract.");
            Require(inSource.at("characters").is_array() && inSource.at("characters").size() <= 256
                && inSource.at("skills").is_array() && inSource.at("skills").size() <= 256, "Catalog limit exceeded.");
            Catalog result;
            std::unordered_set<std::uint32_t> dataIds;
            for (const auto& character : inSource.at("characters"))
            {
                Keys(character, { "id", "dataId" });
                const auto id = character.at("id").get<std::string>();
                const auto dataId = Integer(character.at("dataId"), 1, 1000000);
                Require(IsId(id) && result.characterIds.emplace(id, dataId).second && dataIds.insert(dataId).second,
                    "Invalid/duplicate character ID.");
                Require(id == "Character" + std::to_string(dataId), "Character mapping differs from characters.ini.");
            }
            std::unordered_set<std::string> commands;
            for (auto& skill : inSource.at("skills"))
            {
                Keys(skill, { "id", "name", "characterId", "type", "input", "cooldownSeconds", "execution", "ground", "air" });
                const auto id = skill.at("id").get<std::string>();
                const auto character = skill.at("characterId").get<std::string>();
                const auto type = skill.at("type").get<std::string>();
                Require(IsId(id) && result.characterIds.contains(character) && !result.skills.contains(id), "Invalid skill reference/ID.");
                const auto name = skill.at("name").get<std::string>();
                Require(!name.empty() && name.size() <= 320, "Invalid skill name.");
                Require(type == "direct" || type == "projectile" || type == "buff", "Unsupported skill type.");
                Keys(skill.at("input"), { "command", "maxStepSeconds" });
                const auto& command = skill.at("input").at("command");
                Require(command.is_array() && !command.empty() && command.size() <= 16, "Invalid command.");
                for (const auto& key : command)
                    Require(key == "Left" || key == "Right" || key == "Up" || key == "Down" || key == "Z"
                        || key == "X" || key == "C" || key == "V", "Invalid command key.");
                Require(commands.insert(character + ":" + command.dump()).second, "Duplicate command.");
                Number(skill.at("input").at("maxStepSeconds"), 0.01, 1.5);
                Number(skill.at("cooldownSeconds"), 0, 86400);
                auto& execution = skill.at("execution");
                if (type == "buff")
                {
                    Keys(execution, { "target", "stat", "multiplier", "durationSeconds", "refresh" });
                    Require(execution.at("target") == "self" && execution.at("refresh") == "replaceDuration"
                        && (execution.at("stat") == "damageMultiplier" || execution.at("stat") == "movementMultiplier"), "Unsupported buff policy.");
                    Number(execution.at("multiplier"), 0.1, 10); Number(execution.at("durationSeconds"), 0.05, 3600);
                }
                else
                {
                    // Normalize legacy attack data without replacing an explicitly disabled hitstop.
                    Require(execution.is_object(), "Expected attack execution object.");
                    if (!execution.contains("hitstopSeconds")) execution["hitstopSeconds"] = 0.0;
                    if (type == "direct") Keys(execution, { "damage", "depthRadius", "hitstopSeconds" });
                    else Keys(execution, { "damage", "speed", "radius", "range", "hitstopSeconds" });
                    (void)NonnegativeFloat(execution.at("hitstopSeconds"));
                    Integer(execution.at("damage"), 1, 1000000);
                    if (type == "direct") Number(execution.at("depthRadius"), 0.1, 500);
                    else
                    {
                        Number(execution.at("speed"), 1, 4000); Number(execution.at("radius"), 0.1, 100);
                        Number(execution.at("range"), 1, 10000);
                    }
                }
                Require(!skill.at("ground").is_null() || !skill.at("air").is_null(), "Missing motion variant.");
                Variant(skill.at("ground"), type); Variant(skill.at("air"), type);
                result.skills.emplace(id, skill);
            }
            result.source = std::move(inSource);
            return result;
        }
        [[nodiscard]] static Catalog Load(const std::filesystem::path& inPath)
        {
            if (!std::filesystem::exists(inPath)) return {};
            Require(std::filesystem::is_regular_file(inPath) && std::filesystem::file_size(inPath) <= 4 * 1024 * 1024, "File size exceeded.");
            std::ifstream file(inPath, std::ios::binary); Require(file.good(), "Unable to read catalog.");
            Json source; file >> source; return Parse(std::move(source));
        }
    };
}
