#pragma once

#include "StoreProcedure.h"

#include <asio.hpp>
#include <memory>

namespace TownServer::Database
{
    struct DatabaseOptions
    {
        std::wstring connectionString;
        std::size_t connectionCount = 2;
        std::size_t maxPendingRequests = 128;
        std::uint32_t connectionTimeoutSeconds = 5;
        std::uint32_t queryTimeoutSeconds = 10;
        std::uint32_t queueTimeoutSeconds = 30;

        // Missing environment variable leaves DB access disabled. Never log the connection string.
        [[nodiscard]] static DatabaseOptions FromEnvironment();
    };

    class ProcedureConnection final
    {
    public:
        ProcedureConnection(void* inEnvironment, const DatabaseOptions& inOptions);
        ~ProcedureConnection();
        ProcedureConnection(const ProcedureConnection&) = delete;
        ProcedureConnection& operator=(const ProcedureConnection&) = delete;

        void Execute(std::wstring_view inName, ProcedureParameters& inParameters,
            const std::function<void(ProcedureRow&, std::size_t)>& inReadRow,
            const std::function<void(std::size_t, std::size_t)>& inValidateResults);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    class OdbcDatabase final
    {
    public:
        explicit OdbcDatabase(DatabaseOptions inOptions);
        ~OdbcDatabase();
        OdbcDatabase(const OdbcDatabase&) = delete;
        OdbcDatabase& operator=(const OdbcDatabase&) = delete;

        [[nodiscard]] bool IsConfigured() const noexcept;
        // Non-blocking: reject new requests, drain accepted requests. Destructor joins DB workers.
        void Stop();

        /** Transfer exclusive procedure ownership. Complete once on the supplied live executor;
         * the work guard keeps its context running through delivery. No automatic execution retry.
         */
        template <typename TProcedure, typename THandler>
        void Run(std::unique_ptr<TProcedure> inProcedure, asio::any_io_executor inCompletionExecutor,
            THandler inHandler)
        {
            using Response = typename TProcedure::Response;
            static_assert(std::is_base_of_v<IStoreProcedure<typename TProcedure::Request, Response>, TProcedure>);
            auto procedure = std::shared_ptr<TProcedure>(std::move(inProcedure));
            auto guard = asio::make_work_guard(inCompletionExecutor);
            auto work = [procedure = std::move(procedure), executor = std::move(inCompletionExecutor),
                guard = std::move(guard), handler = std::make_shared<THandler>(std::move(inHandler))]
                (ProcedureConnection* inConnection, std::optional<DatabaseError> inError) mutable
            {
                ProcedureResult<Response> result;
                if (inError) result.error = std::move(inError);
                else if (!procedure)
                    result.error = DatabaseError{ DatabaseErrorCode::InvalidProcedure, "Null procedure." };
                else
                {
                    bool executionCompleted = false;
                    try
                    {
                        Response response{};
                        ProcedureParameters parameters;
                        procedure->BindParameters(parameters, response);
                        inConnection->Execute(procedure->GetName(), parameters,
                            [&](ProcedureRow& inRow, std::size_t inIndex)
                            {
                                procedure->ReadRow(inRow, inIndex, response);
                            },
                            [&](std::size_t inResultSetCount, std::size_t inRowCount)
                            {
                                procedure->ValidateResults(inResultSetCount, inRowCount, response);
                            });
                        executionCompleted = true;
                        result.response = std::move(response);
                    }
                    catch (const DatabaseException& inException) { result.error = inException.error; }
                    catch (...)
                    {
                        result.error = DatabaseError{ DatabaseErrorCode::InternalError,
                            "Procedure binding or result mapping failed.", {}, 0, executionCompleted };
                    }
                }
                asio::post(executor, [handler = std::move(handler), result = std::move(result),
                    guard = std::move(guard)]() mutable
                {
                    std::invoke(*handler, std::move(result));
                });
            };
            auto pending = std::make_shared<decltype(work)>(std::move(work));
            Enqueue([pending](ProcedureConnection* inConnection, std::optional<DatabaseError> inError)
            {
                (*pending)(inConnection, std::move(inError));
            });
        }

    private:
        using Work = std::function<void(ProcedureConnection*, std::optional<DatabaseError>)>;
        void Enqueue(Work inWork);
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}
