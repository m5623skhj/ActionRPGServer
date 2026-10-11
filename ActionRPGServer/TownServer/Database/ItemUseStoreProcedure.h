#pragma once
#include "CharacterStoreProcedure.h"

namespace TownServer::Persistence
{
    enum class ItemUseOperation { Reserve, Get, Complete, Cancel };
    struct ItemUseRequest
    {
        ItemUseOperation operation{ItemUseOperation::Get};
        std::uint64_t accountId{}, characterId{}, generation{}, revision{}, roomId{};
        std::string ownerToken, requestId, instanceId, roomIncarnation, operationJson, executionJson, executionHash, reason;
        std::uint32_t cooldownMs{};
        std::uint32_t disposition{};
    };
    class ItemUseStoreProcedure final : public ActionRPG::Database::IStoreProcedure<ItemUseRequest, Response>
    {
    public:
        std::wstring_view GetName() const noexcept override
        {
            switch (req.operation)
            {
            case ItemUseOperation::Reserve: return L"reserve_item_use";
            case ItemUseOperation::Complete: return L"complete_item_use";
            case ItemUseOperation::Cancel: return L"cancel_item_use";
            default: return L"get_item_use";
            }
        }
        void BindParameters(ActionRPG::Database::ProcedureParameters& inParameters, Response&) const override
        {
            using ActionRPG::Items::IsHexId;
            if (req.accountId == 0 || req.characterId == 0 || req.generation == 0
                || !IsHexId(req.ownerToken, 64) || (!req.requestId.empty() && !IsHexId(req.requestId, 64)))
                InvalidItemUseResult();
            inParameters.AddInput(req.accountId); inParameters.AddInput(req.characterId);
            inParameters.AddInput(Wide(req.ownerToken)); inParameters.AddInput(req.generation);
            if (req.operation == ItemUseOperation::Get)
            {
                inParameters.AddInput(req.requestId.empty() ? std::optional<std::wstring>{}
                    : std::optional<std::wstring>{Wide(req.requestId)});
                return;
            }
            if (req.requestId.empty() || req.roomId == 0 || !IsHexId(req.roomIncarnation, 32)) InvalidItemUseResult();
            inParameters.AddInput(Wide(req.requestId));
            if (req.operation == ItemUseOperation::Reserve)
            {
                if (!IsHexId(req.instanceId, 32) || req.operationJson.empty() || req.executionJson.empty()
                    || req.operationJson.size() > 2048 || req.executionJson.size() > 2048) InvalidItemUseResult();
                inParameters.AddInput(req.revision); inParameters.AddInput(Wide(req.instanceId));
            }
            inParameters.AddInput(req.roomId); inParameters.AddInput(Wide(req.roomIncarnation));
            if (req.operation == ItemUseOperation::Reserve)
            {
                inParameters.AddInput(Wide(req.operationJson)); inParameters.AddInput(Wide(req.executionJson));
                inParameters.AddInput(req.cooldownMs);
            }
            else
            {
                if (!IsHexId(req.executionHash, 64)) InvalidItemUseResult();
                inParameters.AddInput(Wide(req.executionHash));
                if (req.operation == ItemUseOperation::Cancel)
                {
                    if (req.disposition > 1 || req.reason.empty() || req.reason.size() > 64)
                        InvalidItemUseResult();
                    inParameters.AddInput(req.disposition); inParameters.AddInput(Wide(req.reason));
                }
            }
        }
        void ReadRow(ActionRPG::Database::ProcedureRow& inRow, std::size_t inResultIndex, Response& outResponse) const override
        {
            if (inResultIndex != 0) { ReadItemUseResult(inRow, inResultIndex, outResponse, Utf8); return; }
            CharacterStoreProcedure reader; reader.req.operation = Operation::Save;
            reader.ReadRow(inRow, 0, outResponse);
        }
        void ValidateResults(std::size_t inSets, std::size_t inRows, const Response& inResponse) const override
        {
            if (inSets != 3 || inResponse.states.size() != 1 || !inResponse.use || inResponse.cooldowns.empty()
                || inRows != 2 + inResponse.cooldowns.size()) InvalidItemUseResult();
            ValidateItemUseResult(inResponse);
            const auto& state = inResponse.states.front();
            if (state.characterId != 0 && state.characterId != req.characterId) InvalidItemUseResult();
            if (state.result == 0 && (state.characterId != req.characterId || state.generation != req.generation))
                InvalidItemUseResult();
            if (inResponse.use->state != 0 && !req.requestId.empty() && inResponse.use->requestId != req.requestId)
                InvalidItemUseResult();
        }
    };
}
