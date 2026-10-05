#include "LoginGoogleAccountProcedure.h"

namespace TownServer::Database
{
    namespace
    {
        [[noreturn]] void InvalidResult()
        {
            throw DatabaseException({ DatabaseErrorCode::InvalidResult, "Invalid Google account procedure result." });
        }
    }

    void LoginGoogleAccountProcedure::BindParameters(ProcedureParameters& inParameters, Response&) const
    {
        if (req.subject.empty() || req.subject.size() > 255)
            throw DatabaseException({ DatabaseErrorCode::InvalidProcedure, "Invalid Google subject length." });
        for (const wchar_t value : req.subject)
            if (value <= 0 || value > 127)
                throw DatabaseException({ DatabaseErrorCode::InvalidProcedure, "Invalid Google subject encoding." });
        inParameters.AddInput(req.subject);
    }

    void LoginGoogleAccountProcedure::ReadRow(ProcedureRow& inRow, std::size_t inResultIndex,
        Response& outResponse) const
    {
        if (inResultIndex != 0 || outResponse.hasRow || inRow.GetColumnCount() != 4) InvalidResult();
        const auto resultCode = inRow.Read<std::int32_t>(1);
        const auto accountId = inRow.Read<std::uint64_t>(2);
        const auto accountStatus = inRow.Read<std::int32_t>(3);
        const auto wasCreated = inRow.Read<std::int32_t>(4);
        if (!resultCode || !accountId || !accountStatus || !wasCreated || *accountId == 0
            || (*resultCode != 0 && *resultCode != 1) || *accountStatus != *resultCode
            || (*wasCreated != 0 && *wasCreated != 1) || (*resultCode == 1 && *wasCreated != 0)) InvalidResult();
        outResponse = { *resultCode, *accountId, *accountStatus, *wasCreated == 1, true };
    }

    void LoginGoogleAccountProcedure::ValidateResults(std::size_t inResultSetCount,
        std::size_t inRowCount, const Response& inResponse) const
    {
        if (inResultSetCount != 1 || inRowCount != 1 || !inResponse.hasRow) InvalidResult();
    }
}
