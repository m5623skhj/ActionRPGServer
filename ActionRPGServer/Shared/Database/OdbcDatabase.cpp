#include "OdbcDatabase.h"

#include <Windows.h>
#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

namespace ActionRPG::Database
{
    namespace
    {
        constexpr std::size_t MAX_CONNECTION_COUNT = 16;
        constexpr std::size_t MAX_PENDING_REQUESTS = 4096;
        constexpr std::size_t MAX_PARAMETER_COUNT = 64;
        constexpr std::size_t MAX_RESULT_ROWS = 4096;
        constexpr std::size_t MAX_RESULT_SETS = 16;
        constexpr std::size_t MAX_RESULT_TRANSITIONS = 64;
        constexpr std::size_t MAX_RESULT_BYTES = 4 * 1024 * 1024;

        class OdbcHandle final
        {
        public:
            explicit OdbcHandle(SQLSMALLINT inType) : type(inType) {}
            ~OdbcHandle() { Reset(); }
            OdbcHandle(const OdbcHandle&) = delete;
            OdbcHandle& operator=(const OdbcHandle&) = delete;

            void Reset() noexcept
            {
                if (value != SQL_NULL_HANDLE)
                {
                    if (type == SQL_HANDLE_DBC) SQLDisconnect(value);
                    SQLFreeHandle(type, value);
                    value = SQL_NULL_HANDLE;
                }
            }

            SQLSMALLINT type;
            SQLHANDLE value = SQL_NULL_HANDLE;
        };

        [[noreturn]] void Fail(DatabaseErrorCode inCode, const char* inMessage)
        {
            throw DatabaseException(DatabaseError{ inCode, inMessage });
        }

        // Driver text can contain SQL/connection details. Expose only SQLSTATE, native code and fixed context.
        void Check(SQLRETURN inResult, SQLSMALLINT inType, SQLHANDLE inHandle, const char* inContext)
        {
            if (inResult == SQL_SUCCESS) return;
            DatabaseError error{ DatabaseErrorCode::DriverError, inContext };
            SQLWCHAR state[6]{};
            SQLINTEGER nativeCode{};
            SQLWCHAR ignoredMessage[2]{};
            SQLSMALLINT length{};
            if (SQL_SUCCEEDED(SQLGetDiagRecW(inType, inHandle, 1, state, &nativeCode,
                ignoredMessage, 2, &length)))
            {
                for (std::size_t index = 0; index < 5 && state[index] != 0; ++index)
                    error.sqlState.push_back(static_cast<char>(state[index]));
                error.nativeCode = nativeCode;
            }
            // Also reject warnings: truncation and silently substituted options must not look like success.
            throw DatabaseException(std::move(error));
        }

        SQLPOINTER AttributeValue(std::uint32_t inValue)
        {
            return reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(inValue));
        }

        void Allocate(OdbcHandle& inHandle, SQLHANDLE inParent)
        {
            const SQLRETURN result = SQLAllocHandle(inHandle.type, inParent, &inHandle.value);
            if (!SQL_SUCCEEDED(result))
                Fail(DatabaseErrorCode::DriverError, "Unable to allocate ODBC handle.");
        }

        bool IsIntegerType(SQLSMALLINT inType)
        {
            return inType == SQL_BIT || inType == SQL_TINYINT || inType == SQL_SMALLINT
                || inType == SQL_INTEGER || inType == SQL_BIGINT;
        }

        template <typename T>
        std::optional<T> ReadNumber(SQLHSTMT inStatement, std::size_t inColumn, SQLSMALLINT inType)
        {
            T value{};
            SQLLEN indicator{};
            Check(SQLGetData(inStatement, static_cast<SQLUSMALLINT>(inColumn), inType,
                &value, sizeof(value), &indicator), SQL_HANDLE_STMT, inStatement, "Unable to read numeric result.");
            if (indicator == SQL_NULL_DATA) return std::nullopt;
            if (indicator < 0) Fail(DatabaseErrorCode::InvalidResult, "Invalid numeric result length.");
            return value;
        }

        /// Validate procedure identifiers before constructing CALL; values always use parameter markers.
        std::wstring MakeCall(std::wstring_view inName, std::size_t inParameterCount)
        {
            if (inName.empty() || inName.size() > 128 || inParameterCount > MAX_PARAMETER_COUNT)
                Fail(DatabaseErrorCode::InvalidProcedure, "Invalid procedure name or parameter count.");
            bool atStart = true;
            for (wchar_t character : inName)
            {
                if (character == L'.' && !atStart) { atStart = true; continue; }
                const bool letter = (character >= L'A' && character <= L'Z')
                    || (character >= L'a' && character <= L'z') || character == L'_';
                if (!letter && (atStart || character < L'0' || character > L'9'))
                    Fail(DatabaseErrorCode::InvalidProcedure, "Procedure name must use unquoted identifiers.");
                atStart = false;
            }
            if (atStart) Fail(DatabaseErrorCode::InvalidProcedure, "Invalid procedure qualifier.");
            std::wstring call = L"{CALL " + std::wstring(inName) + L"(";
            for (std::size_t index = 0; index < inParameterCount; ++index)
            {
                if (index != 0) call += L',';
                call += L'?';
            }
            return call + L")}";
        }
    }

    DatabaseOptions DatabaseOptions::FromEnvironment()
    {
        DatabaseOptions options;
        constexpr const wchar_t* VARIABLE = L"ACTIONRPG_DB_CONNECTION_STRING";
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(VARIABLE, nullptr, 0);
        if (length == 0)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_SUCCESS && error != ERROR_ENVVAR_NOT_FOUND)
                Fail(DatabaseErrorCode::NotConfigured, "Unable to read DB connection environment variable.");
            return options;
        }
        if (length > 32767)
            Fail(DatabaseErrorCode::NotConfigured, "DB connection environment variable is too long.");
        std::wstring value(length, L'\0');
        const DWORD copied = GetEnvironmentVariableW(VARIABLE, value.data(), length);
        if (copied == 0 || copied >= length)
            Fail(DatabaseErrorCode::NotConfigured, "DB connection environment variable changed while reading.");
        value.resize(copied);
        options.connectionString = std::move(value);
        return options;
    }

    struct ProcedureConnection::Impl
    {
        Impl(SQLHENV inEnvironment, const DatabaseOptions& inOptions)
            : environment(inEnvironment), options(inOptions) {}

        void EnsureConnected()
        {
            if (connection.value != SQL_NULL_HDBC) return;
            Allocate(connection, environment);
            Check(SQLSetConnectAttrW(connection.value, SQL_ATTR_LOGIN_TIMEOUT,
                AttributeValue(options.connectionTimeoutSeconds), 0), SQL_HANDLE_DBC, connection.value,
                "Unable to configure ODBC login timeout.");
            Check(SQLDriverConnectW(connection.value, nullptr,
                reinterpret_cast<SQLWCHAR*>(options.connectionString.data()), SQL_NTS,
                nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT), SQL_HANDLE_DBC, connection.value,
                "Unable to connect through ODBC.");
            SQLUINTEGER timeout{};
            // MySQL supports login timeout; SQL_ATTR_CONNECTION_TIMEOUT is ignored and always reads as zero.
            Check(SQLGetConnectAttrW(connection.value, SQL_ATTR_LOGIN_TIMEOUT, &timeout,
                sizeof(timeout), nullptr), SQL_HANDLE_DBC, connection.value, "Unable to verify ODBC login timeout.");
            if (timeout != options.connectionTimeoutSeconds)
                Fail(DatabaseErrorCode::DriverError, "ODBC driver substituted the login timeout.");
            SQLUSMALLINT transactionCapability{};
            Check(SQLGetInfoW(connection.value, SQL_TXN_CAPABLE, &transactionCapability,
                sizeof(transactionCapability), nullptr), SQL_HANDLE_DBC, connection.value,
                "Unable to inspect transaction support.");
            if (transactionCapability == SQL_TC_NONE)
                Fail(DatabaseErrorCode::DriverError, "Procedure execution requires DB transaction support.");
        }

        SQLHENV environment;
        DatabaseOptions options;
        OdbcHandle connection{ SQL_HANDLE_DBC };
    };

    ProcedureConnection::ProcedureConnection(void* inEnvironment, const DatabaseOptions& inOptions)
        : impl(std::make_unique<Impl>(inEnvironment, inOptions)) {}
    ProcedureConnection::~ProcedureConnection() = default;

    /** Keep one lease/transaction for the complete CALL, including all result sets and output values.
     * Runtime procedures must not commit/fully roll back the owning transaction, alter the session,
     * or execute DDL. A procedure may use a savepoint and partial rollback for an expected conflict.
     */
    void ProcedureConnection::Execute(std::wstring_view inName, ProcedureParameters& inParameters,
        const std::function<void(ProcedureRow&, std::size_t)>& inReadRow,
        const std::function<void(std::size_t, std::size_t)>& inValidateResults)
    {
        bool executionAttempted = false;
        bool transactionStarted = false;
        try
        {
            std::wstring call = MakeCall(inName, inParameters.parameters.size());
            impl->EnsureConnected();
            const SQLHDBC connection = impl->connection.value;
            std::vector<SQLLEN> indicators(inParameters.parameters.size());
            OdbcHandle statement{ SQL_HANDLE_STMT };
            Allocate(statement, connection);
            Check(SQLSetStmtAttrW(statement.value, SQL_ATTR_QUERY_TIMEOUT,
                AttributeValue(impl->options.queryTimeoutSeconds), 0), SQL_HANDLE_STMT, statement.value,
                "ODBC driver must support query timeout.");
            SQLULEN timeout{};
            Check(SQLGetStmtAttrW(statement.value, SQL_ATTR_QUERY_TIMEOUT, &timeout,
                sizeof(timeout), nullptr), SQL_HANDLE_STMT, statement.value, "Unable to verify query timeout.");
            if (timeout != impl->options.queryTimeoutSeconds)
                Fail(DatabaseErrorCode::DriverError, "ODBC driver substituted the query timeout.");
            Check(SQLPrepareW(statement.value, reinterpret_cast<SQLWCHAR*>(call.data()), SQL_NTS),
                SQL_HANDLE_STMT, statement.value, "Unable to prepare procedure call.");
            SQLSMALLINT parameterCount{};
            Check(SQLNumParams(statement.value, &parameterCount), SQL_HANDLE_STMT, statement.value,
                "Unable to inspect procedure parameters.");
            if (parameterCount < 0 || static_cast<std::size_t>(parameterCount) != inParameters.parameters.size())
                Fail(DatabaseErrorCode::InvalidProcedure, "Procedure parameter count does not match bindings.");

            // Final-sized indicators and parameter storage stay stationary until the statement is destroyed.
            for (std::size_t index = 0; index < inParameters.parameters.size(); ++index)
            {
                auto& parameter = inParameters.parameters[index];
                const bool inputOnly = parameter.direction == ProcedureParameters::Direction::Input;
                SQLSMALLINT direction = SQL_PARAM_INPUT;
                if (parameter.direction == ProcedureParameters::Direction::Output) direction = SQL_PARAM_OUTPUT;
                else if (parameter.direction == ProcedureParameters::Direction::InputOutput) direction = SQL_PARAM_INPUT_OUTPUT;
                std::visit([&](auto& value)
                {
                    using T = std::decay_t<decltype(value)>;
                    SQLSMALLINT cType{};
                    SQLSMALLINT sqlType{};
                    SQLULEN columnSize{};
                    SQLPOINTER buffer = &value;
                    SQLLEN bufferLength = sizeof(T);
                    indicators[index] = parameter.isNull ? SQL_NULL_DATA : sizeof(T);
                    if constexpr (std::is_same_v<T, std::int32_t>)
                    { cType = SQL_C_SLONG; sqlType = SQL_INTEGER; columnSize = 10; }
                    else if constexpr (std::is_same_v<T, std::uint32_t>)
                    { cType = SQL_C_ULONG; sqlType = SQL_BIGINT; columnSize = 19; }
                    else if constexpr (std::is_same_v<T, std::int64_t>)
                    { cType = SQL_C_SBIGINT; sqlType = SQL_BIGINT; columnSize = 19; }
                    else if constexpr (std::is_same_v<T, std::uint64_t>)
                    { cType = SQL_C_UBIGINT; sqlType = SQL_NUMERIC; columnSize = 20; }
                    else if constexpr (std::is_same_v<T, double>)
                    { cType = SQL_C_DOUBLE; sqlType = SQL_DOUBLE; columnSize = 15; }
                    else if constexpr (std::is_same_v<T, bool>)
                    { static_assert(sizeof(bool) == sizeof(SQLCHAR)); cType = SQL_C_BIT; sqlType = SQL_BIT; columnSize = 1; }
                    else
                    {
                        static_assert(sizeof(wchar_t) == sizeof(SQLWCHAR));
                        const std::size_t inputLength = value.size();
                        if (inputLength > MAX_STRING_CHARACTERS || (!inputOnly
                            && (parameter.stringCapacity == 0 || parameter.stringCapacity > MAX_STRING_CHARACTERS
                                || inputLength > parameter.stringCapacity)))
                            Fail(DatabaseErrorCode::InvalidProcedure, "Procedure string exceeds its binding capacity.");
                        if (!inputOnly) value.resize(parameter.stringCapacity + 1, L'\0');
                        cType = SQL_C_WCHAR;
                        sqlType = SQL_WVARCHAR;
                        columnSize = std::max<std::size_t>(1, inputOnly ? inputLength : parameter.stringCapacity);
                        buffer = value.data();
                        bufferLength = static_cast<SQLLEN>((inputOnly ? inputLength + 1 : value.size()) * sizeof(wchar_t));
                        indicators[index] = parameter.isNull ? SQL_NULL_DATA : static_cast<SQLLEN>(inputLength * sizeof(wchar_t));
                    }
                    Check(SQLBindParameter(statement.value, static_cast<SQLUSMALLINT>(index + 1), direction,
                        cType, sqlType, columnSize, 0, buffer, bufferLength, &indicators[index]),
                        SQL_HANDLE_STMT, statement.value, "Unable to bind procedure parameter.");
                }, parameter.value);
            }

            Check(SQLSetConnectAttrW(connection, SQL_ATTR_AUTOCOMMIT, AttributeValue(SQL_AUTOCOMMIT_OFF), 0),
                SQL_HANDLE_DBC, connection, "Unable to start procedure transaction.");
            transactionStarted = true;
            executionAttempted = true;
            const SQLRETURN execution = SQLExecute(statement.value);
            if (execution != SQL_NO_DATA)
                Check(execution, SQL_HANDLE_STMT, statement.value, "Unable to execute procedure.");
            std::size_t resultIndex = 0;
            std::size_t rowCount = 0;
            std::size_t resultTransitions = 0;
            std::size_t remainingBytes = MAX_RESULT_BYTES;
            while (true)
            {
                SQLSMALLINT columns{};
                Check(SQLNumResultCols(statement.value, &columns), SQL_HANDLE_STMT, statement.value,
                    "Unable to inspect procedure result.");
                if (columns > 0)
                {
                    if (resultIndex >= MAX_RESULT_SETS)
                        Fail(DatabaseErrorCode::InvalidResult, "Procedure returned too many result sets.");
                    while (true)
                    {
                        const SQLRETURN fetched = SQLFetch(statement.value);
                        if (fetched == SQL_NO_DATA) break;
                        Check(fetched, SQL_HANDLE_STMT, statement.value, "Unable to fetch procedure row.");
                        if (++rowCount > MAX_RESULT_ROWS)
                            Fail(DatabaseErrorCode::InvalidResult, "Procedure returned too many rows.");
                        ProcedureRow row(statement.value, static_cast<std::size_t>(columns), remainingBytes);
                        inReadRow(row, resultIndex);
                    }
                    ++resultIndex;
                }
                const SQLRETURN more = SQLMoreResults(statement.value);
                if (more == SQL_NO_DATA) break;
                Check(more, SQL_HANDLE_STMT, statement.value, "Unable to finish procedure results.");
                if (++resultTransitions > MAX_RESULT_TRANSITIONS)
                    Fail(DatabaseErrorCode::InvalidResult, "Procedure returned too many result transitions.");
            }

            statement.Reset();
            for (std::size_t index = 0; index < inParameters.parameters.size(); ++index)
            {
                auto& parameter = inParameters.parameters[index];
                if (!parameter.assignOutput) continue;
                const SQLLEN indicator = indicators[index];
                if (indicator != SQL_NULL_DATA)
                {
                    if (indicator < 0) Fail(DatabaseErrorCode::InvalidResult, "Unknown output parameter length.");
                    if (auto* value = std::get_if<std::wstring>(&parameter.value))
                    {
                        if (indicator % sizeof(wchar_t) != 0
                            || static_cast<std::size_t>(indicator) / sizeof(wchar_t) > parameter.stringCapacity)
                            Fail(DatabaseErrorCode::InvalidResult, "Output parameter was truncated.");
                        if (static_cast<std::size_t>(indicator) > remainingBytes)
                            Fail(DatabaseErrorCode::InvalidResult, "Procedure response exceeds the byte limit.");
                        remainingBytes -= static_cast<std::size_t>(indicator);
                        value->resize(static_cast<std::size_t>(indicator) / sizeof(wchar_t));
                    }
                }
                parameter.assignOutput(parameter.value, indicator == SQL_NULL_DATA);
            }
            inValidateResults(resultIndex, rowCount);
            Check(SQLEndTran(SQL_HANDLE_DBC, connection, SQL_COMMIT), SQL_HANDLE_DBC, connection,
                "Unable to confirm procedure commit.");
            transactionStarted = false;
            Check(SQLSetConnectAttrW(connection, SQL_ATTR_AUTOCOMMIT, AttributeValue(SQL_AUTOCOMMIT_ON), 0),
                SQL_HANDLE_DBC, connection, "Unable to restore connection after commit.");
        }
        catch (const DatabaseException& inException)
        {
            if (transactionStarted) SQLEndTran(SQL_HANDLE_DBC, impl->connection.value, SQL_ROLLBACK);
            impl->connection.Reset();
            auto error = inException.error;
            error.executionMayHaveOccurred = executionAttempted;
            throw DatabaseException(std::move(error));
        }
        catch (...)
        {
            if (transactionStarted) SQLEndTran(SQL_HANDLE_DBC, impl->connection.value, SQL_ROLLBACK);
            impl->connection.Reset();
            throw DatabaseException(DatabaseError{ DatabaseErrorCode::InvalidResult,
                "Procedure result mapping failed.", {}, 0, executionAttempted });
        }
    }

    void ProcedureRow::ValidateColumn(std::size_t inColumn, bool inString, bool inIntegral)
    {
        if (inColumn <= lastColumn || inColumn > columnCount || inColumn > std::numeric_limits<SQLUSMALLINT>::max())
            Fail(DatabaseErrorCode::InvalidResult, "Read result columns once, in increasing one-based order.");
        SQLSMALLINT dataType{}, scale{}, nullable{};
        SQLULEN size{};
        Check(SQLDescribeColW(statement, static_cast<SQLUSMALLINT>(inColumn), nullptr, 0, nullptr,
            &dataType, &size, &scale, &nullable), SQL_HANDLE_STMT, statement, "Unable to inspect result column.");
        const bool text = dataType == SQL_CHAR || dataType == SQL_VARCHAR || dataType == SQL_LONGVARCHAR
            || dataType == SQL_WCHAR || dataType == SQL_WVARCHAR || dataType == SQL_WLONGVARCHAR;
        const bool decimal = dataType == SQL_DECIMAL || dataType == SQL_NUMERIC;
        const bool numeric = IsIntegerType(dataType) || decimal || dataType == SQL_REAL
            || dataType == SQL_FLOAT || dataType == SQL_DOUBLE;
        if ((inString && !text) || (!inString && (!numeric
            || (inIntegral && !IsIntegerType(dataType) && !(decimal && scale == 0)))))
            throw DatabaseException({ DatabaseErrorCode::InvalidResult,
                "Procedure result column type does not match response; ODBC_type=" + std::to_string(dataType)
                    + "; expected=" + (inString ? "text" : inIntegral ? "integral" : "numeric") });
        lastColumn = inColumn;
    }

    std::optional<std::int32_t> ProcedureRow::ReadInt32(std::size_t inColumn)
    {
        ValidateColumn(inColumn, false, true);
        ConsumeBytes(sizeof(std::int32_t));
        return ReadNumber<std::int32_t>(statement, inColumn, SQL_C_SLONG);
    }
    std::optional<std::uint32_t> ProcedureRow::ReadUInt32(std::size_t inColumn)
    {
        ValidateColumn(inColumn, false, true);
        ConsumeBytes(sizeof(std::uint32_t));
        return ReadNumber<std::uint32_t>(statement, inColumn, SQL_C_ULONG);
    }
    std::optional<std::int64_t> ProcedureRow::ReadInt64(std::size_t inColumn)
    {
        ValidateColumn(inColumn, false, true);
        ConsumeBytes(sizeof(std::int64_t));
        return ReadNumber<std::int64_t>(statement, inColumn, SQL_C_SBIGINT);
    }
    std::optional<std::uint64_t> ProcedureRow::ReadUInt64(std::size_t inColumn)
    {
        ValidateColumn(inColumn, false, true);
        ConsumeBytes(sizeof(std::uint64_t));
        return ReadNumber<std::uint64_t>(statement, inColumn, SQL_C_UBIGINT);
    }
    std::optional<double> ProcedureRow::ReadDouble(std::size_t inColumn)
    {
        ValidateColumn(inColumn, false, false);
        ConsumeBytes(sizeof(double));
        return ReadNumber<double>(statement, inColumn, SQL_C_DOUBLE);
    }
    std::optional<bool> ProcedureRow::ReadBool(std::size_t inColumn)
    {
        ValidateColumn(inColumn, false, true);
        ConsumeBytes(sizeof(bool));
        const auto value = ReadNumber<SQLCHAR>(statement, inColumn, SQL_C_UTINYINT);
        if (!value) return std::nullopt;
        if (*value > 1) Fail(DatabaseErrorCode::InvalidResult, "Boolean result must be zero or one.");
        return *value != 0;
    }
    std::optional<std::wstring> ProcedureRow::ReadString(std::size_t inColumn)
    {
        ValidateColumn(inColumn, true, false);
        std::wstring value(MAX_STRING_CHARACTERS + 1, L'\0');
        SQLLEN indicator{};
        Check(SQLGetData(statement, static_cast<SQLUSMALLINT>(inColumn), SQL_C_WCHAR, value.data(),
            static_cast<SQLLEN>(value.size() * sizeof(wchar_t)), &indicator), SQL_HANDLE_STMT, statement,
            "Unable to read string result, or result exceeds the string limit.");
        if (indicator == SQL_NULL_DATA) return std::nullopt;
        if (indicator < 0 || indicator % sizeof(wchar_t) != 0
            || static_cast<std::size_t>(indicator) / sizeof(wchar_t) > MAX_STRING_CHARACTERS)
            Fail(DatabaseErrorCode::InvalidResult, "Invalid or truncated string result.");
        value.resize(static_cast<std::size_t>(indicator) / sizeof(wchar_t));
        ConsumeBytes(static_cast<std::size_t>(indicator));
        return value;
    }

    void ProcedureRow::ConsumeBytes(std::size_t inBytes)
    {
        if (inBytes > remainingBytes)
            Fail(DatabaseErrorCode::InvalidResult, "Procedure response exceeds the byte limit.");
        remainingBytes -= inBytes;
    }

    struct OdbcDatabase::Impl
    {
        explicit Impl(DatabaseOptions inOptions) : options(std::move(inOptions))
        {
            if (options.connectionCount == 0 || options.connectionCount > MAX_CONNECTION_COUNT
                || options.maxPendingRequests == 0 || options.maxPendingRequests > MAX_PENDING_REQUESTS
                || options.connectionTimeoutSeconds == 0 || options.queryTimeoutSeconds == 0 || options.queueTimeoutSeconds == 0
                || options.connectionString.size() > 32766 || options.connectionString.find(L'\0') != std::wstring::npos)
                Fail(DatabaseErrorCode::NotConfigured, "Invalid DB connection or execution limits.");
            if (options.connectionString.empty()) return;
            Allocate(environment, SQL_NULL_HANDLE);
            Check(SQLSetEnvAttr(environment.value, SQL_ATTR_ODBC_VERSION, AttributeValue(SQL_OV_ODBC3), 0),
                SQL_HANDLE_ENV, environment.value, "Unable to configure ODBC environment.");
            workers.reserve(options.connectionCount);
            for (std::size_t index = 0; index < options.connectionCount; ++index)
                workers.emplace_back([this](std::stop_token inStopToken) { Worker(inStopToken); });
        }

        void Worker(std::stop_token inStopToken)
        {
            // One persistent connection per worker: a bounded pool with exclusive, stable handle ownership.
            std::unique_ptr<ProcedureConnection> connection;
            while (true)
            {
                Work work;
                std::chrono::steady_clock::time_point expires;
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, inStopToken, [&] { return stopping || !queue.empty(); });
                    if (queue.empty()) return;
                    expires = queue.front().expires;
                    work = std::move(queue.front().work);
                    queue.pop_front();
                }
                std::optional<DatabaseError> error;
                if (std::chrono::steady_clock::now() >= expires)
                {
                    work(nullptr, DatabaseError{ DatabaseErrorCode::QueueTimeout, "DB request expired before execution." });
                    continue;
                }
                try
                {
                    if (!connection) connection = std::make_unique<ProcedureConnection>(environment.value, options);
                }
                catch (const DatabaseException& inException) { error = inException.error; }
                catch (...)
                {
                    error = DatabaseError{ DatabaseErrorCode::InternalError, "Unable to initialize DB worker connection." };
                }
                work(connection.get(), std::move(error));
            }
        }

        DatabaseOptions options;
        OdbcHandle environment{ SQL_HANDLE_ENV };
        std::mutex mutex;
        std::condition_variable_any ready;
        struct PendingWork
        {
            Work work;
            std::chrono::steady_clock::time_point expires;
        };
        std::deque<PendingWork> queue;
        bool stopping{};
        // Destroy/join workers before queue, mutex, environment and connection options.
        std::vector<std::jthread> workers;
    };

    OdbcDatabase::OdbcDatabase(DatabaseOptions inOptions) : impl(std::make_unique<Impl>(std::move(inOptions))) {}
    OdbcDatabase::~OdbcDatabase()
    {
        Stop();
        impl->workers.clear();
    }
    bool OdbcDatabase::IsConfigured() const noexcept { return !impl->options.connectionString.empty(); }
    void OdbcDatabase::Stop()
    {
        {
            std::lock_guard lock(impl->mutex);
            impl->stopping = true;
        }
        impl->ready.notify_all();
    }
    void OdbcDatabase::Enqueue(Work inWork)
    {
        std::optional<DatabaseError> error;
        {
            std::lock_guard lock(impl->mutex);
            if (impl->stopping) error = DatabaseError{ DatabaseErrorCode::Stopped, "DB service is stopping." };
            else if (!IsConfigured()) error = DatabaseError{ DatabaseErrorCode::NotConfigured, "DB connection is not configured." };
            else if (impl->queue.size() >= impl->options.maxPendingRequests)
                error = DatabaseError{ DatabaseErrorCode::QueueFull, "DB request queue is full." };
            else impl->queue.push_back({ std::move(inWork), std::chrono::steady_clock::now()
                + std::chrono::seconds(impl->options.queueTimeoutSeconds) });
        }
        if (error) inWork(nullptr, std::move(error));
        else impl->ready.notify_one();
    }
}
