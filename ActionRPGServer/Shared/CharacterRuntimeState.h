#pragma once

#include "Inventory.h"
#include <charconv>

namespace ActionRPG::Items
{
    inline std::uint64_t DecimalId(const Json& inValue, bool inAllowZero = false)
    {
        PlayerSkills::Catalog::Require(inValue.is_string(), "Identifier must be a decimal string.");
        const auto value = inValue.get<std::string>();
        std::uint64_t result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        PlayerSkills::Catalog::Require(!value.empty() && (value.size() == 1 || value.front() != '0')
            && error == std::errc{} && end == value.data() + value.size() && (inAllowZero || result != 0),
            "Invalid decimal identifier.");
        return result;
    }

    struct CharacterRuntimeState
    {
        std::uint64_t characterId{}, revision{}, ownerGeneration{};
        PlayerSkills::CharacterProgression progression;
        Json equipment;

        [[nodiscard]] static CharacterRuntimeState Parse(const Json& inValue)
        {
            using Rules = PlayerSkills::Catalog;
            Rules::Keys(inValue, { "characterId", "revision", "ownerGeneration", "progression", "equipment" });
            CharacterRuntimeState result{ DecimalId(inValue.at("characterId")), DecimalId(inValue.at("revision"), true),
                DecimalId(inValue.at("ownerGeneration")),
                PlayerSkills::CharacterProgression::Parse(inValue.at("progression")), inValue.at("equipment") };
            Rules::Require(result.equipment.is_array() && result.equipment.size() <= EQUIPMENT_SLOT_COUNT,
                "Invalid runtime equipment count.");
            std::unordered_set<std::uint32_t> slots;
            std::unordered_set<std::string> ids;
            for (const auto& row : result.equipment)
            {
                Rules::Keys(row, { "instanceId", "definitionId", "count", "container", "slot" });
                const auto id = row.at("instanceId").get<std::string>();
                Rules::Require(IsHexId(id, 32) && ids.emplace(id).second
                    && Rules::IsId(row.at("definitionId").get<std::string>())
                    && Rules::Integer(row.at("count"), 1, 1) == 1
                    && Rules::Integer(row.at("container"), EQUIPPED_CONTAINER, EQUIPPED_CONTAINER) == EQUIPPED_CONTAINER
                    && slots.emplace(Rules::Integer(row.at("slot"), 0, EQUIPMENT_SLOT_COUNT - 1)).second,
                    "Invalid runtime equipment.");
            }
            return result;
        }
    };
}
