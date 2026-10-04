#pragma once

#include "Player.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace TownServer::Domain
{
    class PartyManager final
    {
    public:
        using PartyId = std::uint64_t;
        using InvitationId = std::uint64_t;

        static constexpr std::size_t MAX_PARTY_MEMBERS = 8;

        enum class Result
        {
            Succeeded,
            AlreadyInParty,
            NotInParty,
            NotLeader,
            PartyFull,
            AlreadyInvited,
            InvitationNotFound,
            InvalidTarget,
            InvalidTitle
        };

        struct MemberSlot
        {
            PlayerId playerId{};
            std::uint8_t slot{};
        };

        struct PartyView
        {
            PartyId partyId{};
            PlayerId leaderPlayerId{};
            std::string title;
            bool isPublic{};
            std::vector<MemberSlot> members;
        };

        struct Invitation
        {
            InvitationId invitationId{};
            PartyId partyId{};
            PlayerId inviterPlayerId{};
            PlayerId targetPlayerId{};
        };

        struct LeaveResult
        {
            Result result = Result::NotInParty;
            PartyId partyId{};
            bool disbanded{};
        };

        [[nodiscard]] Result Invite(PlayerId inInviterPlayerId, PlayerId inTargetPlayerId,
            Invitation& outInvitation);
        [[nodiscard]] Result Create(PlayerId inPlayerId, std::string inTitle,
            bool inIsPublic, PartyId& outPartyId);
        [[nodiscard]] Result AnswerInvitation(PlayerId inTargetPlayerId,
            InvitationId inInvitationId, bool inAccepted, PartyId& outPartyId);
        [[nodiscard]] Result JoinApproved(PartyId inPartyId, PlayerId inLeaderPlayerId,
            PlayerId inRequesterPlayerId);
        [[nodiscard]] LeaveResult Leave(PlayerId inPlayerId);
        [[nodiscard]] Result Kick(PlayerId inLeaderPlayerId, PlayerId inTargetPlayerId,
            PartyId& outPartyId);
        [[nodiscard]] LeaveResult RemovePlayer(PlayerId inPlayerId);
        [[nodiscard]] Result SetSettings(PlayerId inLeaderPlayerId, std::string inTitle,
            bool inIsPublic);
        [[nodiscard]] bool SetLeader(PartyId inPartyId, PlayerId inPlayerId);

        [[nodiscard]] std::optional<PartyView> GetPartyForPlayer(PlayerId inPlayerId) const;
        [[nodiscard]] std::optional<PartyView> GetParty(PartyId inPartyId) const;
        [[nodiscard]] std::optional<Invitation> GetInvitation(PlayerId inTargetPlayerId,
            InvitationId inInvitationId) const;
        [[nodiscard]] bool IsLeader(PlayerId inPlayerId) const;
        [[nodiscard]] std::vector<PartyView> GetPublicParties() const;

    private:
        struct Party
        {
            PartyId partyId{};
            PlayerId leaderPlayerId{};
            std::string title;
            bool isPublic{};
            std::array<std::optional<PlayerId>, MAX_PARTY_MEMBERS> slots;
        };

        [[nodiscard]] Party& CreateParty(PlayerId inLeaderPlayerId);
        [[nodiscard]] static std::optional<std::uint8_t> FindEmptySlot(const Party& inParty);
        [[nodiscard]] PartyView MakeView(const Party& inParty) const;
        void CancelInvitations(PartyId inPartyId);

        std::unordered_map<PartyId, Party> parties;
        std::unordered_map<PlayerId, PartyId> playerParties;
        std::unordered_map<PlayerId, Invitation> invitationsByTarget;
        PartyId nextPartyId = 1;
        InvitationId nextInvitationId = 1;
    };
}
