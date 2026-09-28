#include "NetworkConstants.h"
#include "RoomControlTcpServer.h"
#include "TownClientTcpServer.h"
#include "TownInstance.h"
#include "TownMap.h"

#include <asio.hpp>
#include <Windows.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
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
}

int main(const int inArgumentCount, char* inArguments[])
{
    using namespace TownServer::Network;

    std::uint16_t port = DEFAULT_PORT;
    std::uint16_t roomControlPort = DEFAULT_ROOM_CONTROL_PORT;
    std::size_t ioThreadCount = DEFAULT_IO_THREAD_COUNT;

    if (inArgumentCount > 1 && (!ParseNumber(std::string_view(inArguments[1]), port) || port == 0))
    {
        std::cerr << "Invalid port. Usage: TownServer [client-port] [io-thread-count] [room-control-port]\n";
        return 1;
    }

    if (inArgumentCount > 3
        && (!ParseNumber(std::string_view(inArguments[3]), roomControlPort) || roomControlPort == 0))
    {
        std::cerr << "Invalid room control port.\n";
        return 1;
    }

    if (inArgumentCount > 2
        && (!ParseNumber(std::string_view(inArguments[2]), ioThreadCount)
            || ioThreadCount == 0
            || ioThreadCount > MAX_IO_THREAD_COUNT))
    {
        std::cerr << "Invalid I/O thread count. Valid range: 1-" << MAX_IO_THREAD_COUNT << '\n';
        return 1;
    }

    try
    {
        asio::io_context ioContext;
        TownServer::Domain::TownMap townMap = TownServer::Domain::TownMap::Load(
            GetExecutableDirectory() / "Data" / "TownMap.json");
        std::shared_ptr<TownServer::Domain::TownInstance> townInstance =
            std::make_shared<TownServer::Domain::TownInstance>(ioContext, std::move(townMap));
        std::shared_ptr<RoomControlTcpServer> roomControlServer = std::make_shared<RoomControlTcpServer>(
            ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), roomControlPort), townInstance);
        TownClientTcpServer server(
            ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port), townInstance, roomControlServer);
        asio::signal_set shutdownSignals(ioContext, SIGINT, SIGTERM);

        shutdownSignals.async_wait([&server, &roomControlServer](const asio::error_code& inError, const int)
        {
            if (!inError)
            {
                server.Stop();
                roomControlServer->Stop();
            }
        });

        roomControlServer->Start();
        server.Start();
        std::cout << "TownServer listening on TCP port " << port
            << " and room control port " << roomControlPort
            << " with " << ioThreadCount << " I/O threads.\n";

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
    }
    catch (const std::exception& inException)
    {
        std::cerr << "TownServer failed: " << inException.what() << '\n';
        return 1;
    }

    return 0;
}
