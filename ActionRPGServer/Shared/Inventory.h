#pragma once

#include "CharacterProgression.h"

#include <algorithm>
#include <array>
#include <functional>
#include <unordered_set>

namespace ActionRPG::Items
{
    using Json = PlayerSkills::Catalog::Json;
    inline constexpr std::size_t TAB_COUNT = 4;
    inline constexpr std::size_t TAB_CAPACITY = 40;
    inline constexpr std::size_t EQUIPMENT_SLOT_COUNT = 7;
    inline constexpr std::uint32_t EQUIPPED_CONTAINER = 4;
    inline constexpr std::size_t MAX_ITEMS = TAB_COUNT * TAB_CAPACITY + EQUIPMENT_SLOT_COUNT;
    inline constexpr std::array<const char*, TAB_COUNT> CATEGORY_NAMES{
        "Equipment", "Material", "Consumable", "Quest" };
    inline constexpr std::array<const char*, EQUIPMENT_SLOT_COUNT> EQUIPMENT_NAMES{
        "Weapon", "Top", "Bottom", "Shoes", "Ring", "Necklace", "Bracelet" };

    inline bool IsHexId(const std::string& inValue, std::size_t inLength)
    {
        return inValue.size() == inLength && std::all_of(inValue.begin(), inValue.end(), [](char value)
            { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
    }

    struct Definition
    {
        std::string id;
        std::uint32_t category{}, maxStack{}, equipmentSlot{}, requiredLevel{};
        std::uint32_t restoreHp{};
        std::unordered_set<std::uint32_t> characterDefinitionIds;
    };

    class Catalog final
    {
    public:
        Json source;
        std::unordered_map<std::string, Definition> definitions;
        std::unordered_map<std::string, Json> presentations;

        [[nodiscard]] static Catalog Load(const std::filesystem::path& inPath)
        {
            using Rules = PlayerSkills::Catalog;
            Rules::Require(std::filesystem::is_regular_file(inPath)
                && std::filesystem::file_size(inPath) <= 4 * 1024 * 1024, "Missing/oversized item catalog.");
            std::ifstream input(inPath, std::ios::binary);
            Rules::Require(input.good(), "Cannot read item catalog.");
            auto value = Json::parse(input);
            Rules::Keys(value, { "format", "schemaVersion", "items" });
            Rules::Require(value.at("format") == "Items" && value.at("schemaVersion") == 1
                && value.at("items").is_array(), "Invalid item catalog.");
            Catalog result;
            for (const auto& item : value.at("items"))
            {
                Rules::Require(item.dump().size() <= 8192, "Item definition exceeds transmission limit.");
                Rules::Keys(item, { "id", "name", "description", "icon", "category", "maxStack" },
                    { "equipmentSlot", "requiredLevel", "characterDefinitionIds", "stats", "effects" });
                Definition definition;
                definition.id = item.at("id").get<std::string>();
                Rules::Require(Rules::IsId(definition.id), "Invalid item definition ID.");
                const auto name = item.at("name").get<std::string>();
                const auto description = item.at("description").get<std::string>();
                const auto icon = item.at("icon").get<std::string>();
                Rules::Require(!name.empty() && name.size() <= 128 && description.size() <= 2048,
                    "Invalid item display text.");
                Rules::Require(icon.starts_with("Images/") && icon.ends_with(".png") && icon.size() <= 255
                    && icon.find("..") == std::string::npos && icon.find('\\') == std::string::npos
                    && icon.find(':') == std::string::npos, "Invalid item icon path.");
                const auto category = item.at("category").get<std::string>();
                const auto categoryFound = std::find(CATEGORY_NAMES.begin(), CATEGORY_NAMES.end(), category);
                Rules::Require(categoryFound != CATEGORY_NAMES.end(), "Invalid item category.");
                definition.category = static_cast<std::uint32_t>(categoryFound - CATEGORY_NAMES.begin());
                definition.maxStack = Rules::Integer(item.at("maxStack"), 1,
                    std::numeric_limits<std::uint32_t>::max());
                if (definition.category == 0)
                {
                    Rules::Require(definition.maxStack == 1 && item.contains("equipmentSlot")
                        && item.contains("requiredLevel") && item.contains("characterDefinitionIds")
                        && item.contains("stats") && item.contains("effects"), "Incomplete equipment definition.");
                    const auto slot = item.at("equipmentSlot").get<std::string>();
                    const auto slotFound = std::find(EQUIPMENT_NAMES.begin(), EQUIPMENT_NAMES.end(), slot);
                    Rules::Require(slotFound != EQUIPMENT_NAMES.end(), "Invalid equipment slot.");
                    definition.equipmentSlot = static_cast<std::uint32_t>(slotFound - EQUIPMENT_NAMES.begin());
                    definition.requiredLevel = Rules::Integer(item.at("requiredLevel"), 1, 1000000);
                    Rules::Require(item.at("characterDefinitionIds").is_array()
                        && item.at("characterDefinitionIds").size() <= 256, "Invalid equipment character restrictions.");
                    for (const auto& character : item.at("characterDefinitionIds"))
                        Rules::Require(definition.characterDefinitionIds.emplace(
                            Rules::Integer(character, 1, 1000000)).second, "Duplicate equipment character restriction.");
                    Rules::Require(item.at("stats").is_object() && item.at("effects").is_array(),
                        "Invalid equipment effects data.");
                    for (const auto& [stat, amount] : item.at("stats").items())
                        Rules::Require(Rules::IsId(stat) && amount.is_number() && std::isfinite(amount.get<double>()),
                            "Invalid equipment stat.");
                }
                else
                {
                    Rules::Require(!item.contains("equipmentSlot") && !item.contains("requiredLevel")
                        && !item.contains("characterDefinitionIds") && !item.contains("stats"),
                        "Equipment fields on a non-equipment item.");
                    if (item.contains("effects"))
                    {
                        Rules::Require(definition.category == 2 && item.at("effects").is_array()
                            && item.at("effects").size() <= 1, "Invalid consumable effects.");
                        for (const auto& effect : item.at("effects"))
                        {
                            Rules::Keys(effect, { "type", "amount" });
                            Rules::Require(effect.at("type") == "RestoreHp", "Unsupported consumable effect.");
                            definition.restoreHp = Rules::Integer(effect.at("amount"), 1, 1000000);
                        }
                    }
                }
                result.presentations.emplace(definition.id, item);
                const auto id = definition.id;
                Rules::Require(result.definitions.emplace(id, std::move(definition)).second,
                    "Duplicate item definition.");
            }
            result.source = std::move(value);
            return result;
        }
    };

    struct Stack
    {
        // Opaque persistent identity; its concrete encoding is defined by the DB contract.
        std::string instanceId, definitionId;
        std::uint32_t count{}, container{}, slot{};
    };

    class Inventory final
    {
    public:
        std::vector<Stack> items;

        [[nodiscard]] Json ToJson() const
        {
            auto rows = Json::array();
            for (const auto& item : items)
                rows.push_back({ { "instanceId", item.instanceId }, { "definitionId", item.definitionId },
                    { "count", item.count }, { "container", item.container }, { "slot", item.slot } });
            return { { "items", std::move(rows) } };
        }

        [[nodiscard]] static Inventory Parse(const Json& inValue, const Catalog& inCatalog,
            std::uint32_t inCharacterDefinitionId, std::uint32_t inLevel)
        {
            using Rules = PlayerSkills::Catalog;
            Rules::Keys(inValue, { "items" });
            Rules::Require(inValue.at("items").is_array() && inValue.at("items").size() <= MAX_ITEMS,
                "Invalid inventory item count.");
            Inventory result;
            std::unordered_set<std::string> ids;
            std::unordered_set<std::uint32_t> locations;
            for (const auto& row : inValue.at("items"))
            {
                Rules::Keys(row, { "instanceId", "definitionId", "count", "container", "slot" });
                Stack item{ row.at("instanceId").get<std::string>(), row.at("definitionId").get<std::string>(),
                    Rules::Integer(row.at("count"), 1, std::numeric_limits<std::uint32_t>::max()),
                    Rules::Integer(row.at("container"), 0, EQUIPPED_CONTAINER), Rules::Integer(row.at("slot"), 0, 39) };
                Rules::Require(IsHexId(item.instanceId, 32)
                    && ids.emplace(item.instanceId).second && inCatalog.definitions.contains(item.definitionId)
                    && locations.emplace(item.container * TAB_CAPACITY + item.slot).second, "Invalid inventory identity/location.");
                const auto& definition = inCatalog.definitions.at(item.definitionId);
                Rules::Require(item.count <= definition.maxStack, "Oversized inventory stack.");
                if (item.container == EQUIPPED_CONTAINER)
                    Rules::Require(definition.category == 0 && item.slot == definition.equipmentSlot
                        && CanEquip(definition, inCharacterDefinitionId, inLevel), "Invalid equipped item.");
                else Rules::Require(item.container == definition.category, "Item in wrong inventory tab.");
                result.items.push_back(std::move(item));
            }
            Rules::Require(result.ToJson().dump().size() <= 32768, "Inventory exceeds storage limit.");
            return result;
        }

        /** Preflight all capacity before changing anything. New stack identities come from the persistence owner. */
        [[nodiscard]] std::string Acquire(const Catalog& inCatalog, const std::string& inDefinitionId,
            std::uint32_t inCount, const std::function<std::string()>& inNewIdentity)
        {
            const auto found = inCatalog.definitions.find(inDefinitionId);
            if (found == inCatalog.definitions.end()) return "UnknownItem";
            if (inCount == 0) return "InvalidQuantity";
            const auto& definition = found->second;
            std::uint64_t capacity = 0;
            std::array<bool, TAB_CAPACITY> occupied{};
            for (const auto& item : items)
            {
                if (item.container != definition.category) continue;
                occupied.at(item.slot) = true;
                if (item.definitionId == inDefinitionId) capacity += definition.maxStack - item.count;
            }
            for (const bool taken : occupied) if (!taken) capacity += definition.maxStack;
            if (capacity < inCount) return "InventoryFull";
            auto proposed = *this;
            auto remaining = inCount;
            // Stable slot order makes partial-stack filling independent of vector insertion order.
            std::vector<std::size_t> existing;
            for (std::size_t index = 0; index < proposed.items.size(); ++index)
                if (proposed.items[index].container == definition.category
                    && proposed.items[index].definitionId == inDefinitionId) existing.push_back(index);
            std::sort(existing.begin(), existing.end(), [&](auto left, auto right)
                { return proposed.items[left].slot < proposed.items[right].slot; });
            for (const auto index : existing)
            {
                auto& item = proposed.items[index];
                const auto added = std::min(remaining, definition.maxStack - item.count);
                item.count += added; remaining -= added;
            }
            for (std::uint32_t slot = 0; remaining != 0 && slot < TAB_CAPACITY; ++slot)
            {
                if (occupied[slot]) continue;
                const auto count = std::min(remaining, definition.maxStack);
                const auto id = inNewIdentity();
                if (!IsHexId(id, 32) || std::any_of(proposed.items.begin(), proposed.items.end(),
                    [&](const Stack& item) { return item.instanceId == id; })) return "IdentityConflict";
                proposed.items.push_back({ id, inDefinitionId, count, definition.category, slot });
                remaining -= count;
            }
            if (proposed.ToJson().dump().size() > 32768) return "InventoryFull";
            *this = std::move(proposed);
            return "Succeeded";
        }

        [[nodiscard]] std::string Remove(const std::string& inInstanceId, std::uint32_t inCount)
        {
            const auto found = std::find_if(items.begin(), items.end(), [&](const Stack& item)
                { return item.instanceId == inInstanceId; });
            if (found == items.end()) return "ItemNotFound";
            if (found->container == EQUIPPED_CONTAINER) return "ItemEquipped";
            if (inCount == 0 || inCount > found->count) return "InvalidQuantity";
            found->count -= inCount;
            if (found->count == 0) items.erase(found);
            return "Succeeded";
        }

        [[nodiscard]] std::string Consume(const std::string& inDefinitionId, std::uint32_t inCount)
        {
            if (inCount == 0) return "InvalidQuantity";
            std::vector<Stack*> ordered;
            std::uint64_t available = 0;
            for (auto& item : items)
                if (item.definitionId == inDefinitionId && item.container != EQUIPPED_CONTAINER)
                { available += item.count; ordered.push_back(&item); }
            if (available < inCount) return "InsufficientQuantity";
            std::sort(ordered.begin(), ordered.end(), [](const Stack* left, const Stack* right)
                { return left->count != right->count ? left->count < right->count : left->instanceId < right->instanceId; });
            for (auto* item : ordered)
            {
                const auto removed = std::min(inCount, item->count);
                item->count -= removed; inCount -= removed;
                if (inCount == 0) break;
            }
            std::erase_if(items, [](const Stack& item) { return item.count == 0; });
            return "Succeeded";
        }

        [[nodiscard]] std::string Equip(const Catalog& inCatalog, const std::string& inInstanceId,
            std::uint32_t inCharacterDefinitionId, std::uint32_t inLevel)
        {
            const auto found = std::find_if(items.begin(), items.end(), [&](const Stack& item)
                { return item.instanceId == inInstanceId; });
            if (found == items.end()) return "ItemNotFound";
            const auto& definition = inCatalog.definitions.at(found->definitionId);
            if (found->container != 0 || definition.category != 0) return "InvalidEquipment";
            if (!CanEquip(definition, inCharacterDefinitionId, inLevel)) return "EquipmentRequirements";
            const auto equipped = std::find_if(items.begin(), items.end(), [&](const Stack& item)
                { return item.container == EQUIPPED_CONTAINER && item.slot == definition.equipmentSlot; });
            if (equipped != items.end()) { equipped->container = 0; equipped->slot = found->slot; }
            found->container = EQUIPPED_CONTAINER; found->slot = definition.equipmentSlot;
            return "Succeeded";
        }

        [[nodiscard]] std::string Unequip(const std::string& inInstanceId)
        {
            const auto found = std::find_if(items.begin(), items.end(), [&](const Stack& item)
                { return item.instanceId == inInstanceId; });
            if (found == items.end()) return "ItemNotFound";
            if (found->container != EQUIPPED_CONTAINER) return "InvalidEquipment";
            std::array<bool, TAB_CAPACITY> occupied{};
            for (const auto& item : items) if (item.container == 0) occupied.at(item.slot) = true;
            const auto free = std::find(occupied.begin(), occupied.end(), false);
            if (free == occupied.end()) return "InventoryFull";
            found->container = 0; found->slot = static_cast<std::uint32_t>(free - occupied.begin());
            return "Succeeded";
        }

    private:
        static bool CanEquip(const Definition& inDefinition, std::uint32_t inCharacterDefinitionId,
            std::uint32_t inLevel)
        {
            return inLevel >= inDefinition.requiredLevel && (inDefinition.characterDefinitionIds.empty()
                || inDefinition.characterDefinitionIds.contains(inCharacterDefinitionId));
        }
    };
}
