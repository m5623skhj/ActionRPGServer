#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace TownServer::Database
{
    inline constexpr std::size_t MAX_STRING_CHARACTERS = 32768;

    enum class DatabaseErrorCode
    {
        NotConfigured, Stopped, QueueFull, QueueTimeout, InvalidProcedure, DriverError, InvalidResult, InternalError
    };

    struct DatabaseError
    {
        DatabaseErrorCode code = DatabaseErrorCode::InternalError;
        std::string message;
        std::string sqlState;
        std::int32_t nativeCode{};
        // A failed/absent response does not prove that the DB rejected the operation. Never retry blindly.
        bool executionMayHaveOccurred{};
    };

    class DatabaseException final : public std::runtime_error
    {
    public:
        explicit DatabaseException(DatabaseError inError)
            : std::runtime_error(inError.message), error(std::move(inError)) {}

        DatabaseError error;
    };

    template <typename T>
    inline constexpr bool IS_DATABASE_VALUE = std::is_same_v<T, std::int32_t>
        || std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::int64_t>
        || std::is_same_v<T, std::uint64_t> || std::is_same_v<T, double>
        || std::is_same_v<T, bool> || std::is_same_v<T, std::wstring>;

    class ProcedureConnection;

    /// Collect bindings before execution; buffers are owned here and never moved while bound to ODBC.
    class ProcedureParameters final
    {
    public:
        template <typename T> requires IS_DATABASE_VALUE<T>
        void AddInput(const T& inValue)
        {
            AddInput(std::optional<T>(inValue));
        }

        template <typename T> requires IS_DATABASE_VALUE<T>
        void AddInput(const std::optional<T>& inValue)
        {
            Parameter parameter;
            parameter.value = inValue.value_or(T{});
            parameter.isNull = !inValue.has_value();
            parameters.push_back(std::move(parameter));
        }

        template <typename T> requires IS_DATABASE_VALUE<T>
        void AddOutput(std::optional<T>& outValue, std::size_t inStringCapacity = MAX_STRING_CHARACTERS)
        {
            AddOutputParameter(outValue, Direction::Output, inStringCapacity);
        }

        template <typename T> requires IS_DATABASE_VALUE<T>
        void AddInputOutput(std::optional<T>& inOutValue, std::size_t inStringCapacity = MAX_STRING_CHARACTERS)
        {
            AddOutputParameter(inOutValue, Direction::InputOutput, inStringCapacity);
        }

        ProcedureParameters() = default;
        ProcedureParameters(const ProcedureParameters&) = delete;
        ProcedureParameters& operator=(const ProcedureParameters&) = delete;
        ProcedureParameters(ProcedureParameters&&) = delete;
        ProcedureParameters& operator=(ProcedureParameters&&) = delete;

    private:
        using Value = std::variant<std::int32_t, std::uint32_t, std::int64_t,
            std::uint64_t, double, bool, std::wstring>;
        enum class Direction { Input, Output, InputOutput };
        struct Parameter
        {
            Value value;
            Direction direction = Direction::Input;
            bool isNull{};
            std::size_t stringCapacity = MAX_STRING_CHARACTERS;
            std::function<void(const Value&, bool)> assignOutput;
        };

        template <typename T>
        void AddOutputParameter(std::optional<T>& inOutValue, Direction inDirection, std::size_t inStringCapacity)
        {
            Parameter parameter;
            parameter.value = inDirection == Direction::InputOutput ? inOutValue.value_or(T{}) : T{};
            parameter.isNull = inDirection == Direction::InputOutput && !inOutValue.has_value();
            parameter.direction = inDirection;
            parameter.stringCapacity = inStringCapacity;
            parameter.assignOutput = [&inOutValue](const Value& inValue, bool inIsNull)
            {
                if (inIsNull) inOutValue.reset();
                else inOutValue = std::get<T>(inValue);
            };
            parameters.push_back(std::move(parameter));
        }

        friend class ProcedureConnection;
        std::vector<Parameter> parameters;
    };

    /// Read columns once, in increasing one-based order. NULL is represented by std::optional.
    class ProcedureRow final
    {
    public:
        template <typename T> requires IS_DATABASE_VALUE<T>
        [[nodiscard]] std::optional<T> Read(std::size_t inColumn)
        {
            if constexpr (std::is_same_v<T, std::int32_t>) return ReadInt32(inColumn);
            else if constexpr (std::is_same_v<T, std::uint32_t>) return ReadUInt32(inColumn);
            else if constexpr (std::is_same_v<T, std::int64_t>) return ReadInt64(inColumn);
            else if constexpr (std::is_same_v<T, std::uint64_t>) return ReadUInt64(inColumn);
            else if constexpr (std::is_same_v<T, double>) return ReadDouble(inColumn);
            else if constexpr (std::is_same_v<T, bool>) return ReadBool(inColumn);
            else return ReadString(inColumn);
        }

        [[nodiscard]] std::size_t GetColumnCount() const noexcept { return columnCount; }
        ProcedureRow(const ProcedureRow&) = delete;
        ProcedureRow& operator=(const ProcedureRow&) = delete;

    private:
        friend class ProcedureConnection;
        ProcedureRow(void* inStatement, std::size_t inColumnCount, std::size_t& inRemainingBytes)
            : statement(inStatement), columnCount(inColumnCount), remainingBytes(inRemainingBytes) {}
        void ValidateColumn(std::size_t inColumn, bool inString, bool inIntegral);
        void ConsumeBytes(std::size_t inBytes);
        std::optional<std::int32_t> ReadInt32(std::size_t inColumn);
        std::optional<std::uint32_t> ReadUInt32(std::size_t inColumn);
        std::optional<std::int64_t> ReadInt64(std::size_t inColumn);
        std::optional<std::uint64_t> ReadUInt64(std::size_t inColumn);
        std::optional<double> ReadDouble(std::size_t inColumn);
        std::optional<bool> ReadBool(std::size_t inColumn);
        std::optional<std::wstring> ReadString(std::size_t inColumn);
        void* statement;
        std::size_t columnCount;
        std::size_t lastColumn{};
        std::size_t& remainingBytes;
    };

    template <typename TReq, typename TRes>
    class IStoreProcedure
    {
    public:
        using Request = TReq;
        using Response = TRes;
        virtual ~IStoreProcedure() = default;
        IStoreProcedure() = default;
        IStoreProcedure(const IStoreProcedure&) = delete;
        IStoreProcedure& operator=(const IStoreProcedure&) = delete;

        TReq req{};
        [[nodiscard]] virtual std::wstring_view GetName() const noexcept = 0;
        virtual void BindParameters(ProcedureParameters& inParameters, TRes& outResponse) const = 0;
        // Called on the DB worker, for each row; result index counts only result sets with columns.
        virtual void ReadRow(ProcedureRow& inRow, std::size_t inResultIndex, TRes& outResponse) const = 0;
        // Optional strict shape check, including empty result sets; runs before the transaction commits.
        virtual void ValidateResults(std::size_t, std::size_t, const TRes&) const {}
    };

    template <typename TRes>
    struct ProcedureResult
    {
        std::optional<TRes> response;
        std::optional<DatabaseError> error;
        [[nodiscard]] bool IsSuccess() const noexcept { return response.has_value() && !error.has_value(); }
    };
}
