#pragma once

#include "../../Shared/Database/StoreProcedure.h"
#include <array>

namespace ActionRPG::Database
{
    using SchemaHistoryRow = std::vector<std::optional<std::wstring>>;
    struct SchemaHistoryRequest {};
    struct SchemaHistoryResponse
    {
        std::array<std::vector<SchemaHistoryRow>, 10> resultSets;
    };

    /// Reads the complete audit trail and live information_schema contract under the migration lock.
    class SchemaHistoryProcedure final : public IStoreProcedure<SchemaHistoryRequest, SchemaHistoryResponse>
    {
    public:
        [[nodiscard]] std::wstring_view GetName() const noexcept override { return L"get_schema_migration_history"; }
        void BindParameters(ProcedureParameters&, Response&) const override {}
        void ReadRow(ProcedureRow& inRow, std::size_t inResultIndex, Response& outResponse) const override;
        void ValidateResults(std::size_t inResultSetCount, std::size_t inRowCount,
            const Response& inResponse) const override;
    };
}
