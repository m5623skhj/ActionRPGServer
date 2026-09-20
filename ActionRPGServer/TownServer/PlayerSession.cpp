#include "PlayerSession.h"

#include "Protocol.h"
#include "TcpSession.h"
#include "TownInstance.h"

#include <optional>
#include <utility>

namespace TownServer::Network
{
    PlayerSession::PlayerSession(std::shared_ptr<TcpSession> inTcpSession,
        std::weak_ptr<Domain::TownInstance> inTownInstance)
        : tcpSession(std::move(inTcpSession)),
          townInstance(std::move(inTownInstance))
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
        default:
            tcpSession->Stop();
            return;
        }
    }
}
