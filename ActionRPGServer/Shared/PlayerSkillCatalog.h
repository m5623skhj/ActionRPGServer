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
        static void Keys(const Json& inValue, std::initializer_list<const char*> inNames,
            std::initializer_list<const char*> inOptionalNames = {})
        {
            Require(inValue.is_object(), "Expected object.");
            for (const auto name : inNames) Require(inValue.contains(name), std::string("Missing field: ") + name);
            auto expectedSize = inNames.size();
            for (const auto name : inOptionalNames) if (inValue.contains(name)) ++expectedSize;
            Require(inValue.size() == expectedSize, "Unexpected fields.");
        }
        // Timing helpers consume validated variants; the end index denotes the total motion time.
        [[nodiscard]] static double FrameStartSeconds(const Json& inVariant, std::uint32_t inIndex)
        {
            Require(inIndex <= inVariant.at("frameCount").get<std::uint32_t>(), "Frame index out of range.");
            if (!inVariant.contains("frameDurationsSeconds")) return inIndex / inVariant.at("fps").get<double>();
            double seconds{};
            const auto& durations = inVariant.at("frameDurationsSeconds");
            for (std::uint32_t index = 0; index < inIndex; ++index) seconds += durations.at(index).get<double>();
            return seconds;
        }
        [[nodiscard]] static double FrameDurationSeconds(const Json& inVariant, std::uint32_t inIndex)
        {
            Require(inIndex < inVariant.at("frameCount").get<std::uint32_t>(), "Frame index out of range.");
            return inVariant.contains("frameDurationsSeconds")
                ? inVariant.at("frameDurationsSeconds").at(inIndex).get<double>() : 1.0 / inVariant.at("fps").get<double>();
        }
        [[nodiscard]] static double MotionDurationSeconds(const Json& inVariant)
        {
            return FrameStartSeconds(inVariant, inVariant.at("frameCount").get<std::uint32_t>());
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
            if (inType == "direct") Keys(inValue, { "motionId", "frameCount", "fps", "durationSeconds", "eventFrame", "endFrame", "attackRects" }, { "frameDurationsSeconds" });
            else if (inType == "projectile") Keys(inValue, { "motionId", "frameCount", "fps", "durationSeconds", "eventFrame", "endFrame", "spawn", "yawDegrees", "pitchDegrees" }, { "frameDurationsSeconds" });
            else Keys(inValue, { "motionId", "frameCount", "fps", "durationSeconds", "eventFrame", "endFrame" }, { "frameDurationsSeconds" });
            Require(IsId(inValue.at("motionId").get<std::string>()), "Invalid motion ID.");
            const auto frames = Integer(inValue.at("frameCount"), 1, 512);
            (void)Number(inValue.at("fps"), 0.001, 240);
            const double duration = Number(inValue.at("durationSeconds"), 0.001, 60);
            if (inValue.contains("frameDurationsSeconds"))
            {
                const auto& durations = inValue.at("frameDurationsSeconds");
                Require(durations.is_array() && durations.size() == frames, "Frame duration count differs from frame count.");
                double total{};
                float floatTotal{};
                for (const auto& value : durations)
                {
                    const double seconds = Number(value, 0, 60);
                    const float floatSeconds = static_cast<float>(seconds);
                    Require(floatSeconds > 0, "Frame duration must remain positive as float.");
                    const double next = total + seconds;
                    Require(std::isfinite(next) && next > total && next <= 60, "Invalid cumulative frame time.");
                    const float floatNext = floatTotal + floatSeconds;
                    Require(std::isfinite(floatNext) && floatNext > floatTotal
                        && static_cast<float>(next) > static_cast<float>(total), "Cumulative frame time collapses as float.");
                    total = next;
                    floatTotal = floatNext;
                }
            }
            const double total = MotionDurationSeconds(inValue);
            Require(std::isfinite(total) && total >= 0.001 && total <= 60, "Motion duration out of range.");
            Require(std::abs(duration - total) <= 0.0001, "Motion timing differs from frame durations.");
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
