#include "TownInstance.h"

#include "Database/LoginGoogleAccountProcedure.h"
#include "PlayerSession.h"

namespace TownServer::Domain
{
    namespace
    {
        constexpr auto LOGIN_TIMEOUT = std::chrono::seconds(60);
    }

    void TownInstance::BeginGoogleLogin(std::shared_ptr<Network::PlayerSession> inSession,
        std::function<void(std::uint64_t)> inStarted, GoogleLoginHandler inCompleted)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, session = std::move(inSession),
            started = std::move(inStarted), completed = std::move(inCompleted)]() mutable
        {
            if (!session || !started || !completed) return;
            const auto sessionId = session->GetSessionId();
            if (!self->running || !self->database || !self->database->IsConfigured())
            {
                session->Stop();
                started(0);
                completed(Authentication::LoginResult::Unavailable);
                return;
            }
            if (self->pendingGoogleLogins.contains(sessionId) || session->GetPlayerId() != 0)
            {
                started(0);
                completed(Authentication::LoginResult::Busy);
                return;
            }
            const auto attemptId = session->BeginAuthentication();
            if (attemptId == 0)
            {
                started(0);
                completed(Authentication::LoginResult::Busy);
                return;
            }
            auto deadline = std::make_shared<asio::steady_timer>(self->strand);
            deadline->expires_after(LOGIN_TIMEOUT);
            self->pendingGoogleLogins.emplace(sessionId,
                PendingGoogleLogin{ session, attemptId, deadline, std::move(completed) });
            deadline->async_wait([weakTown = self->weak_from_this(), sessionId, attemptId](const asio::error_code& inError)
            {
                if (!inError)
                    if (const auto town = weakTown.lock())
                        town->FinishGoogleLogin(sessionId, attemptId, Authentication::LoginResult::Expired);
            });
            // This callback only hands the attempt context to the trusted adapter. The adapter must
            // schedule HTTPS/JWKS/crypto work elsewhere; it must never block this strand.
            started(attemptId);
        });
    }

    /** Consume verified proofs once, then resolve an account on DB workers. Never retry a failed transaction. */
    void TownInstance::ResolveGoogleAccount(Authentication::VerifiedGoogleIdentity inIdentity,
        Authentication::VerifiedLoginSchema inSchema)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, identity = std::move(inIdentity), schema = std::move(inSchema)]()
        {
            const auto sessionId = identity.GetSessionId();
            const auto attemptId = identity.GetAttemptId();
            const auto pending = self->pendingGoogleLogins.find(sessionId);
            if (pending == self->pendingGoogleLogins.end() || pending->second.attemptId != attemptId) return;
            const auto session = pending->second.session.lock();
            if (!session || session->GetSessionId() != sessionId || !session->MatchesAuthentication(attemptId))
            {
                self->FinishGoogleLogin(sessionId, attemptId, Authentication::LoginResult::Stopped);
                return;
            }
            if (identity.IsExpired() || std::chrono::steady_clock::now() >= pending->second.deadline->expiry())
            {
                self->FinishGoogleLogin(sessionId, attemptId, Authentication::LoginResult::Expired);
                return;
            }
            if (!self->running || !self->database->IsConfigured() || !schema.Matches(self->database))
            {
                self->FinishGoogleLogin(sessionId, attemptId, Authentication::LoginResult::Unavailable);
                return;
            }
            if (!session->BeginAccountLookup(attemptId)) return;
            auto procedure = std::make_unique<Database::LoginGoogleAccountProcedure>();
            const auto& subject = identity.GetSubject();
            procedure->req.subject.assign(subject.begin(), subject.end());
            self->RunStoreProcedure(std::move(procedure), [identity](TownInstance& inTown,
                Database::ProcedureResult<Database::LoginGoogleAccountResponse> inResult)
            {
                const auto sessionId = identity.GetSessionId();
                const auto attemptId = identity.GetAttemptId();
                const auto pending = inTown.pendingGoogleLogins.find(sessionId);
                if (pending == inTown.pendingGoogleLogins.end() || pending->second.attemptId != attemptId) return;
                const auto session = pending->second.session.lock();
                if (!session || session->GetSessionId() != sessionId || !session->MatchesAuthentication(attemptId))
                {
                    inTown.FinishGoogleLogin(sessionId, attemptId, Authentication::LoginResult::Stopped);
                    return;
                }
                if (identity.IsExpired() || std::chrono::steady_clock::now() >= pending->second.deadline->expiry())
                {
                    inTown.FinishGoogleLogin(sessionId, attemptId, Authentication::LoginResult::Expired);
                    return;
                }
                if (!inTown.running || !inResult.IsSuccess() || !inResult.response->hasRow)
                {
                    inTown.FinishGoogleLogin(sessionId, attemptId, Authentication::LoginResult::Failed);
                    return;
                }
                // Transport success alone does not grant admission. A suspended account remains unauthenticated.
                const auto& response = *inResult.response;
                if (response.resultCode == 1)
                {
                    inTown.FinishGoogleLogin(sessionId, attemptId, Authentication::LoginResult::AccountSuspended);
                    return;
                }
                if (response.resultCode != 0 || response.accountStatus != 0
                    || !session->CompleteAuthentication(attemptId, response.accountId))
                {
                    inTown.FinishGoogleLogin(sessionId, attemptId, Authentication::LoginResult::Failed);
                    return;
                }
                inTown.FinishGoogleLogin(sessionId, attemptId, Authentication::LoginResult::Succeeded);
            });
        });
    }

    void TownInstance::FinishGoogleLogin(const std::uint64_t inSessionId, const std::uint64_t inAttemptId,
        const Authentication::LoginResult inResult)
    {
        const auto found = pendingGoogleLogins.find(inSessionId);
        if (found == pendingGoogleLogins.end() || found->second.attemptId != inAttemptId) return;
        auto pending = std::move(found->second);
        pendingGoogleLogins.erase(found);
        asio::error_code ignoredError;
        pending.deadline->cancel(ignoredError);
        const auto session = pending.session.lock();
        if (!session || session->GetSessionId() != inSessionId) return;
        if (inResult == Authentication::LoginResult::Succeeded)
        {
            if (session->GetAccountId() == 0) return;
        }
        else
        {
            const bool currentAttempt = session->MatchesAuthentication(inAttemptId);
            session->Stop();
            if (!currentAttempt) return;
        }
        pending.completed(inResult);
    }
}
