#include "DungeonSession.h"

#include "DungeonProtocol.h"
#include "RoomManager.h"
#include "GameRoom.h"

#include <chrono>
#include <random>
#include <utility>

namespace GameRoomServer
{
    DungeonSession::DungeonSession(
        MultiSocketRUDPCore& inCore,
        std::weak_ptr<RoomManager> inRoomManager)
        : RUDPSession(inCore),
          roomManager(std::move(inRoomManager))
    {
        using namespace ActionRPG::DungeonProtocol;
        RegisterPacketHandler<DungeonSession, DungeonWorldRequest>(
            static_cast<::PacketId>(PacketType::DUNGEON_WORLD_REQUEST), &DungeonSession::OnWorldRequest);
        RegisterPacketHandler<DungeonSession, DungeonMoveInput>(
            static_cast<::PacketId>(PacketType::DUNGEON_MOVE_INPUT), &DungeonSession::OnMoveInput);
        RegisterPacketHandler<DungeonSession, DungeonActionInput>(
            static_cast<::PacketId>(PacketType::DUNGEON_ACTION_INPUT), &DungeonSession::OnActionInput);
        RegisterPacketHandler<DungeonSession, DungeonSkillInput>(
            static_cast<::PacketId>(PacketType::DUNGEON_SKILL_INPUT), &DungeonSession::OnSkillInput);
        RegisterPacketHandler<DungeonSession, DungeonCombatStateRequest>(
            static_cast<::PacketId>(PacketType::DUNGEON_COMBAT_STATE_REQUEST), &DungeonSession::OnCombatStateRequest);
        RegisterPacketHandler<DungeonSession, DungeonRealtimeRequest>(
            static_cast<::PacketId>(PacketType::DUNGEON_REALTIME_REQUEST), &DungeonSession::OnRealtimeRequest);
    }

    bool DungeonSession::ConfirmAuthentication(
        const ActionRPG::RoomControlProtocol::RoomId inRoomId,
        const ActionRPG::RoomControlProtocol::PlayerId inPlayerId,
        const std::uint64_t inChallenge,
        const std::uint32_t inExpectedGeneration, std::shared_ptr<GameRoom> inGameRoom)
    {
        const auto world = inGameRoom->GetWorldFor(inPlayerId);
        if (!world || world->empty() || world->size() > 4 * 1024 * 1024) return false;
        bool sent = false;
        {
            std::lock_guard lock(bindingMutex);
            if (connectedGeneration != inExpectedGeneration || GetSessionGeneration() != inExpectedGeneration
                || challenge != inChallenge
                || roomId != 0 || playerId != 0 || !IsConnected())
            {
                return false;
            }
            roomId = inRoomId;
            playerId = inPlayerId;
            gameRoom = inGameRoom;
            initialWorld = world;
            nextWorldOffset = 0;
            ResetCombatStream();
            ActionRPG::DungeonProtocol::DungeonAuthResult result;
            result.succeeded = 1;
            sent = SendPacket(result);
        }

        if (!sent)
        {
            DoDisconnect(DISCONNECT_REASON::BY_ERROR);
            return false;
        }
        return true;
    }

    void DungeonSession::OnConnected()
    {
        const std::uint64_t newChallenge = GenerateChallenge();
        const std::uint32_t generation = GetSessionGeneration();
        {
            std::lock_guard lock(bindingMutex);
            challenge = newChallenge;
            roomId = 0;
            playerId = 0;
            connectedGeneration = generation;
            gameRoom.reset();
            initialWorld.reset();
            nextWorldOffset = 0;
            ResetCombatStream();
        }

        const std::shared_ptr<RoomManager> manager = roomManager.lock();
        if (!manager)
        {
            DoDisconnect(DISCONNECT_REASON::BY_ERROR);
            return;
        }
        manager->RegisterChallenge(newChallenge, this, generation);

        ActionRPG::DungeonProtocol::DungeonChallenge packet;
        packet.challenge = newChallenge;
        if (!SendPacket(packet))
        {
            DoDisconnect(DISCONNECT_REASON::BY_ERROR);
        }
    }

    void DungeonSession::OnDisconnected()
    {
        std::uint64_t disconnectedChallenge{};
        ActionRPG::RoomControlProtocol::RoomId disconnectedRoomId{};
        ActionRPG::RoomControlProtocol::PlayerId disconnectedPlayerId{};
        std::uint32_t disconnectedGeneration{};
        {
            std::lock_guard lock(bindingMutex);
            disconnectedChallenge = challenge;
            disconnectedRoomId = roomId;
            disconnectedPlayerId = playerId;
            disconnectedGeneration = connectedGeneration;
            realtimeLease.reset();
        }
        if (const std::shared_ptr<RoomManager> manager = roomManager.lock())
        {
            manager->SessionDisconnected(disconnectedChallenge, this, disconnectedGeneration,
                disconnectedRoomId, disconnectedPlayerId);
        }
    }

    void DungeonSession::OnReleased()
    {
        std::lock_guard lock(bindingMutex);
        challenge = 0;
        roomId = 0;
        playerId = 0;
        connectedGeneration = 0;
        gameRoom.reset();
        initialWorld.reset();
        nextWorldOffset = 0;
        ResetCombatStream();
    }

    // One bounded chunk per request keeps the RUDP pending queue and MTU bounded.
    void DungeonSession::OnWorldRequest(const ActionRPG::DungeonProtocol::DungeonWorldRequest& inPacket)
    {
        bool sent = false;
        {
            std::lock_guard lock(bindingMutex);
            if (!IsConnected() || connectedGeneration != GetSessionGeneration() || roomId == 0
                || !initialWorld || inPacket.offset != nextWorldOffset || nextWorldOffset >= initialWorld->size()) return;
            ActionRPG::DungeonProtocol::DungeonWorldChunk packet;
            packet.totalBytes = static_cast<std::uint32_t>(initialWorld->size());
            packet.offset = nextWorldOffset;
            packet.payload = initialWorld->substr(nextWorldOffset, 768);
            sent = SendPacket(packet);
            if (sent) nextWorldOffset += static_cast<std::uint32_t>(packet.payload.size());
        }
        if (!sent) DoDisconnect(DISCONNECT_REASON::BY_ERROR);
    }

    void DungeonSession::OnMoveInput(const ActionRPG::DungeonProtocol::DungeonMoveInput& inPacket)
    {
        if (inPacket.directionX < -1 || inPacket.directionX > 1 || inPacket.directionY < -1
            || inPacket.directionY > 1 || inPacket.running > 1 || inPacket.sequence == 0) return;
        std::shared_ptr<GameRoom> room;
        std::uint32_t generation{};
        ActionRPG::RoomControlProtocol::RoomId boundRoom{};
        ActionRPG::RoomControlProtocol::PlayerId boundPlayer{};
        {
            std::lock_guard lock(bindingMutex);
            if (!IsConnected() || connectedGeneration != GetSessionGeneration() || !initialWorld
                || nextWorldOffset != initialWorld->size()) return;
            room = gameRoom.lock();
            generation = connectedGeneration;
            boundRoom = roomId;
            boundPlayer = playerId;
        }
        if (room) room->UpdateInput(boundPlayer, inPacket,
            [this, generation, boundRoom, boundPlayer](auto packet)
                { SendPlayerState(generation, boundRoom, boundPlayer, std::move(packet)); });
    }

    void DungeonSession::SendPlayerState(const std::uint32_t inGeneration,
        const ActionRPG::RoomControlProtocol::RoomId inRoomId,
        const ActionRPG::RoomControlProtocol::PlayerId inPlayerId,
        ActionRPG::DungeonProtocol::DungeonPlayerState inPacket)
    {
        bool sent = true;
        {
            std::lock_guard lock(bindingMutex);
            if (connectedGeneration != inGeneration || GetSessionGeneration() != inGeneration
                || roomId != inRoomId || playerId != inPlayerId || !IsConnected()) return;
            sent = SendPacket(inPacket);
        }
        if (!sent) DoDisconnect(DISCONNECT_REASON::BY_ERROR);
    }

    // bindingMutex protects every stream field, including callbacks posted by the room strand.
    void DungeonSession::ResetCombatStream()
    {
        combatSnapshot.reset();
        snapshotId = nextCombatOffset = combatBytesThisSecond = actionsThisSecond = 0;
        snapshotPending = false;
        lastSnapshotRequest = combatWindow = actionWindow = {};
        realtimeLease.reset();
        realtimeBytesThisSecond = 0;
        realtimeWindow = lastRealtimeRequest = {};
    }

    // Opt-in preserves old clients. The weak lease expires at unsubscribe/disconnect/session reuse.
    void DungeonSession::OnRealtimeRequest(const ActionRPG::DungeonProtocol::DungeonRealtimeRequest& inPacket)
    {
        std::shared_ptr<GameRoom> room;
        std::weak_ptr<const std::uint8_t> lease;
        std::uint32_t generation{};
        ActionRPG::RoomControlProtocol::RoomId boundRoom{};
        ActionRPG::RoomControlProtocol::PlayerId boundPlayer{};
        bool sent = true;
        {
            std::lock_guard lock(bindingMutex);
            if (!IsConnected() || connectedGeneration != GetSessionGeneration() || !initialWorld
                || roomId == 0 || nextWorldOffset != initialWorld->size() || inPacket.challenge != challenge) return;
            const auto now = std::chrono::steady_clock::now();
            if (now - lastRealtimeRequest < std::chrono::seconds(1)) return;
            lastRealtimeRequest = now;
            ActionRPG::DungeonProtocol::DungeonRealtimeResult result;
            result.version = 1; result.challenge = challenge; result.roomId = roomId;
            result.tickIntervalMs = static_cast<std::uint16_t>((1000 + GameRoom::TICK_RATE / 2) / GameRoom::TICK_RATE);
            result.snapshotIntervalMs = static_cast<std::uint16_t>((1000 + GameRoom::SNAPSHOT_RATE / 2) / GameRoom::SNAPSHOT_RATE);
            const auto boundGameRoom = gameRoom.lock();
            if (boundGameRoom) result.dungeonId = boundGameRoom->GetDungeonId();
            if (inPacket.version == 1 && inPacket.enabled <= 1 && boundGameRoom)
            {
                result.accepted = 1;
                if (inPacket.enabled == 0) realtimeLease.reset();
                else if (!realtimeLease)
                {
                    realtimeLease = std::make_shared<const std::uint8_t>(0);
                    lease = realtimeLease;
                    room = boundGameRoom;
                    generation = connectedGeneration; boundRoom = roomId; boundPlayer = playerId;
                }
            }
            sent = SendPacket(result);
        }
        if (!sent) { DoDisconnect(DISCONNECT_REASON::BY_ERROR); return; }
        if (room) room->SubscribeRealtime(boundPlayer, lease,
            [this, generation, boundRoom, boundPlayer, lease](std::uint32_t mapEpoch, auto frame)
            { SendRealtimeFrame(generation, boundRoom, boundPlayer, lease, mapEpoch, std::move(frame)); });
    }

    // Drop a whole oversized/budget-limited frame; never queue reliable retries for motion.
    void DungeonSession::SendRealtimeFrame(std::uint32_t inGeneration,
        ActionRPG::RoomControlProtocol::RoomId inRoomId, ActionRPG::RoomControlProtocol::PlayerId inPlayerId,
        std::weak_ptr<const std::uint8_t> inLease, std::uint32_t inMapEpoch,
        std::shared_ptr<const GameRoom::RealtimeFrame> inFrame)
    {
        constexpr std::size_t CHUNK_BYTES = 768;
        constexpr std::uint32_t MAX_BYTES_PER_SECOND = 256 * 1024;
        std::lock_guard lock(bindingMutex);
        if (connectedGeneration != inGeneration || GetSessionGeneration() != inGeneration
            || roomId != inRoomId || playerId != inPlayerId || !IsConnected()
            || !realtimeLease || inLease.lock() != realtimeLease || !inFrame) return;
        if (inFrame->payload.empty() || inFrame->payload.size() > 48 * 1024 || inFrame->mapId.size() > 64) return;
        const auto boundGameRoom = gameRoom.lock();
        if (!boundGameRoom) return;
        const auto now = std::chrono::steady_clock::now();
        if (now - realtimeWindow >= std::chrono::seconds(1)) { realtimeWindow = now; realtimeBytesThisSecond = 0; }
        const auto chunkCount = (inFrame->payload.size() + CHUNK_BYTES - 1) / CHUNK_BYTES;
        // Includes a conservative bound for NetBuffer, RUDP, authentication and IP/UDP headers.
        const auto frameBytes = inFrame->payload.size() + chunkCount * (160 + inFrame->mapId.size());
        if (frameBytes > MAX_BYTES_PER_SECOND - realtimeBytesThisSecond) return;
        realtimeBytesThisSecond += static_cast<std::uint32_t>(frameBytes);
        for (std::size_t offset = 0; offset < inFrame->payload.size(); offset += CHUNK_BYTES)
        {
            ActionRPG::DungeonProtocol::DungeonRealtimeChunk packet;
            packet.version = 1; packet.challenge = challenge; packet.roomId = roomId;
            packet.dungeonId = boundGameRoom->GetDungeonId();
            packet.mapEpoch = inMapEpoch; packet.snapshotSequence = inFrame->sequence;
            packet.serverTick = inFrame->tick; packet.serverTimeMs = inFrame->timeMs;
            packet.mapId = inFrame->mapId; packet.state = inFrame->state;
            packet.totalBytes = static_cast<std::uint32_t>(inFrame->payload.size());
            packet.offset = static_cast<std::uint32_t>(offset);
            packet.payload = inFrame->payload.substr(offset, CHUNK_BYTES);
            if (!SendUnreliablePacket(packet)) break;
        }
    }

    void DungeonSession::OnActionInput(const ActionRPG::DungeonProtocol::DungeonActionInput& inPacket)
    {
        if (inPacket.sequence == 0 || inPacket.action < 1 || inPacket.action > 2 || inPacket.facingLeft > 1) return;
        std::shared_ptr<GameRoom> room;
        std::uint32_t generation{};
        ActionRPG::RoomControlProtocol::RoomId boundRoom{};
        ActionRPG::RoomControlProtocol::PlayerId boundPlayer{};
        {
            std::lock_guard lock(bindingMutex);
            if (!IsConnected() || connectedGeneration != GetSessionGeneration() || !initialWorld
                || roomId == 0 || nextWorldOffset != initialWorld->size()) return;
            const auto now = std::chrono::steady_clock::now();
            if (now - actionWindow >= std::chrono::seconds(1)) { actionWindow = now; actionsThisSecond = 0; }
            if (actionsThisSecond >= 20) return;
            ++actionsThisSecond;
            room = gameRoom.lock();
            generation = connectedGeneration;
            boundRoom = roomId;
            boundPlayer = playerId;
        }
        if (room) room->SubmitAction(boundPlayer, inPacket,
            [this, generation, boundRoom, boundPlayer](auto packet)
                { SendActionResult(generation, boundRoom, boundPlayer, std::move(packet)); });
    }

    void DungeonSession::OnSkillInput(const ActionRPG::DungeonProtocol::DungeonSkillInput& inPacket)
    {
        if (inPacket.sequence == 0 || inPacket.facingLeft > 1
            || !ActionRPG::PlayerSkills::Catalog::IsId(inPacket.skillId)) return;
        std::shared_ptr<GameRoom> room;
        std::uint32_t generation{};
        ActionRPG::RoomControlProtocol::RoomId boundRoom{};
        ActionRPG::RoomControlProtocol::PlayerId boundPlayer{};
        {
            std::lock_guard lock(bindingMutex);
            if (!IsConnected() || connectedGeneration != GetSessionGeneration() || !initialWorld
                || roomId == 0 || nextWorldOffset != initialWorld->size()) return;
            const auto now = std::chrono::steady_clock::now();
            if (now - actionWindow >= std::chrono::seconds(1)) { actionWindow = now; actionsThisSecond = 0; }
            if (actionsThisSecond >= 20) return;
            ++actionsThisSecond; room = gameRoom.lock(); generation = connectedGeneration;
            boundRoom = roomId; boundPlayer = playerId;
        }
        if (room) room->SubmitSkill(boundPlayer, inPacket,
            [this, generation, boundRoom, boundPlayer](auto packet)
                { SendActionResult(generation, boundRoom, boundPlayer, std::move(packet)); });
    }

    void DungeonSession::SendActionResult(std::uint32_t inGeneration,
        ActionRPG::RoomControlProtocol::RoomId inRoomId, ActionRPG::RoomControlProtocol::PlayerId inPlayerId,
        ActionRPG::DungeonProtocol::DungeonActionResult inPacket)
    {
        bool sent = true;
        {
            std::lock_guard lock(bindingMutex);
            if (connectedGeneration != inGeneration || GetSessionGeneration() != inGeneration
                || roomId != inRoomId || playerId != inPlayerId || !IsConnected()) return;
            sent = SendPacket(inPacket);
        }
        if (!sent) DoDisconnect(DISCONNECT_REASON::BY_ERROR);
    }

    // A snapshot is immutable during transfer. Each request queues at most one 768-byte chunk.
    void DungeonSession::OnCombatStateRequest(const ActionRPG::DungeonProtocol::DungeonCombatStateRequest& inPacket)
    {
        std::shared_ptr<GameRoom> room;
        std::uint32_t generation{};
        ActionRPG::RoomControlProtocol::RoomId boundRoom{};
        ActionRPG::RoomControlProtocol::PlayerId boundPlayer{};
        bool sent = true;
        {
            std::lock_guard lock(bindingMutex);
            if (!IsConnected() || connectedGeneration != GetSessionGeneration() || !initialWorld
                || roomId == 0 || nextWorldOffset != initialWorld->size()) return;
            if (inPacket.snapshotId == 0 && inPacket.offset == 0)
            {
                const auto now = std::chrono::steady_clock::now();
                if (snapshotPending || now - lastSnapshotRequest < std::chrono::milliseconds(200))
                {
                    ActionRPG::DungeonProtocol::DungeonCombatStateChunk result;
                    result.status = 1;
                    result.retryAfterMs = 200;
                    sent = SendPacket(result);
                }
                else
                {
                    room = gameRoom.lock();
                    if (room)
                    {
                        snapshotPending = true;
                        lastSnapshotRequest = now;
                        generation = connectedGeneration;
                        boundRoom = roomId;
                        boundPlayer = playerId;
                    }
                    else
                    {
                        ActionRPG::DungeonProtocol::DungeonCombatStateChunk result;
                        result.status = 2;
                        sent = SendPacket(result);
                    }
                }
            }
            else if (inPacket.snapshotId != snapshotId || snapshotPending)
            {
                ActionRPG::DungeonProtocol::DungeonCombatStateChunk result;
                result.snapshotId = inPacket.snapshotId;
                result.offset = inPacket.offset;
                result.status = 3;
                sent = SendPacket(result);
            }
            else sent = SendNextCombatChunk(inPacket.offset);
        }
        if (!sent) DoDisconnect(DISCONNECT_REASON::BY_ERROR);
        if (room) room->GetCombatSnapshot(boundPlayer,
            [this, generation, boundRoom, boundPlayer](auto snapshot)
                { SendCombatChunk(generation, boundRoom, boundPlayer, std::move(snapshot)); });
    }

    bool DungeonSession::SendNextCombatChunk(std::uint32_t inOffset)
    {
        ActionRPG::DungeonProtocol::DungeonCombatStateChunk packet;
        packet.snapshotId = snapshotId;
        packet.offset = inOffset;
        if (!combatSnapshot) packet.status = 2;
        else
        {
            packet.totalBytes = static_cast<std::uint32_t>(combatSnapshot->size());
            if (inOffset != nextCombatOffset || inOffset >= combatSnapshot->size()) packet.status = 3;
            else
            {
                const auto now = std::chrono::steady_clock::now();
                if (now - combatWindow >= std::chrono::seconds(1)) { combatWindow = now; combatBytesThisSecond = 0; }
                const auto payload = combatSnapshot->substr(inOffset, 768);
                if (combatBytesThisSecond + payload.size() + 32 > 64 * 1024)
                {
                    packet.status = 1;
                    packet.retryAfterMs = 1000;
                }
                else packet.payload = payload;
            }
        }
        const bool sent = SendPacket(packet);
        if (sent && packet.status == 0)
        {
            nextCombatOffset += static_cast<std::uint32_t>(packet.payload.size());
            combatBytesThisSecond += static_cast<std::uint32_t>(packet.payload.size() + 32);
        }
        return sent;
    }

    void DungeonSession::SendCombatChunk(std::uint32_t inGeneration,
        ActionRPG::RoomControlProtocol::RoomId inRoomId, ActionRPG::RoomControlProtocol::PlayerId inPlayerId,
        std::shared_ptr<const std::string> inSnapshot)
    {
        bool sent = true;
        {
            std::lock_guard lock(bindingMutex);
            if (connectedGeneration != inGeneration || GetSessionGeneration() != inGeneration
                || roomId != inRoomId || playerId != inPlayerId || !IsConnected() || !snapshotPending) return;
            snapshotPending = false;
            combatSnapshot = std::move(inSnapshot);
            if (++snapshotId == 0) ++snapshotId;
            nextCombatOffset = 0;
            sent = SendNextCombatChunk(0);
        }
        if (!sent) DoDisconnect(DISCONNECT_REASON::BY_ERROR);
    }

    std::uint64_t DungeonSession::GenerateChallenge() const
    {
        std::random_device randomDevice;
        const std::uint64_t high = static_cast<std::uint64_t>(randomDevice()) << 32;
        const std::uint64_t low = randomDevice();
        const std::uint64_t time = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::uint64_t result = high ^ low ^ time ^ GetSessionId();
        return result == 0 ? 1 : result;
    }
}
