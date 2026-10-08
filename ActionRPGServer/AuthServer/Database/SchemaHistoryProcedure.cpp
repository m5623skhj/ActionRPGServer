#include "SchemaHistoryProcedure.h"

namespace ActionRPG::Database
{
    namespace
    {
        [[noreturn]] void InvalidResult(std::string inMessage)
        {
            throw DatabaseException({ DatabaseErrorCode::InvalidResult, std::move(inMessage) });
        }
    }

    void SchemaHistoryProcedure::ReadRow(ProcedureRow& inRow, std::size_t inResultIndex,
        Response& outResponse) const
    {
        constexpr std::array<std::size_t, 18> COLUMN_COUNTS{5, 9, 9, 11, 6, 4, 7, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};
        if (inResultIndex >= COLUMN_COUNTS.size())
            InvalidResult("Unexpected schema result set; result_set=" + std::to_string(inResultIndex));
        if (inRow.GetColumnCount() != COLUMN_COUNTS[inResultIndex])
            InvalidResult("Schema column count mismatch; result_set=" + std::to_string(inResultIndex)
                + "; actual=" + std::to_string(inRow.GetColumnCount())
                + "; expected=" + std::to_string(COLUMN_COUNTS[inResultIndex]));
        SchemaHistoryRow row;
        row.reserve(inRow.GetColumnCount());
        std::size_t column = 1;
        try
        {
            for (; column <= inRow.GetColumnCount(); ++column)
                row.push_back(inRow.Read<std::wstring>(column));
        }
        catch (const DatabaseException& inException)
        {
            auto error = inException.error;
            // Result sets are zero-based; rows and columns are one-based. No values are logged.
            error.message += "; schema_result_set=" + std::to_string(inResultIndex)
                + "; schema_row=" + std::to_string(outResponse.resultSets[inResultIndex].size() + 1)
                + "; schema_column=" + std::to_string(column);
            throw DatabaseException(std::move(error));
        }
        outResponse.resultSets[inResultIndex].push_back(std::move(row));
    }

    void SchemaHistoryProcedure::ValidateResults(std::size_t inResultSetCount,
        std::size_t inRowCount, const Response& inResponse) const
    {
        bool invalidShape = inResultSetCount != 18 || inResponse.resultSets[0].size() != 1
            || inResponse.resultSets[1].empty() || inResponse.resultSets[2].empty()
            || inResponse.resultSets[3].empty() || inResponse.resultSets[4].empty()
            || inResponse.resultSets[5].size() != 11 || inResponse.resultSets[6].size() != 29;
        for (std::size_t index = 7; index < inResponse.resultSets.size(); ++index)
            invalidShape = invalidShape || inResponse.resultSets[index].size() != 1;
        if (invalidShape)
        {
            std::string message = "Schema result shape mismatch; result_sets=" + std::to_string(inResultSetCount)
                + "; total_rows=" + std::to_string(inRowCount) + "; rows_per_set=";
            for (const auto& rows : inResponse.resultSets)
                message += std::to_string(rows.size()) + ",";
            InvalidResult(std::move(message));
        }
        std::size_t total = 0;
        for (const auto& rows : inResponse.resultSets) total += rows.size();
        if (total != inRowCount)
            InvalidResult("Schema row count mismatch; actual=" + std::to_string(inRowCount)
                + "; expected=" + std::to_string(total));
    }
}
