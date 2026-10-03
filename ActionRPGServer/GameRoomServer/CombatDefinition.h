#pragma once

#include "MonsterDefinition.h"
#include "../Shared/PlayerSkillCatalog.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace GameRoomServer
{
    struct SkillEffect
    {
        std::uint32_t damage{};
        float hitSeconds{};
        float reachHeight{};
    };

    struct MonsterCombatProfile
    {
        float walkSpeed{};
        float runSpeed{};
        float detectionRange{};
        float bodyHeight{};
        float hitRadius{};
        std::unordered_map<std::string, SkillEffect> skills;
    };

    // Server-only combat parameters; MonsterEditor schemaVersion 1 remains unchanged.
    struct CombatDefinition
    {
        std::uint32_t playerMaxHp{};
        std::uint32_t shotDamage{};
        float walkSpeed{}, runSpeed{}, bodyHeight{}, hitRadius{};
        float shotPrepareSeconds{}, shotIntervalSeconds{}, shotRecoverSeconds{};
        float projectileSpeed{}, projectileRange{}, projectileRadius{}, muzzleHeight{};
        float jumpPrepareSeconds{}, jumpSpeed{}, gravity{}, hitStunSeconds{}, downSeconds{}, riseSeconds{};
        float airFireLift{}, airRecoilDistance{};
        std::unordered_map<std::uint32_t, MonsterCombatProfile> monsters;
        ActionRPG::PlayerSkills::Catalog playerSkills;

        [[nodiscard]] static std::shared_ptr<const CombatDefinition> Load(
            const std::filesystem::path& inPath, const MonsterDefinition::Catalog& inMonsters);
    };
}
