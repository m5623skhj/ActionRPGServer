#include "SchemaHistoryProcedure.h"

namespace ActionRPG::Database
{
    namespace
    {
        [[noreturn]] void InvalidResult()
        {
            throw DatabaseException({ DatabaseErrorCode::InvalidResult, "Invalid schema inspection result." });
        }
    }

    void SchemaHistoryProcedure::ReadRow(ProcedureRow& inRow, std::size_t inResultIndex,
        Response& outResponse) const
    {
        constexpr std::array<std::size_t, 10> COLUMN_COUNTS{5, 9, 9, 11, 6, 4, 7, 6, 6, 6};
        if (inResultIndex >= COLUMN_COUNTS.size() || inRow.GetColumnCount() != COLUMN_COUNTS[inResultIndex])
            InvalidResult();
        SchemaHistoryRow row;
        row.reserve(inRow.GetColumnCount());
        for (std::size_t column = 1; column <= inRow.GetColumnCount(); ++column)
            row.push_back(inRow.Read<std::wstring>(column));
        outResponse.resultSets[inResultIndex].push_back(std::move(row));
    }

    void SchemaHistoryProcedure::ValidateResults(std::size_t inResultSetCount,
        std::size_t inRowCount, const Response& inResponse) const
    {
        if (inResultSetCount != 10 || inResponse.resultSets[0].size() != 1
            || inResponse.resultSets[1].empty() || inResponse.resultSets[2].empty()
            || inResponse.resultSets[3].empty() || inResponse.resultSets[4].empty()
            || inResponse.resultSets[5].size() != 3 || inResponse.resultSets[6].size() != 2
            || inResponse.resultSets[7].size() != 1 || inResponse.resultSets[8].size() != 1
            || inResponse.resultSets[9].size() != 1)
            InvalidResult();
        std::size_t total = 0;
        for (const auto& rows : inResponse.resultSets) total += rows.size();
        if (total != inRowCount) InvalidResult();
    }
}
