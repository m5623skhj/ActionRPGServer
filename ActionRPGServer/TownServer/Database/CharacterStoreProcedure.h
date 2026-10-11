#pragma once

#include "../../Shared/Database/StoreProcedure.h"
#include "../../Shared/Inventory.h"
#include "ItemUseResult.h"
#include <Windows.h>
#include <openssl/rand.h>

namespace TownServer::Persistence
{
    inline std::string RandomHex(std::size_t inByteCount)
    {
        if (inByteCount != 16 && inByteCount != 32) throw std::invalid_argument("Invalid identity size.");
        std::array<unsigned char, 32> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(inByteCount)) != 1)
            throw std::runtime_error("Unable to create persistent identity.");
        constexpr char HEX[] = "0123456789abcdef";
        std::string result;
        for (std::size_t index = 0; index < inByteCount; ++index)
        { result += HEX[bytes[index] >> 4]; result += HEX[bytes[index] & 15]; }
        return result;
    }
    inline const char* ResultName(std::int32_t inCode)
    {
        static constexpr std::array<const char*, 12> NAMES{ "Succeeded", "AccountUnavailable", "CharacterNotFound",
            "RevisionConflict", "NameTaken", "StaleOwner", "RequestConflict", "Busy", "ItemNotFound", "Cooldown",
            "UseNotFound", "OutcomeConflict" };
        return inCode >= 0 && inCode < static_cast<std::int32_t>(NAMES.size()) ? NAMES[inCode] : "DatabaseUnavailable";
    }
    inline std::wstring Wide(const std::string& inValue)
    {
        if (inValue.empty()) return {};
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, inValue.data(),
            static_cast<int>(inValue.size()), nullptr, 0);
        if (size <= 0) throw std::invalid_argument("Invalid UTF-8 persistence input.");
        std::wstring result(size, L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, inValue.data(),
            static_cast<int>(inValue.size()), result.data(), size) != size)
            throw std::invalid_argument("Invalid UTF-8 persistence input.");
        return result;
    }

    inline std::string Utf8(const std::wstring& inValue)
    {
        if (inValue.empty()) return {};
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, inValue.data(),
            static_cast<int>(inValue.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) throw std::invalid_argument("Invalid UTF-16 persistence result.");
        std::string result(size, '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, inValue.data(),
            static_cast<int>(inValue.size()), result.data(), size, nullptr, nullptr) != size)
            throw std::invalid_argument("Invalid UTF-16 persistence result.");
        return result;
    }

    enum class Operation { List, Create, Claim, Save, Release };
    struct Request
    {
        Operation operation{};
        std::uint64_t accountId{}, characterId{}, generation{}, revision{};
        std::uint32_t definitionId{}, initialLevel{}, initialSp{};
        std::string requestId, ownerToken, name, progression, inventory, operationJson;
    };
    struct CharacterState
    {
        std::int32_t result{};
        std::uint64_t characterId{}, revision{}, generation{};
        std::string name;
        std::uint32_t definitionId{};
        std::string progression, inventory;
    };
    struct Response : ItemUseResult { std::vector<CharacterState> states; };

    /// One CALL owns one transaction. The DB worker commits only after the full result is validated.
    class CharacterStoreProcedure final : public ActionRPG::Database::IStoreProcedure<Request, Response>
    {
    public:
        [[nodiscard]] std::wstring_view GetName() const noexcept override
        {
            switch (req.operation)
            {
            case Operation::List: return L"list_characters";
            case Operation::Create: return L"create_character";
            case Operation::Claim: return L"claim_character";
            case Operation::Save: return L"save_character_state";
            default: return L"release_character";
            }
        }

        void BindParameters(ActionRPG::Database::ProcedureParameters& inParameters, Response&) const override
        {
            using ActionRPG::Items::IsHexId;
            if (req.accountId == 0) Invalid();
            inParameters.AddInput(req.accountId);
            if (req.operation == Operation::List) return;
            if (req.operation == Operation::Create)
            {
                if (!IsHexId(req.requestId, 64) || req.name.empty() || req.name.size() > 32
                    || req.definitionId == 0 || req.initialLevel == 0 || req.initialLevel > 1000000) Invalid();
                inParameters.AddInput(Wide(req.requestId)); inParameters.AddInput(Wide(req.name));
                inParameters.AddInput(req.definitionId); inParameters.AddInput(req.initialLevel);
                inParameters.AddInput(req.initialSp);
                return;
            }
            if (req.characterId == 0 || !IsHexId(req.ownerToken, 64)) Invalid();
            inParameters.AddInput(req.characterId); inParameters.AddInput(Wide(req.ownerToken));
            inParameters.AddInput(req.generation);
            if (req.operation != Operation::Save) return;
            if (!IsHexId(req.requestId, 64) || req.progression.empty() || req.inventory.empty()) Invalid();
            inParameters.AddInput(Wide(req.requestId)); inParameters.AddInput(req.revision);
            if (req.operationJson.empty() || req.operationJson.size() > 2048) Invalid();
            inParameters.AddInput(Wide(req.operationJson));
            inParameters.AddInput(Wide(req.progression)); inParameters.AddInput(Wide(req.inventory));
        }

        void ReadRow(ActionRPG::Database::ProcedureRow& inRow, std::size_t inResultIndex,
            Response& outResponse) const override
        {
            if (req.operation == Operation::Claim && inResultIndex != 0)
            { ReadItemUseResult(inRow, inResultIndex, outResponse, Utf8); return; }
            if (inResultIndex != 0 || inRow.GetColumnCount() != 8
                || (req.operation != Operation::List && !outResponse.states.empty())) Invalid();
            const auto result = inRow.Read<std::int32_t>(1);
            const auto id = inRow.Read<std::uint64_t>(2);
            const auto revision = inRow.Read<std::uint64_t>(3);
            const auto generation = inRow.Read<std::uint64_t>(4);
            const auto name = inRow.Read<std::wstring>(5);
            const auto definition = inRow.Read<std::uint32_t>(6);
            const auto progression = inRow.Read<std::wstring>(7);
            const auto inventory = inRow.Read<std::wstring>(8);
            if (!result || *result < 0 || *result > 11 || !id || !revision || !generation
                || !name || !definition || !progression || !inventory) Invalid();
            CharacterState state{ *result, *id, *revision, *generation, Utf8(*name), *definition,
                Utf8(*progression), Utf8(*inventory) };
            if (state.characterId != 0 && (state.name.empty() || state.name.size() > 32 || state.definitionId == 0
                || state.progression.empty() || state.inventory.empty())) Invalid();
            outResponse.states.push_back(std::move(state));
        }

        void ValidateResults(std::size_t inResultSetCount, std::size_t inRowCount,
            const Response& inResponse) const override
        {
            const bool claim = req.operation == Operation::Claim;
            const auto expectedRows = inResponse.states.size() + (claim ? 1 + inResponse.cooldowns.size() : 0);
            if (inResultSetCount != (claim ? 3 : 1) || inRowCount == 0 || inRowCount != expectedRows
                || (req.operation != Operation::List && inResponse.states.size() != 1)
                || (claim && (!inResponse.use || inResponse.cooldowns.empty()))) Invalid();
            if (claim) ValidateItemUseResult(inResponse);
            std::unordered_set<std::uint64_t> ids;
            for (const auto& state : inResponse.states)
            {
                if (req.operation != Operation::List && state.result == 0 && state.characterId == 0) Invalid();
                if (state.characterId == 0)
                { if (inResponse.states.size() != 1) Invalid(); }
                else if (!ids.emplace(state.characterId).second || (req.characterId != 0
                    && state.characterId != req.characterId)) Invalid();
            }
        }

    private:
        [[noreturn]] static void Invalid()
        {
            throw ActionRPG::Database::DatabaseException({ ActionRPG::Database::DatabaseErrorCode::InvalidResult,
                "Invalid character persistence contract." });
        }
    };
}
