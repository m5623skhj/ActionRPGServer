#pragma once

#include "../Shared/AuthTransport.h"
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace AuthServer
{
    /** All ownership changes are atomic under this mutex; no network or DB work occurs while locked.
     * An old owner is retained until an explicit release, even after expiry. This fences a live dungeon
     * on an isolated server. Loss of its release deliberately prevents automatic takeover.
     */
    class SessionRegistry final
    {
        using Clock = std::chrono::steady_clock;
        struct Account
        {
            std::string token;
            Clock::time_point expires;
            std::string serverId;
            std::string connection;
            std::string lease;
            Clock::time_point leaseExpires;
            bool draining{};
        };
        struct Ticket
        {
            std::uint64_t accountId{};
            std::string token;
            std::string serverId;
            Clock::time_point expires;
        };
        struct Challenge { std::string nonce; Clock::time_point expires; };
        std::mutex mutex;
        std::unordered_map<std::uint64_t, Account> accounts;
        std::unordered_map<std::string, std::uint64_t> tokens;
        std::unordered_map<std::string, Ticket> tickets;
        std::unordered_map<std::string, Challenge> challenges;
        static constexpr std::size_t MAX_ENTRIES = 10000;

        void Prune()
        {
            const auto now = Clock::now();
            std::erase_if(challenges, [now](const auto& inItem) { return inItem.second.expires <= now; });
            std::erase_if(tickets, [now](const auto& inItem) { return inItem.second.expires <= now; });
            for (auto iterator = accounts.begin(); iterator != accounts.end();)
            {
                if (iterator->second.expires <= now)
                {
                    tokens.erase(iterator->second.token);
                    iterator->second.token.clear();
                    iterator->second.draining = true;
                    if (iterator->second.lease.empty()) { iterator = accounts.erase(iterator); continue; }
                }
                ++iterator;
            }
        }

    public:
        nlohmann::json ChallengeLogin()
        {
            std::lock_guard lock(mutex);
            Prune();
            if (challenges.size() >= MAX_ENTRIES) throw std::runtime_error("Capacity reached.");
            const auto id = ActionRPG::Authentication::RandomToken();
            const auto nonce = ActionRPG::Authentication::RandomToken();
            challenges.emplace(id, Challenge{nonce, Clock::now() + std::chrono::minutes(5)});
            return {{"challengeId", id}, {"nonce", nonce}, {"expiresIn", 300}};
        }

        std::string ConsumeChallenge(const std::string& inId)
        {
            std::lock_guard lock(mutex);
            Prune();
            const auto found = challenges.find(inId);
            if (found == challenges.end()) throw std::runtime_error("Invalid challenge.");
            auto nonce = found->second.nonce;
            challenges.erase(found);
            return nonce;
        }

        nlohmann::json Login(std::uint64_t inAccountId)
        {
            std::lock_guard lock(mutex);
            Prune();
            if (!accounts.contains(inAccountId) && accounts.size() >= MAX_ENTRIES)
                throw std::runtime_error("Capacity reached.");
            auto& account = accounts[inAccountId];
            tokens.erase(account.token);
            account.token = ActionRPG::Authentication::RandomToken();
            account.expires = Clock::now() + std::chrono::hours(8);
            account.draining = !account.lease.empty();
            tokens.emplace(account.token, inAccountId);
            return {{"gameToken", account.token}, {"expiresIn", 28800}};
        }

        std::uint64_t Resolve(const std::string& inToken)
        {
            std::lock_guard lock(mutex);
            Prune();
            const auto found = tokens.find(inToken);
            return found == tokens.end() ? 0 : found->second;
        }

        nlohmann::json Issue(const std::string& inToken, const std::string& inServerId)
        {
            std::lock_guard lock(mutex);
            Prune();
            const auto found = tokens.find(inToken);
            if (found == tokens.end() || tickets.size() >= MAX_ENTRIES) throw std::runtime_error("Invalid session.");
            auto& account = accounts.at(found->second);
            // One outstanding ticket per session; a new destination replaces the prior choice.
            std::erase_if(tickets, [&inToken](const auto& inItem) { return inItem.second.token == inToken; });
            account.draining = !account.lease.empty();
            auto ticket = ActionRPG::Authentication::RandomToken();
            tickets.emplace(ticket, Ticket{found->second, inToken, inServerId, Clock::now() + std::chrono::seconds(30)});
            return {{"ticket", ticket}, {"expiresIn", 30}, {"ready", account.lease.empty()}};
        }

        std::uint64_t ResolveTicket(const std::string& inTicket, const std::string& inServerId)
        {
            std::lock_guard lock(mutex);
            Prune();
            const auto found = tickets.find(inTicket);
            return found != tickets.end() && found->second.serverId == inServerId ? found->second.accountId : 0;
        }

        nlohmann::json Consume(const std::string& inTicket, const std::string& inServerId,
            const std::string& inConnection)
        {
            std::lock_guard lock(mutex);
            Prune();
            const auto found = tickets.find(inTicket);
            if (found == tickets.end() || found->second.serverId != inServerId) throw std::runtime_error("Invalid ticket.");
            const auto ticket = found->second;
            tickets.erase(found); // Never reusable, including failed or lost responses.
            const auto owner = accounts.find(ticket.accountId);
            if (owner == accounts.end() || owner->second.token != ticket.token || !owner->second.lease.empty())
                throw std::runtime_error("Prior owner has not released.");
            auto& account = owner->second;
            account.serverId = inServerId;
            account.connection = inConnection;
            account.lease = ActionRPG::Authentication::RandomToken();
            account.leaseExpires = Clock::now() + std::chrono::seconds(15);
            account.draining = false;
            return {{"accountId", ticket.accountId}, {"lease", account.lease}, {"expiresIn", 15}};
        }

        std::uint64_t ResolveLease(const std::string& inServerId, const std::string& inConnection,
            const std::string& inLease)
        {
            std::lock_guard lock(mutex);
            for (const auto& [id, account] : accounts)
                if (account.serverId == inServerId && account.connection == inConnection && account.lease == inLease)
                    return id;
            return 0;
        }

        bool Renew(std::uint64_t inId, const std::string& inServerId, const std::string& inConnection,
            const std::string& inLease)
        {
            std::lock_guard lock(mutex);
            Prune();
            const auto found = accounts.find(inId);
            if (found == accounts.end()) return false;
            auto& account = found->second;
            if (account.serverId != inServerId || account.connection != inConnection || account.lease != inLease
                || account.draining || account.leaseExpires <= Clock::now()) return false;
            account.leaseExpires = Clock::now() + std::chrono::seconds(15);
            return true;
        }

        void Release(const std::string& inServerId, const std::string& inConnection, const std::string& inLease)
        {
            std::lock_guard lock(mutex);
            for (auto& [id, account] : accounts)
                if (account.serverId == inServerId && account.connection == inConnection && account.lease == inLease)
                { account.lease.clear(); account.connection.clear(); account.serverId.clear(); account.draining = false; return; }
        }

        void Logout(const std::string& inToken)
        {
            std::lock_guard lock(mutex);
            const auto found = tokens.find(inToken);
            if (found == tokens.end()) return;
            auto& account = accounts.at(found->second);
            account.token.clear();
            account.expires = Clock::now();
            account.draining = true;
            tokens.erase(found);
        }

        void Revoke(std::uint64_t inAccountId)
        {
            std::lock_guard lock(mutex);
            const auto found = accounts.find(inAccountId);
            if (found == accounts.end()) return;
            auto& account = found->second;
            tokens.erase(account.token);
            account.token.clear();
            account.expires = Clock::now();
            account.draining = true;
            std::erase_if(tickets, [inAccountId](const auto& inItem) { return inItem.second.accountId == inAccountId; });
        }
    };
}
