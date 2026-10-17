#pragma once

#include "CharacterStoreProcedure.h"
#include "../../Shared/QuestState.h"

namespace TownServer::Persistence
{
    struct QuestRequest
    {
        bool save{};
        std::uint64_t accountId{}, characterId{}, generation{}, revision{};
        std::string ownerToken, requestId, mode{"Progress"}, operationJson, changesJson;
        std::optional<std::string> progression, inventory;
        std::string questIdsJson;
    };
    struct QuestResponse
    {
        std::int32_t result{};
        std::uint64_t characterId{}, revision{}, generation{}, serverNowMs{};
        std::optional<std::string> progression, inventory;
        ActionRPG::Quests::State quests;
        bool headerRead{};
        std::size_t rows{};
    };

    // A V7 CALL is one existing ODBC transaction; state mapping must succeed before commit.
    class QuestStoreProcedure final : public ActionRPG::Database::IStoreProcedure<QuestRequest, QuestResponse>
    {
        [[noreturn]] static void Invalid()
        {
            throw ActionRPG::Database::DatabaseException({ActionRPG::Database::DatabaseErrorCode::InvalidResult,
                "Invalid quest persistence contract."});
        }
    public:
        std::wstring_view GetName() const noexcept override
        { return req.save ? L"save_character_quests" : L"get_character_quests"; }
        void BindParameters(ActionRPG::Database::ProcedureParameters& parameters, QuestResponse&) const override
        {
            using namespace ActionRPG::Quests;
            if (!req.accountId || !req.characterId || !req.generation || !ActionRPG::Items::IsHexId(req.ownerToken, 64)) Invalid();
            parameters.AddInput(req.accountId); parameters.AddInput(req.characterId);
            parameters.AddInput(Wide(req.ownerToken)); parameters.AddInput(req.generation);
            const auto projection = Json::parse(req.questIdsJson);
            if (!projection.is_array() || projection.size() > 64 || req.questIdsJson.size() > 8192) Invalid();
            std::string previous;
            for (const auto& id : projection) { Id(id); const auto current = id.get<std::string>(); if (!previous.empty() && current <= previous) Invalid(); previous = current; }
            if (!req.save) { parameters.AddInput(Wide(req.questIdsJson)); return; }
            if (!ActionRPG::Items::IsHexId(req.requestId, 64) || (req.mode != "Progress" && req.mode != "Growth" && req.mode != "Inventory" && req.mode != "Claim")
                || req.operationJson.empty() || req.operationJson.size() > 2048 || req.changesJson.size() > 256 * 1024) Invalid();
            const auto changes = Json::parse(req.changesJson);
            Rules::Keys(changes, {"upserts"}, {"trackedQuestIds"});
            if (!changes.at("upserts").is_array() || changes.at("upserts").size() > 32) Invalid();
            for (const auto& row : changes.at("upserts"))
            {
                Rules::Keys(row, {"questId", "cycleKey", "state"}); State::ValidateInstance(row.at("state"));
                if (row.at("questId") != row.at("state").at("questId") || row.at("cycleKey") != row.at("state").at("cycleKey")) Invalid();
            }
            if (req.mode == "Progress" && (req.progression || req.inventory)) Invalid();
            if (req.mode == "Growth" && (!req.progression || req.inventory)) Invalid();
            if ((req.mode == "Inventory" || req.mode == "Claim") && (!req.progression || !req.inventory)) Invalid();
            if ((req.progression && req.progression->size() > 32768) || (req.inventory && req.inventory->size() > 32768)) Invalid();
            parameters.AddInput(Wide(req.requestId)); parameters.AddInput(req.revision); parameters.AddInput(Wide(req.mode));
            parameters.AddInput(Wide(req.operationJson)); parameters.AddInput(Wide(req.changesJson));
            parameters.AddInput(req.progression ? std::optional<std::wstring>(Wide(*req.progression)) : std::optional<std::wstring>{});
            parameters.AddInput(req.inventory ? std::optional<std::wstring>(Wide(*req.inventory)) : std::optional<std::wstring>{});
            parameters.AddInput(Wide(req.questIdsJson));
        }
        void ReadRow(ActionRPG::Database::ProcedureRow& row, std::size_t set, QuestResponse& response) const override
        {
            using namespace ActionRPG::Quests;
            if (set == 0)
            {
                if (response.headerRead || row.GetColumnCount() != 8) Invalid();
                const auto code = row.Read<std::int32_t>(1); const auto id = row.Read<std::uint64_t>(2);
                const auto revision = row.Read<std::uint64_t>(3); const auto generation = row.Read<std::uint64_t>(4);
                const auto now = row.Read<std::uint64_t>(5); const auto growth = row.Read<std::wstring>(6);
                const auto inventory = row.Read<std::wstring>(7); const auto tracking = row.Read<std::wstring>(8);
                if (!code || *code < 0 || *code > 14 || !id || !revision || !generation || !now || !tracking) Invalid();
                response.result = *code; response.characterId = *id; response.revision = *revision;
                response.generation = *generation; response.serverNowMs = *now;
                if (growth) response.progression = Utf8(*growth); if (inventory) response.inventory = Utf8(*inventory);
                const auto value = Json::parse(Utf8(*tracking)); Rules::Keys(value, {"trackedQuestIds"});
                if (!value.at("trackedQuestIds").is_array() || value.at("trackedQuestIds").size() > 3) Invalid();
                std::set<std::string> unique;
                for (const auto& idValue : value.at("trackedQuestIds"))
                {
                    Id(idValue); if (!unique.insert(idValue.get<std::string>()).second) Invalid();
                    response.quests.tracked.push_back(idValue.get<std::string>());
                }
                response.headerRead = true;
            }
            else if (set == 1)
            {
                if (!response.headerRead || row.GetColumnCount() != 3 || ++response.rows > 96) Invalid();
                const auto id = row.Read<std::wstring>(1), cycle = row.Read<std::wstring>(2), state = row.Read<std::wstring>(3);
                if (!id || !cycle || !state) Invalid();
                const auto text = Utf8(*state); if (text.size() > 8192) Invalid();
                response.quests.Add(Utf8(*id), Utf8(*cycle), Json::parse(text));
            }
            else Invalid();
        }
        void ValidateResults(std::size_t sets, std::size_t rows, const QuestResponse& response) const override
        {
            if (sets != 2 || !response.headerRead || rows != response.rows + 1) Invalid();
            if (response.inventory && !response.progression) Invalid();
            if (req.mode != "Growth" && response.progression.has_value() != response.inventory.has_value()) Invalid();
            if (req.mode == "Growth" && response.inventory) Invalid();
            if ((response.result == 0 || response.result == 3 || response.result == 13)
                && (response.characterId != req.characterId || response.generation != req.generation)) Invalid();
            if (response.result == 0 && req.save && req.mode == "Growth" && !response.progression) Invalid();
            if (response.result == 0 && req.save && (req.mode == "Inventory" || req.mode == "Claim")
                && (!response.progression || !response.inventory)) Invalid();
            if ((!req.save || req.mode == "Progress") && (response.progression || response.inventory)) Invalid();
            std::size_t active{}; for (const auto& [key, state] : response.quests.instances) if (ActionRPG::Quests::State::Live(state)) ++active;
            if (active > ActionRPG::Quests::MAX_ACTIVE || response.quests.summaries.size() > 64) Invalid();
        }
    };
}
