#include "TownInstance.h"
#include "PlayerSession.h"
#include "Protocol.h"
#include <algorithm>

namespace TownServer::Domain
{
    void TownInstance::Admit(std::shared_ptr<Network::PlayerSession> inSession, std::string inTicket)
    {
        const auto self = shared_from_this();
        asio::post(strand, [self, session = std::move(inSession), ticket = std::move(inTicket)]
        {
            if (!self->running || !ActionRPG::Authentication::IsToken(ticket)) { session->Stop(); return; }
            const auto attempt = session->BeginAuthentication();
            if (!attempt || self->admissions.size() >= 10000) { session->Stop(); return; }
            const auto id = session->GetSessionId();
            const auto connection = ActionRPG::Authentication::RandomToken();
            const auto started = std::chrono::steady_clock::now();
            self->admissions.emplace(id, Admission{session, attempt, connection, {}, started + std::chrono::seconds(30), {}});
            self->admissions.at(id).ticket = ticket;
            if (!self->admissionTimer)
            {
                self->admissionTimer = std::make_shared<asio::steady_timer>(self->strand);
                self->PollAdmissions();
            }
            self->ConsumeAdmission(id);
        });
    }

    // Retry only an explicit Pending: lost/error responses may already have granted a single-use ticket.
    void TownInstance::ConsumeAdmission(std::uint64_t inSessionId)
    {
        const auto found = admissions.find(inSessionId);
        if (found == admissions.end() || found->second.renewing || !found->second.lease.empty()) return;
        auto& admission = found->second;
        const auto session = admission.session.lock();
        if (!session || !session->MatchesAuthentication(admission.attemptId)) return;
        admission.renewing = true;
        const auto started = std::chrono::steady_clock::now();
        const auto connection = admission.connection;
        const auto attempt = admission.attemptId;
        authClient->Request("/internal/consume", {{"ticket", admission.ticket}, {"connection", connection}}, strand,
            [weakTown = weak_from_this(), id = inSessionId, attempt, connection, started](auto response)
        {
            const auto town = weakTown.lock();
            if (!town) return;
            std::string lease;
            std::uint64_t accountId{};
            bool pending{};
            std::int64_t waitRemainingMs{};
            try
            {
                pending = response && response->value("status", std::string{}) == "Pending"
                    && response->at("ready") == false && response->at("retryAfterMs") == 1000
                    && response->at("waitRemainingMs").is_number_integer();
                if (pending)
                {
                    waitRemainingMs = response->at("waitRemainingMs").template get<std::int64_t>();
                    if (waitRemainingMs < 0 || waitRemainingMs > 30000) pending = false;
                }
                if (response && !pending && response->at("expiresIn") == 15)
                {
                    lease = response->at("lease").template get<std::string>();
                    accountId = response->at("accountId").template get<std::uint64_t>();
                    if (!ActionRPG::Authentication::IsToken(lease)) lease.clear();
                }
            }
            catch (...) { pending = false; }
            const auto found = town->admissions.find(id);
            const bool current = found != town->admissions.end() && found->second.connection == connection
                && found->second.attemptId == attempt;
            const auto session = current ? found->second.session.lock() : nullptr;
            const auto now = std::chrono::steady_clock::now();
            const auto deadline = started + std::chrono::seconds(15);
            if (!current || !town->running || !session || !session->MatchesAuthentication(attempt)
                || now >= found->second.deadline || (!pending && (lease.empty() || accountId == 0 || now >= deadline)))
            {
                if (!lease.empty()) town->authClient->Request("/internal/release",
                    {{"lease", lease}, {"connection", connection}}, town->strand, [](auto) {});
                if (current)
                {
                    const bool timeout = now >= found->second.deadline;
                    town->admissions.erase(found);
                    if (session) session->StopWithStatus(timeout ? TownProtocol::SessionStatusCode::AdmissionTimeout
                        : TownProtocol::SessionStatusCode::AuthUnavailable);
                }
                return;
            }
            auto& admission = found->second;
            admission.renewing = false;
            admission.nextRenewal = started + std::chrono::seconds(1);
            if (pending)
            {
                admission.deadline = std::min(admission.deadline, started + std::chrono::milliseconds(waitRemainingMs));
                if (now >= admission.deadline)
                {
                    town->admissions.erase(found);
                    session->StopWithStatus(TownProtocol::SessionStatusCode::AdmissionTimeout);
                    return;
                }
                if (!admission.pendingNotified)
                {
                    admission.pendingNotified = true;
                    session->Send(TownProtocol::Encode(TownProtocol::SessionStatus{
                        static_cast<std::uint8_t>(TownProtocol::SessionStatusCode::WaitingForPriorOwner), 1000 }));
                }
                return;
            }
            admission.lease = lease;
            admission.ticket.clear();
            admission.deadline = deadline;
            session->SetAdmissionDeadline(deadline);
            if (!session->CompleteAuthentication(attempt, accountId)) { session->Stop(); return; }
            session->Send(TownProtocol::Encode(TownProtocol::AdmissionResult{0}));
        });
    }

    void TownInstance::ReleaseAdmission(std::uint64_t inSessionId)
    {
        const auto self = shared_from_this();
        asio::post(strand, [self, inSessionId]
        {
            if (self->characterWork.contains(inSessionId) || self->characterReleases.contains(inSessionId))
            {
                self->deferredAdmissionReleases.emplace(inSessionId);
                self->DrainCharacterRelease(inSessionId);
                return;
            }
            const auto found = self->admissions.find(inSessionId);
            if (found == self->admissions.end()) return;
            const auto admission = found->second;
            self->admissions.erase(found);
            if (!admission.lease.empty()) self->authClient->Request("/internal/release",
                {{"lease", admission.lease}, {"connection", admission.connection}}, self->strand, [](auto) {});
        });
    }

    void TownInstance::PollAdmissions()
    {
        if (!running) return;
        const auto now = std::chrono::steady_clock::now();
        const std::vector<std::uint64_t> releases(deferredAdmissionReleases.begin(), deferredAdmissionReleases.end());
        for (const auto id : releases) DrainCharacterRelease(id);
        for (auto& [id, admission] : admissions)
        {
            const auto session = admission.session.lock();
            if (!session) continue; // An unconfirmed dungeon release retains ownership in Auth.
            if (admission.deadline <= now)
            {
                session->StopWithStatus(admission.lease.empty() ? TownProtocol::SessionStatusCode::AdmissionTimeout
                    : TownProtocol::SessionStatusCode::AuthorizationExpired);
                continue;
            }
            if (admission.renewing || admission.nextRenewal > now) continue;
            if (admission.lease.empty()) { ConsumeAdmission(id); continue; }
            admission.renewing = true;
            const auto connection = admission.connection;
            authClient->Request("/internal/renew", {{"lease", admission.lease}, {"connection", connection}}, strand,
                [weakTown = weak_from_this(), id, connection, now](auto response)
            {
                const auto town = weakTown.lock();
                if (!town) return;
                const auto found = town->admissions.find(id);
                if (found == town->admissions.end() || found->second.connection != connection) return;
                auto& admission = found->second;
                admission.renewing = false;
                const auto session = admission.session.lock();
                if (!session) return;
                bool valid{};
                try { valid = response && response->at("valid") == true && response->at("expiresIn") == 15; }
                catch (...) {}
                if (!valid || std::chrono::steady_clock::now() >= admission.deadline)
                {
                    auto reason = TownProtocol::SessionStatusCode::AuthUnavailable;
                    if (std::chrono::steady_clock::now() >= admission.deadline)
                        reason = TownProtocol::SessionStatusCode::AuthorizationExpired;
                    else if (response)
                    {
                        try
                        {
                            const auto code = response->value("reason", std::string{});
                            if (code == "Replaced") reason = TownProtocol::SessionStatusCode::Replaced;
                            else if (code == "AuthorizationExpired") reason = TownProtocol::SessionStatusCode::AuthorizationExpired;
                            else if (code == "LoggedOut") reason = TownProtocol::SessionStatusCode::LoggedOut;
                            else if (code == "ServerTransfer") reason = TownProtocol::SessionStatusCode::ServerTransfer;
                            else if (code == "AccountUnavailable") reason = TownProtocol::SessionStatusCode::AccountUnavailable;
                        }
                        catch (...) {} // Malformed reason is an unavailable authority, never a guessed replacement.
                    }
                    session->StopWithStatus(reason); return;
                }
                admission.deadline = now + std::chrono::seconds(15);
                admission.nextRenewal = now + std::chrono::seconds(1);
                session->SetAdmissionDeadline(admission.deadline);
            });
        }
        admissionTimer->expires_after(std::chrono::seconds(1));
        admissionTimer->async_wait([weakTown = weak_from_this()](const asio::error_code& inError)
        { if (!inError) if (const auto town = weakTown.lock()) town->PollAdmissions(); });
    }
}
