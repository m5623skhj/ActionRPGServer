#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace TownServer::Database { class OdbcDatabase; }

namespace TownServer::Authentication
{
    enum class LoginResult { Succeeded, Busy, Unavailable, Failed, Stopped, Expired, AccountSuspended };

    // These producers are deliberately not implemented or installed until the authentication
    // flow/SDK and live schema inspection contract are agreed. There is no public success factory.
    class GoogleIdTokenVerifier;
    class LoginSchemaVerifier;

    class VerifiedGoogleIdentity final
    {
    public:
        [[nodiscard]] const std::string& GetSubject() const noexcept { return subject; }
        [[nodiscard]] std::uint64_t GetSessionId() const noexcept { return sessionId; }
        [[nodiscard]] std::uint64_t GetAttemptId() const noexcept { return attemptId; }
        [[nodiscard]] bool IsExpired() const noexcept { return std::chrono::system_clock::now() >= expiresAt; }

    private:
        friend class GoogleIdTokenVerifier;
        // The verifier must first validate Google signature/JWKS/allowed algorithm, iss,
        // aud/azp, exp and flow-specific nonce bound to this session and attempt.
        VerifiedGoogleIdentity(std::string inSubject, std::uint64_t inSessionId,
            std::uint64_t inAttemptId, std::chrono::system_clock::time_point inExpiresAt)
            : subject(std::move(inSubject)), sessionId(inSessionId), attemptId(inAttemptId), expiresAt(inExpiresAt)
        {
            if (subject.empty() || subject.size() > 255 || sessionId == 0 || attemptId == 0)
                throw std::invalid_argument("Invalid verified Google identity context.");
            for (const unsigned char value : subject)
                if (value == 0 || value > 127) throw std::invalid_argument("Invalid Google subject encoding.");
        }

        std::string subject;
        std::uint64_t sessionId{}, attemptId{};
        std::chrono::system_clock::time_point expiresAt;
    };

    class VerifiedLoginSchema final
    {
    public:
        static constexpr std::uint32_t REQUIRED_SCHEMA_VERSION = 2;
        [[nodiscard]] bool Matches(const std::shared_ptr<Database::OdbcDatabase>& inDatabase) const noexcept
        {
            return inDatabase && database.lock() == inDatabase;
        }

    private:
        friend class LoginSchemaVerifier;
        // Only create after inspecting this database's complete migration history/checksums
        // and verifying the V000002 login procedure contract. Configuration alone is insufficient.
        VerifiedLoginSchema(const std::shared_ptr<Database::OdbcDatabase>& inDatabase, std::uint32_t inVersion)
            : database(inDatabase)
        {
            if (!inDatabase || inVersion != REQUIRED_SCHEMA_VERSION)
                throw std::invalid_argument("Incompatible Google login schema.");
        }
        std::weak_ptr<Database::OdbcDatabase> database;
    };
}
