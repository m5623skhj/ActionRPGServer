#include "PartyManager.h"

#include <algorithm>
#include <string_view>

namespace
{
    bool IsValidPartyTitle(const std::string_view inTitle)
    {
        if (inTitle.empty() || inTitle.size() > 96)
        {
            return false;
        }
        bool hasVisibleCharacter = false;
        for (std::size_t index = 0; index < inTitle.size();)
        {
            const auto first = static_cast<unsigned char>(inTitle[index]);
            if (first < 0x80)
            {
                if (first < 0x20 || first == 0x7F)
                {
                    return false;
                }
                hasVisibleCharacter = hasVisibleCharacter || first != ' ';
                ++index;
                continue;
            }
            const std::size_t length = first >= 0xC2 && first <= 0xDF ? 2
                : first >= 0xE0 && first <= 0xEF ? 3
                : first >= 0xF0 && first <= 0xF4 ? 4 : 0;
            if (length == 0 || index + length > inTitle.size())
            {
                return false;
            }
            const auto second = static_cast<unsigned char>(inTitle[index + 1]);
            if (second < 0x80 || second > 0xBF
                || (first == 0xE0 && second < 0xA0)
                || (first == 0xED && second > 0x9F)
                || (first == 0xF0 && second < 0x90)
                || (first == 0xF4 && second > 0x8F))
            {
                return false;
            }
            for (std::size_t offset = 2; offset < length; ++offset)
            {
                const auto continuation = static_cast<unsigned char>(inTitle[index + offset]);
                if (continuation < 0x80 || continuation > 0xBF)
                {
                    return false;
                }
            }
            hasVisibleCharacter = true;
            index += length;
        }
        return hasVisibleCharacter;
    }
}

namespace TownServer::Domain
{
    PartyManager::Result PartyManager::Create(const PlayerId inPlayerId,
        std::string inTitle, const bool inIsPublic, PartyId& outPartyId)
    {
        if (inPlayerId == 0)
        {
            return Result::InvalidTarget;
        }
        if (playerParties.contains(inPlayerId))
        {
            return Result::AlreadyInParty;
        }
        if (!IsValidPartyTitle(inTitle))
        {
            return Result::InvalidTitle;
        }
        Party& party = CreateParty(inPlayerId);
        party.title = std::move(inTitle);
        party.isPublic = inIsPublic;
        outPartyId = party.partyId;
        return Result::Succeeded;
    }

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

    PartyManager::Result PartyManager::SetSettings(const PlayerId inLeaderPlayerId,
        std::string inTitle, const bool inIsPublic)
    {
        const auto partyIterator = playerParties.find(inLeaderPlayerId);
        if (partyIterator == playerParties.end())
        {
            return Result::NotInParty;
        }
        Party& party = parties.at(partyIterator->second);
        if (party.leaderPlayerId != inLeaderPlayerId)
        {
            return Result::NotLeader;
        }
        if (!IsValidPartyTitle(inTitle))
        {
            return Result::InvalidTitle;
        }
        party.title = std::move(inTitle);
        party.isPublic = inIsPublic;
        return Result::Succeeded;
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

    std::vector<PartyManager::PartyView> PartyManager::GetPublicParties() const
    {
        std::vector<PartyView> result;
        for (const auto& [partyId, party] : parties)
        {
            if (party.isPublic)
            {
                result.push_back(MakeView(party));
            }
        }
        std::ranges::sort(result, {}, &PartyView::partyId);
        return result;
    }

    PartyManager::Party& PartyManager::CreateParty(const PlayerId inLeaderPlayerId)
    {
        const PartyId partyId = nextPartyId++;
        Party party;
        party.partyId = partyId;
        party.leaderPlayerId = inLeaderPlayerId;
        party.title = "Party #" + std::to_string(partyId);
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
        PartyView view{ inParty.partyId, inParty.leaderPlayerId,
            inParty.title, inParty.isPublic, {} };
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
