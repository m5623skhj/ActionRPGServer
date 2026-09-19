#include "NetworkConstants.h"
#include "TcpServer.h"

#include <asio.hpp>

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
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
    std::size_t ioThreadCount = DEFAULT_IO_THREAD_COUNT;

    if (inArgumentCount > 1 && (!ParseNumber(std::string_view(inArguments[1]), port) || port == 0))
    {
        std::cerr << "Invalid port. Usage: TownServer [port] [io-thread-count]\n";
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
        TcpServer server(ioContext, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port));
        asio::signal_set shutdownSignals(ioContext, SIGINT, SIGTERM);

        shutdownSignals.async_wait([&server](const asio::error_code& inError, const int)
        {
            if (!inError)
            {
                server.Stop();
            }
        });

        server.Start();
        std::cout << "TownServer listening on TCP port " << port
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
