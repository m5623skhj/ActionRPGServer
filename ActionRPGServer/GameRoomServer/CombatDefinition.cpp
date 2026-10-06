#include "CombatDefinition.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace GameRoomServer
{
    namespace
    {
        using Json = nlohmann::json;
        void Require(bool inCondition, const std::string& inMessage)
        {
            if (!inCondition) throw std::runtime_error(inMessage);
        }
        void Keys(const Json& inValue, std::initializer_list<std::string_view> inKeys)
        {
            Require(inValue.is_object(), "Expected combat object.");
            for (const auto& [key, value] : inValue.items())
                Require(std::find(inKeys.begin(), inKeys.end(), key) != inKeys.end(), "Unknown combat field: " + key);
        }
        float Number(const Json& inValue, double inMin, double inMax)
        {
            Require(inValue.is_number(), "Expected combat number.");
            const double value = inValue.get<double>();
            Require(std::isfinite(value) && value >= inMin && value <= inMax, "Invalid combat number.");
            return static_cast<float>(value);
        }
        std::uint32_t Integer(const Json& inValue, std::uint32_t inMin, std::uint32_t inMax)
        {
            Require(inValue.is_number_integer(), "Expected combat integer.");
            const auto value = inValue.get<std::int64_t>();
            Require(value >= inMin && value <= inMax, "Invalid combat integer.");
            return static_cast<std::uint32_t>(value);
        }
    }

    std::shared_ptr<const CombatDefinition> CombatDefinition::Load(
        const std::filesystem::path& inPath, const MonsterDefinition::Catalog& inMonsters)
    {
        Require(std::filesystem::file_size(inPath) <= 1024 * 1024, "Combat.json exceeds 1 MB.");
        std::ifstream input(inPath, std::ios::binary);
        Require(input.good(), "Cannot read Combat.json.");
        const Json source = Json::parse(input);
        Keys(source, { "version", "player", "reactions", "monsters" });
        Require(source.at("version") == 2, "Unsupported combat version.");
        auto result = std::make_shared<CombatDefinition>();
        const auto& player = source.at("player");
        Keys(player, { "maxHp", "walkSpeed", "runSpeed", "bodyHeight", "hitRadius", "shot", "jump", "characters" });
        result->playerMaxHp = Integer(player.at("maxHp"), 1, 1000000);
        result->walkSpeed = Number(player.at("walkSpeed"), 1, 2000);
        result->runSpeed = Number(player.at("runSpeed"), player.at("walkSpeed").get<double>(), 2000);
        result->bodyHeight = Number(player.at("bodyHeight"), 1, 1000);
        result->hitRadius = Number(player.at("hitRadius"), 1, 100);
        const auto& shot = player.at("shot");
        Keys(shot, { "damage", "prepareSeconds", "intervalSeconds", "recoverSeconds", "speed", "range",
            "radius", "muzzleForward", "muzzleHeight", "airMuzzleForward", "airMuzzleHeight",
            "airFireLift", "airRecoilDistance" });
        result->shotDamage = Integer(shot.at("damage"), 1, 1000000);
        result->shotPrepareSeconds = Number(shot.at("prepareSeconds"), 0.05, 10);
        result->shotIntervalSeconds = Number(shot.at("intervalSeconds"), 0.05, 10);
        result->shotRecoverSeconds = Number(shot.at("recoverSeconds"), 0.05, 10);
        result->projectileSpeed = Number(shot.at("speed"), 1, 5000);
        result->projectileRange = Number(shot.at("range"), 1, 5000);
        result->projectileRadius = Number(shot.at("radius"), 0.1, 100);
        result->muzzleForward = Number(shot.at("muzzleForward"), 0, 1000);
        result->muzzleHeight = Number(shot.at("muzzleHeight"), 0, 1000);
        result->airMuzzleForward = Number(shot.at("airMuzzleForward"), 0, 1000);
        result->airMuzzleHeight = Number(shot.at("airMuzzleHeight"), 0, 1000);
        result->airFireLift = Number(shot.at("airFireLift"), 0, 1000);
        result->airRecoilDistance = Number(shot.at("airRecoilDistance"), 0, 100);
        const auto& jump = player.at("jump");
        Keys(jump, { "prepareSeconds", "speed", "gravity" });
        result->jumpPrepareSeconds = Number(jump.at("prepareSeconds"), 0.05, 1);
        result->jumpSpeed = Number(jump.at("speed"), 1, 2000);
        result->gravity = Number(jump.at("gravity"), 1, 5000);
        const auto& reactions = source.at("reactions");
        Keys(reactions, { "hitStunSeconds", "downSeconds", "riseSeconds" });
        result->hitStunSeconds = Number(reactions.at("hitStunSeconds"), 0.05, 10);
        result->downSeconds = Number(reactions.at("downSeconds"), 0.05, 10);
        result->riseSeconds = Number(reactions.at("riseSeconds"), 0.05, 10);
        const auto& profiles = source.at("monsters");
        Require(profiles.is_array() && profiles.size() <= 256, "Invalid combat profile list.");
        for (const auto& profile : profiles)
        {
            Keys(profile, { "dataId", "walkSpeed", "runSpeed", "detectionRange", "bodyHeight", "hitRadius", "skills" });
            const auto dataId = Integer(profile.at("dataId"), 1, 1000000);
            Require(inMonsters.contains(dataId), "Combat profile references unknown monster.");
            MonsterCombatProfile monster;
            monster.walkSpeed = Number(profile.at("walkSpeed"), 0, 2000);
            monster.runSpeed = Number(profile.at("runSpeed"), profile.at("walkSpeed").get<double>(), 2000);
            monster.detectionRange = Number(profile.at("detectionRange"), 0, 10000);
            monster.bodyHeight = Number(profile.at("bodyHeight"), 1, 1000);
            monster.hitRadius = Number(profile.at("hitRadius"), 1, 100);
            Require(profile.at("skills").is_array() && profile.at("skills").size() <= 2048, "Invalid combat skills.");
            for (const auto& effect : profile.at("skills"))
            {
                Keys(effect, { "id", "damage", "hitSeconds", "reachHeight" });
                const std::string id = effect.at("id").get<std::string>();
                const auto& skills = inMonsters.at(dataId)->GetSkills();
                const auto skill = std::find_if(skills.begin(), skills.end(), [&id](const auto& value)
                    { return value.at("id") == id; });
                Require(skill != skills.end(), "Combat effect references unknown skill: " + id);
                SkillEffect value{ Integer(effect.at("damage"), 1, 1000000),
                    Number(effect.at("hitSeconds"), 0, skill->at("durationSeconds").get<double>()),
                    Number(effect.at("reachHeight"), 0, 1000) };
                Require(monster.skills.emplace(id, value).second, "Duplicate combat skill: " + id);
            }
            for (const auto& node : inMonsters.at(dataId)->GetAi().at("nodes"))
            {
                const auto& action = node.at("action");
                const std::string type = action.at("type");
                if (type == "UseSkill")
                    Require(monster.skills.contains(action.at("parameters").at("skillId").get<std::string>()),
                        "AI skill needs a combat effect.");
                if (type == "MoveToTarget" || type == "ReturnToSpawn")
                    Require(monster.walkSpeed > 0, "Moving monster needs a positive speed.");
            }
            for (const auto& skill : inMonsters.at(dataId)->GetSkills())
            {
                (void)Number(skill.at("durationSeconds"), 0.001, 3600);
                (void)Number(skill.at("cooldownSeconds"), 0, 86400);
            }
            for (const auto& motion : inMonsters.at(dataId)->GetMotions())
                (void)Number(motion.at("durationSeconds"), 0.001, 3600);
            Require(result->monsters.emplace(dataId, std::move(monster)).second, "Duplicate combat profile.");
        }
        for (const auto& [dataId, definition] : inMonsters)
            Require(result->monsters.contains(dataId), "Missing combat profile for monster " + std::to_string(dataId));
        result->playerSkills = ActionRPG::PlayerSkills::Catalog::Load(inPath.parent_path() / "PlayerSkills.json");
        result->skillTrees = ActionRPG::PlayerSkills::SkillTreeCatalog::Load(inPath.parent_path() / "SkillTrees.json", result->playerSkills);
        const auto& characters = player.at("characters");
        Require(characters.is_array() && characters.size() == result->playerSkills.characterIds.size()
            && !characters.empty() && characters.size() <= 256, "Every character needs one combat definition.");
        for (const auto& character : characters)
        {
            Keys(character, { "characterId", "attackPower", "slide" });
            const auto characterId = Integer(character.at("characterId"), 1, 1000000);
            Require(result->playerSkills.characterIds.contains("Character" + std::to_string(characterId)),
                "Combat definition references an unknown character.");
            const auto& slide = character.at("slide");
            Keys(slide, { "durationSeconds", "motionId" });
            CharacterCombatDefinition definition{ Integer(character.at("attackPower"), 1, 1000000),
                { Number(slide.at("durationSeconds"), 0.05, 2), slide.at("motionId").get<std::string>() } };
            Require(ActionRPG::PlayerSkills::Catalog::IsId(definition.slide.motionId), "Invalid slide motion ID.");
            Require(result->characters.emplace(characterId, std::move(definition)).second, "Duplicate combat character.");
        }
        return result;
    }
}
