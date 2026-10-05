#pragma once

#include "../../Shared/AuthTransport.h"
#include <asio.hpp>
#include <atomic>
#include <functional>
#include <optional>

namespace TownServer::Authentication
{
    /** Bounded HTTPS worker queue; callbacks return to the caller's executor with a work guard.
     * Requests are never automatically retried: consumption/release may already have occurred.
     */
    class AuthControlClient final
    {
        std::string host = ActionRPG::Authentication::RequiredEnvironment("ACTIONRPG_AUTH_HOST");
        std::string ca = ActionRPG::Authentication::RequiredEnvironment("ACTIONRPG_AUTH_CA_FILE");
        std::string serverId = ActionRPG::Authentication::RequiredEnvironment("ACTIONRPG_TOWN_ID");
        std::string key = ActionRPG::Authentication::RequiredEnvironment("ACTIONRPG_TOWN_AUTH_KEY");
        asio::thread_pool workers{2};
        std::atomic_size_t pending{};
    public:
        AuthControlClient()
        {
            if (!ActionRPG::Authentication::IsToken(key)) throw std::runtime_error("Invalid town authentication key.");
        }
        ~AuthControlClient() { workers.join(); }
        using Handler = std::function<void(std::optional<nlohmann::json>)>;
        void Request(std::string inPath, nlohmann::json inBody, asio::any_io_executor inExecutor, Handler inHandler)
        {
            auto guard = asio::make_work_guard(inExecutor);
            if (pending.fetch_add(1) >= 64)
            {
                pending.fetch_sub(1);
                asio::post(inExecutor, [handler = std::move(inHandler), guard = std::move(guard)]() mutable { handler({}); });
                return;
            }
            asio::post(workers, [this, path = std::move(inPath), body = std::move(inBody), inExecutor,
                handler = std::move(inHandler), guard = std::move(guard)]() mutable
            {
                std::optional<nlohmann::json> result;
                try
                {
                    httplib::SSLClient client(host, 8443);
                    ActionRPG::Authentication::ConfigureClient(client, ca);
                    const httplib::Headers headers{{"X-Town-Id", serverId}, {"X-Town-Key", key}};
                    const auto response = client.Post(path, headers, body.dump(), "application/json");
                    if (response && response->status == 200 && response->body.size() <= 32768)
                        result = nlohmann::json::parse(response->body);
                }
                catch (...) {}
                pending.fetch_sub(1);
                asio::post(inExecutor, [handler = std::move(handler), result = std::move(result),
                    guard = std::move(guard)]() mutable { handler(std::move(result)); });
            });
        }
    };
}
