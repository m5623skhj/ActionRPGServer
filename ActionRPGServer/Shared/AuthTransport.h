#pragma once

#if defined(_MSC_VER)
// cpp-httplib 0.40.0's compatibility wrappers call its own deprecated APIs.
// Keep the suppression inside this dependency header; restore checks below.
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
#include <httplib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ActionRPG::Authentication
{
    inline std::string RequiredEnvironment(const char* inName)
    {
        const std::string_view name(inName);
        const std::wstring wideName(name.begin(), name.end());
        const DWORD length = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
        if (length == 0 || length > 32767)
            throw std::runtime_error(std::string("Missing configuration: ") + inName);
        std::wstring value(length, L'\0');
        const DWORD copied = GetEnvironmentVariableW(wideName.c_str(), value.data(), length);
        if (copied == 0 || copied >= length) throw std::runtime_error("Configuration changed while reading.");
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), copied,
            nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) throw std::runtime_error("Invalid configuration encoding.");
        std::string result(bytes, '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), copied,
            result.data(), bytes, nullptr, nullptr) != bytes) throw std::runtime_error("Invalid configuration encoding.");
        return result;
    }

    inline std::string RandomToken()
    {
        unsigned char bytes[32];
        if (RAND_bytes(bytes, sizeof(bytes)) != 1) throw std::runtime_error("Secure random generation failed.");
        constexpr char HEX[] = "0123456789abcdef";
        std::string token;
        token.reserve(64);
        for (const auto value : bytes) { token += HEX[value >> 4]; token += HEX[value & 15]; }
        return token;
    }

    inline bool IsToken(std::string_view inValue)
    {
        if (inValue.size() != 64) return false;
        for (const char value : inValue)
            if (!(value >= '0' && value <= '9') && !(value >= 'a' && value <= 'f')) return false;
        return true;
    }

    inline bool EqualSecret(std::string_view inLeft, std::string_view inRight)
    {
        return inLeft.size() == inRight.size() && !inLeft.empty()
            && CRYPTO_memcmp(inLeft.data(), inRight.data(), inLeft.size()) == 0;
    }

    inline void ConfigureClient(httplib::SSLClient& inClient, const std::string& inCaPath)
    {
        inClient.set_ca_cert_path(inCaPath.c_str());
        inClient.enable_server_certificate_verification(true);
        inClient.enable_server_hostname_verification(true);
        inClient.set_follow_location(false);
        inClient.set_connection_timeout(3, 0);
        inClient.set_read_timeout(3, 0);
        inClient.set_write_timeout(3, 0);
    }
}
