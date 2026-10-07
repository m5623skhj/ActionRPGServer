#include "SessionRegistry.h"
#include "GoogleIdTokenVerifier.h"
#include "Database/LoginGoogleAccountProcedure.h"
#include "Database/GetAuthAccountStatusProcedure.h"
#include "Database/LoginSchemaVerifier.h"
#include "../Shared/Database/OdbcDatabase.h"
#include <asio.hpp>
#include <future>
#include <iostream>
#include <thread>
#include <type_traits>

namespace
{
    using Json = nlohmann::json;
    using namespace ActionRPG::Authentication;

    template<typename TPool>
    TPool* CreateHttpPool()
    {
        // Older httplib uses (threads, queue limit); newer releases add maximum threads.
        if constexpr (std::is_constructible_v<TPool, std::size_t, std::size_t, std::size_t>)
            return new TPool(8, 8, 64);
        else return new TPool(8, 64);
    }

    template<typename TProcedure>
    auto Execute(ActionRPG::Database::OdbcDatabase& inDatabase, asio::io_context& inIo,
        std::unique_ptr<TProcedure> inProcedure)
    {
        using Result = ActionRPG::Database::ProcedureResult<typename TProcedure::Response>;
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        inDatabase.Run(std::move(inProcedure), inIo.get_executor(),
            [promise](Result inResult) { promise->set_value(std::move(inResult)); });
        // HTTP worker waits; DB completion has its own live I/O thread. No retry on timeout.
        if (future.wait_for(std::chrono::seconds(15)) != std::future_status::ready)
            throw std::runtime_error("Database unavailable.");
        return future.get();
    }

    std::string Bearer(const httplib::Request& inRequest)
    {
        const auto value = inRequest.get_header_value("Authorization");
        if (!value.starts_with("Bearer ") || !IsToken(std::string_view(value).substr(7)))
            throw std::runtime_error("Invalid session.");
        return value.substr(7);
    }
}

int main()
{
    try
    {
        const auto certificate = RequiredEnvironment("ACTIONRPG_AUTH_TLS_CERT");
        const auto privateKey = RequiredEnvironment("ACTIONRPG_AUTH_TLS_KEY");
        const auto ca = RequiredEnvironment("ACTIONRPG_AUTH_CA_FILE");
        const auto clientId = RequiredEnvironment("ACTIONRPG_GOOGLE_CLIENT_ID");
        const auto registry = Json::parse(RequiredEnvironment("ACTIONRPG_AUTH_TOWN_REGISTRY"));
        if (!registry.is_object() || registry.empty() || registry.size() > 128)
            throw std::runtime_error("Invalid town registry.");
        for (const auto& [id, key] : registry.items())
            if (id.empty() || id.size() > 64 || !key.is_string() || !IsToken(key.get<std::string>()))
                throw std::runtime_error("Invalid town identity.");

        asio::io_context io;
        auto guard = asio::make_work_guard(io);
        std::jthread ioThread([&io] { io.run(); });
        // This guard also runs on configuration/listen failures before joining the I/O thread.
        struct IoShutdown { asio::io_context& io; ~IoShutdown() { io.stop(); } } shutdown{io};
        auto database = std::make_shared<ActionRPG::Database::OdbcDatabase>(
            ActionRPG::Database::DatabaseOptions::FromEnvironment());
        auto verificationPromise = std::make_shared<std::promise<ActionRPG::Database::SchemaVerificationResult>>();
        auto verificationFuture = verificationPromise->get_future();
        ActionRPG::Database::LoginSchemaVerifier::Verify(database, io.get_executor(),
            [verificationPromise](auto inResult) { verificationPromise->set_value(std::move(inResult)); });
        ActionRPG::Database::SchemaVerificationResult verification;
        if (verificationFuture.wait_for(std::chrono::seconds(15)) == std::future_status::ready)
            verification = verificationFuture.get();
        const bool schemaReady = verification.IsReady() && verification.schema->Matches(database);
        if (!schemaReady)
        {
            std::cerr << "Auth login/admission disabled: account database verification failed. stage="
                << verification.stage;
            if (verification.databaseError)
            {
                const auto& error = *verification.databaseError;
                std::cerr << "; database_error=" << static_cast<int>(error.code)
                    << "; SQLSTATE=" << (error.sqlState.empty() ? "unavailable" : error.sqlState)
                    << "; native_code=" << error.nativeCode
                    // SchemaHistoryProcedure and ODBC supply fixed context plus numeric metadata only.
                    << "; context=" << error.message;
            }
            std::cerr << '\n';
        }

        AuthServer::SessionRegistry sessions;
        AuthServer::GoogleIdTokenVerifier google(clientId, ca);
        httplib::SSLServer server(certificate.c_str(), privateKey.c_str());
        if (!server.is_valid()) throw std::runtime_error("Invalid HTTPS certificate configuration.");
        server.new_task_queue = [] { return CreateHttpPool<httplib::ThreadPool>(); };
        server.set_payload_max_length(32768);
        server.set_read_timeout(5, 0);
        server.set_write_timeout(5, 0);
        server.set_keep_alive_max_count(8);
        auto activeAccount = [&](std::uint64_t inId)
        {
            if (inId == 0) return false;
            auto procedure = std::make_unique<ActionRPG::Database::GetAuthAccountStatusProcedure>();
            procedure->req.accountId = inId;
            const auto result = Execute(*database, io, std::move(procedure));
            if (result.IsSuccess() && result.response->resultCode != 0) sessions.Revoke(inId);
            return result.IsSuccess() && result.response->resultCode == 0;
        };

        auto add = [&](const char* inPath, bool inInternal, auto inHandler)
        {
            server.Post(inPath, [&, inInternal, handler = std::move(inHandler)](const httplib::Request& request,
                httplib::Response& response)
            {
                response.set_header("Cache-Control", "no-store");
                try
                {
                    const auto body = Json::parse(request.body);
                    if (!body.is_object()) throw std::runtime_error("Invalid request.");
                    std::string serverId;
                    if (inInternal)
                    {
                        serverId = request.get_header_value("X-Town-Id");
                        const auto key = request.get_header_value("X-Town-Key");
                        if (!registry.contains(serverId) || !EqualSecret(key, registry.at(serverId).get<std::string>()))
                        { response.status = 401; response.set_content("{}", "application/json"); return; }
                    }
                    // Releases remain available during schema failure to clear old ownership safely.
                    if (!schemaReady && request.path != "/internal/release" && request.path != "/v1/logout")
                    { response.status = 503; response.set_content("{}", "application/json"); return; }
                    response.set_content(handler(request, body, serverId).dump(), "application/json");
                }
                catch (...)
                {
                    // Never log JWTs, credentials, DB connection strings, or exception payloads.
                    response.status = 403;
                    response.set_content("{}", "application/json");
                }
            });
        };
        add("/v1/challenges", false, [&](const auto&, const auto&, const auto&) { return sessions.ChallengeLogin(); });
        add("/v1/login", false, [&](const auto&, const Json& body, const auto&)
        {
            const auto nonce = sessions.ConsumeChallenge(body.at("challengeId").get<std::string>());
            const auto subject = google.Verify(body.at("idToken").get<std::string>(), nonce);
            auto procedure = std::make_unique<ActionRPG::Database::LoginGoogleAccountProcedure>();
            procedure->req.subject.assign(subject.begin(), subject.end());
            const auto result = Execute(*database, io, std::move(procedure));
            if (!result.IsSuccess()) throw std::runtime_error("Login denied.");
            if (result.response->resultCode != 0)
            {
                sessions.Revoke(result.response->accountId);
                throw std::runtime_error("Login denied.");
            }
            return sessions.Login(result.response->accountId);
        });
        add("/v1/tickets", false, [&](const auto& request, const Json& body, const auto&)
        {
            const auto token = Bearer(request);
            const auto target = body.at("serverId").get<std::string>();
            if (!registry.contains(target) || !activeAccount(sessions.Resolve(token))) throw std::runtime_error("Admission denied.");
            return sessions.Issue(token, target);
        });
        add("/v1/logout", false, [&](const auto& request, const auto&, const auto&)
        { sessions.Logout(Bearer(request)); return Json::object(); });
        add("/internal/consume", true, [&](const auto&, const Json& body, const std::string& serverId)
        {
            const auto ticket = body.at("ticket").get<std::string>();
            const auto connection = body.at("connection").get<std::string>();
            if (!IsToken(ticket) || !IsToken(connection) || !activeAccount(sessions.ResolveTicket(ticket, serverId)))
                throw std::runtime_error("Admission denied.");
            return sessions.Consume(ticket, serverId, connection);
        });
        add("/internal/renew", true, [&](const auto&, const Json& body, const std::string& serverId)
        {
            const auto connection = body.at("connection").get<std::string>();
            const auto lease = body.at("lease").get<std::string>();
            const auto id = sessions.ResolveLease(serverId, connection, lease);
            const bool valid = activeAccount(id) && sessions.Renew(id, serverId, connection, lease);
            return Json{{"valid", valid}, {"expiresIn", valid ? 15 : 0}};
        });
        add("/internal/release", true, [&](const auto&, const Json& body, const std::string& serverId)
        {
            sessions.Release(serverId, body.at("connection").get<std::string>(), body.at("lease").get<std::string>());
            return Json::object();
        });
        std::cout << "AuthServer HTTPS listening on port 8443.\n";
        const bool listened = server.listen("0.0.0.0", 8443);
        database->Stop();
        guard.reset();
        io.stop();
        return listened ? 0 : 1;
    }
    catch (...) { std::cerr << "AuthServer stopped: invalid configuration or unavailable dependency.\n"; return 1; }
}
