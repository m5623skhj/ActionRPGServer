#include "TownInstance.h"
#include "PlayerSession.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace TownServer::Domain
{
    namespace
    {
        // Decode Unicode scalars without locale dependence; preserve Korean and joined emoji.
        bool IsValidChatMessage(std::string_view inMessage)
        {
            if (inMessage.empty() || inMessage.size() > TownProtocol::MAX_CHAT_UTF8_BYTES) return false;
            std::size_t offset{}, codePoints{};
            bool hasContent{};
            while (offset < inMessage.size())
            {
                const auto first = static_cast<std::uint8_t>(inMessage[offset++]);
                std::uint32_t codePoint{}, minimum{};
                int continuationCount{};
                if (first < 0x80) codePoint = first;
                else if (first >= 0xC2 && first <= 0xDF)
                { codePoint = first & 0x1F; minimum = 0x80; continuationCount = 1; }
                else if (first >= 0xE0 && first <= 0xEF)
                { codePoint = first & 0x0F; minimum = 0x800; continuationCount = 2; }
                else if (first >= 0xF0 && first <= 0xF4)
                { codePoint = first & 0x07; minimum = 0x10000; continuationCount = 3; }
                else return false;
                for (int index = 0; index < continuationCount; ++index)
                {
                    if (offset == inMessage.size()) return false;
                    const auto next = static_cast<std::uint8_t>(inMessage[offset++]);
                    if ((next & 0xC0) != 0x80) return false;
                    codePoint = (codePoint << 6) | (next & 0x3F);
                }
                if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)
                    || ++codePoints > TownProtocol::MAX_CHAT_CODE_POINTS) return false;
                // C0/C1, line separators, BOM and bidi formatting must not alter the chat's UI structure.
                if (codePoint <= 0x1F || (codePoint >= 0x7F && codePoint <= 0x9F)
                    || codePoint == 0x2028 || codePoint == 0x2029 || codePoint == 0xFEFF
                    || codePoint == 0x061C || codePoint == 0x200E || codePoint == 0x200F
                    || (codePoint >= 0x202A && codePoint <= 0x202E)
                    || (codePoint >= 0x2066 && codePoint <= 0x206F)) return false;
                const bool blank = codePoint == 0x20 || codePoint == 0xA0 || codePoint == 0x1680 || codePoint == 0x180E
                    || (codePoint >= 0x2000 && codePoint <= 0x200D) || codePoint == 0x202F || codePoint == 0x205F
                    || codePoint == 0x2060 || codePoint == 0x3000 || (codePoint >= 0xFE00 && codePoint <= 0xFE0F)
                    || (codePoint >= 0xE0100 && codePoint <= 0xE01EF);
                hasContent = hasContent || !blank;
            }
            return hasContent;
        }
    }

    /** Linearize validation, channel resolution and recipients on the town strand.
     * Only the last request/result is retained; duplicates never replay a broadcast or create a party.
     */
    void TownInstance::SendChat(std::shared_ptr<Network::PlayerSession> inSession, TownProtocol::ChatRequest inRequest)
    {
        const auto self = shared_from_this();
        asio::post(strand, [self, session = std::move(inSession), request = std::move(inRequest)]() mutable
        {
            using Channel = TownProtocol::ChatChannel;
            using Result = TownProtocol::ChatResultCode;
            TownProtocol::ChatResult reply{request.sequence, Result::NotEntered, Channel::General, 0, 0};
            const auto playerId = session->GetPlayerId();
            const auto mapped = self->sessionToPlayer.find(session->GetSessionId());
            const auto found = self->players.find(playerId);
            if (!self->running || mapped == self->sessionToPlayer.end() || mapped->second != playerId
                || found == self->players.end() || found->second.session != session
                || !session->enterRequested.load(std::memory_order_acquire) || session->GetAccountId() == 0
                || session->GetAccountId() != found->second.accountId)
            { session->Send(TownProtocol::Encode(reply)); return; }
            auto& entry = found->second;
            const auto isInTown = [self](PlayerId id, const PlayerEntry& player)
            {
                return player.dungeonRoomId == 0 && player.reservedDungeonRoomId == 0
                    && !self->pendingDungeonPlayers.contains(id);
            };
            reply.channel = request.channel == Channel::Party || !isInTown(playerId, entry) ? Channel::Party : Channel::General;
            if (request.sequence <= entry.chatSequence)
            {
                if (request.sequence == entry.chatSequence)
                {
                    if (request.channel == entry.chatRequestedChannel && request.message == entry.chatLastMessage)
                    { session->Send(TownProtocol::Encode(entry.chatLastResult)); return; }
                    reply.result = Result::SequenceConflict;
                }
                else reply.result = Result::StaleSequence;
                session->Send(TownProtocol::Encode(reply)); return;
            }

            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(now - entry.chatRefillTime).count();
            entry.chatTokens = std::min(CHAT_BURST_CAPACITY, entry.chatTokens + elapsed * CHAT_REFILL_PER_SECOND);
            entry.chatRefillTime = now;
            std::unordered_set<PlayerId> recipients;
            if (entry.chatTokens < 1.0)
            {
                reply.result = Result::RateLimited;
                reply.retryAfterMs = static_cast<std::uint32_t>(std::ceil((1.0 - entry.chatTokens) * 1000.0 / CHAT_REFILL_PER_SECOND));
            }
            else
            {
                entry.chatTokens -= 1.0;
                if (request.channel != Channel::General && request.channel != Channel::Party) reply.result = Result::InvalidChannel;
                else if (!IsValidChatMessage(request.message)) reply.result = Result::InvalidMessage;
                else if (reply.channel == Channel::Party)
                {
                    const auto party = self->partyManager.GetPartyForPlayer(playerId);
                    if (!party) reply.result = Result::NotInParty;
                    else
                    {
                        for (const auto& member : party->members) recipients.insert(member.playerId);
                        reply.result = Result::Succeeded;
                    }
                }
                else
                { recipients = self->FindVisiblePlayers(playerId); reply.result = Result::Succeeded; }
            }
            if (reply.result == Result::Succeeded && self->nextChatMessageId == std::numeric_limits<std::uint64_t>::max())
                reply.result = Result::Unavailable;

            std::vector<std::uint8_t> broadcast;
            if (reply.result == Result::Succeeded)
            {
                reply.messageId = self->nextChatMessageId;
                broadcast = TownProtocol::Encode(TownProtocol::ChatMessage{reply.messageId, request.sequence, reply.channel,
                    playerId, entry.persistentCharacterId, entry.player.GetName(), request.message});
                recipients.insert(playerId); // Own text arrives through exactly the same server event.
                ++self->nextChatMessageId;
            }
            entry.chatSequence = request.sequence;
            entry.chatRequestedChannel = request.channel;
            entry.chatLastMessage = std::move(request.message);
            entry.chatLastResult = reply;
            // Send may synchronously close an overloaded session; retain no player reference across it.
            const auto senderMapId = entry.mapId;
            session->Send(TownProtocol::Encode(reply));
            if (reply.result != Result::Succeeded) return;
            for (const auto id : recipients)
            {
                const auto receiver = self->players.find(id);
                if (receiver == self->players.end() || receiver->second.session->GetPlayerId() != id
                    || receiver->second.session->GetAccountId() != receiver->second.accountId) continue;
                if (reply.channel == Channel::General && (receiver->second.mapId != senderMapId
                    || !isInTown(id, receiver->second))) continue;
                receiver->second.session->Send(broadcast);
            }
        });
    }
}
