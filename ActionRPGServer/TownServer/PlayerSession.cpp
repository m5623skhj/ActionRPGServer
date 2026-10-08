#include "PlayerSession.h"

#include "../Shared/TcpSession.h"
#include "Protocol.h"
#include "RoomControlTcpServer.h"
#include "TownInstance.h"

#include <optional>
#include <limits>
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
        CloseAuthentication();
        if (const std::shared_ptr<Domain::TownInstance> town = townInstance.lock())
        {
            town->Leave(GetSessionId(), [weakControl = roomControlServer](const auto inRoomId, const auto inPlayerId, std::function<void()> inReleased)
            {
                if (const auto control = weakControl.lock()) control->LeaveRoom(inRoomId, inPlayerId, std::move(inReleased));
            });
        }
    }

    void PlayerSession::Stop()
    {
        CloseAuthentication();
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

    std::uint64_t PlayerSession::GetAccountId() const noexcept
    {
        return admissionDeadline.load(std::memory_order_acquire) > std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
            && loginState.load(std::memory_order_acquire) == LoginState::Authenticated
            ? accountId.load(std::memory_order_acquire) : 0;
    }

    std::uint64_t PlayerSession::BeginAuthentication() noexcept
    {
        if (loginAttemptId.load() == std::numeric_limits<std::uint64_t>::max()) return 0;
        auto expected = LoginState::Unauthenticated;
        if (!loginState.compare_exchange_strong(expected, LoginState::ResolvingAccount)) return 0;
        return loginAttemptId.fetch_add(1) + 1;
    }

    bool PlayerSession::MatchesAuthentication(const std::uint64_t inAttemptId) const noexcept
    {
        const auto state = loginState.load(std::memory_order_acquire);
        return inAttemptId != 0 && loginAttemptId.load(std::memory_order_acquire) == inAttemptId
            && state == LoginState::ResolvingAccount;
    }

    bool PlayerSession::CompleteAuthentication(const std::uint64_t inAttemptId,
        const std::uint64_t inAccountId) noexcept
    {
        if (inAccountId == 0 || !MatchesAuthentication(inAttemptId)) return false;
        accountId.store(inAccountId, std::memory_order_release);
        auto expected = LoginState::ResolvingAccount;
        if (loginState.compare_exchange_strong(expected, LoginState::Authenticated)) return true;
        accountId.store(0, std::memory_order_release);
        return false;
    }

    void PlayerSession::SetAdmissionDeadline(std::chrono::steady_clock::time_point inDeadline) noexcept
    {
        admissionDeadline.store(std::chrono::duration_cast<std::chrono::milliseconds>(
            inDeadline.time_since_epoch()).count(), std::memory_order_release);
    }
    void PlayerSession::CloseAuthentication() noexcept
    {
        loginState.store(LoginState::Closed, std::memory_order_release);
        accountId.store(0, std::memory_order_release);
    }

    void PlayerSession::HandlePacket(std::vector<std::uint8_t> inPacket)
    {
        const std::optional<TownProtocol::PacketType> type = TownProtocol::ReadPacketType(inPacket);
        const std::shared_ptr<Domain::TownInstance> town = townInstance.lock();
        if (!type || !town) { Stop(); return; }
        if (*type == TownProtocol::PacketType::AdmissionTicketRequest)
        {
            const auto request = TownProtocol::DecodeAdmissionTicketRequest(inPacket);
            if (!request || !tcpSession->IsSecure() || GetAccountId() != 0)
            { Stop(); return; }
            town->Admit(shared_from_this(), request->ticket);
            return;
        }
        if (GetAccountId() == 0) { Stop(); return; }
        switch (*type)
        {
        case TownProtocol::PacketType::CharacterListRequest:
        case TownProtocol::PacketType::CharacterCreateRequest:
        case TownProtocol::PacketType::CharacterSelectRequest:
        {
            std::string json;
            if (*type == TownProtocol::PacketType::CharacterListRequest)
            { const auto request = TownProtocol::DecodeCharacterListRequest(inPacket); if (request) json = request->json; }
            else if (*type == TownProtocol::PacketType::CharacterCreateRequest)
            { const auto request = TownProtocol::DecodeCharacterCreateRequest(inPacket); if (request) json = request->json; }
            else
            { const auto request = TownProtocol::DecodeCharacterSelectRequest(inPacket); if (request) json = request->json; }
            if (json.empty() || enterRequested.load()) { Stop(); return; }
            town->CharacterRequest(shared_from_this(), *type, std::move(json));
            return;
        }
        case TownProtocol::PacketType::InventoryStateRequest:
        case TownProtocol::PacketType::InventoryOperationRequest:
        {
            std::string json;
            if (*type == TownProtocol::PacketType::InventoryStateRequest)
            { const auto request = TownProtocol::DecodeInventoryStateRequest(inPacket); if (request) json = request->json; }
            else
            { const auto request = TownProtocol::DecodeInventoryOperationRequest(inPacket); if (request) json = request->json; }
            if (json.empty() || !enterRequested.load() || GetPlayerId() == 0) { Stop(); return; }
            town->InventoryRequest(shared_from_this(), *type, std::move(json));
            return;
        }
        case TownProtocol::PacketType::EnterTownRequest:
        {
            // Character identity, name and definition must originate from the owned DB row.
            Stop();
            return;
        }
        case TownProtocol::PacketType::MoveInput:
        {
            const std::optional<TownProtocol::MoveInput> request = TownProtocol::DecodeMoveInput(inPacket);
            if (!request.has_value() || !enterRequested
                || request->directionX < -1 || request->directionX > 1
                || request->directionY < -1 || request->directionY > 1)
            {
                Stop();
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
                Stop();
                return;
            }
            town->ValidateDungeonJoin(authenticatedPlayerId, request->roomId,
                [town, roomControl, request = *request, authenticatedPlayerId,
                    selectedCharacterId = characterId.load(std::memory_order_acquire)](const bool valid)
                {
                    if (valid) town->GetProgression(authenticatedPlayerId,
                        [roomControl, request, authenticatedPlayerId, selectedCharacterId](std::string progression)
                    {
                        if (!progression.empty()) roomControl->ConfirmJoin(request.roomId, authenticatedPlayerId,
                            request.challenge, selectedCharacterId, std::move(progression));
                    });
                });
            return;
        }
        case TownProtocol::PacketType::SkillStateRequest:
        {
            if (!enterRequested || GetPlayerId() == 0 || !TownProtocol::DecodeSkillStateRequest(inPacket))
            { Stop(); return; }
            town->RequestSkillState(GetPlayerId());
            return;
        }
        case TownProtocol::PacketType::LearnSkillRequest:
        {
            const auto request = TownProtocol::DecodeLearnSkillRequest(inPacket);
            if (!request || !enterRequested || GetPlayerId() == 0) { Stop(); return; }
            town->LearnSkill(GetPlayerId(), request->skillId, request->expectedSkillLevel);
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
                Stop();
                return;
            }
            const std::weak_ptr<PlayerSession> weakSelf = weak_from_this();
            const std::weak_ptr<Domain::TownInstance> weakTown = town;
            town->ValidateDungeonRequest(GetSessionId(), request->zoneId, request->dungeonId,
                [weakSelf, weakTown, roomControl,
                    dungeonId = request->dungeonId](const bool inValid,
                        std::vector<Domain::PlayerId> inParticipantPlayerIds)
                {
                    if (!inValid)
                    {
                        if (const std::shared_ptr<PlayerSession> self = weakSelf.lock())
                        {
                            self->Send(TownProtocol::Encode(TownProtocol::EnterDungeonResponse{}));
                        }
                        return;
                    }
                    std::vector<Domain::PlayerId> responseParticipantPlayerIds =
                        inParticipantPlayerIds;
                    roomControl->CreateRoom(dungeonId, std::move(inParticipantPlayerIds),
                        [weakTown, participantPlayerIds = std::move(responseParticipantPlayerIds)](
                            ActionRPG::RoomControlProtocol::CreateRoomResult inResult) mutable
                        {
                            if (const std::shared_ptr<Domain::TownInstance> activeTown = weakTown.lock())
                            {
                                activeTown->CompleteDungeonRequest(
                                    std::move(participantPlayerIds), std::move(inResult));
                            }
                        });
                });
            return;
        }
        case TownProtocol::PacketType::DungeonCompletionRequest:
        {
            const auto request = TownProtocol::DecodeDungeonCompletionRequest(inPacket);
            const auto roomControl = roomControlServer.lock();
            if (!request || !enterRequested || GetPlayerId() == 0 || !roomControl)
            { Stop(); return; }
            const auto weakSelf = weak_from_this();
            const std::weak_ptr<Domain::TownInstance> weakTown = town;
            town->ValidateDungeonCompletion(GetSessionId(), request->roomId,
                [weakSelf, weakTown, roomControl, roomId = request->roomId, retry = request->retry]
                (const bool inValid, std::vector<Domain::PlayerId> inParticipants) mutable
                {
                    if (!inValid)
                    {
                        if (const auto self = weakSelf.lock()) self->Send(TownProtocol::Encode(
                            TownProtocol::DungeonCompletionResponse{roomId, false, retry}));
                        return;
                    }
                    auto responseParticipants = inParticipants;
                    roomControl->FinishRoom(roomId, retry, std::move(inParticipants),
                        [weakTown, retry, participants = std::move(responseParticipants)]
                        (ActionRPG::RoomControlProtocol::FinishRoomResult inResult) mutable
                        {
                            if (const auto activeTown = weakTown.lock()) activeTown->CompleteDungeonCompletion(
                                std::move(participants), retry, std::move(inResult));
                        });
                });
            return;
        }
        case TownProtocol::PacketType::PartyDetailRequest:
        {
            const auto request = TownProtocol::DecodePartyDetailRequest(inPacket);
            if (!request || !enterRequested || GetPlayerId() == 0) { Stop(); return; }
            town->RequestPartyDetail(GetSessionId(), request->partyId);
            return;
        }
        case TownProtocol::PacketType::PartyJoinRequest:
        {
            const auto request = TownProtocol::DecodePartyJoinRequest(inPacket);
            if (!request || !enterRequested || GetPlayerId() == 0) { Stop(); return; }
            town->RequestPartyJoin(GetSessionId(), request->partyId);
            return;
        }
        case TownProtocol::PacketType::PartyJoinAnswer:
        {
            const auto request = TownProtocol::DecodePartyJoinAnswer(inPacket);
            if (!request || !enterRequested || GetPlayerId() == 0) { Stop(); return; }
            town->AnswerPartyJoin(GetSessionId(), request->requestId, request->accepted);
            return;
        }
        case TownProtocol::PacketType::PartyInviteRequest:
        {
            const std::optional<TownProtocol::PartyInviteRequest> request =
                TownProtocol::DecodePartyInviteRequest(inPacket);
            if (!request.has_value() || !enterRequested || GetPlayerId() == 0)
            {
                Stop();
                return;
            }
            town->InviteToParty(GetSessionId(), request->targetPlayerId);
            return;
        }
        case TownProtocol::PacketType::PartyInviteAnswer:
        {
            const std::optional<TownProtocol::PartyInviteAnswer> request =
                TownProtocol::DecodePartyInviteAnswer(inPacket);
            if (!request.has_value() || !enterRequested || GetPlayerId() == 0)
            {
                Stop();
                return;
            }
            town->AnswerPartyInvitation(
                GetSessionId(), request->invitationId, request->accepted);
            return;
        }
        case TownProtocol::PacketType::PartyLeaveRequest:
        {
            if (!TownProtocol::DecodePartyLeaveRequest(inPacket).has_value()
                || !enterRequested || GetPlayerId() == 0)
            {
                Stop();
                return;
            }
            town->LeaveParty(GetSessionId());
            return;
        }
        case TownProtocol::PacketType::PartyKickRequest:
        {
            const std::optional<TownProtocol::PartyKickRequest> request =
                TownProtocol::DecodePartyKickRequest(inPacket);
            if (!request.has_value() || !enterRequested || GetPlayerId() == 0)
            {
                Stop();
                return;
            }
            town->KickPartyMember(GetSessionId(), request->targetPlayerId);
            return;
        }
        case TownProtocol::PacketType::PartySettingsRequest:
        {
            const auto request = TownProtocol::DecodePartySettingsRequest(inPacket);
            if (!request.has_value() || !enterRequested || GetPlayerId() == 0)
            {
                Stop();
                return;
            }
            town->UpdatePartySettings(GetSessionId(), request->title, request->isPublic);
            return;
        }
        case TownProtocol::PacketType::PartyCreateRequest:
        {
            const auto request = TownProtocol::DecodePartyCreateRequest(inPacket);
            if (!request.has_value() || !enterRequested || GetPlayerId() == 0)
            {
                Stop();
                return;
            }
            town->CreateParty(GetSessionId(), request->title, request->isPublic);
            return;
        }
        case TownProtocol::PacketType::PartyDirectoryPageRequest:
        {
            const auto request = TownProtocol::DecodePartyDirectoryPageRequest(inPacket);
            if (!request.has_value() || !enterRequested || GetPlayerId() == 0)
            {
                Stop();
                return;
            }
            town->RequestPartyDirectoryPage(GetSessionId(), request->page);
            return;
        }
        case TownProtocol::PacketType::PartyDirectoryUnsubscribe:
        {
            if (!TownProtocol::DecodePartyDirectoryUnsubscribe(inPacket).has_value()
                || !enterRequested || GetPlayerId() == 0)
            {
                Stop();
                return;
            }
            town->UnsubscribePartyDirectory(GetSessionId());
            return;
        }
        default:
            Stop();
            return;
        }
    }
}
