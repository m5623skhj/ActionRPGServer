#include "GetAuthAccountStatusProcedure.h"

namespace ActionRPG::Database
{
    namespace
    {
        [[noreturn]] void InvalidResult()
        {
            throw DatabaseException({ DatabaseErrorCode::InvalidResult, "Invalid Auth account status procedure result." });
        }
    }

    void GetAuthAccountStatusProcedure::BindParameters(ProcedureParameters& inParameters, Response&) const
    {
        if (req.accountId == 0)
            throw DatabaseException({ DatabaseErrorCode::InvalidProcedure, "Invalid Auth account identifier." });
        inParameters.AddInput(req.accountId);
    }

    void GetAuthAccountStatusProcedure::ReadRow(ProcedureRow& inRow, std::size_t inResultIndex,
        Response& outResponse) const
    {
        if (inResultIndex != 0 || outResponse.hasRow || inRow.GetColumnCount() != 3) InvalidResult();
        const auto resultCode = inRow.Read<std::int32_t>(1);
        const auto accountId = inRow.Read<std::uint64_t>(2);
        const auto accountStatus = inRow.Read<std::int32_t>(3);
        if (!resultCode || !accountId || *accountId == 0 || *accountId != req.accountId
            || *resultCode < 0 || *resultCode > 2) InvalidResult();
        if ((*resultCode == 2 && accountStatus.has_value())
            || (*resultCode != 2 && (!accountStatus || *accountStatus != *resultCode))) InvalidResult();
        outResponse = { *resultCode, *accountId, accountStatus, true };
    }

    void GetAuthAccountStatusProcedure::ValidateResults(std::size_t inResultSetCount,
        std::size_t inRowCount, const Response& inResponse) const
    {
        if (inResultSetCount != 1 || inRowCount != 1 || !inResponse.hasRow) InvalidResult();
    }
}
