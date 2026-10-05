#include "TcpSession.h"

#include "NetworkConstants.h"

#include <algorithm>
#include <utility>

namespace ActionRPG::Network
{
    TcpSession::TcpSession(
        const std::uint64_t inSessionId,
        asio::ip::tcp::socket inSocket,
        CloseHandler inCloseHandler, std::shared_ptr<asio::ssl::context> inTlsContext)
        : sessionId(inSessionId),
          socket(std::move(inSocket)),
          tlsContext(std::move(inTlsContext)),
          handshakeDeadline(socket.get_executor()),
          closeHandler(std::move(inCloseHandler))
    {
        if (tlsContext) tls = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket, *tlsContext);
    }

    void TcpSession::Start()
    {
        const std::shared_ptr<TcpSession> self = shared_from_this();
        asio::dispatch(socket.get_executor(), [self]()
        {
            if (self->stopped) return;
            if (!self->tls) { self->ReadHeader(); return; }
            self->handshakeDeadline.expires_after(std::chrono::seconds(10));
            self->handshakeDeadline.async_wait([self](const asio::error_code& inError)
            { if (!inError) self->Close(); });
            self->tls->async_handshake(asio::ssl::stream_base::server, [self](const asio::error_code& inError)
            {
                self->handshakeDeadline.cancel();
                if (inError || self->stopped) { self->Close(); return; }
                self->ReadHeader();
            });
        });
    }

    void TcpSession::Stop()
    {
        const std::shared_ptr<TcpSession> self = shared_from_this();
        asio::dispatch(socket.get_executor(), [self]()
        {
            self->Close();
        });
    }

    void TcpSession::Send(std::vector<std::uint8_t> inPacketBody)
    {
        const std::shared_ptr<TcpSession> self = shared_from_this();
        asio::dispatch(socket.get_executor(), [self, packetBody = std::move(inPacketBody)]() mutable
        {
            self->QueuePacket(std::move(packetBody));
        });
    }

    void TcpSession::SetReceiveHandler(ReceiveHandler inReceiveHandler)
    {
        receiveHandler = std::move(inReceiveHandler);
    }

    std::uint64_t TcpSession::GetSessionId() const noexcept
    {
        return sessionId;
    }

    void TcpSession::ReadHeader()
    {
        const std::shared_ptr<TcpSession> self = shared_from_this();
        Read(asio::buffer(receiveHeader), [self](const asio::error_code& inError, const std::size_t)
        {
            if (inError)
            {
                self->Close();
                return;
            }

            const std::uint32_t bodySize = DecodeBodySize(self->receiveHeader);
            if (bodySize > MAX_PACKET_BODY_SIZE)
            {
                self->Close();
                return;
            }

            self->ReadBody(bodySize);
        });
    }

    void TcpSession::ReadBody(const std::uint32_t inBodySize)
    {
        receiveBody.resize(inBodySize);
        if (receiveBody.empty())
        {
            HandlePacket();
            ReadHeader();
            return;
        }

        const std::shared_ptr<TcpSession> self = shared_from_this();
        Read(asio::buffer(receiveBody), [self](const asio::error_code& inError, const std::size_t)
        {
            if (inError)
            {
                self->Close();
                return;
            }

            self->HandlePacket();
            self->ReadHeader();
        });
    }

    void TcpSession::HandlePacket()
    {
        if (!receiveHandler)
        {
            Close();
            return;
        }
        receiveHandler(std::move(receiveBody));
    }

    void TcpSession::QueuePacket(std::vector<std::uint8_t> inPacketBody)
    {
        if (stopped || inPacketBody.size() > MAX_PACKET_BODY_SIZE)
        {
            return;
        }

        std::vector<std::uint8_t> framedPacket(PACKET_HEADER_SIZE + inPacketBody.size());
        EncodeBodySize(static_cast<std::uint32_t>(inPacketBody.size()), framedPacket);
        std::copy(inPacketBody.begin(), inPacketBody.end(), framedPacket.begin() + PACKET_HEADER_SIZE);

        if (queuedSendBytes + framedPacket.size() > MAX_QUEUED_SEND_BYTES)
        {
            Close();
            return;
        }

        queuedSendBytes += framedPacket.size();
        sendQueue.push_back(std::move(framedPacket));

        if (!writeInProgress)
        {
            WriteNext();
        }
    }

    void TcpSession::WriteNext()
    {
        if (stopped || writeInProgress || sendQueue.empty())
        {
            return;
        }

        const std::shared_ptr<TcpSession> self = shared_from_this();
        // Closing the socket cancels I/O but does not end the buffer's lifetime requirement.
        const auto packet = std::make_shared<std::vector<std::uint8_t>>(std::move(sendQueue.front()));
        sendQueue.pop_front();
        writeInProgress = true;
        Write(asio::buffer(*packet), [self, packet](const asio::error_code& inError, const std::size_t)
        {
            if (self->stopped) return;
            self->writeInProgress = false;
            if (inError)
            {
                self->Close();
                return;
            }

            self->queuedSendBytes -= packet->size();
            self->WriteNext();
        });
    }

    void TcpSession::Close()
    {
        if (stopped)
        {
            return;
        }

        stopped = true;
        asio::error_code ignoredError;
        handshakeDeadline.cancel(ignoredError);
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignoredError);
        socket.close(ignoredError);
        sendQueue.clear();
        queuedSendBytes = 0;

        if (closeHandler)
        {
            closeHandler(sessionId);
        }
    }

    std::uint32_t TcpSession::DecodeBodySize(const std::array<std::uint8_t, 4>& inHeader)
    {
        return (static_cast<std::uint32_t>(inHeader[0]) << 24)
            | (static_cast<std::uint32_t>(inHeader[1]) << 16)
            | (static_cast<std::uint32_t>(inHeader[2]) << 8)
            | static_cast<std::uint32_t>(inHeader[3]);
    }

    void TcpSession::EncodeBodySize(const std::uint32_t inBodySize, std::vector<std::uint8_t>& outPacket)
    {
        outPacket[0] = static_cast<std::uint8_t>((inBodySize >> 24) & 0xFF);
        outPacket[1] = static_cast<std::uint8_t>((inBodySize >> 16) & 0xFF);
        outPacket[2] = static_cast<std::uint8_t>((inBodySize >> 8) & 0xFF);
        outPacket[3] = static_cast<std::uint8_t>(inBodySize & 0xFF);
    }
}
