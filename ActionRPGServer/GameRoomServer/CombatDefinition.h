#pragma once

#include "MonsterDefinition.h"
#include "../Shared/PlayerSkillCatalog.h"
#include "../Shared/SkillTreeCatalog.h"
#include "../Shared/CombatTempo.h"
#include "../Shared/QuestCatalog.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameRoomServer
{
    struct SkillEffect
    {
        std::uint32_t damage{};
        float hitSeconds{};
        float reachHeight{};
        float hitstopSeconds{};
    };

    struct MonsterCombatProfile
    {
        float walkSpeed{};
        float runSpeed{};
        float detectionRange{};
        float bodyHeight{};
        float hitRadius{};
        float hitRecovery{};
        std::unordered_map<std::string, SkillEffect> skills;
    };

    // Server-only combat parameters; MonsterEditor schemaVersion 1 remains unchanged.
    struct SlideDefinition
    {
        float durationSeconds{};
        // Seconds of buffed run speed converted into the total slide distance.
        float distancePerRunSpeedSeconds{};
        std::string motionId;
        float hitstopSeconds{};
        float force{};
        bool airborne{};
        // Fractions of action duration; start inclusive, end exclusive.
        float hitStartRatio{}, hitEndRatio{};
    };

    struct MeleeStrikeDefinition
    {
        std::string motionId;
        float durationSeconds{}, prepareSeconds{}, hitSeconds{}, recoverSeconds{};
        float reach{}, depthRadius{}, bottom{}, height{}, damageRatio{}, force{}, hitstopSeconds{};
        bool airborne{};
    };

    struct MeleeDefinition
    {
        std::vector<MeleeStrikeDefinition> ground;
        MeleeStrikeDefinition air, dash;
        nlohmann::json source;
    };

    struct CharacterCombatDefinition
    {
        std::uint32_t attackPower{};
        float hitRecovery{};
        SlideDefinition slide;
        std::optional<std::uint32_t> maxMp;
        float bodyHeight{};
        std::optional<MeleeDefinition> melee;
    };

    struct CombatDefinition
    {
        ActionRPG::Combat::CombatTempo tempo;
        std::uint32_t playerMaxHp{};
        std::optional<std::uint32_t> playerMaxMp;
        std::uint32_t shotDamage{};
        float walkSpeed{}, runSpeed{}, bodyHeight{}, hitRadius{};
        float shotPrepareSeconds{}, shotIntervalSeconds{}, shotRecoverSeconds{};
        float shotHitstopSeconds{};
        float shotForce{}, knockbackSeconds{};
        bool shotAirborne{};
        float projectileSpeed{}, projectileRange{}, projectileRadius{};
        float muzzleForward{}, muzzleHeight{}, airMuzzleForward{}, airMuzzleHeight{};
        float jumpPrepareSeconds{}, jumpSpeed{}, gravity{}, hitStunSeconds{}, downSeconds{}, riseSeconds{};
        float airFireLift{}, airRecoilDistance{};
        std::unordered_map<std::uint32_t, CharacterCombatDefinition> characters;
        std::unordered_map<std::uint32_t, MonsterCombatProfile> monsters;
        ActionRPG::PlayerSkills::Catalog playerSkills;
        ActionRPG::PlayerSkills::SkillTreeCatalog skillTrees;
        ActionRPG::Quests::Catalog quests;

        [[nodiscard]] static std::shared_ptr<const CombatDefinition> Load(
            const std::filesystem::path& inPath, const MonsterDefinition::Catalog& inMonsters);
    };
}
