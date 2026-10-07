#include "LoginSchemaVerifier.h"
#include "SchemaHistoryProcedure.h"

#include <Windows.h>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

namespace ActionRPG::Database
{
    namespace
    {
        struct Migration
        {
            std::wstring name;
            std::wstring upSql;
            std::wstring upChecksum;
            std::optional<std::wstring> downChecksum;
        };
        using Deployment = std::array<Migration, 4>;

        [[noreturn]] void InvalidSchema()
        {
            throw std::runtime_error("Account database schema verification failed.");
        }

        const std::wstring& Required(const SchemaHistoryRow& inRow, std::size_t inIndex)
        {
            if (inIndex >= inRow.size() || !inRow[inIndex]) InvalidSchema();
            return *inRow[inIndex];
        }

        std::wstring Environment(const wchar_t* inName)
        {
            const DWORD length = GetEnvironmentVariableW(inName, nullptr, 0);
            if (length == 0 || length > 32767) InvalidSchema();
            std::wstring value(length, L'\0');
            const DWORD copied = GetEnvironmentVariableW(inName, value.data(), length);
            if (copied == 0 || copied >= length) InvalidSchema();
            value.resize(copied);
            return value;
        }

        std::wstring ReadSql(const std::filesystem::path& inPath)
        {
            std::ifstream input(inPath, std::ios::binary | std::ios::ate);
            const auto size = input.tellg();
            if (!input || size <= 0 || size > 1024 * 1024) InvalidSchema();
            std::string bytes(static_cast<std::size_t>(size), '\0');
            input.seekg(0);
            if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) InvalidSchema();
            const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                static_cast<int>(bytes.size()), nullptr, 0);
            if (count <= 0) InvalidSchema();
            std::wstring decoded(static_cast<std::size_t>(count), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
                decoded.data(), count) != count) InvalidSchema();
            std::wstring normalized;
            normalized.reserve(decoded.size());
            const std::size_t first = decoded.front() == L'\xfeff' ? 1 : 0;
            for (std::size_t index = first; index < decoded.size(); ++index)
            {
                if (decoded[index] == L'\0') InvalidSchema();
                if (decoded[index] == L'\r')
                {
                    normalized += L'\n';
                    if (index + 1 < decoded.size() && decoded[index + 1] == L'\n') ++index;
                }
                else normalized += decoded[index];
            }
            return normalized;
        }

        std::wstring Checksum(const std::wstring& inSql)
        {
            const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, inSql.data(),
                static_cast<int>(inSql.size()), nullptr, 0, nullptr, nullptr);
            if (count <= 0) InvalidSchema();
            std::string bytes(static_cast<std::size_t>(count), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, inSql.data(), static_cast<int>(inSql.size()),
                bytes.data(), count, nullptr, nullptr) != count) InvalidSchema();
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned int length = 0;
            if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1
                || length != 32) InvalidSchema();
            constexpr wchar_t HEX[] = L"0123456789abcdef";
            std::wstring result;
            for (unsigned int index = 0; index < length; ++index)
            { result += HEX[digest[index] >> 4]; result += HEX[digest[index] & 15]; }
            return result;
        }

        Deployment LoadDeployment()
        {
            const std::filesystem::path directory(Environment(L"ACTIONRPG_DB_MIGRATIONS_DIRECTORY"));
            if (!directory.is_absolute() || !std::filesystem::is_directory(directory)) InvalidSchema();
            Deployment deployment;
            const std::array<std::wstring, 4> names{L"migration_history", L"create_login_accounts",
                L"create_google_login_procedure", L"create_auth_account_status_procedure"};
            for (std::size_t version = 0; version < deployment.size(); ++version)
            {
                auto& migration = deployment[version];
                migration.name = names[version];
                const auto filename = L"V00000" + std::to_wstring(version) + L"__" + migration.name + L".sql";
                migration.upSql = ReadSql(version == 0 ? directory / L"Infrastructure" / filename : directory / filename);
                migration.upChecksum = Checksum(migration.upSql);
                if (version != 0) migration.downChecksum = Checksum(ReadSql(directory / L"Down" / filename));
            }
            return deployment;
        }

        std::uint64_t Number(const std::wstring& inValue)
        {
            if (inValue.empty()) InvalidSchema();
            std::uint64_t result = 0;
            for (const auto value : inValue)
            {
                if (value < L'0' || value > L'9'
                    || result > (std::numeric_limits<std::uint64_t>::max() - (value - L'0')) / 10) InvalidSchema();
                result = result * 10 + (value - L'0');
            }
            if (std::to_wstring(result) != inValue) InvalidSchema();
            return result;
        }

        void ValidateTime(const std::wstring& inValue)
        {
            if (inValue.size() != 27 || inValue[4] != L'-' || inValue[7] != L'-' || inValue[10] != L'T'
                || inValue[13] != L':' || inValue[16] != L':' || inValue[19] != L'.' || inValue[26] != L'Z') InvalidSchema();
            for (std::size_t index = 0; index < inValue.size(); ++index)
                if (index != 4 && index != 7 && index != 10 && index != 13 && index != 16 && index != 19 && index != 26
                    && (inValue[index] < L'0' || inValue[index] > L'9')) InvalidSchema();
            const auto component = [&](std::size_t offset, std::size_t length)
            { return std::stoi(inValue.substr(offset, length)); };
            const std::chrono::year_month_day day{std::chrono::year(component(0, 4)),
                std::chrono::month(static_cast<unsigned>(component(5, 2))),
                std::chrono::day(static_cast<unsigned>(component(8, 2)))};
            if (!day.ok() || component(0, 4) == 0 || component(11, 2) > 23
                || component(14, 2) > 59 || component(17, 2) > 59) InvalidSchema();
        }

        void ValidateHistory(const SchemaHistoryResponse& inResponse, const Deployment& inDeployment)
        {
            std::uint64_t lastExecution = 0;
            std::uint64_t head = 0;
            bool bootstrapped = false;
            std::optional<std::wstring> bootstrapFailureFinished;
            std::optional<std::wstring> accountsFailureFinished;
            bool hasApplicationHistory = false;
            for (const auto& row : inResponse.resultSets[1])
            {
                const auto execution = Number(Required(row, 0));
                const auto version = Number(Required(row, 1));
                if (execution <= lastExecution || version >= inDeployment.size()) InvalidSchema();
                lastExecution = execution;
                const auto& migration = inDeployment[version];
                if (Required(row, 4) != migration.upChecksum || row[5] != migration.downChecksum) InvalidSchema();
                const auto& started = Required(row, 7);
                const auto& finished = Required(row, 8);
                ValidateTime(started);
                ValidateTime(finished);
                if (finished < started) InvalidSchema();
                const auto& direction = Required(row, 3);
                const auto& state = Required(row, 6);
                // Initial V0/V1 failures require an immediate, checksum-bound
                // confirmation. Later failures and all unfinished attempts block.
                if (bootstrapFailureFinished)
                {
                    if (version != 0 || Required(row, 2) != L"migration_history_recovery" || direction != L"UP"
                        || state != L"SUCCEEDED" || started < *bootstrapFailureFinished) InvalidSchema();
                    bootstrapFailureFinished.reset();
                    bootstrapped = true;
                    continue;
                }
                if (accountsFailureFinished)
                {
                    if (version != 1 || Required(row, 2) != L"create_login_accounts_recovery" || direction != L"UP"
                        || state != L"SUCCEEDED" || started < *accountsFailureFinished) InvalidSchema();
                    accountsFailureFinished.reset();
                    hasApplicationHistory = true;
                    head = 1;
                    continue;
                }
                if (Required(row, 2) != migration.name) InvalidSchema();
                if (!bootstrapped)
                {
                    if (version != 0 || direction != L"UP") InvalidSchema();
                    if (state == L"FAILED")
                    {
                        bootstrapFailureFinished = finished;
                        continue;
                    }
                    if (state != L"SUCCEEDED") InvalidSchema();
                    bootstrapped = true;
                }
                else
                {
                    if (!hasApplicationHistory && head == 0 && version == 1 && direction == L"UP" && state == L"FAILED")
                    {
                        accountsFailureFinished = finished;
                        continue;
                    }
                    if (state != L"SUCCEEDED") InvalidSchema();
                    hasApplicationHistory = true;
                    if (direction == L"UP" && version == head + 1) head = version;
                    else if (direction == L"DOWN" && version == head && head > 0) --head;
                    else InvalidSchema();
                }
            }
            if (!bootstrapped || bootstrapFailureFinished || accountsFailureFinished
                || head != VerifiedLoginSchema::REQUIRED_SCHEMA_VERSION) InvalidSchema();
        }

        std::wstring Lower(std::wstring inText)
        {
            for (auto& value : inText)
                if (value >= L'A' && value <= L'Z') value = static_cast<wchar_t>(value + L'a' - L'A');
            return inText;
        }

        void ValidateTarget(const SchemaHistoryResponse& inResponse, const std::wstring& inDatabase)
        {
            const auto& row = inResponse.resultSets[0].front();
            const auto& version = Required(row, 3);
            const auto comment = Lower(Required(row, 4));
            if (Required(row, 0) != L"1" || Required(row, 1) != L"sha256-utf8-lf-v1"
                || Required(row, 2) != inDatabase || (version != L"8.0.46" && !version.starts_with(L"8.0.46-")
                    && !version.starts_with(L"8.0.46+"))
                || comment.find(L"mysql") == std::wstring::npos || comment.find(L"mariadb") != std::wstring::npos
                || Lower(version).find(L"mariadb") != std::wstring::npos) InvalidSchema();
        }

        bool IsSpace(wchar_t inValue)
        {
            return inValue == L' ' || inValue == L'\t' || inValue == L'\n' || inValue == L'\r'
                || inValue == L'\f' || inValue == L'\v';
        }

        std::wstring Identifier(const std::wstring& inToken)
        {
            std::wstring identifier;
            for (std::size_t index = 1; index + 1 < inToken.size(); ++index)
            {
                identifier += inToken[index];
                if (inToken[index] == L'`' && index + 2 < inToken.size() && inToken[index + 1] == L'`') ++index;
            }
            if (identifier.empty()) InvalidSchema();
            return Lower(std::move(identifier));
        }

        bool IsCheckCharset(std::wstring_view inToken)
        {
            return inToken == L"_ascii" || inToken == L"_utf8mb4" || inToken == L"_utf8mb3"
                || inToken == L"_utf8" || inToken == L"_latin1" || inToken == L"_binary";
        }

        // Ignore SQL layout/comments; retain literal contents and operators.
        // CHECK metadata alone can escape delimiter quotes after a known charset.
        std::vector<std::wstring> SqlTokens(const std::wstring& inSql, bool inNormalizeIdentifiers = true,
            bool inCheckMetadata = false)
        {
            std::vector<std::wstring> tokens;
            for (std::size_t index = 0; index < inSql.size();)
            {
                const auto value = inSql[index];
                if (IsSpace(value)) { ++index; continue; }
                if (value == L'#' || (value == L'-' && index + 2 < inSql.size()
                    && inSql[index + 1] == L'-' && IsSpace(inSql[index + 2])))
                {
                    const auto end = inSql.find(L'\n', index);
                    index = end == std::wstring::npos ? inSql.size() : end + 1;
                    continue;
                }
                if (value == L'/' && index + 1 < inSql.size() && inSql[index + 1] == L'*')
                {
                    if (index + 2 < inSql.size() && (inSql[index + 2] == L'!' || inSql[index + 2] == L'+')) InvalidSchema();
                    const auto end = inSql.find(L"*/", index + 2);
                    if (end == std::wstring::npos) InvalidSchema();
                    index = end + 2;
                    continue;
                }
                const auto start = index++;
                if (inCheckMetadata && value == L'\\')
                {
                    if (tokens.empty() || !IsCheckCharset(tokens.back())
                        || index == inSql.size() || inSql[index++] != L'\'') InvalidSchema();
                    const auto literalStart = index;
                    while (index < inSql.size())
                    {
                        const auto literalValue = inSql[index];
                        if (!(literalValue >= L'A' && literalValue <= L'Z')
                            && !(literalValue >= L'a' && literalValue <= L'z')
                            && !(literalValue >= L'0' && literalValue <= L'9') && literalValue != L'_') break;
                        ++index;
                    }
                    if (index == literalStart || index + 1 >= inSql.size()
                        || inSql[index] != L'\\' || inSql[index + 1] != L'\'') InvalidSchema();
                    tokens.push_back(L"'" + inSql.substr(literalStart, index - literalStart) + L"'");
                    index += 2;
                }
                else if (value == L'\'' || value == L'"' || value == L'`')
                {
                    bool closed = false;
                    while (index < inSql.size())
                    {
                        if (inSql[index] == L'\\' && value != L'`')
                        {
                            if (index + 1 >= inSql.size()) InvalidSchema();
                            index += 2;
                        }
                        else if (inSql[index++] == value)
                        {
                            if (index < inSql.size() && inSql[index] == value) ++index;
                            else { closed = true; break; }
                        }
                    }
                    if (!closed) InvalidSchema();
                    auto token = inSql.substr(start, index - start);
                    if (value == L'`' && inNormalizeIdentifiers) token = Identifier(token);
                    tokens.push_back(std::move(token));
                }
                else if ((value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z')
                    || (value >= L'0' && value <= L'9') || value == L'_')
                {
                    while (index < inSql.size())
                    {
                        const auto next = inSql[index];
                        if (!(next >= L'A' && next <= L'Z') && !(next >= L'a' && next <= L'z')
                            && !(next >= L'0' && next <= L'9') && next != L'_') break;
                        ++index;
                    }
                    tokens.push_back(Lower(inSql.substr(start, index - start)));
                }
                else
                {
                    if (value == L'\0' || value > 127) InvalidSchema();
                    if (index < inSql.size() && ((value == L'<' && (inSql[index] == L'=' || inSql[index] == L'>'))
                        || ((value == L'>' || value == L'!') && inSql[index] == L'='))) ++index;
                    tokens.push_back(inSql.substr(start, index - start));
                }
                if (tokens.size() > 32768) InvalidSchema();
            }
            return tokens;
        }

        /** MySQL renders CHECK expressions with redundant parentheses/backticks and ASCII
         * charset introducers. Compare their expression trees, preserving precedence and literals.
         * Only the operators used by this schema are accepted; unknown syntax fails closed.
         */
        class CheckExpression final
        {
        public:
            explicit CheckExpression(const std::wstring& inSql, bool inMetadata = false)
                : tokens(SqlTokens(inSql, true, inMetadata)) {}
            std::wstring Parse()
            {
                auto result = Or();
                if (position != tokens.size()) InvalidSchema();
                return result;
            }
        private:
            std::vector<std::wstring> tokens;
            std::size_t position{};
            std::size_t depth{};

            bool Take(std::wstring_view inToken)
            {
                if (position == tokens.size() || tokens[position] != inToken) return false;
                ++position;
                return true;
            }
            void Expect(std::wstring_view inToken) { if (!Take(inToken)) InvalidSchema(); }
            std::wstring Or()
            {
                auto result = And();
                while (Take(L"or")) result = L"(" + result + L" or " + And() + L")";
                return result;
            }
            std::wstring And()
            {
                auto result = Comparison();
                while (Take(L"and")) result = L"(" + result + L" and " + Comparison() + L")";
                return result;
            }
            std::wstring Comparison()
            {
                auto result = Primary();
                if (Take(L"=")) return L"(" + result + L"=" + Primary() + L")";
                if (Take(L">")) return L"(" + result + L">" + Primary() + L")";
                if (Take(L"is"))
                {
                    const bool negated = Take(L"not");
                    Expect(L"null");
                    return L"(" + result + (negated ? L" is not null)" : L" is null)");
                }
                if (Take(L"in"))
                {
                    Expect(L"(");
                    auto list = Primary();
                    while (Take(L",")) list += L"," + Primary();
                    Expect(L")");
                    return L"(" + result + L" in(" + list + L"))";
                }
                return result;
            }
            std::wstring Primary()
            {
                if (++depth > 128 || position == tokens.size()) InvalidSchema();
                std::wstring result;
                if (Take(L"(")) { result = Or(); Expect(L")"); }
                else
                {
                    result = tokens[position++];
                    if (IsCheckCharset(result))
                    {
                        if (position == tokens.size() || tokens[position].size() < 2 || tokens[position].front() != L'\'') InvalidSchema();
                        const auto literal = tokens[position++];
                        if (literal.find_first_not_of(L"'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")
                            != std::wstring::npos) InvalidSchema();
                        result = (result == L"_binary" ? result : L"") + literal;
                    }
                    else if (Take(L"("))
                    {
                        // CHECK metadata prints the byte-length synonym LENGTH.
                        if (result == L"octet_length") result = L"length";
                        result += L"(" + Or();
                        while (Take(L",")) result += L"," + Or();
                        Expect(L")");
                        result += L")";
                    }
                }
                --depth;
                return result;
            }
        };

        SchemaHistoryRow Column(std::wstring inTable, std::wstring inName, std::wstring inType,
            std::wstring inNullable, std::optional<std::wstring> inCollation = {}, std::wstring inExtra = L"",
            std::optional<std::wstring> inDefault = {}, std::optional<std::wstring> inCharset = {})
        {
            return {std::move(inTable), std::move(inName), std::move(inType), std::move(inNullable),
                std::move(inCollation), std::move(inExtra), L"InnoDB", std::move(inDefault), std::move(inCharset)};
        }

        SchemaHistoryRow Primary(std::wstring inTable, std::wstring inColumn)
        { return {std::move(inTable), L"PRIMARY", L"PRIMARY KEY", std::move(inColumn), {}, {}, {}, {}, {}, {}, {}}; }

        SchemaHistoryRow Check(std::wstring inTable, std::wstring inName, std::wstring inExpression)
        { return {std::move(inTable), std::move(inName), L"CHECK", {}, {}, {}, std::move(inExpression), L"YES", {}, {}, {}}; }

        void SameRows(const std::vector<SchemaHistoryRow>& inActual, const std::vector<SchemaHistoryRow>& inExpected)
        {
            const std::multiset<SchemaHistoryRow> actual(inActual.begin(), inActual.end());
            const std::multiset<SchemaHistoryRow> expected(inExpected.begin(), inExpected.end());
            if (actual != expected) InvalidSchema();
        }

        void ValidateColumns(const SchemaHistoryResponse& inResponse)
        {
            // Table/ordinal order is part of the inspection query. Defaults, charsets,
            // precision, nullability, auto-increment and engine must match the deployed DDL.
            const std::vector<SchemaHistoryRow> expected{
                Column(L"account_identities", L"account_id", L"bigint unsigned", L"NO"),
                Column(L"account_identities", L"provider", L"varbinary(32)", L"NO"),
                Column(L"account_identities", L"subject", L"varbinary(255)", L"NO"),
                Column(L"account_identities", L"linked_at", L"datetime(6)", L"NO"),
                Column(L"accounts", L"account_id", L"bigint unsigned", L"NO", {}, L"auto_increment"),
                Column(L"accounts", L"status", L"int", L"NO", {}, L"", L"0"),
                Column(L"accounts", L"created_at", L"datetime(6)", L"NO"),
                Column(L"accounts", L"last_login_at", L"datetime(6)", L"YES"),
                Column(L"schema_migrations", L"execution_id", L"bigint unsigned", L"NO", {}, L"auto_increment"),
                Column(L"schema_migrations", L"version", L"int unsigned", L"NO"),
                Column(L"schema_migrations", L"name", L"varchar(128)", L"NO", L"ascii_bin", L"", {}, L"ascii"),
                Column(L"schema_migrations", L"direction", L"enum('UP','DOWN')", L"NO", L"ascii_bin", L"", {}, L"ascii"),
                Column(L"schema_migrations", L"up_checksum", L"char(64)", L"NO", L"ascii_bin", L"", {}, L"ascii"),
                Column(L"schema_migrations", L"down_checksum", L"char(64)", L"YES", L"ascii_bin", L"", {}, L"ascii"),
                Column(L"schema_migrations", L"state", L"enum('RUNNING','SUCCEEDED','FAILED')", L"NO", L"ascii_bin", L"", {}, L"ascii"),
                Column(L"schema_migrations", L"started_at", L"datetime(6)", L"NO"),
                Column(L"schema_migrations", L"finished_at", L"datetime(6)", L"YES")};
            if (inResponse.resultSets[2] != expected) InvalidSchema();
        }

        void ValidateConstraints(const SchemaHistoryResponse& inResponse, const std::wstring& inDatabase)
        {
            std::vector<SchemaHistoryRow> expected{
                Primary(L"accounts", L"account_id"), Check(L"accounts", L"ck_accounts_status", L"status IN (0, 1)"),
                Primary(L"account_identities", L"provider"), Primary(L"account_identities", L"subject"),
                Check(L"account_identities", L"ck_account_identities_provider", L"provider = _binary'google'"),
                Check(L"account_identities", L"ck_account_identities_subject", L"OCTET_LENGTH(subject) > 0"),
                {L"account_identities", L"fk_account_identities_account", L"FOREIGN KEY", L"account_id", L"accounts",
                    L"account_id", {}, {}, inDatabase, L"RESTRICT", L"RESTRICT"},
                Primary(L"schema_migrations", L"execution_id"),
                Check(L"schema_migrations", L"ck_migrations_completion",
                    L"(state = 'RUNNING' AND finished_at IS NULL) OR (state IN ('SUCCEEDED', 'FAILED') AND finished_at IS NOT NULL)"),
                Check(L"schema_migrations", L"ck_migrations_bootstrap",
                    L"(version = 0 AND direction = 'UP' AND down_checksum IS NULL) OR (version > 0 AND down_checksum IS NOT NULL)")};
            auto actual = inResponse.resultSets[3];
            for (auto& row : actual)
                if (row[6]) row[6] = CheckExpression(*row[6], true).Parse();
            for (auto& row : expected)
                if (row[6]) row[6] = CheckExpression(*row[6]).Parse();
            SameRows(actual, expected);
            SameRows(inResponse.resultSets[4], {
                {L"accounts", L"PRIMARY", L"0", L"1", L"account_id", {}},
                {L"account_identities", L"PRIMARY", L"0", L"1", L"provider", {}},
                {L"account_identities", L"PRIMARY", L"0", L"2", L"subject", {}},
                {L"account_identities", L"ix_account_identities_account_id", L"1", L"1", L"account_id", {}},
                {L"schema_migrations", L"PRIMARY", L"0", L"1", L"execution_id", {}}});
        }

        std::vector<std::wstring> RoutineBody(const std::wstring& inSql)
        {
            auto tokens = SqlTokens(inSql, false);
            const auto begin = std::find(tokens.begin(), tokens.end(), L"begin");
            const auto end = std::find(tokens.rbegin(), tokens.rend(), L"end");
            if (begin == tokens.end() || end == tokens.rend() || begin >= end.base()) InvalidSchema();
            std::vector<std::wstring> body(begin, end.base());
            for (auto& token : body)
                if (!token.empty() && token.front() == L'`') token = Identifier(token);
            return body;
        }

        void ValidateMode(const std::wstring& inValue)
        {
            std::set<std::wstring> modes;
            for (std::size_t start = 0; start < inValue.size();)
            {
                const auto comma = inValue.find(L',', start);
                const auto end = comma == std::wstring::npos ? inValue.size() : comma;
                const auto mode = inValue.substr(start, end - start);
                if (mode.empty() || !modes.insert(mode).second) InvalidSchema();
                if (comma == std::wstring::npos) break;
                start = comma + 1;
                if (start == inValue.size()) InvalidSchema();
            }
            if ((!modes.contains(L"STRICT_TRANS_TABLES") && !modes.contains(L"STRICT_ALL_TABLES"))
                || modes.contains(L"NO_BACKSLASH_ESCAPES") || modes.contains(L"ANSI_QUOTES")
                || modes.contains(L"PIPES_AS_CONCAT")) InvalidSchema();
        }

        void ValidateRoutines(const SchemaHistoryResponse& inResponse, const Deployment& inDeployment)
        {
            const std::array<std::wstring, 4> names{L"get_schema_migration_history", L"", L"login_google_account", L"get_auth_account_status"};
            std::set<std::wstring> seen;
            for (const auto& row : inResponse.resultSets[5])
            {
                const auto& name = Required(row, 0);
                const auto found = std::find(names.begin(), names.end(), name);
                if (found == names.end() || name.empty() || !seen.insert(name).second) InvalidSchema();
                const auto version = static_cast<std::size_t>(std::distance(names.begin(), found));
                if (Required(row, 1) != L"DEFINER" || Required(row, 2) != (version == 2 ? L"MODIFIES SQL DATA" : L"READS SQL DATA"))
                    InvalidSchema();
                // definition_utf8 drops charset introducers; use it only to require visibility.
                if (Required(row, 3).empty()) InvalidSchema();
            }
            constexpr std::array<std::size_t, 3> VERSIONS{0, 2, 3};
            for (std::size_t index = 0; index < VERSIONS.size(); ++index)
            {
                const auto version = VERSIONS[index];
                const auto& row = inResponse.resultSets[index + 7].front();
                if (Required(row, 0) != names[version]) InvalidSchema();
                ValidateMode(Required(row, 1));
                for (std::size_t column = 2; column < row.size(); ++column)
                    if (Required(row, column).empty()) InvalidSchema();
                if (RoutineBody(Required(row, 2)) != RoutineBody(inDeployment[version].upSql)) InvalidSchema();
            }
            SameRows(inResponse.resultSets[6], {
                {L"login_google_account", L"1", L"IN", L"inSubject", L"text", L"utf8mb4", L"utf8mb4_bin"},
                {L"get_auth_account_status", L"1", L"IN", L"inAccountId", L"bigint unsigned", {}, {}}});
        }

        void ValidateStructure(const SchemaHistoryResponse& inResponse, const Deployment& inDeployment,
            const std::wstring& inDatabase, std::string_view& outStage)
        {
            outStage = "table columns";
            ValidateColumns(inResponse);
            outStage = "constraints and indexes";
            ValidateConstraints(inResponse, inDatabase);
            outStage = "stored procedure definitions and parameters";
            ValidateRoutines(inResponse, inDeployment);
        }
    }

    void LoginSchemaVerifier::Verify(std::shared_ptr<OdbcDatabase> inDatabase,
        asio::any_io_executor inCompletionExecutor, CompletionHandler inHandler)
    {
        Deployment deployment;
        std::wstring expectedDatabase;
        std::string_view stage = "database configuration";
        try
        {
            if (!inDatabase || !inDatabase->IsConfigured()) throw std::runtime_error("Database not configured.");
            expectedDatabase = Environment(L"ACTIONRPG_DB_SCHEMA");
            if (expectedDatabase.size() > 64) InvalidSchema();
            stage = "deployed migration files";
            deployment = LoadDeployment();
        }
        catch (...)
        {
            auto guard = asio::make_work_guard(inCompletionExecutor);
            asio::post(inCompletionExecutor, [database = std::move(inDatabase), handler = std::move(inHandler),
                guard = std::move(guard), stage]() mutable
            {
                SchemaVerificationResult result;
                result.stage = stage;
                result.status = database && database->IsConfigured()
                    ? SchemaVerificationStatus::NotReady : SchemaVerificationStatus::DatabaseNotConfigured;
                if (handler) handler(std::move(result));
            });
            return;
        }
        auto procedure = std::make_unique<SchemaHistoryProcedure>();
        auto database = inDatabase;
        inDatabase->Run(std::move(procedure), std::move(inCompletionExecutor),
            [database = std::move(database), deployment = std::move(deployment), expectedDatabase = std::move(expectedDatabase),
                handler = std::move(inHandler)](ProcedureResult<SchemaHistoryResponse> inResult) mutable
        {
            SchemaVerificationResult result;
            result.stage = "schema history query and result mapping";
            result.databaseError = std::move(inResult.error);
            try
            {
                if (result.databaseError || !inResult.response) InvalidSchema();
                result.stage = "database target and version";
                ValidateTarget(*inResult.response, expectedDatabase);
                result.stage = "migration audit history";
                ValidateHistory(*inResult.response, deployment);
                ValidateStructure(*inResult.response, deployment, expectedDatabase, result.stage);
                result.schema = std::shared_ptr<const VerifiedLoginSchema>(new VerifiedLoginSchema(database));
                result.status = SchemaVerificationStatus::Ready;
                result.stage = "ready";
            }
            catch (...) { result.status = SchemaVerificationStatus::NotReady; }
            if (handler) handler(std::move(result));
        });
    }
}
