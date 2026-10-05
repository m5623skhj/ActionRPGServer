#pragma once

#include "../Shared/AuthTransport.h"
#include <jwt-cpp/traits/nlohmann-json/defaults.h>
#include <chrono>
#include <algorithm>
#include <mutex>
#include <regex>
#include <unordered_map>

namespace AuthServer
{
    class GoogleIdTokenVerifier final
    {
        std::string clientId;
        std::string caPath;
        std::mutex mutex;
        std::unordered_map<std::string, std::string> keys;
        std::chrono::steady_clock::time_point expires{};
        std::chrono::steady_clock::time_point nextRefresh{};

        std::string GetKey(const std::string& inKid)
        {
            // Refresh is serialized on HTTP workers, never on the town/game strand.
            std::lock_guard lock(mutex);
            auto now = std::chrono::steady_clock::now();
            if (now < expires && keys.contains(inKid)) return keys.at(inKid);
            if (now < nextRefresh) throw std::runtime_error("Key refresh throttled.");
            nextRefresh = now + std::chrono::seconds(30);
            httplib::SSLClient client("www.googleapis.com", 443);
            ActionRPG::Authentication::ConfigureClient(client, caPath);
            std::string body;
            auto response = client.Get("/oauth2/v3/certs", [&](const char* inData, std::size_t inSize)
            {
                if (body.size() + inSize > 1024 * 1024) return false;
                body.append(inData, inSize);
                return true;
            });
            if (!response || response->status != 200) throw std::runtime_error("Key service unavailable.");
            std::smatch match;
            const auto cache = response->get_header_value("Cache-Control");
            if (!std::regex_search(cache, match, std::regex("max-age=([0-9]{1,8})")))
                throw std::runtime_error("Invalid key cache lifetime.");
            const auto lifetime = std::min<unsigned long>(std::stoul(match[1]), 86400);
            if (lifetime == 0) throw std::runtime_error("Expired keys.");
            const auto document = nlohmann::json::parse(body);
            const auto& entries = document.at("keys");
            if (!entries.is_array() || entries.empty() || entries.size() > 32)
                throw std::runtime_error("Invalid key set.");
            std::unordered_map<std::string, std::string> replacement;
            for (const auto& entry : entries)
            {
                if (entry.at("kty") != "RSA" || entry.at("alg") != "RS256" || entry.at("use") != "sig") continue;
                const auto kid = entry.at("kid").get<std::string>();
                const auto modulus = entry.at("n").get<std::string>();
                const auto exponent = entry.at("e").get<std::string>();
                if (kid.empty() || kid.size() > 256 || modulus.size() < 256 || modulus.size() > 2048
                    || exponent.empty() || exponent.size() > 16 || replacement.contains(kid))
                    throw std::runtime_error("Invalid signing key.");
                replacement.emplace(kid, jwt::helper::create_public_key_from_rsa_components(modulus, exponent));
            }
            keys = std::move(replacement);
            expires = std::chrono::steady_clock::now() + std::chrono::seconds(lifetime);
            if (!keys.contains(inKid)) throw std::runtime_error("Unknown signing key.");
            return keys.at(inKid);
        }

    public:
        GoogleIdTokenVerifier(std::string inClientId, std::string inCaPath)
            : clientId(std::move(inClientId)), caPath(std::move(inCaPath)) {}

        std::string Verify(const std::string& inToken, const std::string& inNonce)
        {
            if (inToken.empty() || inToken.size() > 16384) throw std::runtime_error("Invalid identity token.");
            const auto decoded = jwt::decode<jwt::traits::nlohmann_json>(inToken);
            const auto header = nlohmann::json::parse(decoded.get_header());
            if (header.at("alg") != "RS256" || header.contains("crit") || header.contains("b64"))
                throw std::runtime_error("Unsupported signature.");
            const auto pem = GetKey(header.at("kid").get<std::string>());
            auto verifier = jwt::verify<jwt::traits::nlohmann_json>();
            verifier.allow_algorithm(jwt::algorithm::rs256(pem)).with_audience(clientId);
            verifier.verify(decoded);
            // Only inspect identity claims after cryptographic verification.
            const auto claims = nlohmann::json::parse(decoded.get_payload());
            const auto issuer = claims.at("iss").get<std::string>();
            if (issuer != "accounts.google.com" && issuer != "https://accounts.google.com")
                throw std::runtime_error("Invalid issuer.");
            if (claims.contains("azp") && claims.at("azp") != clientId)
                throw std::runtime_error("Invalid authorized party.");
            if (claims.at("aud").is_array() && claims.at("aud").size() > 1 && !claims.contains("azp"))
                throw std::runtime_error("Missing authorized party.");
            const auto nonce = claims.at("nonce").get<std::string>();
            if (!ActionRPG::Authentication::EqualSecret(nonce, inNonce)) throw std::runtime_error("Invalid nonce.");
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (!claims.at("exp").is_number_integer() || claims.at("exp").get<std::int64_t>() <= now
                || !claims.at("iat").is_number_integer() || claims.at("iat").get<std::int64_t>() > now + 30)
                throw std::runtime_error("Invalid identity lifetime.");
            const auto subject = claims.at("sub").get<std::string>();
            if (subject.empty() || subject.size() > 255) throw std::runtime_error("Invalid subject.");
            for (const unsigned char value : subject)
                if (value == 0 || value > 127) throw std::runtime_error("Invalid subject.");
            return subject;
        }
    };
}
