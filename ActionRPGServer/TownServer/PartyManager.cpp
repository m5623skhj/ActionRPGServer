#include "PartyManager.h"

#include <algorithm>

namespace TownServer::Domain
{
    PartyManager::Result PartyManager::Invite(const PlayerId inInviterPlayerId,
        const PlayerId inTargetPlayerId, Invitation& outInvitation)
    {
        if (inInviterPlayerId == 0 || inTargetPlayerId == 0
            || inInviterPlayerId == inTargetPlayerId)
        {
            return Result::InvalidTarget;
        }
        if (playerParties.contains(inTargetPlayerId))
        {
            return Result::AlreadyInParty;
        }
        if (invitationsByTarget.contains(inTargetPlayerId))
        {
            return Result::AlreadyInvited;
        }

        Party* party = nullptr;
        const auto partyIdIterator = playerParties.find(inInviterPlayerId);
        if (partyIdIterator == playerParties.end())
        {
            party = &CreateParty(inInviterPlayerId);
        }
        else
        {
            party = &parties.at(partyIdIterator->second);
            if (party->leaderPlayerId != inInviterPlayerId)
            {
                return Result::NotLeader;
            }
        }

        if (!FindEmptySlot(*party).has_value())
        {
            return Result::PartyFull;
        }

        outInvitation = Invitation{
            nextInvitationId++, party->partyId, inInviterPlayerId, inTargetPlayerId
        };
        invitationsByTarget.emplace(inTargetPlayerId, outInvitation);
        return Result::Succeeded;
    }

    PartyManager::Result PartyManager::AnswerInvitation(const PlayerId inTargetPlayerId,
        const InvitationId inInvitationId, const bool inAccepted, PartyId& outPartyId)
    {
        const auto invitationIterator = invitationsByTarget.find(inTargetPlayerId);
        if (invitationIterator == invitationsByTarget.end()
            || invitationIterator->second.invitationId != inInvitationId)
        {
            return Result::InvitationNotFound;
        }

        const Invitation invitation = invitationIterator->second;
        invitationsByTarget.erase(invitationIterator);
        outPartyId = invitation.partyId;
        if (!inAccepted)
        {
            return Result::Succeeded;
        }
        if (playerParties.contains(inTargetPlayerId))
        {
            return Result::AlreadyInParty;
        }

        const auto partyIterator = parties.find(invitation.partyId);
        if (partyIterator == parties.end()
            || partyIterator->second.leaderPlayerId != invitation.inviterPlayerId)
        {
            return Result::InvitationNotFound;
        }

        Party& party = partyIterator->second;
        const std::optional<std::uint8_t> emptySlot = FindEmptySlot(party);
        if (!emptySlot.has_value())
        {
            return Result::PartyFull;
        }

        party.slots[*emptySlot] = inTargetPlayerId;
        playerParties.emplace(inTargetPlayerId, party.partyId);
        return Result::Succeeded;
    }

    PartyManager::LeaveResult PartyManager::Leave(const PlayerId inPlayerId)
    {
        const auto playerPartyIterator = playerParties.find(inPlayerId);
        if (playerPartyIterator == playerParties.end())
        {
            return {};
        }

        const PartyId partyId = playerPartyIterator->second;
        Party& party = parties.at(partyId);
        for (std::optional<PlayerId>& slot : party.slots)
        {
            if (slot == inPlayerId)
            {
                slot.reset();
                break;
            }
        }
        playerParties.erase(playerPartyIterator);

        const auto nextLeader = std::find_if(party.slots.begin(), party.slots.end(),
            [](const std::optional<PlayerId>& inSlot)
            {
                return inSlot.has_value();
            });
        if (nextLeader == party.slots.end())
        {
            CancelInvitations(partyId);
            parties.erase(partyId);
            return LeaveResult{ Result::Succeeded, partyId, true };
        }

        if (party.leaderPlayerId == inPlayerId)
        {
            party.leaderPlayerId = **nextLeader;
            CancelInvitations(partyId);
        }
        return LeaveResult{ Result::Succeeded, partyId, false };
    }

    PartyManager::Result PartyManager::Kick(const PlayerId inLeaderPlayerId,
        const PlayerId inTargetPlayerId, PartyId& outPartyId)
    {
        if (inLeaderPlayerId == inTargetPlayerId)
        {
            return Result::InvalidTarget;
        }
        const auto leaderPartyIterator = playerParties.find(inLeaderPlayerId);
        if (leaderPartyIterator == playerParties.end())
        {
            return Result::NotInParty;
        }

        Party& party = parties.at(leaderPartyIterator->second);
        if (party.leaderPlayerId != inLeaderPlayerId)
        {
            return Result::NotLeader;
        }
        const auto targetPartyIterator = playerParties.find(inTargetPlayerId);
        if (targetPartyIterator == playerParties.end()
            || targetPartyIterator->second != party.partyId)
        {
            return Result::InvalidTarget;
        }

        outPartyId = party.partyId;
        for (std::optional<PlayerId>& slot : party.slots)
        {
            if (slot == inTargetPlayerId)
            {
                slot.reset();
                break;
            }
        }
        playerParties.erase(targetPartyIterator);
        return Result::Succeeded;
    }

    PartyManager::LeaveResult PartyManager::RemovePlayer(const PlayerId inPlayerId)
    {
        invitationsByTarget.erase(inPlayerId);
        return Leave(inPlayerId);
    }

    std::optional<PartyManager::PartyView> PartyManager::GetPartyForPlayer(
        const PlayerId inPlayerId) const
    {
        const auto iterator = playerParties.find(inPlayerId);
        return iterator == playerParties.end() ? std::nullopt : GetParty(iterator->second);
    }

    std::optional<PartyManager::PartyView> PartyManager::GetParty(const PartyId inPartyId) const
    {
        const auto iterator = parties.find(inPartyId);
        return iterator == parties.end() ? std::nullopt
            : std::optional<PartyView>(MakeView(iterator->second));
    }

    std::optional<PartyManager::Invitation> PartyManager::GetInvitation(
        const PlayerId inTargetPlayerId, const InvitationId inInvitationId) const
    {
        const auto iterator = invitationsByTarget.find(inTargetPlayerId);
        if (iterator == invitationsByTarget.end()
            || iterator->second.invitationId != inInvitationId)
        {
            return std::nullopt;
        }
        return iterator->second;
    }

    bool PartyManager::IsLeader(const PlayerId inPlayerId) const
    {
        const std::optional<PartyView> party = GetPartyForPlayer(inPlayerId);
        return party.has_value() && party->leaderPlayerId == inPlayerId;
    }

    PartyManager::Party& PartyManager::CreateParty(const PlayerId inLeaderPlayerId)
    {
        const PartyId partyId = nextPartyId++;
        Party party;
        party.partyId = partyId;
        party.leaderPlayerId = inLeaderPlayerId;
        party.slots[0] = inLeaderPlayerId;
        playerParties.emplace(inLeaderPlayerId, partyId);
        return parties.emplace(partyId, std::move(party)).first->second;
    }

    std::optional<std::uint8_t> PartyManager::FindEmptySlot(const Party& inParty)
    {
        for (std::size_t index = 0; index < inParty.slots.size(); ++index)
        {
            if (!inParty.slots[index].has_value())
            {
                return static_cast<std::uint8_t>(index);
            }
        }
        return std::nullopt;
    }

    PartyManager::PartyView PartyManager::MakeView(const Party& inParty) const
    {
        PartyView view{ inParty.partyId, inParty.leaderPlayerId, {} };
        view.members.reserve(MAX_PARTY_MEMBERS);
        for (std::size_t index = 0; index < inParty.slots.size(); ++index)
        {
            if (inParty.slots[index].has_value())
            {
                view.members.push_back(MemberSlot{
                    *inParty.slots[index], static_cast<std::uint8_t>(index)
                });
            }
        }
        return view;
    }

    void PartyManager::CancelInvitations(const PartyId inPartyId)
    {
        for (auto iterator = invitationsByTarget.begin(); iterator != invitationsByTarget.end();)
        {
            if (iterator->second.partyId == inPartyId)
            {
                iterator = invitationsByTarget.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
    }
}
