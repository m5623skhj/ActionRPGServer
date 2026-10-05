#pragma once

#include "../../Shared/Database/StoreProcedure.h"

namespace ActionRPG::Database
{
    struct GetAuthAccountStatusRequest { std::uint64_t accountId{}; };
    struct GetAuthAccountStatusResponse
    {
        std::int32_t resultCode{};
        std::uint64_t accountId{};
        std::optional<std::int32_t> accountStatus;
        bool hasRow{};
    };

    class GetAuthAccountStatusProcedure final
        : public IStoreProcedure<GetAuthAccountStatusRequest, GetAuthAccountStatusResponse>
    {
    public:
        [[nodiscard]] std::wstring_view GetName() const noexcept override { return L"get_auth_account_status"; }
        void BindParameters(ProcedureParameters& inParameters, Response& outResponse) const override;
        void ReadRow(ProcedureRow& inRow, std::size_t inResultIndex, Response& outResponse) const override;
        void ValidateResults(std::size_t inResultSetCount, std::size_t inRowCount,
            const Response& inResponse) const override;
    };
}
