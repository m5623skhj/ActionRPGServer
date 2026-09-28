#pragma once

#include "../Shared/RoomControlProtocol.h"

#include <asio.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ActionRPG::Network
{
    class TcpSession;
}

namespace GameRoomServer
{
    class RoomManager;

    class TownControlClient final : public std::enable_shared_from_this<TownControlClient>
    {
    public:
        TownControlClient(asio::io_context& inIoContext, std::shared_ptr<RoomManager> inRoomManager,
            ActionRPG::RoomControlProtocol::RoomServerId inRoomServerId, std::uint32_t inMaxRoomCount);

        void Start(std::string inHost, std::uint16_t inPort);
        void Stop();
        void Send(std::vector<std::uint8_t> inPacket);

    private:
        void HandleResolved(const asio::error_code& inError,
            const asio::ip::tcp::resolver::results_type& inResults);
        void HandleConnected(const asio::error_code& inError);
        void HandlePacket(std::vector<std::uint8_t> inPacket);
        void HandleClosed();

        asio::strand<asio::io_context::executor_type> strand;
        asio::ip::tcp::resolver resolver;
        asio::ip::tcp::socket connectingSocket;
        std::shared_ptr<ActionRPG::Network::TcpSession> session;
        std::shared_ptr<RoomManager> roomManager;
        ActionRPG::RoomControlProtocol::RoomServerId roomServerId;
        std::uint32_t maxRoomCount;
        bool stopped{};
    };
}
