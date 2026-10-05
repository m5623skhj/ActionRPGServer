#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>
#include <utility>

namespace ActionRPG::Network
{
    class TcpSession final : public std::enable_shared_from_this<TcpSession>
    {
    public:
        using CloseHandler = std::function<void(std::uint64_t)>;
        using ReceiveHandler = std::function<void(std::vector<std::uint8_t>)>;

        TcpSession(std::uint64_t inSessionId, asio::ip::tcp::socket inSocket, CloseHandler inCloseHandler,
            std::shared_ptr<asio::ssl::context> inTlsContext = {});
        [[nodiscard]] bool IsSecure() const noexcept { return tls != nullptr; }

        void Start();
        void Stop();
        void Send(std::vector<std::uint8_t> inPacketBody);
        void SetReceiveHandler(ReceiveHandler inReceiveHandler);

        [[nodiscard]] std::uint64_t GetSessionId() const noexcept;

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
        std::shared_ptr<asio::ssl::context> tlsContext;
        std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> tls;
        asio::steady_timer handshakeDeadline;
        template<typename TBuffer, typename THandler>
        void Read(const TBuffer& inBuffer, THandler inHandler)
        {
            if (tls) asio::async_read(*tls, inBuffer, std::move(inHandler));
            else asio::async_read(socket, inBuffer, std::move(inHandler));
        }
        template<typename TBuffer, typename THandler>
        void Write(const TBuffer& inBuffer, THandler inHandler)
        {
            if (tls) asio::async_write(*tls, inBuffer, std::move(inHandler));
            else asio::async_write(socket, inBuffer, std::move(inHandler));
        }
        CloseHandler closeHandler;
        ReceiveHandler receiveHandler;
        std::array<std::uint8_t, 4> receiveHeader{};
        std::vector<std::uint8_t> receiveBody;
        std::deque<std::vector<std::uint8_t>> sendQueue;
        std::size_t queuedSendBytes = 0;
        bool writeInProgress = false;
        bool stopped = false;
    };
}
