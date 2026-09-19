#pragma once

#include <asio.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace TownServer::Network
{
    class TcpSession;

    class TcpServer final
    {
    public:
        TcpServer(asio::io_context& inIoContext, const asio::ip::tcp::endpoint& inEndpoint);

        void Start();
        void Stop();

    private:
        void AcceptNext();
        void RemoveSession(std::uint64_t inSessionId);

        asio::io_context& ioContext;
        asio::ip::tcp::endpoint endpoint;
        asio::ip::tcp::acceptor acceptor;
        std::unordered_map<std::uint64_t, std::shared_ptr<TcpSession>> sessions;
        std::uint64_t nextSessionId = 1;
        bool running = false;
    };
}
