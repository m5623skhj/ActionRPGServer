#include "DungeonProtocol.h"

#include <NetServerSerializeBuffer.h>

namespace GameRoomServer
{
    PacketId DungeonChallenge::GetPacketId() const
    {
        return DUNGEON_CHALLENGE_PACKET_ID;
    }

    void DungeonChallenge::BufferToPacket(NetBuffer& inBuffer)
    {
        inBuffer >> challenge;
    }

    void DungeonChallenge::PacketToBuffer(NetBuffer& outBuffer)
    {
        outBuffer << challenge;
    }

    PacketId DungeonAuthResult::GetPacketId() const
    {
        return DUNGEON_AUTH_RESULT_PACKET_ID;
    }

    void DungeonAuthResult::BufferToPacket(NetBuffer& inBuffer)
    {
        inBuffer >> succeeded;
    }

    void DungeonAuthResult::PacketToBuffer(NetBuffer& outBuffer)
    {
        outBuffer << succeeded;
    }

    void RegisterDungeonPackets()
    {
        PacketManager::GetInst().RegisterPacket<DungeonChallenge>();
        PacketManager::GetInst().RegisterPacket<DungeonAuthResult>();
    }
}
