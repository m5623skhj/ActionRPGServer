#pragma once

#include "../../Shared/Database/StoreProcedure.h"
#include "../../Shared/Inventory.h"
#include <charconv>
#include <functional>

namespace TownServer::Persistence
{
    struct ItemUseRow
    {
        std::string requestId, instanceId, definitionId, roomIncarnation, operationJson, executionJson,
            executionHash, cooldownUntilMs, reason, serverNowMs, pendingRequestId;
        std::int32_t state{};
        std::uint64_t originGeneration{}, expectedRevision{}, reserveRevision{}, roomId{};
        std::uint32_t container{}, slot{}, cooldownMs{};
    };
    struct ItemCooldown
    {
        std::string definitionId, state, readyMs, serverNowMs;
        std::optional<std::uint32_t> remainingMs;
    };
    struct ItemUseResult
    {
        std::optional<ItemUseRow> use;
        std::vector<ItemCooldown> cooldowns;
    };
    [[noreturn]] inline void InvalidItemUseResult()
    {
        throw ActionRPG::Database::DatabaseException({ActionRPG::Database::DatabaseErrorCode::InvalidResult,
            "Invalid item use persistence contract."});
    }
    inline bool IsEpochMilliseconds(const std::string& inValue)
    {
        std::uint64_t value{};
        const auto [end, error] = std::from_chars(inValue.data(), inValue.data() + inValue.size(), value);
        return !inValue.empty() && error == std::errc{} && end == inValue.data() + inValue.size()
            && (inValue.size() == 1 || inValue.front() != '0');
    }
    template <typename T> T RequiredColumn(ActionRPG::Database::ProcedureRow& inRow, std::size_t inColumn)
    {
        const auto value = inRow.Read<T>(inColumn);
        if (!value) InvalidItemUseResult();
        return *value;
    }
    inline void ReadItemUseResult(ActionRPG::Database::ProcedureRow& inRow, std::size_t inIndex,
        ItemUseResult& outResult, const std::function<std::string(const std::wstring&)>& inUtf8)
    {
        auto text = [&](std::size_t column) { return inUtf8(RequiredColumn<std::wstring>(inRow, column)); };
        if (inIndex == 1)
        {
            if (inRow.GetColumnCount() != 19 || outResult.use) InvalidItemUseResult();
            ItemUseRow row;
            row.requestId = text(1); row.state = RequiredColumn<std::int32_t>(inRow, 2);
            row.originGeneration = RequiredColumn<std::uint64_t>(inRow, 3);
            row.expectedRevision = RequiredColumn<std::uint64_t>(inRow, 4);
            row.reserveRevision = RequiredColumn<std::uint64_t>(inRow, 5);
            row.instanceId = text(6); row.definitionId = text(7);
            row.container = RequiredColumn<std::uint32_t>(inRow, 8); row.slot = RequiredColumn<std::uint32_t>(inRow, 9);
            row.roomId = RequiredColumn<std::uint64_t>(inRow, 10); row.roomIncarnation = text(11);
            row.operationJson = text(12); row.executionJson = text(13); row.executionHash = text(14);
            row.cooldownMs = RequiredColumn<std::uint32_t>(inRow, 15);
            row.cooldownUntilMs = text(16); row.reason = text(17); row.serverNowMs = text(18);
            row.pendingRequestId = text(19);
            if (row.state < 0 || row.state > 4 || !IsEpochMilliseconds(row.serverNowMs)
                || (!row.pendingRequestId.empty() && !ActionRPG::Items::IsHexId(row.pendingRequestId, 64)))
                InvalidItemUseResult();
            if (row.state != 0 && (!ActionRPG::Items::IsHexId(row.requestId, 64)
                || !ActionRPG::Items::IsHexId(row.instanceId, 32)
                || !ActionRPG::Items::IsHexId(row.roomIncarnation, 32)
                || !ActionRPG::Items::IsHexId(row.executionHash, 64) || row.roomId == 0
                || row.originGeneration == 0 || !ActionRPG::PlayerSkills::Catalog::IsId(row.definitionId)
                || row.container != 2 || row.slot >= 40 || row.cooldownMs == 0 || row.cooldownMs > 3600000
                || row.expectedRevision == std::numeric_limits<std::uint64_t>::max()
                || row.reserveRevision != row.expectedRevision + 1
                || row.operationJson.empty() || row.executionJson.empty()
                || row.operationJson.size() > 2048 || row.executionJson.size() > 2048 || row.reason.size() > 64))
                InvalidItemUseResult();
            if ((row.state == 2 || row.state == 4) && !IsEpochMilliseconds(row.cooldownUntilMs))
                InvalidItemUseResult();
            if ((row.state == 1 || row.state == 3) && !row.cooldownUntilMs.empty()) InvalidItemUseResult();
            if (row.state == 1 && row.pendingRequestId != row.requestId) InvalidItemUseResult();
            if (row.state == 0 && (!row.requestId.empty() || row.originGeneration != 0 || row.expectedRevision != 0
                || row.reserveRevision != 0 || !row.instanceId.empty() || !row.definitionId.empty() || row.container != 0
                || row.slot != 0 || row.roomId != 0 || !row.roomIncarnation.empty() || !row.operationJson.empty()
                || !row.executionJson.empty() || !row.executionHash.empty() || row.cooldownMs != 0
                || !row.cooldownUntilMs.empty() || !row.reason.empty())) InvalidItemUseResult();
            outResult.use = std::move(row);
        }
        else if (inIndex == 2)
        {
            if (inRow.GetColumnCount() != 5) InvalidItemUseResult();
            ItemCooldown row;
            row.definitionId = text(1); row.state = text(2); row.readyMs = text(3);
            row.remainingMs = inRow.Read<std::uint32_t>(4); row.serverNowMs = text(5);
            if (!IsEpochMilliseconds(row.serverNowMs)
                || (row.state != "Pending" && row.state != "Active" && row.state != "None")
                || (row.state == "Pending" && (row.remainingMs || !row.readyMs.empty()))
                || (row.state == "Active" && (!row.remainingMs || !IsEpochMilliseconds(row.readyMs)))
                || (row.state == "None" && (!row.remainingMs || *row.remainingMs != 0 || !row.readyMs.empty())))
                InvalidItemUseResult();
            if ((row.state == "None" && !row.definitionId.empty())
                || (row.state != "None" && !ActionRPG::PlayerSkills::Catalog::IsId(row.definitionId))) InvalidItemUseResult();
            for (const auto& previous : outResult.cooldowns)
                if (previous.definitionId == row.definitionId) InvalidItemUseResult();
            outResult.cooldowns.push_back(std::move(row));
        }
        else InvalidItemUseResult();
    }
    inline void ValidateItemUseResult(const ItemUseResult& inResult)
    {
        if (!inResult.use || inResult.cooldowns.empty()) InvalidItemUseResult();
        for (const auto& row : inResult.cooldowns)
            if (row.serverNowMs != inResult.use->serverNowMs
                || (row.state == "None" && inResult.cooldowns.size() != 1)) InvalidItemUseResult();
    }
    inline ActionRPG::Items::Json CooldownsJson(const ItemUseResult& inResult)
    {
        auto result = ActionRPG::Items::Json::array();
        for (const auto& row : inResult.cooldowns)
        {
            if (row.definitionId.empty()) continue;
            result.push_back({{"definitionId", row.definitionId}, {"cooldownState", row.state},
                {"cooldownServerTimeMs", row.serverNowMs},
                {"cooldownReadyAtMs", row.readyMs.empty() ? ActionRPG::Items::Json(nullptr) : ActionRPG::Items::Json(row.readyMs)},
                {"cooldownRemainingMs", row.remainingMs ? ActionRPG::Items::Json(*row.remainingMs) : ActionRPG::Items::Json(nullptr)}});
        }
        return result;
    }
}
