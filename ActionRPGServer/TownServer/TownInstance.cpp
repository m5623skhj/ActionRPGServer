#include "TownInstance.h"

#include "PlayerSession.h"
#include "Protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <utility>

namespace
{
    constexpr auto TICK_INTERVAL = std::chrono::milliseconds(50);
    constexpr float MAX_SIMULATION_SECONDS = 0.25f;
    constexpr std::uint32_t SNAPSHOT_TICK_INTERVAL = 2;

    [[nodiscard]] TownProtocol::PartyResultCode ToProtocolResult(
        const TownServer::Domain::PartyManager::Result inResult)
    {
        using ManagerResult = TownServer::Domain::PartyManager::Result;
        switch (inResult)
        {
        case ManagerResult::Succeeded:
            return TownProtocol::PartyResultCode::Succeeded;
        case ManagerResult::AlreadyInParty:
            return TownProtocol::PartyResultCode::AlreadyInParty;
        case ManagerResult::NotInParty:
            return TownProtocol::PartyResultCode::NotInParty;
        case ManagerResult::NotLeader:
            return TownProtocol::PartyResultCode::NotLeader;
        case ManagerResult::PartyFull:
            return TownProtocol::PartyResultCode::PartyFull;
        case ManagerResult::AlreadyInvited:
            return TownProtocol::PartyResultCode::AlreadyInvited;
        case ManagerResult::InvitationNotFound:
            return TownProtocol::PartyResultCode::InvitationNotFound;
        case ManagerResult::InvalidTarget:
            return TownProtocol::PartyResultCode::InvalidTarget;
        case ManagerResult::InvalidTitle:
            return TownProtocol::PartyResultCode::InvalidTitle;
        }
        return TownProtocol::PartyResultCode::InvalidTarget;
    }
}

namespace TownServer::Domain
{
    TownInstance::TownInstance(asio::io_context& inIoContext, std::vector<TownMap> inMaps,
        DungeonCatalog inDungeonCatalog, const std::filesystem::path& inDataDirectory,
        std::shared_ptr<Database::OdbcDatabase> inDatabase)
        : strand(asio::make_strand(inIoContext)),
          database(std::move(inDatabase)),
          tickTimer(strand),
          dungeonCatalog(std::move(inDungeonCatalog)),
          progressionPolicy(ActionRPG::PlayerSkills::ProgressionPolicy::Load(inDataDirectory / "CharacterProgression.json")),
          playerSkills(ActionRPG::PlayerSkills::Catalog::Load(inDataDirectory / "PlayerSkills.json")),
          skillTrees(ActionRPG::PlayerSkills::SkillTreeCatalog::Load(inDataDirectory / "SkillTrees.json", playerSkills))
    {
        if (!database)
            throw std::invalid_argument("TownInstance requires a DB execution service, even when disabled.");
        if (inMaps.empty())
        {
            throw std::invalid_argument("TownInstance requires at least one map.");
        }
        const ActionRPG::PlayerSkills::Catalog::Json skillPayload{
            { "characterId", 1 }, { "progression", progressionPolicy.Create().ToJson() },
            { "skillTrees", skillTrees.source }, { "playerSkills", playerSkills.source }, { "result", "State" } };
        // Leave room for the bounded learned-skill map; never truncate the catalog inside a TCP string.
        if (skillPayload.dump().size() + 24000 > std::numeric_limits<std::uint16_t>::max())
            throw std::invalid_argument("Skill catalog exceeds the town skill-state packet limit.");
        defaultMapId = inMaps.front().GetInfo().mapId;
        for (TownMap& map : inMaps)
        {
            const std::string mapId = map.GetInfo().mapId;
            if (!maps.emplace(mapId, std::move(map)).second)
            {
                throw std::invalid_argument("Duplicate town map id: " + mapId);
            }
        }
        for (const auto& [mapId, map] : maps)
        {
            for (const TownProtocol::TransitionZone& zone : map.GetInfo().transitionZones)
            {
                if (zone.actionType == TownProtocol::TransitionActionType::DungeonSelection)
                {
                    if (dungeonCatalog.FindGroup(zone.dungeonGroupId) == nullptr)
                    {
                        throw std::invalid_argument(
                            "Unresolved dungeon group: " + mapId + "/" + zone.id);
                    }
                    continue;
                }
                const auto targetMap = maps.find(zone.targetMapId);
                if (targetMap == maps.end()
                    || targetMap->second.FindEntryPoint(zone.targetEntryPointId) == nullptr)
                {
                    throw std::invalid_argument("Unresolved map transition: " + mapId + "/" + zone.id);
                }
            }
        }
    }

    void TownInstance::SetProgressionChangedHandler(ProgressionChangedHandler inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, handler = std::move(inHandler)]() mutable
        {
            self->progressionChangedHandler = std::move(handler);
        });
    }

    void TownInstance::SendSkillState(PlayerEntry& inEntry, const std::string& inResult)
    {
        const ActionRPG::PlayerSkills::Catalog::Json payload{
            { "characterId", inEntry.player.GetCharacterId() }, { "progression", inEntry.progression.ToJson() },
            { "skillTrees", skillTrees.source }, { "playerSkills", playerSkills.source }, { "result", inResult } };
        inEntry.session->Send(TownProtocol::Encode(TownProtocol::SkillStateResponse{ payload.dump() }));
    }

    void TownInstance::NotifyProgression(PlayerId inPlayerId, const PlayerEntry& inEntry)
    {
        const auto roomId = inEntry.dungeonRoomId != 0 ? inEntry.dungeonRoomId : inEntry.reservedDungeonRoomId;
        if (roomId != 0 && progressionChangedHandler)
            progressionChangedHandler(roomId, inPlayerId, inEntry.progression.ToJson().dump());
    }

    void TownInstance::RequestSkillState(PlayerId inPlayerId)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId]()
        {
            const auto found = self->players.find(inPlayerId);
            if (found != self->players.end()) self->SendSkillState(found->second, "State");
        });
    }

    void TownInstance::GetProgression(PlayerId inPlayerId, std::function<void(std::string)> inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, handler = std::move(inHandler)]()
        {
            const auto found = self->players.find(inPlayerId);
            handler(found == self->players.end() ? std::string{} : found->second.progression.ToJson().dump());
        });
    }

    void TownInstance::LearnSkill(PlayerId inPlayerId, std::string inSkillId, std::uint32_t inExpectedSkillLevel)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, id = std::move(inSkillId), inExpectedSkillLevel]()
        {
            const auto found = self->players.find(inPlayerId);
            if (found == self->players.end()) return;
            auto& entry = found->second;
            // Include reserved rooms so delayed entry confirmation cannot permit a skill purchase.
            if (entry.dungeonRoomId != 0 || entry.reservedDungeonRoomId != 0)
            {
                self->SendSkillState(entry, "InDungeon");
                return;
            }
            auto proposed = entry.progression;
            auto result = self->skillTrees.Learn(self->playerSkills, proposed,
                entry.player.GetCharacterId(), id, inExpectedSkillLevel);
            if (result == "Succeeded")
            {
                entry.progression = std::move(proposed);
                self->NotifyProgression(inPlayerId, entry);
            }
            self->SendSkillState(entry, result);
        });
    }

    void TownInstance::AdvancePlayerLevel(PlayerId inPlayerId, std::uint32_t inLevel, std::function<void(bool)> inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, inLevel, handler = std::move(inHandler)]()
        {
            const auto found = self->players.find(inPlayerId);
            bool accepted = false;
            if (found != self->players.end())
            {
                auto& entry = found->second;
                auto proposed = entry.progression;
                accepted = self->progressionPolicy.AdvanceLevel(proposed, inLevel);
                if (accepted)
                {
                    entry.progression = std::move(proposed);
                    self->NotifyProgression(inPlayerId, entry);
                    self->SendSkillState(entry, "LevelAdvanced");
                }
            }
            if (handler) handler(accepted);
        });
    }

    void TownInstance::Start()
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self]()
        {
            if (self->running)
            {
                return;
            }
            self->running = true;
            const auto now = std::chrono::steady_clock::now();
            for (auto& [playerId, entry] : self->players)
                entry.lastSimulationTime = now;
            self->nextTickTime = now + TICK_INTERVAL;
            self->ScheduleTick();
        });
    }

    void TownInstance::Stop()
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self]()
        {
            self->running = false;
            if (self->admissionTimer) self->admissionTimer->cancel();
            for (const auto& [id, admission] : self->admissions)
                if (const auto session = admission.session.lock()) session->Stop();
            asio::error_code ignoredError;
            self->tickTimer.cancel(ignoredError);
        });
    }

    void TownInstance::Enter(std::shared_ptr<Network::PlayerSession> inSession, std::string inPlayerName,
        const std::uint32_t inCharacterId)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, session = std::move(inSession),
            playerName = std::move(inPlayerName), inCharacterId]() mutable
        {
            self->EnterOnStrand(std::move(session), std::move(playerName), inCharacterId);
        });
    }

    void TownInstance::Leave(const std::uint64_t inSessionId,
        std::function<void(ActionRPG::RoomControlProtocol::RoomId, PlayerId, std::function<void()>)> inDungeonLeaveHandler)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, handler = std::move(inDungeonLeaveHandler)]()
        {
            bool awaitingRoom = false;
            ActionRPG::RoomControlProtocol::RoomId previousRoomId = 0;
            const auto session = self->sessionToPlayer.find(inSessionId);
            if (session != self->sessionToPlayer.end())
            {
                const auto player = self->players.find(session->second);
                if (player != self->players.end() && handler)
                {
                    previousRoomId = player->second.dungeonRoomId;
                    const auto roomId = player->second.dungeonRoomId != 0
                        ? player->second.dungeonRoomId : player->second.reservedDungeonRoomId;
                    if (roomId != 0)
                    {
                        awaitingRoom = true;
                        handler(roomId, session->second, [weakTown = self->weak_from_this(), inSessionId]
                        { if (const auto town = weakTown.lock()) town->ReleaseAdmission(inSessionId); });
                    }
                }
            }
            self->LeaveOnStrand(inSessionId);
            if (!awaitingRoom) self->ReleaseAdmission(inSessionId);
            if (previousRoomId != 0) self->RefreshDungeonLeader(previousRoomId);
        });
    }

    void TownInstance::ApplyMovementInput(const std::uint64_t inSessionId, const TownProtocol::MoveInput inInput)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, inInput]()
        {
            const auto sessionIterator = self->sessionToPlayer.find(inSessionId);
            if (sessionIterator == self->sessionToPlayer.end())
            {
                return;
            }

            const auto playerIterator = self->players.find(sessionIterator->second);
            if (playerIterator != self->players.end() && playerIterator->second.dungeonRoomId == 0)
            {
                auto& entry = playerIterator->second;
                if (inInput.sequence <= entry.player.GetLastProcessedInput()) return;
                // Use the strand's acceptance time so queued input never rewrites an integrated interval.
                const auto now = std::chrono::steady_clock::now();
                const bool changedSector = self->running
                    && self->SimulateMovement(sessionIterator->second, entry, now);
                entry.player.SetMovementInput(inInput, now);
                if (self->running)
                {
                    self->ProcessTransition(sessionIterator->second, entry);
                    if (changedSector) self->RefreshVisibility(sessionIterator->second);
                }
            }
        });
    }

    void TownInstance::ValidateDungeonRequest(const std::uint64_t inSessionId, std::string inZoneId,
        const std::uint32_t inDungeonId, DungeonRequestHandler inHandler)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, zoneId = std::move(inZoneId), inDungeonId,
            handler = std::move(inHandler)]() mutable
        {
            bool valid = false;
            std::vector<PlayerId> participantPlayerIds;
            const auto sessionIterator = self->sessionToPlayer.find(inSessionId);
            if (sessionIterator != self->sessionToPlayer.end())
            {
                const PlayerId requestingPlayerId = sessionIterator->second;
                const auto playerIterator = self->players.find(requestingPlayerId);
                if (playerIterator != self->players.end() && playerIterator->second.dungeonRoomId == 0
                    && playerIterator->second.reservedDungeonRoomId == 0
                    && !self->pendingDungeonPlayers.contains(requestingPlayerId))
                {
                    const PlayerEntry& entry = playerIterator->second;
                    const TownMap& map = self->maps.at(entry.mapId);
                    const TownProtocol::TransitionZone* zone = map.FindTransitionZone(
                        entry.player.GetPosition());
                    valid = zone != nullptr && zone->id == zoneId
                        && zone->actionType == TownProtocol::TransitionActionType::DungeonSelection
                        && self->dungeonCatalog.Contains(zone->dungeonGroupId, inDungeonId);
                    const std::optional<PartyManager::PartyView> party =
                        self->partyManager.GetPartyForPlayer(requestingPlayerId);
                    if (valid && party.has_value())
                    {
                        valid = party->leaderPlayerId == requestingPlayerId
                            && !self->IsPartyBusy(party->partyId);
                        if (valid)
                        {
                            participantPlayerIds.reserve(party->members.size());
                            for (const PartyManager::MemberSlot& member : party->members)
                            {
                                const auto memberIterator = self->players.find(member.playerId);
                                if (memberIterator == self->players.end()
                                    || memberIterator->second.dungeonRoomId != 0)
                                {
                                    valid = false;
                                    participantPlayerIds.clear();
                                    break;
                                }
                                participantPlayerIds.push_back(member.playerId);
                            }
                        }
                    }
                    else if (valid)
                    {
                        participantPlayerIds.push_back(requestingPlayerId);
                    }
                }
            }
            if (valid)
            {
                self->pendingDungeonPlayers.insert(
                    participantPlayerIds.begin(), participantPlayerIds.end());
                self->PrunePartyJoinRequests();
            }
            handler(valid, std::move(participantPlayerIds));
        });
    }

    void TownInstance::CompleteDungeonRequest(std::vector<PlayerId> inParticipantPlayerIds,
        ActionRPG::RoomControlProtocol::CreateRoomResult inResult)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, participantPlayerIds = std::move(inParticipantPlayerIds),
            result = std::move(inResult)]() mutable
        {
            const TownProtocol::EnterDungeonResponse response{
                result.succeeded,
                result.roomId,
                result.combatSeed,
                std::move(result.sessionBrokerAddress),
                result.sessionBrokerPort
            };
            for (const PlayerId playerId : participantPlayerIds)
            {
                self->pendingDungeonPlayers.erase(playerId);
                const auto iterator = self->players.find(playerId);
                if (iterator != self->players.end())
                {
                    if (result.succeeded) iterator->second.reservedDungeonRoomId = result.roomId;
                    iterator->second.session->Send(TownProtocol::Encode(response));
                }
            }
            self->PrunePartyJoinRequests();
        });
    }

    void TownInstance::ValidateDungeonCompletion(const std::uint64_t inSessionId,
        const ActionRPG::RoomControlProtocol::RoomId inRoomId, DungeonRequestHandler inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, inRoomId, handler = std::move(inHandler)]() mutable
        {
            std::vector<PlayerId> participants;
            const auto session = self->sessionToPlayer.find(inSessionId);
            if (session != self->sessionToPlayer.end())
            {
                const auto playerId = session->second;
                const auto party = self->partyManager.GetPartyForPlayer(playerId);
                const auto player = self->players.find(playerId);
                // Missing party members stay in town; the room verifies this actual entered set again.
                if (player != self->players.end() && player->second.dungeonRoomId == inRoomId
                    && (!party || party->leaderPlayerId == playerId))
                    for (const auto& [id, entry] : self->players)
                        if (entry.dungeonRoomId == inRoomId) participants.push_back(id);
            }
            const bool valid = inRoomId != 0 && !participants.empty()
                && std::all_of(participants.begin(), participants.end(), [self, inRoomId](const auto playerId)
                {
                    const auto found = self->players.find(playerId);
                    return found != self->players.end() && found->second.dungeonRoomId == inRoomId
                        && !self->pendingDungeonPlayers.contains(playerId);
                });
            if (valid) self->pendingDungeonPlayers.insert(participants.begin(), participants.end());
            handler(valid, std::move(participants));
        });
    }

    void TownInstance::ValidateDungeonJoin(const PlayerId inPlayerId,
        const ActionRPG::RoomControlProtocol::RoomId inRoomId, std::function<void(bool)> inHandler)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, inRoomId, handler = std::move(inHandler)]()
        {
            const auto player = self->players.find(inPlayerId);
            handler(inRoomId != 0 && player != self->players.end()
                && player->second.dungeonRoomId == 0 && player->second.reservedDungeonRoomId == inRoomId);
        });
    }

    // Response precedes town visibility events so clients reset before recreating remote players.
    void TownInstance::CompleteDungeonCompletion(std::vector<PlayerId> inParticipants, const bool inRetry,
        ActionRPG::RoomControlProtocol::FinishRoomResult inResult)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, participants = std::move(inParticipants), inRetry, result = std::move(inResult)]()
        {
            const TownProtocol::DungeonCompletionResponse response{result.previousRoomId, result.succeeded,
                inRetry, result.roomId, result.combatSeed, result.sessionBrokerAddress, result.sessionBrokerPort};
            std::vector<PlayerId> returningPlayers;
            for (const auto playerId : participants)
            {
                self->pendingDungeonPlayers.erase(playerId);
                const auto found = self->players.find(playerId);
                if (found != self->players.end() && found->second.dungeonRoomId == result.previousRoomId)
                {
                    found->second.session->Send(TownProtocol::Encode(response));
                    returningPlayers.push_back(playerId);
                }
            }
            if (result.succeeded)
                for (const auto playerId : returningPlayers)
                {
                    self->LeaveDungeon(playerId, result.previousRoomId, false);
                    const auto found = self->players.find(playerId);
                    if (inRetry && found != self->players.end()) found->second.reservedDungeonRoomId = result.roomId;
                }
        });
    }

    void TownInstance::CreateParty(const std::uint64_t inSessionId,
        std::string inTitle, const bool inIsPublic)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, title = std::move(inTitle), inIsPublic]() mutable
        {
            const auto sessionIterator = self->sessionToPlayer.find(inSessionId);
            if (sessionIterator == self->sessionToPlayer.end())
            {
                return;
            }
            const PlayerId playerId = sessionIterator->second;
            if (self->pendingDungeonPlayers.contains(playerId)
                || self->players.at(playerId).dungeonRoomId != 0
                || self->players.at(playerId).reservedDungeonRoomId != 0)
            {
                self->SendPartyResult(playerId, TownProtocol::PartyOperationType::Create,
                    TownProtocol::PartyResultCode::Busy);
                return;
            }
            PartyManager::PartyId partyId{};
            const PartyManager::Result result = self->partyManager.Create(
                playerId, std::move(title), inIsPublic, partyId);
            self->SendPartyResult(playerId, TownProtocol::PartyOperationType::Create,
                ToProtocolResult(result));
            if (result == PartyManager::Result::Succeeded)
            {
                self->BroadcastPartySnapshot(partyId);
                if (inIsPublic)
                {
                    self->NotifyPartyDirectoryChanged();
                }
            }
        });
    }

    void TownInstance::InviteToParty(const std::uint64_t inSessionId,
        const PlayerId inTargetPlayerId)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, inTargetPlayerId]()
        {
            const auto sessionIterator = self->sessionToPlayer.find(inSessionId);
            if (sessionIterator == self->sessionToPlayer.end())
            {
                return;
            }
            const PlayerId inviterPlayerId = sessionIterator->second;
            if (!self->players.contains(inTargetPlayerId))
            {
                self->SendPartyResult(inviterPlayerId, TownProtocol::PartyOperationType::Invite,
                    TownProtocol::PartyResultCode::PlayerNotFound);
                return;
            }
            const std::optional<PartyManager::PartyView> currentParty =
                self->partyManager.GetPartyForPlayer(inviterPlayerId);
            if (self->IsPlayerPartyBusy(inviterPlayerId)
                || self->IsPlayerPartyBusy(inTargetPlayerId)
                || (currentParty.has_value() && self->IsPartyBusy(currentParty->partyId)))
            {
                self->SendPartyResult(inviterPlayerId, TownProtocol::PartyOperationType::Invite,
                    TownProtocol::PartyResultCode::Busy);
                return;
            }

            PartyManager::Invitation invitation;
            const PartyManager::Result result = self->partyManager.Invite(
                inviterPlayerId, inTargetPlayerId, invitation);
            self->SendPartyResult(inviterPlayerId, TownProtocol::PartyOperationType::Invite,
                ToProtocolResult(result));
            if (result != PartyManager::Result::Succeeded)
            {
                return;
            }

            self->BroadcastPartySnapshot(invitation.partyId);
            const auto inviterIterator = self->players.find(inviterPlayerId);
            const auto targetIterator = self->players.find(inTargetPlayerId);
            if (inviterIterator != self->players.end() && targetIterator != self->players.end())
            {
                targetIterator->second.session->Send(TownProtocol::Encode(
                    TownProtocol::PartyInvitation{
                        invitation.invitationId,
                        inviterPlayerId,
                        inviterIterator->second.player.GetName()
                    }));
            }
        });
    }

    void TownInstance::AnswerPartyInvitation(const std::uint64_t inSessionId,
        const std::uint64_t inInvitationId, const bool inAccepted)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, inInvitationId, inAccepted]()
        {
            const auto sessionIterator = self->sessionToPlayer.find(inSessionId);
            if (sessionIterator == self->sessionToPlayer.end())
            {
                return;
            }
            const PlayerId playerId = sessionIterator->second;
            const std::optional<PartyManager::Invitation> invitation =
                self->partyManager.GetInvitation(playerId, inInvitationId);
            if (self->IsPlayerPartyBusy(playerId)
                || (invitation.has_value() && self->IsPartyBusy(invitation->partyId)))
            {
                self->SendPartyResult(playerId,
                    TownProtocol::PartyOperationType::AnswerInvitation,
                    TownProtocol::PartyResultCode::Busy);
                return;
            }

            PartyManager::PartyId partyId{};
            const PartyManager::Result result = self->partyManager.AnswerInvitation(
                playerId, inInvitationId, inAccepted, partyId);
            self->SendPartyResult(playerId, TownProtocol::PartyOperationType::AnswerInvitation,
                ToProtocolResult(result));
            if (result == PartyManager::Result::Succeeded && inAccepted)
            {
                self->BroadcastPartySnapshot(partyId);
                if (const auto party = self->partyManager.GetParty(partyId);
                    party.has_value() && party->isPublic)
                {
                    self->NotifyPartyDirectoryChanged();
                }
            }
        });
    }

    void TownInstance::LeaveParty(const std::uint64_t inSessionId)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId]()
        {
            const auto sessionIterator = self->sessionToPlayer.find(inSessionId);
            if (sessionIterator == self->sessionToPlayer.end())
            {
                return;
            }
            const PlayerId playerId = sessionIterator->second;
            const std::optional<PartyManager::PartyView> party =
                self->partyManager.GetPartyForPlayer(playerId);
            if (party.has_value() && self->IsPartyBusy(party->partyId))
            {
                self->SendPartyResult(playerId, TownProtocol::PartyOperationType::Leave,
                    TownProtocol::PartyResultCode::Busy);
                return;
            }

            const PartyManager::LeaveResult result = self->partyManager.Leave(playerId);
            self->SendPartyResult(playerId, TownProtocol::PartyOperationType::Leave,
                ToProtocolResult(result.result));
            if (result.result != PartyManager::Result::Succeeded)
            {
                return;
            }
            self->SendEmptyPartySnapshot(playerId);
            if (!result.disbanded)
            {
                self->BroadcastPartySnapshot(result.partyId);
            }
            if (party.has_value() && party->isPublic)
            {
                self->NotifyPartyDirectoryChanged();
            }
        });
    }

    void TownInstance::KickPartyMember(const std::uint64_t inSessionId,
        const PlayerId inTargetPlayerId)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, inTargetPlayerId]()
        {
            const auto sessionIterator = self->sessionToPlayer.find(inSessionId);
            if (sessionIterator == self->sessionToPlayer.end())
            {
                return;
            }
            const PlayerId leaderPlayerId = sessionIterator->second;
            const std::optional<PartyManager::PartyView> party =
                self->partyManager.GetPartyForPlayer(leaderPlayerId);
            if (party.has_value() && self->IsPartyBusy(party->partyId))
            {
                self->SendPartyResult(leaderPlayerId, TownProtocol::PartyOperationType::Kick,
                    TownProtocol::PartyResultCode::Busy);
                return;
            }

            PartyManager::PartyId partyId{};
            const PartyManager::Result result = self->partyManager.Kick(
                leaderPlayerId, inTargetPlayerId, partyId);
            self->SendPartyResult(leaderPlayerId, TownProtocol::PartyOperationType::Kick,
                ToProtocolResult(result));
            if (result != PartyManager::Result::Succeeded)
            {
                return;
            }
            self->players.at(inTargetPlayerId).session->Send(TownProtocol::Encode(
                TownProtocol::PartyKicked{ partyId, leaderPlayerId }));
            self->SendEmptyPartySnapshot(inTargetPlayerId);
            self->BroadcastPartySnapshot(partyId);
            if (party.has_value() && party->isPublic)
            {
                self->NotifyPartyDirectoryChanged();
            }
        });
    }

    void TownInstance::UpdatePartySettings(const std::uint64_t inSessionId,
        std::string inTitle, const bool inIsPublic)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, title = std::move(inTitle), inIsPublic]() mutable
        {
            const auto sessionIterator = self->sessionToPlayer.find(inSessionId);
            if (sessionIterator == self->sessionToPlayer.end())
            {
                return;
            }
            const PlayerId playerId = sessionIterator->second;
            const auto before = self->partyManager.GetPartyForPlayer(playerId);
            if (before.has_value() && self->IsPartyBusy(before->partyId))
            {
                self->SendPartyResult(playerId, TownProtocol::PartyOperationType::Settings,
                    TownProtocol::PartyResultCode::Busy);
                return;
            }
            const PartyManager::Result result = self->partyManager.SetSettings(
                playerId, std::move(title), inIsPublic);
            self->SendPartyResult(playerId, TownProtocol::PartyOperationType::Settings,
                ToProtocolResult(result));
            if (result != PartyManager::Result::Succeeded)
            {
                return;
            }
            const auto after = self->partyManager.GetPartyForPlayer(playerId);
            self->BroadcastPartySnapshot(after->partyId);
            if ((before.has_value() && before->isPublic) || after->isPublic)
            {
                if (!before.has_value() || before->title != after->title
                    || before->isPublic != after->isPublic)
                {
                    self->NotifyPartyDirectoryChanged();
                }
            }
        });
    }

    void TownInstance::RequestPartyDirectoryPage(const std::uint64_t inSessionId,
        const std::uint32_t inPage)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, inPage]()
        {
            const auto sessionIterator = self->sessionToPlayer.find(inSessionId);
            if (sessionIterator == self->sessionToPlayer.end())
            {
                return;
            }
            const PlayerId playerId = sessionIterator->second;
            if (self->players.at(playerId).dungeonRoomId != 0)
            {
                return;
            }
            self->partyDirectorySubscribers.insert(playerId);
            self->SendPartyDirectoryPage(playerId, inPage);
        });
    }

    void TownInstance::UnsubscribePartyDirectory(const std::uint64_t inSessionId)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId]()
        {
            const auto sessionIterator = self->sessionToPlayer.find(inSessionId);
            if (sessionIterator != self->sessionToPlayer.end())
            {
                self->partyDirectorySubscribers.erase(sessionIterator->second);
            }
        });
    }

    void TownInstance::RequestPartyDetail(const std::uint64_t inSessionId, const PartyManager::PartyId inPartyId)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, inPartyId]()
        {
            const auto session = self->sessionToPlayer.find(inSessionId);
            if (session == self->sessionToPlayer.end()) return;
            TownProtocol::PartyDetailResponse response;
            response.partyId = inPartyId;
            const auto party = self->partyManager.GetParty(inPartyId);
            if (self->IsPlayerPartyBusy(session->second)) response.result = TownProtocol::PartyResultCode::Busy;
            else if (!party) response.result = TownProtocol::PartyResultCode::PartyNotFound;
            else if (!party->isPublic) response.result = TownProtocol::PartyResultCode::NotPublic;
            else
            {
                response.title = party->title;
                response.leaderPlayerId = party->leaderPlayerId;
                response.isPublic = party->isPublic;
                response.busy = self->IsPartyBusy(inPartyId);
                for (const auto& member : party->members)
                {
                    const auto player = self->players.find(member.playerId);
                    if (player == self->players.end())
                    {
                        response = TownProtocol::PartyDetailResponse{};
                        response.partyId = inPartyId;
                        response.result = TownProtocol::PartyResultCode::PlayerNotFound;
                        break;
                    }
                    response.members.push_back({ member.playerId, player->second.player.GetName(), member.slot });
                }
            }
            self->players.at(session->second).session->Send(TownProtocol::Encode(response));
        });
    }

    void TownInstance::RequestPartyJoin(const std::uint64_t inSessionId, const PartyManager::PartyId inPartyId)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, inPartyId]()
        {
            const auto session = self->sessionToPlayer.find(inSessionId);
            if (session == self->sessionToPlayer.end()) return;
            const auto playerId = session->second;
            self->PrunePartyJoinRequests();
            if (std::ranges::any_of(self->partyJoinRequests, [playerId](const auto& inPending)
                { return inPending.second.requesterPlayerId == playerId; }))
            {
                self->SendPartyResult(playerId, TownProtocol::PartyOperationType::RequestJoin,
                    TownProtocol::PartyResultCode::AlreadyRequested);
                return;
            }
            const auto party = self->partyManager.GetParty(inPartyId);
            PendingPartyJoin request{ 0, inPartyId, party ? party->leaderPlayerId : 0,
                playerId, self->players.at(playerId).player.GetName() };
            const auto result = self->ValidatePartyJoin(request);
            self->SendPartyResult(playerId, TownProtocol::PartyOperationType::RequestJoin, result);
            if (result != TownProtocol::PartyResultCode::Succeeded) return;
            request.requestId = self->nextPartyJoinRequestId++;
            self->partyJoinRequests.emplace(request.requestId, request);
            self->SendPartyJoinUpdate(request, TownProtocol::PartyJoinRequestState::Pending, result);
        });
    }

    /** Revalidate and consume one request on the town strand before applying an approved membership change. */
    void TownInstance::AnswerPartyJoin(const std::uint64_t inSessionId, const std::uint64_t inRequestId,
        const bool inAccepted)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, inSessionId, inRequestId, inAccepted]()
        {
            const auto session = self->sessionToPlayer.find(inSessionId);
            if (session == self->sessionToPlayer.end()) return;
            const auto playerId = session->second;
            self->PrunePartyJoinRequests();
            const auto found = self->partyJoinRequests.find(inRequestId);
            if (found == self->partyJoinRequests.end())
            {
                self->SendPartyResult(playerId, TownProtocol::PartyOperationType::AnswerJoin,
                    TownProtocol::PartyResultCode::JoinRequestNotFound);
                return;
            }
            if (found->second.leaderPlayerId != playerId)
            {
                self->SendPartyResult(playerId, TownProtocol::PartyOperationType::AnswerJoin,
                    TownProtocol::PartyResultCode::NotLeader);
                return;
            }
            const auto request = found->second;
            auto result = self->ValidatePartyJoin(request);
            if (result == TownProtocol::PartyResultCode::Succeeded && inAccepted)
                result = ToProtocolResult(self->partyManager.JoinApproved(
                    request.partyId, playerId, request.requesterPlayerId));
            self->partyJoinRequests.erase(found);
            self->SendPartyResult(playerId, TownProtocol::PartyOperationType::AnswerJoin, result);
            const auto state = result != TownProtocol::PartyResultCode::Succeeded
                ? TownProtocol::PartyJoinRequestState::Invalidated
                : inAccepted ? TownProtocol::PartyJoinRequestState::Accepted : TownProtocol::PartyJoinRequestState::Rejected;
            self->SendPartyJoinUpdate(request, state, result);
            if (state == TownProtocol::PartyJoinRequestState::Accepted)
            {
                self->BroadcastPartySnapshot(request.partyId);
                self->NotifyPartyDirectoryChanged();
            }
        });
    }

    void TownInstance::EnterDungeon(
        const PlayerId inPlayerId,
        const ActionRPG::RoomControlProtocol::RoomId inRoomId, std::function<void(bool)> inHandler)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, inRoomId, handler = std::move(inHandler)]()
        {
            handler(self->EnterDungeonOnStrand(inPlayerId, inRoomId));
        });
    }

    /** Apply a confirmed room entry on the town strand; repeated confirmations are harmless. */
    bool TownInstance::EnterDungeonOnStrand(
        const PlayerId inPlayerId, const ActionRPG::RoomControlProtocol::RoomId inRoomId)
    {
        const auto iterator = players.find(inPlayerId);
        if (inRoomId == 0 || iterator == players.end()) return false;
        auto& entry = iterator->second;
        if (entry.dungeonRoomId == inRoomId) return true;
        if (entry.dungeonRoomId != 0 || entry.reservedDungeonRoomId != inRoomId) return false;

        HideFromTown(inPlayerId, entry);
        partyDirectorySubscribers.erase(inPlayerId);
        entry.dungeonRoomId = inRoomId;
        entry.reservedDungeonRoomId = 0;
        entry.player.StopMovement();
        PrunePartyJoinRequests();
        return true;
    }

    void TownInstance::LeaveDungeon(
        const PlayerId inPlayerId,
        const ActionRPG::RoomControlProtocol::RoomId inRoomId, const bool inNotify)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, inPlayerId, inRoomId, inNotify]()
        {
            const auto iterator = self->players.find(inPlayerId);
            if (iterator == self->players.end()) return;
            auto& entry = iterator->second;
            if (entry.reservedDungeonRoomId == inRoomId)
            {
                entry.reservedDungeonRoomId = 0;
                if (inNotify) entry.session->Send(TownProtocol::Encode(TownProtocol::EnterDungeonResponse{}));
            }
            if (entry.dungeonRoomId != inRoomId)
            {
                return;
            }
            // Reuse the existing successful town-return response, before visibility events.
            if (inNotify) entry.session->Send(TownProtocol::Encode(
                TownProtocol::DungeonCompletionResponse{inRoomId, true, false}));
            iterator->second.dungeonRoomId = 0;
            entry.lastSimulationTime = std::chrono::steady_clock::now();
            iterator->second.sector = self->GetSector(
                iterator->second.mapId, iterator->second.player.GetPosition());
            self->AddToSector(inPlayerId, iterator->second.sector);
            self->RefreshVisibility(inPlayerId);
            // Defer leadership selection until a batch return has removed all participants.
            asio::post(self->strand, [self, inRoomId]() { self->RefreshDungeonLeader(inRoomId); });
        });
    }

    void TownInstance::HandleRoomStarted(ActionRPG::RoomControlProtocol::RoomStarted inRoomStarted)
    {
        const auto self = shared_from_this();
        asio::dispatch(strand, [self, started = std::move(inRoomStarted)]()
        {
            const std::unordered_set<PlayerId> participants(
                started.participantPlayerIds.begin(), started.participantPlayerIds.end());
            // Room start also confirms the admitted party, even before individual entry notifications arrive.
            for (const PlayerId playerId : participants)
                static_cast<void>(self->EnterDungeonOnStrand(playerId, started.roomId));
            for (const auto& [id, entry] : self->players)
                if (entry.reservedDungeonRoomId == started.roomId && !participants.contains(id))
                    self->LeaveDungeon(id, started.roomId);
        });
    }

    void TownInstance::HandleRoomEnded(ActionRPG::RoomControlProtocol::RoomEnded inRoomEnded)
    {
        const std::shared_ptr<TownInstance> self = shared_from_this();
        asio::dispatch(strand, [self, roomEnded = std::move(inRoomEnded)]()
        {
            if (roomEnded.reason != ActionRPG::RoomControlProtocol::RoomEndReason::Cleared)
            {
                for (const auto& [id, entry] : self->players)
                    if (entry.dungeonRoomId == roomEnded.roomId || entry.reservedDungeonRoomId == roomEnded.roomId)
                        self->LeaveDungeon(id, roomEnded.roomId);
                return;
            }
            self->RefreshDungeonLeader(roomEnded.roomId);
            for (const PlayerId playerId : roomEnded.rewardPlayerIds)
            {
                const auto iterator = self->players.find(playerId);
                if (iterator != self->players.end() && iterator->second.dungeonRoomId == roomEnded.roomId)
                {
                    std::cout << "Reward target confirmed: room=" << roomEnded.roomId
                        << ", player=" << playerId << '\n';
                }
            }
        });
    }

    void TownInstance::HandleRoomControlLost(ActionRPG::RoomControlProtocol::RoomId inRoomId)
    {
        const auto self = shared_from_this();
        asio::post(strand, [self, inRoomId]
        {
            // Preserve the room association until Disconnect: channel loss is not proof of removal.
            for (const auto& [id, entry] : self->players)
                if (entry.dungeonRoomId == inRoomId || entry.reservedDungeonRoomId == inRoomId)
                    entry.session->Stop();
        });
    }

    std::size_t TownInstance::SectorHash::operator()(const SectorCoordinate& inCoordinate) const noexcept
    {
        const std::uint64_t x = static_cast<std::uint32_t>(inCoordinate.x);
        const std::uint64_t y = static_cast<std::uint32_t>(inCoordinate.y);
        const std::size_t coordinateHash = static_cast<std::size_t>((x << 32) ^ y);
        return std::hash<std::string>{}(inCoordinate.mapId)
            ^ (coordinateHash + 0x9e3779b9U + (coordinateHash << 6) + (coordinateHash >> 2));
    }

    void TownInstance::ScheduleTick()
    {
        tickTimer.expires_at(nextTickTime);
        const std::shared_ptr<TownInstance> self = shared_from_this();
        tickTimer.async_wait([self](const asio::error_code& inError)
        {
            if (!inError && self->running)
            {
                self->Tick();
                self->nextTickTime += TICK_INTERVAL;
                const auto now = std::chrono::steady_clock::now();
                // Skip overdue deadlines after a stall; integrate elapsed time instead of bursting ticks.
                if (self->nextTickTime <= now)
                    self->nextTickTime += TICK_INTERVAL * ((now - self->nextTickTime) / TICK_INTERVAL + 1);
                self->ScheduleTick();
            }
        });
    }

    void TownInstance::Tick()
    {
        ++serverTick;
        const auto now = std::chrono::steady_clock::now();
        std::vector<PlayerId> changedSectorPlayers;
        for (auto& [playerId, entry] : players)
        {
            if (entry.session->GetAccountId() == 0) { entry.session->Stop(); continue; }
            if (entry.dungeonRoomId != 0)
            {
                continue;
            }
            if (SimulateMovement(playerId, entry, now))
            {
                changedSectorPlayers.push_back(playerId);
            }
            ProcessTransition(playerId, entry);
        }

        for (const PlayerId playerId : changedSectorPlayers)
        {
            RefreshVisibility(playerId);
        }

        if (serverTick % SNAPSHOT_TICK_INTERVAL == 0)
        {
            BroadcastMovement();
        }
    }

    /** Integrate the current input up to inNow on the town strand, applying collision and sector limits. */
    bool TownInstance::SimulateMovement(const PlayerId inPlayerId, PlayerEntry& inEntry,
        const std::chrono::steady_clock::time_point inNow)
    {
        const float deltaSeconds = std::clamp(
            std::chrono::duration<float>(inNow - inEntry.lastSimulationTime).count(),
            0.0f, MAX_SIMULATION_SECONDS);
        inEntry.lastSimulationTime = inNow;
        TownMap& currentMap = maps.at(inEntry.mapId);
        const TownProtocol::Vector2 previousPosition = inEntry.player.GetPosition();
        inEntry.player.Simulate(deltaSeconds, currentMap.GetInfo().walkSpeed, inNow);
        const TownProtocol::Vector2 proposedPosition = inEntry.player.GetPosition();
        const TownProtocol::Vector2 constrainedPosition = currentMap.ConstrainMovement(
            previousPosition, proposedPosition);
        inEntry.player.SetPosition(constrainedPosition);
        if (constrainedPosition.x != proposedPosition.x || constrainedPosition.y != proposedPosition.y)
        {
            inEntry.player.StopMovement();
        }

        const SectorCoordinate newSector = GetSector(inEntry.mapId, inEntry.player.GetPosition());
        if (newSector == inEntry.sector) return false;
        RemoveFromSector(inPlayerId, inEntry.sector);
        inEntry.sector = newSector;
        AddToSector(inPlayerId, inEntry.sector);
        return true;
    }

    void TownInstance::EnterOnStrand(std::shared_ptr<Network::PlayerSession> inSession,
        std::string inPlayerName, const std::uint32_t inCharacterId)
    {
        const std::uint64_t sessionId = inSession->GetSessionId();
        const auto accountId = inSession->GetAccountId();
        if (!running || accountId == 0)
        {
            inSession->Stop();
            return;
        }
        if (sessionToPlayer.contains(sessionId))
        {
            return;
        }

        if (!playerSkills.characterIds.contains("Character" + std::to_string(inCharacterId)))
        {
            inSession->Disconnect();
            return;
        }
        auto progression = progressionPolicy.Create();

        const PlayerId playerId = nextPlayerId++;
        const TownProtocol::MapInfo& mapInfo = maps.at(defaultMapId).GetInfo();
        const TownProtocol::Vector2 spawn{ mapInfo.spawnX, mapInfo.spawnY };
        const SectorCoordinate sector = GetSector(defaultMapId, spawn);

        PlayerEntry entry{
            Player(playerId, std::move(inPlayerName), inCharacterId, spawn),
            std::move(inSession),
            defaultMapId,
            sector,
            {},
            spawn,
            false,
            0,
            {}
        };
        entry.progression = std::move(progression);
        entry.accountId = accountId;
        players.emplace(playerId, std::move(entry));
        sessionToPlayer.emplace(sessionId, playerId);
        AddToSector(playerId, sector);

        PlayerEntry& playerEntry = players.at(playerId);
        playerEntry.session->SetPlayerId(playerId);
        playerEntry.session->Send(TownProtocol::Encode(TownProtocol::EnterTownResponse{
            playerId,
            playerEntry.player.GetCharacterId(),
            mapInfo
        }));
        RefreshVisibility(playerId);
        SendSkillState(playerEntry, "State");
    }

    void TownInstance::LeaveOnStrand(const std::uint64_t inSessionId)
    {
        const auto sessionIterator = sessionToPlayer.find(inSessionId);
        if (sessionIterator == sessionToPlayer.end())
        {
            return;
        }

        const PlayerId playerId = sessionIterator->second;
        partyDirectorySubscribers.erase(playerId);
        auto playerIterator = players.find(playerId);
        if (playerIterator == players.end())
        {
            sessionToPlayer.erase(sessionIterator);
            return;
        }

        pendingDungeonPlayers.erase(playerId);
        const auto currentParty = partyManager.GetPartyForPlayer(playerId);
        const PartyManager::LeaveResult partyResult = partyManager.RemovePlayer(playerId);
        if (partyResult.result == PartyManager::Result::Succeeded && !partyResult.disbanded)
        {
            BroadcastPartySnapshot(partyResult.partyId);
        }
        if (partyResult.result == PartyManager::Result::Succeeded
            && currentParty.has_value() && currentParty->isPublic)
        {
            NotifyPartyDirectoryChanged();
        }

        if (playerIterator->second.dungeonRoomId == 0)
        {
            HideFromTown(playerId, playerIterator->second);
        }
        players.erase(playerIterator);
        sessionToPlayer.erase(sessionIterator);
        PrunePartyJoinRequests();
    }

    void TownInstance::HideFromTown(const PlayerId inPlayerId, PlayerEntry& inEntry)
    {
        const std::vector<PlayerId> visiblePlayers(
            inEntry.visiblePlayers.begin(), inEntry.visiblePlayers.end());
        for (const PlayerId visibleId : visiblePlayers)
        {
            const auto visibleIterator = players.find(visibleId);
            if (visibleIterator != players.end())
            {
                visibleIterator->second.visiblePlayers.erase(inPlayerId);
                SendDisappear(visibleIterator->second, inPlayerId);
            }
        }
        inEntry.visiblePlayers.clear();
        RemoveFromSector(inPlayerId, inEntry.sector);
    }

    void TownInstance::ProcessTransition(const PlayerId inPlayerId, PlayerEntry& inEntry)
    {
        const TownMap& currentMap = maps.at(inEntry.mapId);
        const TownProtocol::TransitionZone* zone = currentMap.FindTransitionZone(
            inEntry.player.GetPosition());
        if (zone == nullptr)
        {
            inEntry.activeTransitionZoneId.clear();
            return;
        }
        if (inEntry.activeTransitionZoneId == zone->id)
        {
            return;
        }

        inEntry.activeTransitionZoneId = zone->id;
        inEntry.player.StopMovement();
        if (zone->actionType == TownProtocol::TransitionActionType::MapTransfer)
        {
            static_cast<void>(TransferMap(inPlayerId, inEntry, *zone));
            return;
        }

        const std::vector<TownProtocol::DungeonOption>* dungeons = dungeonCatalog.FindGroup(
            zone->dungeonGroupId);
        if (dungeons != nullptr)
        {
            inEntry.session->Send(TownProtocol::Encode(TownProtocol::DungeonSelectionOpen{
                zone->id,
                zone->dungeonGroupId,
                *dungeons
            }));
        }
    }

    bool TownInstance::TransferMap(const PlayerId inPlayerId, PlayerEntry& inEntry,
        const TownProtocol::TransitionZone& inZone)
    {
        const auto targetMapIterator = maps.find(inZone.targetMapId);
        if (targetMapIterator == maps.end())
        {
            return false;
        }
        const TownProtocol::EntryPoint* entryPoint = targetMapIterator->second.FindEntryPoint(
            inZone.targetEntryPointId);
        if (entryPoint == nullptr)
        {
            return false;
        }

        HideFromTown(inPlayerId, inEntry);
        inEntry.mapId = inZone.targetMapId;
        inEntry.player.SetPosition(entryPoint->position);
        inEntry.player.StopMovement();
        inEntry.sector = GetSector(inEntry.mapId, entryPoint->position);
        inEntry.lastBroadcastPosition = entryPoint->position;
        inEntry.wasMovingOnLastBroadcast = false;
        AddToSector(inPlayerId, inEntry.sector);

        const TownProtocol::TransitionZone* arrivalZone = targetMapIterator->second.FindTransitionZone(
            entryPoint->position);
        inEntry.activeTransitionZoneId = arrivalZone == nullptr ? std::string{} : arrivalZone->id;
        inEntry.session->Send(TownProtocol::Encode(TownProtocol::MapChanged{
            targetMapIterator->second.GetInfo(),
            entryPoint->position
        }));
        RefreshVisibility(inPlayerId);
        return true;
    }

    void TownInstance::RefreshVisibility(const PlayerId inPlayerId)
    {
        auto playerIterator = players.find(inPlayerId);
        if (playerIterator == players.end())
        {
            return;
        }

        PlayerEntry& entry = playerIterator->second;
        const std::unordered_set<PlayerId> newVisiblePlayers = FindVisiblePlayers(inPlayerId);
        const std::vector<PlayerId> oldVisiblePlayers(entry.visiblePlayers.begin(), entry.visiblePlayers.end());

        for (const PlayerId previousId : oldVisiblePlayers)
        {
            if (newVisiblePlayers.contains(previousId))
            {
                continue;
            }

            entry.visiblePlayers.erase(previousId);
            SendDisappear(entry, previousId);
            if (auto previousIterator = players.find(previousId); previousIterator != players.end())
            {
                previousIterator->second.visiblePlayers.erase(inPlayerId);
                SendDisappear(previousIterator->second, inPlayerId);
            }
        }

        for (const PlayerId newId : newVisiblePlayers)
        {
            if (entry.visiblePlayers.contains(newId))
            {
                continue;
            }

            auto newIterator = players.find(newId);
            if (newIterator == players.end())
            {
                continue;
            }

            entry.visiblePlayers.insert(newId);
            newIterator->second.visiblePlayers.insert(inPlayerId);
            SendAppear(entry, newIterator->second);
            SendAppear(newIterator->second, entry);
        }
    }

    void TownInstance::BroadcastMovement()
    {
        for (auto& [playerId, entry] : players)
        {
            if (entry.dungeonRoomId != 0)
            {
                continue;
            }
            const TownProtocol::Vector2 position = entry.player.GetPosition();
            const TownProtocol::Vector2 velocity = entry.player.GetVelocity();
            const bool isMoving = velocity.x != 0.0f || velocity.y != 0.0f;
            const bool positionChanged = position.x != entry.lastBroadcastPosition.x
                || position.y != entry.lastBroadcastPosition.y;
            if (!isMoving && !entry.wasMovingOnLastBroadcast && !positionChanged)
            {
                continue;
            }

            const TownProtocol::PlayerMove movement{
                playerId,
                serverTick,
                entry.player.GetLastProcessedInput(),
                position,
                velocity
            };
            const std::vector<std::uint8_t> encoded = TownProtocol::Encode(movement);
            entry.session->Send(encoded);

            for (const PlayerId observerId : entry.visiblePlayers)
            {
                if (auto observerIterator = players.find(observerId); observerIterator != players.end())
                {
                    observerIterator->second.session->Send(encoded);
                }
            }

            entry.lastBroadcastPosition = position;
            entry.wasMovingOnLastBroadcast = isMoving;
        }
    }

    void TownInstance::AddToSector(const PlayerId inPlayerId, const SectorCoordinate inSector)
    {
        sectors[inSector].insert(inPlayerId);
    }

    void TownInstance::RemoveFromSector(const PlayerId inPlayerId, const SectorCoordinate inSector)
    {
        const auto iterator = sectors.find(inSector);
        if (iterator == sectors.end())
        {
            return;
        }
        iterator->second.erase(inPlayerId);
        if (iterator->second.empty())
        {
            sectors.erase(iterator);
        }
    }

    void TownInstance::SendAppear(PlayerEntry& inReceiver, const PlayerEntry& inSubject)
    {
        inReceiver.session->Send(TownProtocol::Encode(TownProtocol::PlayerAppear{
            inSubject.player.GetId(),
            inSubject.player.GetName(),
            inSubject.player.GetCharacterId(),
            inSubject.player.GetPosition(),
            inSubject.player.GetVelocity()
        }));
    }

    void TownInstance::SendDisappear(PlayerEntry& inReceiver, const PlayerId inSubjectId)
    {
        inReceiver.session->Send(TownProtocol::Encode(TownProtocol::PlayerDisappear{ inSubjectId }));
    }

    void TownInstance::SendPartyResult(const PlayerId inPlayerId,
        const TownProtocol::PartyOperationType inOperation,
        const TownProtocol::PartyResultCode inResult)
    {
        const auto iterator = players.find(inPlayerId);
        if (iterator != players.end())
        {
            iterator->second.session->Send(TownProtocol::Encode(
                TownProtocol::PartyOperationResult{ inOperation, inResult }));
        }
    }

    void TownInstance::SendEmptyPartySnapshot(const PlayerId inPlayerId)
    {
        const auto iterator = players.find(inPlayerId);
        if (iterator != players.end())
        {
            iterator->second.session->Send(TownProtocol::Encode(TownProtocol::PartySnapshot{}));
        }
    }

    void TownInstance::BroadcastPartySnapshot(const PartyManager::PartyId inPartyId)
    {
        PrunePartyJoinRequests();
        const std::optional<PartyManager::PartyView> party = partyManager.GetParty(inPartyId);
        if (!party.has_value())
        {
            return;
        }

        TownProtocol::PartySnapshot snapshot;
        snapshot.partyId = party->partyId;
        snapshot.leaderPlayerId = party->leaderPlayerId;
        snapshot.title = party->title;
        snapshot.isPublic = party->isPublic;
        snapshot.members.reserve(party->members.size());
        for (const PartyManager::MemberSlot& member : party->members)
        {
            const auto playerIterator = players.find(member.playerId);
            if (playerIterator == players.end())
            {
                continue;
            }
            snapshot.members.push_back(TownProtocol::PartyMemberInfo{
                member.playerId,
                playerIterator->second.player.GetName(),
                member.slot
            });
        }

        const std::vector<std::uint8_t> encoded = TownProtocol::Encode(snapshot);
        for (const PartyManager::MemberSlot& member : party->members)
        {
            const auto playerIterator = players.find(member.playerId);
            if (playerIterator != players.end())
            {
                playerIterator->second.session->Send(encoded);
            }
        }
    }

    void TownInstance::SendPartyDirectoryPage(const PlayerId inPlayerId,
        const std::uint32_t inPage)
    {
        constexpr std::uint32_t PAGE_SIZE = 8;
        const auto playerIterator = players.find(inPlayerId);
        if (playerIterator == players.end())
        {
            return;
        }
        const std::vector<PartyManager::PartyView> publicParties = partyManager.GetPublicParties();
        const std::uint32_t totalCount = static_cast<std::uint32_t>(publicParties.size());
        const std::uint32_t totalPages = std::max(1U,
            (totalCount + PAGE_SIZE - 1) / PAGE_SIZE);
        const std::uint32_t page = std::clamp(inPage, 1U, totalPages);
        TownProtocol::PartyDirectoryPage response;
        response.page = page;
        response.totalPages = totalPages;
        response.totalCount = totalCount;
        response.revision = partyDirectoryRevision;
        const std::size_t first = static_cast<std::size_t>(page - 1) * PAGE_SIZE;
        const std::size_t last = std::min(first + PAGE_SIZE, publicParties.size());
        for (std::size_t index = first; index < last; ++index)
        {
            const PartyManager::PartyView& party = publicParties[index];
            const auto leaderIterator = players.find(party.leaderPlayerId);
            if (leaderIterator == players.end())
            {
                continue;
            }
            TownProtocol::PartyDirectoryEntry entry;
            entry.partyId = party.partyId;
            entry.title = party.title;
            entry.leaderName = leaderIterator->second.player.GetName();
            for (const PartyManager::MemberSlot& member : party.members)
            {
                const auto memberIterator = players.find(member.playerId);
                if (memberIterator != players.end())
                {
                    entry.members.push_back(TownProtocol::PartyMemberInfo{
                        member.playerId, memberIterator->second.player.GetName(), member.slot
                    });
                }
            }
            response.parties.push_back(std::move(entry));
        }
        playerIterator->second.session->Send(TownProtocol::Encode(response));
    }

    void TownInstance::NotifyPartyDirectoryChanged()
    {
        PrunePartyJoinRequests();
        const std::vector<std::uint8_t> packet = TownProtocol::Encode(
            TownProtocol::PartyDirectoryChanged{ ++partyDirectoryRevision });
        for (const PlayerId playerId : partyDirectorySubscribers)
        {
            const auto iterator = players.find(playerId);
            if (iterator != players.end())
            {
                iterator->second.session->Send(packet);
            }
        }
    }

    bool TownInstance::IsPartyBusy(const PartyManager::PartyId inPartyId) const
    {
        const std::optional<PartyManager::PartyView> party = partyManager.GetParty(inPartyId);
        if (!party.has_value())
        {
            return false;
        }
        return std::ranges::any_of(party->members,
            [this](const PartyManager::MemberSlot& inMember)
            {
                const auto player = players.find(inMember.playerId);
                return pendingDungeonPlayers.contains(inMember.playerId)
                    || (player != players.end() && (player->second.dungeonRoomId != 0
                        || player->second.reservedDungeonRoomId != 0));
            });
    }

    bool TownInstance::IsPlayerPartyBusy(const PlayerId inPlayerId) const
    {
        const auto found = players.find(inPlayerId);
        return pendingDungeonPlayers.contains(inPlayerId)
            || (found != players.end() && (found->second.dungeonRoomId != 0
                || found->second.reservedDungeonRoomId != 0));
    }

    TownProtocol::PartyResultCode TownInstance::ValidatePartyJoin(const PendingPartyJoin& inRequest) const
    {
        const auto party = partyManager.GetParty(inRequest.partyId);
        if (!party) return TownProtocol::PartyResultCode::PartyNotFound;
        if (party->leaderPlayerId != inRequest.leaderPlayerId) return TownProtocol::PartyResultCode::NotLeader;
        if (!party->isPublic) return TownProtocol::PartyResultCode::NotPublic;
        if (!players.contains(inRequest.requesterPlayerId) || !players.contains(inRequest.leaderPlayerId))
            return TownProtocol::PartyResultCode::PlayerNotFound;
        if (partyManager.GetPartyForPlayer(inRequest.requesterPlayerId)) return TownProtocol::PartyResultCode::AlreadyInParty;
        if (IsPlayerPartyBusy(inRequest.requesterPlayerId) || IsPartyBusy(inRequest.partyId))
            return TownProtocol::PartyResultCode::Busy;
        if (party->members.size() >= PartyManager::MAX_PARTY_MEMBERS) return TownProtocol::PartyResultCode::PartyFull;
        return TownProtocol::PartyResultCode::Succeeded;
    }

    void TownInstance::SendPartyJoinUpdate(const PendingPartyJoin& inRequest,
        const TownProtocol::PartyJoinRequestState inState, const TownProtocol::PartyResultCode inResult)
    {
        const auto packet = TownProtocol::Encode(TownProtocol::PartyJoinRequestUpdate{
            inRequest.requestId, inRequest.partyId, inRequest.requesterPlayerId, inRequest.requesterName, inState, inResult });
        for (const auto playerId : { inRequest.leaderPlayerId, inRequest.requesterPlayerId })
            if (const auto player = players.find(playerId); player != players.end()) player->second.session->Send(packet);
    }

    /** Remove obsolete prompts in request order; called only on the owning town strand. */
    void TownInstance::PrunePartyJoinRequests()
    {
        for (auto iterator = partyJoinRequests.begin(); iterator != partyJoinRequests.end();)
        {
            const auto result = ValidatePartyJoin(iterator->second);
            if (result == TownProtocol::PartyResultCode::Succeeded) ++iterator;
            else
            {
                SendPartyJoinUpdate(iterator->second, TownProtocol::PartyJoinRequestState::Invalidated, result);
                iterator = partyJoinRequests.erase(iterator);
            }
        }
    }

    // Keep the client-visible party leader eligible to choose for the actual room participants.
    void TownInstance::RefreshDungeonLeader(const ActionRPG::RoomControlProtocol::RoomId inRoomId)
    {
        for (const auto& [id, entry] : players)
        {
            if (entry.dungeonRoomId != inRoomId) continue;
            const auto party = partyManager.GetPartyForPlayer(id);
            if (!party) continue;
            const auto leader = players.find(party->leaderPlayerId);
            if (leader != players.end() && leader->second.dungeonRoomId == inRoomId) continue;
            for (const auto& member : party->members)
            {
                const auto player = players.find(member.playerId);
                if (player == players.end() || player->second.dungeonRoomId != inRoomId) continue;
                if (partyManager.SetLeader(party->partyId, member.playerId))
                {
                    BroadcastPartySnapshot(party->partyId);
                    if (party->isPublic) NotifyPartyDirectoryChanged();
                }
                break;
            }
        }
    }

    TownInstance::SectorCoordinate TownInstance::GetSector(const std::string_view inMapId,
        const TownProtocol::Vector2 inPosition) const
    {
        const TownProtocol::MapInfo& mapInfo = maps.at(std::string(inMapId)).GetInfo();
        return SectorCoordinate{
            std::string(inMapId),
            static_cast<int>(std::floor((inPosition.x - mapInfo.worldLeft) / mapInfo.sectorWidth)),
            static_cast<int>(std::floor((inPosition.y - mapInfo.worldTop) / mapInfo.sectorHeight))
        };
    }

    std::unordered_set<PlayerId> TownInstance::FindVisiblePlayers(const PlayerId inPlayerId) const
    {
        std::unordered_set<PlayerId> result;
        const auto playerIterator = players.find(inPlayerId);
        if (playerIterator == players.end() || playerIterator->second.dungeonRoomId != 0)
        {
            return result;
        }

        const SectorCoordinate center = playerIterator->second.sector;
        for (int y = center.y - 1; y <= center.y + 1; ++y)
        {
            for (int x = center.x - 1; x <= center.x + 1; ++x)
            {
                const auto sectorIterator = sectors.find(SectorCoordinate{ center.mapId, x, y });
                if (sectorIterator == sectors.end())
                {
                    continue;
                }
                for (const PlayerId candidateId : sectorIterator->second)
                {
                    const auto candidate = players.find(candidateId);
                    if (candidate != players.end() && candidate->second.dungeonRoomId == 0)
                    {
                        result.insert(candidateId);
                    }
                }
            }
        }
        result.erase(inPlayerId);
        return result;
    }
}
