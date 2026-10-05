#include "TownInstance.h"
#include "PlayerSession.h"
#include "Protocol.h"

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
            self->admissions.emplace(id, Admission{session, attempt, connection, {}, started + std::chrono::seconds(10), {}});
            if (!self->admissionTimer)
            {
                self->admissionTimer = std::make_shared<asio::steady_timer>(self->strand);
                self->PollAdmissions();
            }
            self->authClient->Request("/internal/consume", {{"ticket", ticket}, {"connection", connection}}, self->strand,
                [weakTown = self->weak_from_this(), id, attempt, connection, started](auto response)
            {
                const auto town = weakTown.lock();
                if (!town) return;
                std::string lease;
                std::uint64_t accountId{};
                try
                {
                    if (response && response->at("expiresIn") == 15)
                    {
                        lease = response->at("lease").template get<std::string>();
                        accountId = response->at("accountId").template get<std::uint64_t>();
                        if (!ActionRPG::Authentication::IsToken(lease)) lease.clear();
                    }
                }
                catch (...) { lease.clear(); }
                const auto found = town->admissions.find(id);
                const auto session = found != town->admissions.end() ? found->second.session.lock() : nullptr;
                const auto deadline = started + std::chrono::seconds(15);
                if (!town->running || !session || !session->MatchesAuthentication(attempt)
                    || lease.empty() || accountId == 0 || std::chrono::steady_clock::now() >= deadline)
                {
                    if (!lease.empty()) town->authClient->Request("/internal/release",
                        {{"lease", lease}, {"connection", connection}}, town->strand, [](auto) {});
                    if (session) session->Stop();
                    town->admissions.erase(id);
                    return;
                }
                found->second.lease = lease;
                found->second.deadline = deadline;
                found->second.nextRenewal = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                session->SetAdmissionDeadline(deadline);
                if (!session->CompleteAuthentication(attempt, accountId)) { session->Stop(); return; }
                session->Send(TownProtocol::Encode(TownProtocol::AdmissionResult{0}));
            });
        });
    }

    void TownInstance::ReleaseAdmission(std::uint64_t inSessionId)
    {
        const auto self = shared_from_this();
        asio::post(strand, [self, inSessionId]
        {
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
        for (auto& [id, admission] : admissions)
        {
            const auto session = admission.session.lock();
            if (!session) continue; // An unconfirmed dungeon release retains ownership in Auth.
            if (admission.deadline <= now) { session->Stop(); continue; }
            if (admission.lease.empty() || admission.renewing || admission.nextRenewal > now) continue;
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
                { session->Stop(); return; }
                admission.deadline = now + std::chrono::seconds(15);
                admission.nextRenewal = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                session->SetAdmissionDeadline(admission.deadline);
            });
        }
        admissionTimer->expires_after(std::chrono::seconds(1));
        admissionTimer->async_wait([weakTown = weak_from_this()](const asio::error_code& inError)
        { if (!inError) if (const auto town = weakTown.lock()) town->PollAdmissions(); });
    }
}