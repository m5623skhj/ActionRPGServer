#pragma once

#include "RudpPrerequisites.h"

#include <cstdint>

#include <PacketManager.h>

namespace GameRoomServer
{
    inline constexpr PacketId DUNGEON_CHALLENGE_PACKET_ID = 1001;
    inline constexpr PacketId DUNGEON_AUTH_RESULT_PACKET_ID = 1002;

    class DungeonChallenge final : public IPacket
    {
    public:
        [[nodiscard]] PacketId GetPacketId() const override;
        void BufferToPacket(NetBuffer& inBuffer) override;
        void PacketToBuffer(NetBuffer& outBuffer) override;

        std::uint64_t challenge{};
    };

    class DungeonAuthResult final : public IPacket
    {
    public:
        [[nodiscard]] PacketId GetPacketId() const override;
        void BufferToPacket(NetBuffer& inBuffer) override;
        void PacketToBuffer(NetBuffer& outBuffer) override;

        std::uint8_t succeeded{};
    };

    void RegisterDungeonPackets();
}
