#pragma once
#include "../Shared/Inventory.h"
#include <functional>

namespace GameRoomServer
{
    struct ItemUseContext
    {
        std::uint32_t& hp;
        std::uint32_t maxHp{}, level{};
        bool running{}, throwReady{};
        std::function<void()> spawnPreparedProjectile;
    };
    class ItemUseAction
    {
    public:
        virtual ~ItemUseAction() = default;
        virtual const char* Validate(const ItemUseContext& inContext, const ActionRPG::Items::Definition::Use& inUse) const = 0;
        virtual void DoAction(ItemUseContext& inContext, const ActionRPG::Items::Definition::Use& inUse) const = 0;
    protected:
        static const char* ValidateCommon(const ItemUseContext& inContext, const ActionRPG::Items::Definition::Use& inUse)
        {
            if (!inContext.running) return "NotInDungeon";
            if (inContext.hp == 0) return "Dead";
            if (inContext.level < inUse.requiredLevel) return "LevelRestricted";
            return "Succeeded";
        }
    };
    class RecoveryItemAction final : public ItemUseAction
    {
    public:
        const char* Validate(const ItemUseContext& inContext, const ActionRPG::Items::Definition::Use& inUse) const override
        {
            const auto common = ValidateCommon(inContext, inUse);
            if (std::string_view(common) != "Succeeded") return common;
            return inContext.hp < inContext.maxHp ? "Succeeded" : "FullHealth";
        }
        void DoAction(ItemUseContext& inContext, const ActionRPG::Items::Definition::Use& inUse) const override
        {
            inContext.hp += std::min(inUse.amount, inContext.maxHp - inContext.hp);
        }
    };
    class ThrowItemAction final : public ItemUseAction
    {
    public:
        const char* Validate(const ItemUseContext& inContext, const ActionRPG::Items::Definition::Use& inUse) const override
        {
            const auto common = ValidateCommon(inContext, inUse);
            if (std::string_view(common) != "Succeeded") return common;
            return inContext.throwReady ? "Succeeded" : "ActionUnavailable";
        }
        void DoAction(ItemUseContext& inContext, const ActionRPG::Items::Definition::Use&) const override
        { inContext.spawnPreparedProjectile(); }
    };
}
