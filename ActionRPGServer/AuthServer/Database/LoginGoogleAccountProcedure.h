#pragma once

#include "../../Shared/Database/StoreProcedure.h"

namespace ActionRPG::Database
{
    struct LoginGoogleAccountRequest { std::wstring subject; };
    struct LoginGoogleAccountResponse
    {
        std::int32_t resultCode{};
        std::uint64_t accountId{};
        std::int32_t accountStatus{};
        bool wasCreated{};
        bool hasRow{};
    };

    class LoginGoogleAccountProcedure final
        : public IStoreProcedure<LoginGoogleAccountRequest, LoginGoogleAccountResponse>
    {
    public:
        [[nodiscard]] std::wstring_view GetName() const noexcept override { return L"login_google_account"; }
        void BindParameters(ProcedureParameters& inParameters, Response& outResponse) const override;
        void ReadRow(ProcedureRow& inRow, std::size_t inResultIndex, Response& outResponse) const override;
        void ValidateResults(std::size_t inResultSetCount, std::size_t inRowCount,
            const Response& inResponse) const override;
    };
}
