#pragma once

#include "../../Shared/Database/OdbcDatabase.h"

namespace ActionRPG::Database
{
    class LoginSchemaVerifier;

    // A proof is tied to the account DB actually inspected, never to a configuration flag.
    class VerifiedLoginSchema final
    {
    public:
        static constexpr std::uint32_t REQUIRED_SCHEMA_VERSION = 3;
        [[nodiscard]] bool Matches(const std::shared_ptr<OdbcDatabase>& inDatabase) const noexcept
        {
            return inDatabase && database.lock() == inDatabase;
        }

    private:
        friend class LoginSchemaVerifier;
        explicit VerifiedLoginSchema(const std::shared_ptr<OdbcDatabase>& inDatabase)
            : database(inDatabase) {}
        std::weak_ptr<OdbcDatabase> database;
    };

    enum class SchemaVerificationStatus { NotReady, DatabaseNotConfigured, Ready };

    struct SchemaVerificationResult
    {
        SchemaVerificationStatus status = SchemaVerificationStatus::NotReady;
        std::shared_ptr<const VerifiedLoginSchema> schema;
        [[nodiscard]] bool IsReady() const noexcept
        {
            return status == SchemaVerificationStatus::Ready && schema != nullptr;
        }
    };

    class LoginSchemaVerifier final
    {
    public:
        using CompletionHandler = std::function<void(SchemaVerificationResult)>;

        /** Load the configured deployed SQL files, then inspect this account DB asynchronously.
         * Complete on the supplied live executor. Any history/structure error blocks approval.
         * This is a startup proof: migration and DDL require the service to remain stopped.
         */
        static void Verify(std::shared_ptr<OdbcDatabase> inDatabase,
            asio::any_io_executor inCompletionExecutor, CompletionHandler inHandler);
    };
}
