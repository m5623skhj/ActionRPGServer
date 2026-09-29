#include "PlayerSession.h"

#include "../Shared/TcpSession.h"
#include "Protocol.h"
#include "RoomControlTcpServer.h"
#include "TownInstance.h"

#include <optional>
#include <utility>

namespace TownServer::Network
{
    PlayerSession::PlayerSession(std::shared_ptr<ActionRPG::Network::TcpSession> inTcpSession,
        std::weak_ptr<Domain::TownInstance> inTownInstance,
        std::weak_ptr<RoomControlTcpServer> inRoomControlServer)
        : tcpSession(std::move(inTcpSession)),
          townInstance(std::move(inTownInstance)),
          roomControlServer(std::move(inRoomControlServer))
    {
    }

    void PlayerSession::Start()
    {
        const std::weak_ptr<PlayerSession> weakSelf = weak_from_this();
        tcpSession->SetReceiveHandler([weakSelf](std::vector<std::uint8_t> inPacket)
        {
            if (const std::shared_ptr<PlayerSession> self = weakSelf.lock())
            {
                self->HandlePacket(std::move(inPacket));
            }
        });
        tcpSession->Start();
    }

    void PlayerSession::Disconnect()
    {
        if (const std::shared_ptr<Domain::TownInstance> town = townInstance.lock())
        {
            town->Leave(GetSessionId());
        }
    }

    void PlayerSession::Stop()
    {
        tcpSession->Stop();
    }

    void PlayerSession::Send(std::vector<std::uint8_t> inPacket)
    {
        tcpSession->Send(std::move(inPacket));
    }

    std::uint64_t PlayerSession::GetSessionId() const noexcept
    {
        return tcpSession->GetSessionId();
    }

    std::uint64_t PlayerSession::GetPlayerId() const noexcept
    {
        return playerId.load(std::memory_order_acquire);
    }

    void PlayerSession::SetPlayerId(const std::uint64_t inPlayerId) noexcept
    {
        playerId.store(inPlayerId, std::memory_order_release);
    }

    void PlayerSession::HandlePacket(std::vector<std::uint8_t> inPacket)
    {
        const std::optional<TownProtocol::PacketType> type = TownProtocol::ReadPacketType(inPacket);
        const std::shared_ptr<Domain::TownInstance> town = townInstance.lock();
        if (!type.has_value() || !town)
        {
            tcpSession->Stop();
            return;
        }

        switch (*type)
        {
        case TownProtocol::PacketType::EnterTownRequest:
        {
            const std::optional<TownProtocol::EnterTownRequest> request =
                TownProtocol::DecodeEnterTownRequest(inPacket);
            if (!request.has_value() || enterRequested || request->playerName.empty()
                || request->playerName.size() > 32)
            {
                tcpSession->Stop();
                return;
            }
            enterRequested = true;
            town->Enter(shared_from_this(), request->playerName);
            return;
        }
        case TownProtocol::PacketType::MoveInput:
        {
            const std::optional<TownProtocol::MoveInput> request = TownProtocol::DecodeMoveInput(inPacket);
            if (!request.has_value() || !enterRequested
                || request->directionX < -1 || request->directionX > 1
                || request->directionY < -1 || request->directionY > 1)
            {
                tcpSession->Stop();
                return;
            }
            town->ApplyMovementInput(GetSessionId(), *request);
            return;
        }
        case TownProtocol::PacketType::ConfirmDungeonJoin:
        {
            const std::optional<TownProtocol::ConfirmDungeonJoin> request =
                TownProtocol::DecodeConfirmDungeonJoin(inPacket);
            const std::shared_ptr<RoomControlTcpServer> roomControl = roomControlServer.lock();
            const std::uint64_t authenticatedPlayerId = GetPlayerId();
            if (!request.has_value() || !enterRequested || authenticatedPlayerId == 0 || !roomControl)
            {
                tcpSession->Stop();
                return;
            }
            roomControl->ConfirmJoin(request->roomId, authenticatedPlayerId, request->challenge);
            return;
        }
        case TownProtocol::PacketType::EnterDungeonRequest:
        {
            const std::optional<TownProtocol::EnterDungeonRequest> request =
                TownProtocol::DecodeEnterDungeonRequest(inPacket);
            const std::shared_ptr<RoomControlTcpServer> roomControl = roomControlServer.lock();
            const std::uint64_t authenticatedPlayerId = GetPlayerId();
            if (!request.has_value() || !enterRequested || authenticatedPlayerId == 0 || !roomControl)
            {
                tcpSession->Stop();
                return;
            }
            const std::weak_ptr<PlayerSession> weakSelf = weak_from_this();
            town->ValidateDungeonRequest(GetSessionId(), request->zoneId, request->dungeonId,
                [weakSelf, roomControl, authenticatedPlayerId,
                    dungeonId = request->dungeonId](const bool inValid)
                {
                    const std::shared_ptr<PlayerSession> self = weakSelf.lock();
                    if (!self)
                    {
                        return;
                    }
                    if (!inValid)
                    {
                        self->Send(TownProtocol::Encode(TownProtocol::EnterDungeonResponse{}));
                        return;
                    }
                    roomControl->CreateRoom(dungeonId, { authenticatedPlayerId },
                        [weakSelf](ActionRPG::RoomControlProtocol::CreateRoomResult inResult)
                        {
                            if (const std::shared_ptr<PlayerSession> activeSelf = weakSelf.lock())
                            {
                                activeSelf->Send(TownProtocol::Encode(TownProtocol::EnterDungeonResponse{
                                    inResult.succeeded,
                                    inResult.roomId,
                                    inResult.combatSeed,
                                    std::move(inResult.sessionBrokerAddress),
                                    inResult.sessionBrokerPort
                                }));
                            }
                        });
                });
            return;
        }
        default:
            tcpSession->Stop();
            return;
        }
    }
}
