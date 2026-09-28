#include "DungeonProtocol.h"
#include "DungeonSession.h"
#include "RoomManager.h"
#include "TownControlClient.h"

#include <MultiSocketRUDPCore.h>

#include <asio.hpp>
#include <Windows.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    constexpr std::uint16_t DEFAULT_TOWN_CONTROL_PORT = 7780;
    constexpr std::uint32_t DEFAULT_MAX_ROOM_COUNT = 1000;
    constexpr std::uint32_t MAX_ROOM_COUNT = 100000;
    constexpr std::size_t DEFAULT_IO_THREAD_COUNT = 4;
    constexpr std::size_t MAX_IO_THREAD_COUNT = 64;
    constexpr auto ENTER_TIMEOUT = std::chrono::seconds(30);

    struct SessionBrokerEndpoint
    {
        std::string address;
        std::uint16_t port{};
    };

    std::filesystem::path GetExecutableDirectory()
    {
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length == path.size())
        {
            throw std::runtime_error("Unable to resolve the executable directory.");
        }
        return std::filesystem::path(std::wstring_view(path.data(), length)).parent_path();
    }

    template <typename T>
    bool ParseNumber(const std::string_view inText, T& outValue)
    {
        const auto [end, error] = std::from_chars(inText.data(), inText.data() + inText.size(), outValue);
        return error == std::errc{} && end == inText.data() + inText.size();
    }

    std::wstring ReadUtf16File(const std::filesystem::path& inPath)
    {
        std::ifstream file(inPath, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("Unable to open the session broker option file.");
        }
        const std::vector<char> bytes(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (bytes.size() < 2 || bytes.size() % 2 != 0
            || static_cast<std::uint8_t>(bytes[0]) != 0xFF
            || static_cast<std::uint8_t>(bytes[1]) != 0xFE)
        {
            throw std::runtime_error("The session broker option file must be UTF-16 LE with BOM.");
        }

        std::wstring result;
        result.reserve((bytes.size() - 2) / 2);
        for (std::size_t index = 2; index < bytes.size(); index += 2)
        {
            result.push_back(static_cast<wchar_t>(
                static_cast<std::uint8_t>(bytes[index])
                | (static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[index + 1])) << 8)));
        }
        return result;
    }

    std::wstring ReadOptionValue(const std::wstring& inText, const std::wstring_view inKey)
    {
        const std::size_t keyPosition = inText.find(inKey);
        const std::size_t equalsPosition = keyPosition == std::wstring::npos
            ? std::wstring::npos : inText.find(L'=', keyPosition + inKey.size());
        if (equalsPosition == std::wstring::npos)
        {
            throw std::runtime_error("A required session broker option is missing.");
        }
        std::size_t begin = inText.find_first_not_of(L" \t\r\n", equalsPosition + 1);
        if (begin == std::wstring::npos)
        {
            throw std::runtime_error("A session broker option has no value.");
        }
        if (inText[begin] == L'\"')
        {
            const std::size_t end = inText.find(L'\"', begin + 1);
            if (end == std::wstring::npos)
            {
                throw std::runtime_error("A session broker string option is malformed.");
            }
            return inText.substr(begin + 1, end - begin - 1);
        }
        const std::size_t end = inText.find_first_of(L" \t\r\n}", begin);
        return inText.substr(begin, end - begin);
    }

    std::string ToUtf8(const std::wstring& inText)
    {
        if (inText.empty())
        {
            return {};
        }
        const int size = WideCharToMultiByte(CP_UTF8, 0, inText.data(),
            static_cast<int>(inText.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0)
        {
            throw std::runtime_error("Unable to encode the session broker address.");
        }
        std::string result(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, inText.data(), static_cast<int>(inText.size()),
            result.data(), size, nullptr, nullptr);
        return result;
    }

    SessionBrokerEndpoint LoadSessionBrokerEndpoint(const std::filesystem::path& inPath)
    {
        const std::wstring text = ReadUtf16File(inPath);
        const std::wstring address = ReadOptionValue(text, L"CORE_IP");
        const std::wstring portText = ReadOptionValue(text, L"SESSION_BROKER_PORT");
        unsigned long parsedPort{};
        try
        {
            parsedPort = std::stoul(portText);
        }
        catch (const std::exception&)
        {
            throw std::runtime_error("The session broker port is invalid.");
        }
        if (address.empty() || parsedPort == 0 || parsedPort > 65535)
        {
            throw std::runtime_error("The session broker endpoint is invalid.");
        }
        return SessionBrokerEndpoint{ ToUtf8(address), static_cast<std::uint16_t>(parsedPort) };
    }
}

int main(const int inArgumentCount, char* inArguments[])
{
    std::string townHost = "127.0.0.1";
    std::uint16_t townControlPort = DEFAULT_TOWN_CONTROL_PORT;
    ActionRPG::RoomControlProtocol::RoomServerId roomServerId = 1;
    std::uint32_t maxRoomCount = DEFAULT_MAX_ROOM_COUNT;
    std::size_t ioThreadCount = DEFAULT_IO_THREAD_COUNT;
    std::filesystem::path coreOptionPath;
    std::filesystem::path brokerOptionPath;

    if (inArgumentCount > 1)
    {
        townHost = inArguments[1];
    }
    if (inArgumentCount > 2
        && (!ParseNumber(std::string_view(inArguments[2]), townControlPort) || townControlPort == 0))
    {
        std::cerr << "Invalid TownServer control port.\n";
        return 1;
    }
    if (inArgumentCount > 3
        && (!ParseNumber(std::string_view(inArguments[3]), roomServerId) || roomServerId == 0
            || roomServerId > std::numeric_limits<std::uint32_t>::max()))
    {
        std::cerr << "Invalid room server id.\n";
        return 1;
    }
    if (inArgumentCount > 4
        && (!ParseNumber(std::string_view(inArguments[4]), maxRoomCount)
            || maxRoomCount == 0 || maxRoomCount > MAX_ROOM_COUNT))
    {
        std::cerr << "Invalid max room count.\n";
        return 1;
    }
    if (inArgumentCount > 5
        && (!ParseNumber(std::string_view(inArguments[5]), ioThreadCount)
            || ioThreadCount == 0 || ioThreadCount > MAX_IO_THREAD_COUNT))
    {
        std::cerr << "Invalid I/O thread count.\n";
        return 1;
    }
    if (inArgumentCount > 6)
    {
        coreOptionPath = inArguments[6];
    }
    if (inArgumentCount > 7)
    {
        brokerOptionPath = inArguments[7];
    }

    try
    {
        asio::io_context ioContext;
        auto workGuard = asio::make_work_guard(ioContext);
        const std::filesystem::path executableDirectory = GetExecutableDirectory();
        if (coreOptionPath.empty())
        {
            coreOptionPath = executableDirectory / "ServerOptionFile" / "CoreOption.txt";
        }
        if (brokerOptionPath.empty())
        {
            brokerOptionPath = executableDirectory / "ServerOptionFile" / "SessionBrokerOption.txt";
        }
        const SessionBrokerEndpoint brokerEndpoint = LoadSessionBrokerEndpoint(brokerOptionPath);
        std::shared_ptr<GameRoomServer::RoomManager> roomManager =
            std::make_shared<GameRoomServer::RoomManager>(ioContext, roomServerId, maxRoomCount,
                brokerEndpoint.address, brokerEndpoint.port, ENTER_TIMEOUT);
        std::shared_ptr<GameRoomServer::TownControlClient> townControlClient =
            std::make_shared<GameRoomServer::TownControlClient>(
                ioContext, roomManager, roomServerId, maxRoomCount);

        MultiSocketRUDPCore rudpCore(L"MY", L"DevServerCert");
        GameRoomServer::RegisterDungeonPackets();
        const std::weak_ptr<GameRoomServer::RoomManager> weakRoomManager = roomManager;
        if (!rudpCore.StartServer(coreOptionPath.wstring(), brokerOptionPath.wstring(),
            [weakRoomManager](MultiSocketRUDPCore& inCore) -> RUDPSession*
            {
                return new GameRoomServer::DungeonSession(inCore, weakRoomManager);
            }, true))
        {
            std::cerr << "Unable to start RUDP server.\n";
            return 1;
        }

        std::atomic_bool shutdownRequested{};
        const auto requestShutdown = [&]()
        {
            if (shutdownRequested.exchange(true))
            {
                return;
            }
            townControlClient->Stop();
            roomManager->Stop();
            workGuard.reset();
        };
        rudpCore.SetFatalErrorHandler([&ioContext, &requestShutdown](const ServerFatalError&)
        {
            asio::post(ioContext, requestShutdown);
        });

        asio::signal_set shutdownSignals(ioContext, SIGINT, SIGTERM);
        shutdownSignals.async_wait([&requestShutdown](const asio::error_code& inError, const int)
        {
            if (!inError)
            {
                requestShutdown();
            }
        });

        townControlClient->Start(townHost, townControlPort);
        std::cout << "GameRoomServer " << roomServerId << " started. RUDP session broker "
            << brokerEndpoint.address << ':' << brokerEndpoint.port << ".\n";

        std::vector<std::thread> ioThreads;
        ioThreads.reserve(ioThreadCount - 1);
        for (std::size_t index = 1; index < ioThreadCount; ++index)
        {
            ioThreads.emplace_back([&ioContext]()
            {
                ioContext.run();
            });
        }
        ioContext.run();
        for (std::thread& ioThread : ioThreads)
        {
            ioThread.join();
        }

        rudpCore.StopServer();
    }
    catch (const std::exception& inException)
    {
        std::cerr << "GameRoomServer failed: " << inException.what() << '\n';
        return 1;
    }

    return 0;
}
