#pragma once

#include <asio.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace TownServer::Network
{
    class TcpSession final : public std::enable_shared_from_this<TcpSession>
    {
    public:
        using CloseHandler = std::function<void(std::uint64_t)>;

        TcpSession(std::uint64_t inSessionId, asio::ip::tcp::socket inSocket, CloseHandler inCloseHandler);

        void Start();
        void Stop();
        void Send(std::vector<std::uint8_t> inPacketBody);

    private:
        void ReadHeader();
        void ReadBody(std::uint32_t inBodySize);
        void HandlePacket();
        void QueuePacket(std::vector<std::uint8_t> inPacketBody);
        void WriteNext();
        void Close();

        static std::uint32_t DecodeBodySize(const std::array<std::uint8_t, 4>& inHeader);
        static void EncodeBodySize(std::uint32_t inBodySize, std::vector<std::uint8_t>& outPacket);

        std::uint64_t sessionId;
        asio::ip::tcp::socket socket;
        CloseHandler closeHandler;
        std::array<std::uint8_t, 4> receiveHeader{};
        std::vector<std::uint8_t> receiveBody;
        std::deque<std::vector<std::uint8_t>> sendQueue;
        std::size_t queuedSendBytes = 0;
        bool stopped = false;
    };
}
