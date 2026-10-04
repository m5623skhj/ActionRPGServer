#include "TownControlClient.h"

#include "../Shared/TcpSession.h"
#include "RoomManager.h"

#include <iostream>
#include <optional>
#include <utility>

namespace GameRoomServer
{
    namespace Protocol = ActionRPG::RoomControlProtocol;

    TownControlClient::TownControlClient(
        asio::io_context& inIoContext,
        std::shared_ptr<RoomManager> inRoomManager,
        const Protocol::RoomServerId inRoomServerId,
        const std::uint32_t inMaxRoomCount)
        : strand(asio::make_strand(inIoContext)),
          resolver(strand),
          connectingSocket(strand),
          roomManager(std::move(inRoomManager)),
          roomServerId(inRoomServerId),
          maxRoomCount(inMaxRoomCount),
          authenticationKey(Protocol::LoadAuthenticationKey())
    {
    }

    void TownControlClient::Start(std::string inHost, const std::uint16_t inPort)
    {
        const std::shared_ptr<TownControlClient> self = shared_from_this();
        asio::dispatch(strand, [self, host = std::move(inHost), inPort]() mutable
        {
            self->roomManager->SetSendHandler([weakSelf = self->weak_from_this()](std::vector<std::uint8_t> inPacket)
            {
                if (const std::shared_ptr<TownControlClient> client = weakSelf.lock())
                {
                    client->Send(std::move(inPacket));
                }
            });
            self->resolver.async_resolve(std::move(host), std::to_string(inPort),
                [self](const asio::error_code& inError, const asio::ip::tcp::resolver::results_type& inResults)
                {
                    self->HandleResolved(inError, inResults);
                });
        });
    }

    void TownControlClient::Stop()
    {
        const std::shared_ptr<TownControlClient> self = shared_from_this();
        asio::dispatch(strand, [self]()
        {
            self->stopped = true;
            self->resolver.cancel();
            if (self->session)
            {
                self->session->Stop();
            }
            asio::error_code ignoredError;
            self->connectingSocket.close(ignoredError);
        });
    }

    void TownControlClient::Send(std::vector<std::uint8_t> inPacket)
    {
        const std::shared_ptr<TownControlClient> self = shared_from_this();
        asio::dispatch(strand, [self, packet = std::move(inPacket)]() mutable
        {
            if (self->session)
            {
                self->session->Send(std::move(packet));
            }
        });
    }

    void TownControlClient::HandleResolved(
        const asio::error_code& inError,
        const asio::ip::tcp::resolver::results_type& inResults)
    {
        if (inError || stopped)
        {
            HandleClosed();
            return;
        }
        const std::shared_ptr<TownControlClient> self = shared_from_this();
        asio::async_connect(connectingSocket, inResults,
            [self](const asio::error_code& inConnectError, const asio::ip::tcp::endpoint&)
            {
                self->HandleConnected(inConnectError);
            });
    }

    void TownControlClient::HandleConnected(const asio::error_code& inError)
    {
        if (inError || stopped)
        {
            HandleClosed();
            return;
        }

        asio::error_code endpointError;
        const auto endpoint = connectingSocket.remote_endpoint(endpointError);
        if (endpointError || !endpoint.address().is_loopback())
        {
            asio::error_code ignoredError;
            connectingSocket.close(ignoredError);
            HandleClosed();
            return;
        }
        connectingSocket.set_option(asio::ip::tcp::no_delay(true));
        const std::weak_ptr<TownControlClient> weakSelf = weak_from_this();
        session = std::make_shared<ActionRPG::Network::TcpSession>(
            1,
            std::move(connectingSocket),
            [weakSelf](const std::uint64_t)
            {
                if (const std::shared_ptr<TownControlClient> self = weakSelf.lock())
                {
                    self->HandleClosed();
                }
            });
        session->SetReceiveHandler([weakSelf](std::vector<std::uint8_t> inPacket)
        {
            if (const std::shared_ptr<TownControlClient> self = weakSelf.lock())
            {
                self->HandlePacket(std::move(inPacket));
            }
        });
        session->Start();
        session->Send(Protocol::Encode(Protocol::RegisterRoomServer{ roomServerId, maxRoomCount, authenticationKey }));
        std::cout << "Connected to TownServer room control channel.\n";
    }

    void TownControlClient::HandlePacket(std::vector<std::uint8_t> inPacket)
    {
        const std::optional<Protocol::PacketType> type = Protocol::ReadPacketType(inPacket);
        if (!type.has_value())
        {
            session->Stop();
            return;
        }

        switch (*type)
        {
        case Protocol::PacketType::CreateRoom:
        {
            const std::optional<Protocol::CreateRoom> request = Protocol::DecodeCreateRoom(inPacket);
            if (!request.has_value())
            {
                session->Stop();
                return;
            }
            const std::weak_ptr<TownControlClient> weakSelf = weak_from_this();
            roomManager->CreateRoom(*request,
                [weakSelf](Protocol::CreateRoomResult inResult)
                {
                    if (const std::shared_ptr<TownControlClient> self = weakSelf.lock())
                    {
                        self->Send(Protocol::Encode(inResult));
                    }
                });
            return;
        }
        case Protocol::PacketType::FinishRoom:
        {
            const auto request = Protocol::DecodeFinishRoom(inPacket);
            if (!request) { session->Stop(); return; }
            const auto weakSelf = weak_from_this();
            roomManager->FinishRoom(*request, [weakSelf](Protocol::FinishRoomResult inResult)
            {
                if (const auto self = weakSelf.lock()) self->Send(Protocol::Encode(inResult));
            });
            return;
        }
        case Protocol::PacketType::ConfirmJoin:
        {
            const std::optional<Protocol::ConfirmJoin> request = Protocol::DecodeConfirmJoin(inPacket);
            if (!request.has_value())
            {
                session->Stop();
                return;
            }
            roomManager->ConfirmJoin(*request);
            return;
        }
        case Protocol::PacketType::UpdatePlayerProgress:
        {
            const auto request = Protocol::DecodeUpdatePlayerProgress(inPacket);
            if (!request) { session->Stop(); return; }
            roomManager->UpdatePlayerProgress(*request);
            return;
        }
        case Protocol::PacketType::LeaveRoom:
        {
            const auto request = Protocol::DecodeLeaveRoom(inPacket);
            if (!request) { session->Stop(); return; }
            roomManager->LeaveRoom(*request);
            return;
        }
        default:
            session->Stop();
            return;
        }
    }

    void TownControlClient::HandleClosed()
    {
        if (!stopped)
        {
            std::cerr << "TownServer room control channel disconnected.\n";
        }
        session.reset();
        roomManager->Stop();
    }
}
