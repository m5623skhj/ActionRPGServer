#include "NetworkConstants.h"
#include "RoomControlTcpServer.h"
#include "TownClientTcpServer.h"
#include "DungeonCatalog.h"
#include "TownInstance.h"
#include "TownMap.h"

#include <asio.hpp>
#include <Windows.h>

#include <array>
#include <algorithm>
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

    std::vector<TownServer::Domain::TownMap> LoadTownMaps(const std::filesystem::path& inDataDirectory)
    {
        std::vector<std::filesystem::path> mapPaths;
        const std::filesystem::path defaultMapPath = inDataDirectory / "TownMap.json";
        if (std::filesystem::is_regular_file(defaultMapPath))
        {
            mapPaths.push_back(defaultMapPath);
        }
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(inDataDirectory))
        {
            const std::filesystem::path& path = entry.path();
            if (!entry.is_regular_file() || path == defaultMapPath || path.extension() != ".json")
            {
                continue;
            }
            const std::string stem = path.stem().string();
            if (stem.starts_with("TownMap"))
            {
                mapPaths.push_back(path);
            }
        }
        if (mapPaths.size() > 1)
        {
            std::sort(mapPaths.begin() + 1, mapPaths.end());
        }

        std::vector<TownServer::Domain::TownMap> maps;
        maps.reserve(mapPaths.size());
        for (const std::filesystem::path& path : mapPaths)
        {
            maps.push_back(TownServer::Domain::TownMap::Load(path));
        }
        return maps;
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
        const std::filesystem::path dataDirectory = GetExecutableDirectory() / "Data";
        std::vector<TownServer::Domain::TownMap> townMaps = LoadTownMaps(dataDirectory);
        TownServer::Domain::DungeonCatalog dungeonCatalog = TownServer::Domain::DungeonCatalog::Load(
            dataDirectory / "DungeonCatalog.json");
        std::shared_ptr<TownServer::Domain::TownInstance> townInstance =
            std::make_shared<TownServer::Domain::TownInstance>(
                ioContext, std::move(townMaps), std::move(dungeonCatalog));
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
