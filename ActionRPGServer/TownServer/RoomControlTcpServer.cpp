#include "RoomControlTcpServer.h"

#include "../Shared/TcpSession.h"
#include "TownInstance.h"

#include <algorithm>
#include <iostream>
#include <optional>
#include <utility>

namespace TownServer::Network
{
    namespace Protocol = ActionRPG::RoomControlProtocol;

    RoomControlTcpServer::RoomControlTcpServer(
        asio::io_context& inIoContext,
        const asio::ip::tcp::endpoint& inEndpoint,
        std::shared_ptr<Domain::TownInstance> inTownInstance)
        : TcpAcceptServer(inIoContext, inEndpoint),
          townInstance(std::move(inTownInstance)),
          authenticationKey(Protocol::LoadAuthenticationKey())
    {
    }

    void RoomControlTcpServer::CreateRoom(
        const std::uint32_t inDungeonId,
        std::vector<Protocol::PlayerId> inParticipantPlayerIds,
        CreateRoomResultHandler inResultHandler)
    {
        asio::dispatch(GetExecutor(),
            [this, inDungeonId, participantPlayerIds = std::move(inParticipantPlayerIds),
                resultHandler = std::move(inResultHandler)]() mutable
            {
                auto selected = roomServers.end();
                for (auto iterator = roomServers.begin(); iterator != roomServers.end(); ++iterator)
                {
                    const RoomServerState& candidate = iterator->second;
                    if (!candidate.registered || candidate.roomCount >= candidate.maxRoomCount)
                    {
                        continue;
                    }
                    if (selected == roomServers.end() || candidate.roomCount < selected->second.roomCount)
                    {
                        selected = iterator;
                    }
                }

                if (selected == roomServers.end())
                {
                    if (resultHandler)
                    {
                        resultHandler(Protocol::CreateRoomResult{});
                    }
                    return;
                }

                const Protocol::RequestId requestId = nextRequestId++;
                pendingCreateRooms.emplace(requestId,
                    PendingCreateRoom{ selected->first, std::move(resultHandler) });
                selected->second.session->Send(Protocol::Encode(Protocol::CreateRoom{
                    requestId, inDungeonId, std::move(participantPlayerIds)
                }));
            });
    }

    void RoomControlTcpServer::FinishRoom(const Protocol::RoomId inRoomId, const bool inRetry,
        std::vector<Protocol::PlayerId> inParticipants, FinishRoomResultHandler inHandler)
    {
        asio::dispatch(GetExecutor(), [this, inRoomId, inRetry, participants = std::move(inParticipants),
            handler = std::move(inHandler)]() mutable
        {
            const auto found = roomToServerSession.find(inRoomId);
            if (found == roomToServerSession.end() || !endingRoomPlayers.contains(inRoomId)
                || !roomServers.contains(found->second))
            {
                handler(Protocol::FinishRoomResult{0, inRoomId});
                return;
            }
            const auto requestId = nextRequestId++;
            pendingFinishRooms.emplace(requestId, PendingFinishRoom{found->second, inRoomId, std::move(handler)});
            roomServers.at(found->second).session->Send(Protocol::Encode(
                Protocol::FinishRoom{requestId, inRoomId, inRetry, std::move(participants)}));
        });
    }

    void RoomControlTcpServer::ConfirmJoin(
        const Protocol::RoomId inRoomId,
        const Protocol::PlayerId inPlayerId,
        const std::uint64_t inChallenge, const std::uint32_t inCharacterId)
    {
        asio::dispatch(GetExecutor(), [this, inRoomId, inPlayerId, inChallenge, inCharacterId]()
        {
            const auto roomIterator = roomToServerSession.find(inRoomId);
            if (roomIterator == roomToServerSession.end())
            {
                return;
            }
            const auto serverIterator = roomServers.find(roomIterator->second);
            if (serverIterator == roomServers.end() || !serverIterator->second.registered)
            {
                return;
            }
            serverIterator->second.session->Send(Protocol::Encode(
                Protocol::ConfirmJoin{ inRoomId, inPlayerId, inChallenge, inCharacterId }));
        });
    }

    void RoomControlTcpServer::HandleAcceptedSession(
        std::shared_ptr<ActionRPG::Network::TcpSession> inSession)
    {
        const std::uint64_t sessionId = inSession->GetSessionId();
        const std::weak_ptr<ActionRPG::Network::TcpSession> weakSession = inSession;
        inSession->SetReceiveHandler([this, sessionId, weakSession](std::vector<std::uint8_t> inPacket)
        {
            if (weakSession.expired())
            {
                return;
            }
            HandlePacket(sessionId, std::move(inPacket));
        });
        roomServers.emplace(sessionId, RoomServerState{ std::move(inSession) });
        roomServers.at(sessionId).session->Start();
    }

    void RoomControlTcpServer::LeaveRoom(const Protocol::RoomId inRoomId, const Protocol::PlayerId inPlayerId)
    {
        asio::dispatch(GetExecutor(), [this, inRoomId, inPlayerId]()
        {
            const auto room = roomToServerSession.find(inRoomId);
            if (room == roomToServerSession.end()) return;
            const auto server = roomServers.find(room->second);
            if (server != roomServers.end()) server->second.session->Send(
                Protocol::Encode(Protocol::LeaveRoom{inRoomId, inPlayerId}));
        });
    }

    void RoomControlTcpServer::HandleClosedSession(const std::uint64_t inSessionId)
    {
        for (auto iterator = roomToServerSession.begin(); iterator != roomToServerSession.end();)
        {
            if (iterator->second == inSessionId)
            {
                townInstance->HandleRoomEnded(Protocol::RoomEnded{
                    iterator->first, Protocol::RoomEndReason::Aborted, {}});
                iterator = roomToServerSession.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
        for (auto iterator = endingRoomPlayers.begin(); iterator != endingRoomPlayers.end();)
        {
            if (!roomToServerSession.contains(iterator->first))
            {
                iterator = endingRoomPlayers.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }

        for (auto iterator = pendingCreateRooms.begin(); iterator != pendingCreateRooms.end();)
        {
            if (iterator->second.roomServerSessionId != inSessionId)
            {
                ++iterator;
                continue;
            }
            if (iterator->second.resultHandler)
            {
                iterator->second.resultHandler(Protocol::CreateRoomResult{ iterator->first });
            }
            iterator = pendingCreateRooms.erase(iterator);
        }
        for (auto iterator = pendingFinishRooms.begin(); iterator != pendingFinishRooms.end();)
        {
            if (iterator->second.roomServerSessionId != inSessionId) { ++iterator; continue; }
            iterator->second.handler(Protocol::FinishRoomResult{iterator->first, iterator->second.roomId});
            iterator = pendingFinishRooms.erase(iterator);
        }
        roomServers.erase(inSessionId);
    }

    void RoomControlTcpServer::HandlePacket(
        const std::uint64_t inSessionId,
        std::vector<std::uint8_t> inPacket)
    {
        asio::dispatch(GetExecutor(),
            [this, inSessionId, packet = std::move(inPacket)]() mutable
            {
                HandlePacketOnStrand(inSessionId, std::move(packet));
            });
    }

    void RoomControlTcpServer::HandlePacketOnStrand(
        const std::uint64_t inSessionId,
        std::vector<std::uint8_t> inPacket)
    {
        const auto stateIterator = roomServers.find(inSessionId);
        const std::optional<Protocol::PacketType> type = Protocol::ReadPacketType(inPacket);
        if (stateIterator == roomServers.end() || !type.has_value())
        {
            return;
        }
        RoomServerState& state = stateIterator->second;

        if (!state.registered && *type != Protocol::PacketType::RegisterRoomServer)
        {
            CloseInvalidSession(state);
            return;
        }

        switch (*type)
        {
        case Protocol::PacketType::RegisterRoomServer:
        {
            const std::optional<Protocol::RegisterRoomServer> packet =
                Protocol::DecodeRegisterRoomServer(inPacket);
            const bool duplicateId = packet.has_value() && std::any_of(
                roomServers.begin(), roomServers.end(),
                [inSessionId, &packet](const auto& inPair)
                {
                    return inPair.first != inSessionId && inPair.second.registered
                        && inPair.second.roomServerId == packet->roomServerId;
                });
            if (!packet.has_value() || state.registered || duplicateId
                || packet->authenticationKey != authenticationKey)
            {
                CloseInvalidSession(state);
                return;
            }
            state.roomServerId = packet->roomServerId;
            state.maxRoomCount = packet->maxRoomCount;
            state.registered = true;
            std::cout << "Room server " << state.roomServerId << " registered.\n";
            return;
        }
        case Protocol::PacketType::CreateRoomResult:
        {
            const std::optional<Protocol::CreateRoomResult> packet =
                Protocol::DecodeCreateRoomResult(inPacket);
            if (!packet.has_value())
            {
                CloseInvalidSession(state);
                return;
            }
            const auto pendingIterator = pendingCreateRooms.find(packet->requestId);
            if (pendingIterator == pendingCreateRooms.end()
                || pendingIterator->second.roomServerSessionId != inSessionId)
            {
                CloseInvalidSession(state);
                return;
            }
            if (packet->succeeded)
            {
                roomToServerSession[packet->roomId] = inSessionId;
                ++state.roomCount;
            }
            CreateRoomResultHandler handler = std::move(pendingIterator->second.resultHandler);
            pendingCreateRooms.erase(pendingIterator);
            if (handler)
            {
                handler(*packet);
            }
            return;
        }
        case Protocol::PacketType::FinishRoomResult:
        {
            const auto packet = Protocol::DecodeFinishRoomResult(inPacket);
            if (!packet) { CloseInvalidSession(state); return; }
            const auto pending = pendingFinishRooms.find(packet->requestId);
            if (pending == pendingFinishRooms.end() || pending->second.roomServerSessionId != inSessionId
                || pending->second.roomId != packet->previousRoomId)
            { CloseInvalidSession(state); return; }
            if (packet->succeeded)
            {
                endingRoomPlayers.erase(packet->previousRoomId);
                roomToServerSession.erase(packet->previousRoomId);
                if (state.roomCount > 0) --state.roomCount;
                if (packet->roomId != 0)
                {
                    roomToServerSession[packet->roomId] = inSessionId;
                    ++state.roomCount;
                }
            }
            auto handler = std::move(pending->second.handler);
            pendingFinishRooms.erase(pending);
            handler(*packet);
            return;
        }
        case Protocol::PacketType::EnterRoom:
        {
            const std::optional<Protocol::EnterRoom> packet = Protocol::DecodeEnterRoom(inPacket);
            if (!packet.has_value() || !roomToServerSession.contains(packet->roomId)
                || roomToServerSession.at(packet->roomId) != inSessionId)
            {
                CloseInvalidSession(state);
                return;
            }
            townInstance->EnterDungeon(packet->playerId, packet->roomId,
                [this, roomId = packet->roomId, playerId = packet->playerId](const bool accepted)
                {
                    if (!accepted) LeaveRoom(roomId, playerId);
                });
            return;
        }
        case Protocol::PacketType::RoomStarted:
        {
            const auto packet = Protocol::DecodeRoomStarted(inPacket);
            if (!packet || !roomToServerSession.contains(packet->roomId)
                || roomToServerSession.at(packet->roomId) != inSessionId)
            { CloseInvalidSession(state); return; }
            townInstance->HandleRoomStarted(*packet);
            return;
        }
        case Protocol::PacketType::LeaveRoom:
        {
            const std::optional<Protocol::LeaveRoom> packet = Protocol::DecodeLeaveRoom(inPacket);
            if (!packet.has_value())
            { CloseInvalidSession(state); return; }
            // A disconnect notification may arrive after a successful finish removed the mapping.
            if (!roomToServerSession.contains(packet->roomId)) return;
            if (roomToServerSession.at(packet->roomId) != inSessionId)
            {
                CloseInvalidSession(state);
                return;
            }
            townInstance->LeaveDungeon(packet->playerId, packet->roomId);
            const auto endingIterator = endingRoomPlayers.find(packet->roomId);
            if (endingIterator != endingRoomPlayers.end())
            {
                endingIterator->second.erase(packet->playerId);
                if (endingIterator->second.empty())
                {
                    endingRoomPlayers.erase(endingIterator);
                    roomToServerSession.erase(packet->roomId);
                    if (state.roomCount > 0)
                    {
                        --state.roomCount;
                    }
                }
            }
            return;
        }
        case Protocol::PacketType::RoomEnded:
        {
            const std::optional<Protocol::RoomEnded> packet = Protocol::DecodeRoomEnded(inPacket);
            if (!packet.has_value() || !roomToServerSession.contains(packet->roomId)
                || roomToServerSession.at(packet->roomId) != inSessionId)
            {
                CloseInvalidSession(state);
                return;
            }
            townInstance->HandleRoomEnded(*packet);
            if (packet->rewardPlayerIds.empty())
            {
                roomToServerSession.erase(packet->roomId);
                if (state.roomCount > 0)
                {
                    --state.roomCount;
                }
            }
            else
            {
                endingRoomPlayers.emplace(packet->roomId,
                    std::unordered_set<Protocol::PlayerId>(
                        packet->rewardPlayerIds.begin(), packet->rewardPlayerIds.end()));
            }
            return;
        }
        default:
            CloseInvalidSession(state);
            return;
        }
    }

    void RoomControlTcpServer::CloseInvalidSession(const RoomServerState& inState)
    {
        inState.session->Stop();
    }
}
