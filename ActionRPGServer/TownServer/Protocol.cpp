#include "Protocol.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace
{
    class PacketWriter final
    {
    public:
        explicit PacketWriter(const TownProtocol::PacketType inType)
        {
            WriteUInt16(static_cast<std::uint16_t>(inType));
        }

        void WriteUInt8(const std::uint8_t inValue)
        {
            data.push_back(inValue);
        }

        void WriteInt8(const std::int8_t inValue)
        {
            WriteUInt8(static_cast<std::uint8_t>(inValue));
        }

        void WriteUInt16(const std::uint16_t inValue)
        {
            data.push_back(static_cast<std::uint8_t>((inValue >> 8) & 0xFF));
            data.push_back(static_cast<std::uint8_t>(inValue & 0xFF));
        }

        void WriteUInt32(const std::uint32_t inValue)
        {
            data.push_back(static_cast<std::uint8_t>((inValue >> 24) & 0xFF));
            data.push_back(static_cast<std::uint8_t>((inValue >> 16) & 0xFF));
            data.push_back(static_cast<std::uint8_t>((inValue >> 8) & 0xFF));
            data.push_back(static_cast<std::uint8_t>(inValue & 0xFF));
        }

        void WriteUInt64(const std::uint64_t inValue)
        {
            WriteUInt32(static_cast<std::uint32_t>(inValue >> 32));
            WriteUInt32(static_cast<std::uint32_t>(inValue & 0xFFFFFFFF));
        }

        void WriteFloat(const float inValue)
        {
            WriteUInt32(std::bit_cast<std::uint32_t>(inValue));
        }

        void WriteString(const std::string& inValue)
        {
            const std::size_t length = std::min<std::size_t>(inValue.size(), std::numeric_limits<std::uint16_t>::max());
            WriteUInt16(static_cast<std::uint16_t>(length));
            data.insert(data.end(), inValue.begin(), inValue.begin() + length);
        }

        [[nodiscard]] std::vector<std::uint8_t> Finish()
        {
            return std::move(data);
        }

    private:
        std::vector<std::uint8_t> data;
    };

    class PacketReader final
    {
    public:
        explicit PacketReader(const std::vector<std::uint8_t>& inData)
            : data(inData)
        {
        }

        [[nodiscard]] bool ReadUInt8(std::uint8_t& outValue)
        {
            if (!CanRead(1))
            {
                return false;
            }
            outValue = data[offset++];
            return true;
        }

        [[nodiscard]] bool ReadInt8(std::int8_t& outValue)
        {
            std::uint8_t value{};
            if (!ReadUInt8(value))
            {
                return false;
            }
            outValue = static_cast<std::int8_t>(value);
            return true;
        }

        [[nodiscard]] bool ReadUInt16(std::uint16_t& outValue)
        {
            if (!CanRead(2))
            {
                return false;
            }
            outValue = static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8)
                | static_cast<std::uint16_t>(data[offset + 1]));
            offset += 2;
            return true;
        }

        [[nodiscard]] bool ReadUInt32(std::uint32_t& outValue)
        {
            if (!CanRead(4))
            {
                return false;
            }
            outValue = (static_cast<std::uint32_t>(data[offset]) << 24)
                | (static_cast<std::uint32_t>(data[offset + 1]) << 16)
                | (static_cast<std::uint32_t>(data[offset + 2]) << 8)
                | static_cast<std::uint32_t>(data[offset + 3]);
            offset += 4;
            return true;
        }

        [[nodiscard]] bool ReadUInt64(std::uint64_t& outValue)
        {
            std::uint32_t high{};
            std::uint32_t low{};
            if (!ReadUInt32(high) || !ReadUInt32(low))
            {
                return false;
            }
            outValue = (static_cast<std::uint64_t>(high) << 32) | low;
            return true;
        }

        [[nodiscard]] bool ReadFloat(float& outValue)
        {
            std::uint32_t bits{};
            if (!ReadUInt32(bits))
            {
                return false;
            }
            outValue = std::bit_cast<float>(bits);
            return true;
        }

        [[nodiscard]] bool ReadString(std::string& outValue)
        {
            std::uint16_t length{};
            if (!ReadUInt16(length) || !CanRead(length))
            {
                return false;
            }
            outValue.assign(reinterpret_cast<const char*>(data.data() + offset), length);
            offset += length;
            return true;
        }

        [[nodiscard]] bool Finished() const noexcept
        {
            return offset == data.size();
        }

    private:
        [[nodiscard]] bool CanRead(const std::size_t inSize) const noexcept
        {
            return inSize <= data.size() - offset;
        }

        const std::vector<std::uint8_t>& data;
        std::size_t offset{};
    };

    void WriteMapInfo(PacketWriter& inWriter, const TownProtocol::MapInfo& inMap)
    {
        inWriter.WriteString(inMap.mapId);
        inWriter.WriteFloat(inMap.worldLeft);
        inWriter.WriteFloat(inMap.worldTop);
        inWriter.WriteFloat(inMap.worldRight);
        inWriter.WriteFloat(inMap.worldBottom);
        inWriter.WriteUInt16(static_cast<std::uint16_t>(inMap.images.size()));
        for (const TownProtocol::MapImage& image : inMap.images)
        {
            inWriter.WriteString(image.asset);
            inWriter.WriteFloat(image.x);
            inWriter.WriteFloat(image.y);
            inWriter.WriteFloat(image.width);
            inWriter.WriteFloat(image.height);
        }
        const auto writePolygons = [&inWriter](const std::vector<TownProtocol::Polygon>& inPolygons)
        {
            inWriter.WriteUInt16(static_cast<std::uint16_t>(inPolygons.size()));
            for (const TownProtocol::Polygon& polygon : inPolygons)
            {
                inWriter.WriteUInt16(static_cast<std::uint16_t>(polygon.size()));
                for (const TownProtocol::Vector2 point : polygon)
                {
                    inWriter.WriteFloat(point.x);
                    inWriter.WriteFloat(point.y);
                }
            }
        };
        writePolygons(inMap.walkablePolygons);
        writePolygons(inMap.blockedPolygons);
        inWriter.WriteUInt16(static_cast<std::uint16_t>(inMap.entryPoints.size()));
        for (const TownProtocol::EntryPoint& entryPoint : inMap.entryPoints)
        {
            inWriter.WriteString(entryPoint.id);
            inWriter.WriteFloat(entryPoint.position.x);
            inWriter.WriteFloat(entryPoint.position.y);
        }
        inWriter.WriteUInt16(static_cast<std::uint16_t>(inMap.transitionZones.size()));
        for (const TownProtocol::TransitionZone& zone : inMap.transitionZones)
        {
            inWriter.WriteString(zone.id);
            inWriter.WriteUInt8(static_cast<std::uint8_t>(zone.actionType));
            inWriter.WriteString(zone.targetMapId);
            inWriter.WriteString(zone.targetEntryPointId);
            inWriter.WriteString(zone.dungeonGroupId);
            writePolygons({ zone.polygon });
        }
        inWriter.WriteFloat(inMap.spawnX);
        inWriter.WriteFloat(inMap.spawnY);
        inWriter.WriteFloat(inMap.sectorWidth);
        inWriter.WriteFloat(inMap.sectorHeight);
        inWriter.WriteFloat(inMap.walkSpeed);
        inWriter.WriteFloat(inMap.runSpeed);
    }

    bool ReadMapInfo(PacketReader& inReader, TownProtocol::MapInfo& outMap)
    {
        constexpr std::uint16_t MAX_IMAGES = 2048;
        constexpr std::uint16_t MAX_POLYGONS = 256;
        constexpr std::uint16_t MAX_VERTICES = 2048;
        constexpr std::uint16_t MAX_ENTRY_POINTS = 256;
        constexpr std::uint16_t MAX_TRANSITION_ZONES = 256;
        constexpr std::size_t MAX_TOTAL_VERTICES = 32768;
        if (!inReader.ReadString(outMap.mapId)
            || !inReader.ReadFloat(outMap.worldLeft)
            || !inReader.ReadFloat(outMap.worldTop)
            || !inReader.ReadFloat(outMap.worldRight)
            || !inReader.ReadFloat(outMap.worldBottom))
        {
            return false;
        }

        std::uint16_t imageCount{};
        if (!inReader.ReadUInt16(imageCount) || imageCount > MAX_IMAGES)
        {
            return false;
        }
        outMap.images.resize(imageCount);
        for (TownProtocol::MapImage& image : outMap.images)
        {
            if (!inReader.ReadString(image.asset) || !inReader.ReadFloat(image.x)
                || !inReader.ReadFloat(image.y) || !inReader.ReadFloat(image.width)
                || !inReader.ReadFloat(image.height) || image.asset.size() > 240)
            {
                return false;
            }
        }

        std::size_t totalVertexCount{};
        const auto readPolygons = [&inReader, &totalVertexCount](std::vector<TownProtocol::Polygon>& outPolygons)
        {
            std::uint16_t polygonCount{};
            if (!inReader.ReadUInt16(polygonCount) || polygonCount > MAX_POLYGONS)
            {
                return false;
            }
            outPolygons.resize(polygonCount);
            for (TownProtocol::Polygon& polygon : outPolygons)
            {
                std::uint16_t vertexCount{};
                if (!inReader.ReadUInt16(vertexCount) || vertexCount < 3 || vertexCount > MAX_VERTICES)
                {
                    return false;
                }
                totalVertexCount += vertexCount;
                if (totalVertexCount > MAX_TOTAL_VERTICES)
                {
                    return false;
                }
                polygon.resize(vertexCount);
                for (TownProtocol::Vector2& point : polygon)
                {
                    if (!inReader.ReadFloat(point.x) || !inReader.ReadFloat(point.y))
                    {
                        return false;
                    }
                }
            }
            return true;
        };

        if (!readPolygons(outMap.walkablePolygons) || !readPolygons(outMap.blockedPolygons))
        {
            return false;
        }
        std::uint16_t entryPointCount{};
        if (!inReader.ReadUInt16(entryPointCount) || entryPointCount > MAX_ENTRY_POINTS)
        {
            return false;
        }
        outMap.entryPoints.resize(entryPointCount);
        for (TownProtocol::EntryPoint& entryPoint : outMap.entryPoints)
        {
            if (!inReader.ReadString(entryPoint.id) || entryPoint.id.empty() || entryPoint.id.size() > 64
                || !inReader.ReadFloat(entryPoint.position.x)
                || !inReader.ReadFloat(entryPoint.position.y))
            {
                return false;
            }
        }
        std::uint16_t transitionZoneCount{};
        if (!inReader.ReadUInt16(transitionZoneCount) || transitionZoneCount > MAX_TRANSITION_ZONES)
        {
            return false;
        }
        outMap.transitionZones.resize(transitionZoneCount);
        for (TownProtocol::TransitionZone& zone : outMap.transitionZones)
        {
            std::uint8_t actionType{};
            std::vector<TownProtocol::Polygon> polygons;
            if (!inReader.ReadString(zone.id) || zone.id.empty() || zone.id.size() > 64
                || !inReader.ReadUInt8(actionType)
                || actionType < static_cast<std::uint8_t>(TownProtocol::TransitionActionType::MapTransfer)
                || actionType > static_cast<std::uint8_t>(TownProtocol::TransitionActionType::DungeonSelection)
                || !inReader.ReadString(zone.targetMapId)
                || !inReader.ReadString(zone.targetEntryPointId)
                || !inReader.ReadString(zone.dungeonGroupId)
                || !readPolygons(polygons) || polygons.size() != 1)
            {
                return false;
            }
            zone.actionType = static_cast<TownProtocol::TransitionActionType>(actionType);
            zone.polygon = std::move(polygons.front());
        }
        return inReader.ReadFloat(outMap.spawnX)
            && inReader.ReadFloat(outMap.spawnY)
            && inReader.ReadFloat(outMap.sectorWidth)
            && inReader.ReadFloat(outMap.sectorHeight)
            && inReader.ReadFloat(outMap.walkSpeed)
            && inReader.ReadFloat(outMap.runSpeed);
    }

    bool ReadExpectedType(PacketReader& inReader, const TownProtocol::PacketType inExpected)
    {
        std::uint16_t type{};
        return inReader.ReadUInt16(type) && type == static_cast<std::uint16_t>(inExpected);
    }
}

namespace TownProtocol
{
    std::optional<PacketType> ReadPacketType(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        std::uint16_t type{};
        if (!reader.ReadUInt16(type))
        {
            return std::nullopt;
        }
        return static_cast<PacketType>(type);
    }

    std::vector<std::uint8_t> Encode(const EnterTownRequest& inPacket)
    {
        PacketWriter writer(PacketType::EnterTownRequest);
        writer.WriteString(inPacket.playerName);
        writer.WriteUInt32(inPacket.characterId);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const EnterTownResponse& inPacket)
    {
        PacketWriter writer(PacketType::EnterTownResponse);
        writer.WriteUInt64(inPacket.playerId);
        writer.WriteUInt32(inPacket.characterId);
        WriteMapInfo(writer, inPacket.map);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const MoveInput& inPacket)
    {
        PacketWriter writer(PacketType::MoveInput);
        writer.WriteUInt32(inPacket.sequence);
        writer.WriteInt8(inPacket.directionX);
        writer.WriteInt8(inPacket.directionY);
        writer.WriteUInt8(inPacket.running ? 1 : 0);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PlayerAppear& inPacket)
    {
        PacketWriter writer(PacketType::PlayerAppear);
        writer.WriteUInt64(inPacket.playerId);
        writer.WriteString(inPacket.playerName);
        writer.WriteUInt32(inPacket.characterId);
        writer.WriteFloat(inPacket.position.x);
        writer.WriteFloat(inPacket.position.y);
        writer.WriteFloat(inPacket.velocity.x);
        writer.WriteFloat(inPacket.velocity.y);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PlayerMove& inPacket)
    {
        PacketWriter writer(PacketType::PlayerMove);
        writer.WriteUInt64(inPacket.playerId);
        writer.WriteUInt32(inPacket.serverTick);
        writer.WriteUInt32(inPacket.lastProcessedInput);
        writer.WriteFloat(inPacket.position.x);
        writer.WriteFloat(inPacket.position.y);
        writer.WriteFloat(inPacket.velocity.x);
        writer.WriteFloat(inPacket.velocity.y);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PlayerDisappear& inPacket)
    {
        PacketWriter writer(PacketType::PlayerDisappear);
        writer.WriteUInt64(inPacket.playerId);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const ConfirmDungeonJoin& inPacket)
    {
        PacketWriter writer(PacketType::ConfirmDungeonJoin);
        writer.WriteUInt64(inPacket.roomId);
        writer.WriteUInt64(inPacket.challenge);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const EnterDungeonRequest& inPacket)
    {
        PacketWriter writer(PacketType::EnterDungeonRequest);
        writer.WriteString(inPacket.zoneId);
        writer.WriteUInt32(inPacket.dungeonId);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const EnterDungeonResponse& inPacket)
    {
        PacketWriter writer(PacketType::EnterDungeonResponse);
        writer.WriteUInt8(inPacket.succeeded ? 1 : 0);
        writer.WriteUInt64(inPacket.roomId);
        writer.WriteUInt64(inPacket.combatSeed);
        writer.WriteString(inPacket.sessionBrokerAddress);
        writer.WriteUInt16(inPacket.sessionBrokerPort);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const MapChanged& inPacket)
    {
        PacketWriter writer(PacketType::MapChanged);
        WriteMapInfo(writer, inPacket.map);
        writer.WriteFloat(inPacket.position.x);
        writer.WriteFloat(inPacket.position.y);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const DungeonSelectionOpen& inPacket)
    {
        PacketWriter writer(PacketType::DungeonSelectionOpen);
        writer.WriteString(inPacket.zoneId);
        writer.WriteString(inPacket.dungeonGroupId);
        writer.WriteUInt16(static_cast<std::uint16_t>(inPacket.dungeons.size()));
        for (const DungeonOption& dungeon : inPacket.dungeons)
        {
            writer.WriteUInt32(dungeon.dungeonId);
            writer.WriteString(dungeon.name);
            writer.WriteString(dungeon.levelRange);
            writer.WriteString(dungeon.description);
        }
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyInviteRequest& inPacket)
    {
        PacketWriter writer(PacketType::PartyInviteRequest);
        writer.WriteUInt64(inPacket.targetPlayerId);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyInviteAnswer& inPacket)
    {
        PacketWriter writer(PacketType::PartyInviteAnswer);
        writer.WriteUInt64(inPacket.invitationId);
        writer.WriteUInt8(inPacket.accepted ? 1 : 0);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyLeaveRequest&)
    {
        return PacketWriter(PacketType::PartyLeaveRequest).Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyKickRequest& inPacket)
    {
        PacketWriter writer(PacketType::PartyKickRequest);
        writer.WriteUInt64(inPacket.targetPlayerId);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyInvitation& inPacket)
    {
        PacketWriter writer(PacketType::PartyInvitation);
        writer.WriteUInt64(inPacket.invitationId);
        writer.WriteUInt64(inPacket.inviterPlayerId);
        writer.WriteString(inPacket.inviterName);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartySnapshot& inPacket)
    {
        PacketWriter writer(PacketType::PartySnapshot);
        writer.WriteUInt64(inPacket.partyId);
        writer.WriteUInt64(inPacket.leaderPlayerId);
        writer.WriteUInt8(static_cast<std::uint8_t>(inPacket.members.size()));
        for (const PartyMemberInfo& member : inPacket.members)
        {
            writer.WriteUInt64(member.playerId);
            writer.WriteString(member.playerName);
            writer.WriteUInt8(member.slot);
        }
        writer.WriteString(inPacket.title);
        writer.WriteUInt8(inPacket.isPublic ? 1 : 0);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyOperationResult& inPacket)
    {
        PacketWriter writer(PacketType::PartyOperationResult);
        writer.WriteUInt8(static_cast<std::uint8_t>(inPacket.operation));
        writer.WriteUInt8(static_cast<std::uint8_t>(inPacket.result));
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartySettingsRequest& inPacket)
    {
        PacketWriter writer(PacketType::PartySettingsRequest);
        writer.WriteString(inPacket.title);
        writer.WriteUInt8(inPacket.isPublic ? 1 : 0);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyDirectoryPageRequest& inPacket)
    {
        PacketWriter writer(PacketType::PartyDirectoryPageRequest);
        writer.WriteUInt32(inPacket.page);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyDirectoryUnsubscribe&)
    {
        return PacketWriter(PacketType::PartyDirectoryUnsubscribe).Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyDirectoryPage& inPacket)
    {
        PacketWriter writer(PacketType::PartyDirectoryPage);
        writer.WriteUInt32(inPacket.page);
        writer.WriteUInt32(inPacket.totalPages);
        writer.WriteUInt32(inPacket.totalCount);
        writer.WriteUInt64(inPacket.revision);
        writer.WriteUInt8(static_cast<std::uint8_t>(inPacket.parties.size()));
        for (const PartyDirectoryEntry& party : inPacket.parties)
        {
            writer.WriteUInt64(party.partyId);
            writer.WriteString(party.title);
            writer.WriteString(party.leaderName);
            writer.WriteUInt8(static_cast<std::uint8_t>(party.members.size()));
            for (const PartyMemberInfo& member : party.members)
            {
                writer.WriteUInt64(member.playerId);
                writer.WriteString(member.playerName);
                writer.WriteUInt8(member.slot);
            }
        }
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyDirectoryChanged& inPacket)
    {
        PacketWriter writer(PacketType::PartyDirectoryChanged);
        writer.WriteUInt64(inPacket.revision);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyCreateRequest& inPacket)
    {
        PacketWriter writer(PacketType::PartyCreateRequest);
        writer.WriteString(inPacket.title);
        writer.WriteUInt8(inPacket.isPublic ? 1 : 0);
        return writer.Finish();
    }

    std::optional<EnterTownRequest> DecodeEnterTownRequest(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        EnterTownRequest packet;
        if (!ReadExpectedType(reader, PacketType::EnterTownRequest)
            || !reader.ReadString(packet.playerName)
            || !reader.ReadUInt32(packet.characterId)
            || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<EnterTownResponse> DecodeEnterTownResponse(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        EnterTownResponse packet;
        if (!ReadExpectedType(reader, PacketType::EnterTownResponse)
            || !reader.ReadUInt64(packet.playerId)
            || !reader.ReadUInt32(packet.characterId)
            || !ReadMapInfo(reader, packet.map)
            || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<MoveInput> DecodeMoveInput(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        MoveInput packet;
        std::uint8_t running{};
        if (!ReadExpectedType(reader, PacketType::MoveInput)
            || !reader.ReadUInt32(packet.sequence)
            || !reader.ReadInt8(packet.directionX)
            || !reader.ReadInt8(packet.directionY)
            || !reader.ReadUInt8(running)
            || running > 1
            || !reader.Finished())
        {
            return std::nullopt;
        }
        packet.running = running != 0;
        return packet;
    }

    std::optional<PlayerAppear> DecodePlayerAppear(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PlayerAppear packet;
        if (!ReadExpectedType(reader, PacketType::PlayerAppear)
            || !reader.ReadUInt64(packet.playerId)
            || !reader.ReadString(packet.playerName)
            || !reader.ReadUInt32(packet.characterId)
            || !reader.ReadFloat(packet.position.x)
            || !reader.ReadFloat(packet.position.y)
            || !reader.ReadFloat(packet.velocity.x)
            || !reader.ReadFloat(packet.velocity.y)
            || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<PlayerMove> DecodePlayerMove(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PlayerMove packet;
        if (!ReadExpectedType(reader, PacketType::PlayerMove)
            || !reader.ReadUInt64(packet.playerId)
            || !reader.ReadUInt32(packet.serverTick)
            || !reader.ReadUInt32(packet.lastProcessedInput)
            || !reader.ReadFloat(packet.position.x)
            || !reader.ReadFloat(packet.position.y)
            || !reader.ReadFloat(packet.velocity.x)
            || !reader.ReadFloat(packet.velocity.y)
            || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<PlayerDisappear> DecodePlayerDisappear(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PlayerDisappear packet;
        if (!ReadExpectedType(reader, PacketType::PlayerDisappear)
            || !reader.ReadUInt64(packet.playerId)
            || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<ConfirmDungeonJoin> DecodeConfirmDungeonJoin(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        ConfirmDungeonJoin packet;
        if (!ReadExpectedType(reader, PacketType::ConfirmDungeonJoin)
            || !reader.ReadUInt64(packet.roomId)
            || !reader.ReadUInt64(packet.challenge)
            || packet.roomId == 0
            || packet.challenge == 0
            || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<EnterDungeonRequest> DecodeEnterDungeonRequest(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        EnterDungeonRequest packet;
        if (!ReadExpectedType(reader, PacketType::EnterDungeonRequest)
            || !reader.ReadString(packet.zoneId)
            || !reader.ReadUInt32(packet.dungeonId)
            || packet.zoneId.empty() || packet.zoneId.size() > 64 || packet.dungeonId == 0
            || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<EnterDungeonResponse> DecodeEnterDungeonResponse(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        EnterDungeonResponse packet;
        std::uint8_t succeeded{};
        if (!ReadExpectedType(reader, PacketType::EnterDungeonResponse)
            || !reader.ReadUInt8(succeeded)
            || succeeded > 1
            || !reader.ReadUInt64(packet.roomId)
            || !reader.ReadUInt64(packet.combatSeed)
            || !reader.ReadString(packet.sessionBrokerAddress)
            || !reader.ReadUInt16(packet.sessionBrokerPort)
            || packet.sessionBrokerAddress.size() > 255
            || !reader.Finished())
        {
            return std::nullopt;
        }
        packet.succeeded = succeeded != 0;
        if (packet.succeeded && (packet.roomId == 0 || packet.combatSeed == 0
            || packet.sessionBrokerAddress.empty() || packet.sessionBrokerPort == 0))
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<MapChanged> DecodeMapChanged(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        MapChanged packet;
        if (!ReadExpectedType(reader, PacketType::MapChanged)
            || !ReadMapInfo(reader, packet.map)
            || !reader.ReadFloat(packet.position.x)
            || !reader.ReadFloat(packet.position.y)
            || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<DungeonSelectionOpen> DecodeDungeonSelectionOpen(
        const std::vector<std::uint8_t>& inPacket)
    {
        constexpr std::uint16_t MAX_DUNGEONS = 64;
        PacketReader reader(inPacket);
        DungeonSelectionOpen packet;
        std::uint16_t dungeonCount{};
        if (!ReadExpectedType(reader, PacketType::DungeonSelectionOpen)
            || !reader.ReadString(packet.zoneId) || packet.zoneId.empty() || packet.zoneId.size() > 64
            || !reader.ReadString(packet.dungeonGroupId) || packet.dungeonGroupId.empty()
            || packet.dungeonGroupId.size() > 64
            || !reader.ReadUInt16(dungeonCount) || dungeonCount == 0 || dungeonCount > MAX_DUNGEONS)
        {
            return std::nullopt;
        }
        packet.dungeons.resize(dungeonCount);
        for (DungeonOption& dungeon : packet.dungeons)
        {
            if (!reader.ReadUInt32(dungeon.dungeonId) || dungeon.dungeonId == 0
                || !reader.ReadString(dungeon.name) || dungeon.name.empty() || dungeon.name.size() > 96
                || !reader.ReadString(dungeon.levelRange) || dungeon.levelRange.size() > 48
                || !reader.ReadString(dungeon.description) || dungeon.description.size() > 256)
            {
                return std::nullopt;
            }
        }
        return reader.Finished() ? std::optional<DungeonSelectionOpen>(std::move(packet)) : std::nullopt;
    }

    std::optional<PartyInviteRequest> DecodePartyInviteRequest(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyInviteRequest packet;
        if (!ReadExpectedType(reader, PacketType::PartyInviteRequest)
            || !reader.ReadUInt64(packet.targetPlayerId) || packet.targetPlayerId == 0
            || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<PartyInviteAnswer> DecodePartyInviteAnswer(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyInviteAnswer packet;
        std::uint8_t accepted{};
        if (!ReadExpectedType(reader, PacketType::PartyInviteAnswer)
            || !reader.ReadUInt64(packet.invitationId) || packet.invitationId == 0
            || !reader.ReadUInt8(accepted) || accepted > 1 || !reader.Finished())
        {
            return std::nullopt;
        }
        packet.accepted = accepted != 0;
        return packet;
    }

    std::optional<PartyLeaveRequest> DecodePartyLeaveRequest(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        if (!ReadExpectedType(reader, PacketType::PartyLeaveRequest) || !reader.Finished())
        {
            return std::nullopt;
        }
        return PartyLeaveRequest{};
    }

    std::optional<PartyKickRequest> DecodePartyKickRequest(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyKickRequest packet;
        if (!ReadExpectedType(reader, PacketType::PartyKickRequest)
            || !reader.ReadUInt64(packet.targetPlayerId) || packet.targetPlayerId == 0
            || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<PartyInvitation> DecodePartyInvitation(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyInvitation packet;
        if (!ReadExpectedType(reader, PacketType::PartyInvitation)
            || !reader.ReadUInt64(packet.invitationId) || packet.invitationId == 0
            || !reader.ReadUInt64(packet.inviterPlayerId) || packet.inviterPlayerId == 0
            || !reader.ReadString(packet.inviterName) || packet.inviterName.empty()
            || packet.inviterName.size() > 32 || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<PartySnapshot> DecodePartySnapshot(const std::vector<std::uint8_t>& inPacket)
    {
        constexpr std::uint8_t MAX_PARTY_MEMBERS = 8;
        PacketReader reader(inPacket);
        PartySnapshot packet;
        std::uint8_t memberCount{};
        std::uint8_t isPublic{};
        if (!ReadExpectedType(reader, PacketType::PartySnapshot)
            || !reader.ReadUInt64(packet.partyId)
            || !reader.ReadUInt64(packet.leaderPlayerId)
            || !reader.ReadUInt8(memberCount) || memberCount > MAX_PARTY_MEMBERS)
        {
            return std::nullopt;
        }
        if ((packet.partyId == 0) != (packet.leaderPlayerId == 0)
            || (packet.partyId == 0 && memberCount != 0)
            || (packet.partyId != 0 && memberCount == 0))
        {
            return std::nullopt;
        }

        std::unordered_set<std::uint64_t> playerIds;
        std::unordered_set<std::uint8_t> slots;
        bool leaderFound = packet.partyId == 0;
        packet.members.resize(memberCount);
        for (PartyMemberInfo& member : packet.members)
        {
            if (!reader.ReadUInt64(member.playerId) || member.playerId == 0
                || !reader.ReadString(member.playerName) || member.playerName.empty()
                || member.playerName.size() > 32
                || !reader.ReadUInt8(member.slot) || member.slot >= MAX_PARTY_MEMBERS
                || !playerIds.emplace(member.playerId).second
                || !slots.emplace(member.slot).second)
            {
                return std::nullopt;
            }
            leaderFound = leaderFound || member.playerId == packet.leaderPlayerId;
        }
        if (!reader.ReadString(packet.title) || packet.title.size() > 96
            || !reader.ReadUInt8(isPublic) || isPublic > 1
            || (packet.partyId == 0 && (!packet.title.empty() || isPublic != 0))
            || (packet.partyId != 0 && packet.title.empty()))
        {
            return std::nullopt;
        }
        packet.isPublic = isPublic != 0;
        return leaderFound && reader.Finished()
            ? std::optional<PartySnapshot>(std::move(packet)) : std::nullopt;
    }

    std::optional<PartyOperationResult> DecodePartyOperationResult(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyOperationResult packet;
        std::uint8_t operation{};
        std::uint8_t result{};
        if (!ReadExpectedType(reader, PacketType::PartyOperationResult)
            || !reader.ReadUInt8(operation)
            || operation < static_cast<std::uint8_t>(PartyOperationType::Invite)
            || operation > static_cast<std::uint8_t>(PartyOperationType::AnswerJoin)
            || !reader.ReadUInt8(result)
            || result > static_cast<std::uint8_t>(PartyResultCode::NotPublic)
            || !reader.Finished())
        {
            return std::nullopt;
        }
        packet.operation = static_cast<PartyOperationType>(operation);
        packet.result = static_cast<PartyResultCode>(result);
        return packet;
    }

    std::optional<PartySettingsRequest> DecodePartySettingsRequest(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartySettingsRequest packet;
        std::uint8_t isPublic{};
        if (!ReadExpectedType(reader, PacketType::PartySettingsRequest)
            || !reader.ReadString(packet.title) || packet.title.empty() || packet.title.size() > 96
            || !reader.ReadUInt8(isPublic) || isPublic > 1 || !reader.Finished())
        {
            return std::nullopt;
        }
        packet.isPublic = isPublic != 0;
        return packet;
    }

    std::optional<PartyDirectoryPageRequest> DecodePartyDirectoryPageRequest(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyDirectoryPageRequest packet;
        if (!ReadExpectedType(reader, PacketType::PartyDirectoryPageRequest)
            || !reader.ReadUInt32(packet.page) || packet.page == 0 || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<PartyDirectoryUnsubscribe> DecodePartyDirectoryUnsubscribe(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        if (!ReadExpectedType(reader, PacketType::PartyDirectoryUnsubscribe) || !reader.Finished())
        {
            return std::nullopt;
        }
        return PartyDirectoryUnsubscribe{};
    }

    std::optional<PartyDirectoryPage> DecodePartyDirectoryPage(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyDirectoryPage packet;
        std::uint8_t partyCount{};
        if (!ReadExpectedType(reader, PacketType::PartyDirectoryPage)
            || !reader.ReadUInt32(packet.page) || packet.page == 0
            || !reader.ReadUInt32(packet.totalPages) || packet.totalPages == 0
            || packet.page > packet.totalPages
            || !reader.ReadUInt32(packet.totalCount)
            || !reader.ReadUInt64(packet.revision)
            || !reader.ReadUInt8(partyCount) || partyCount > 8)
        {
            return std::nullopt;
        }
        packet.parties.resize(partyCount);
        for (PartyDirectoryEntry& party : packet.parties)
        {
            std::uint8_t memberCount{};
            if (!reader.ReadUInt64(party.partyId) || party.partyId == 0
                || !reader.ReadString(party.title) || party.title.empty() || party.title.size() > 96
                || !reader.ReadString(party.leaderName) || party.leaderName.empty()
                || party.leaderName.size() > 32
                || !reader.ReadUInt8(memberCount) || memberCount == 0 || memberCount > 8)
            {
                return std::nullopt;
            }
            party.members.resize(memberCount);
            std::unordered_set<std::uint8_t> slots;
            for (PartyMemberInfo& member : party.members)
            {
                if (!reader.ReadUInt64(member.playerId) || member.playerId == 0
                    || !reader.ReadString(member.playerName) || member.playerName.empty()
                    || member.playerName.size() > 32
                    || !reader.ReadUInt8(member.slot) || member.slot >= 8
                    || !slots.emplace(member.slot).second)
                {
                    return std::nullopt;
                }
            }
        }
        return reader.Finished() ? std::optional<PartyDirectoryPage>(std::move(packet)) : std::nullopt;
    }

    std::optional<PartyDirectoryChanged> DecodePartyDirectoryChanged(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyDirectoryChanged packet;
        if (!ReadExpectedType(reader, PacketType::PartyDirectoryChanged)
            || !reader.ReadUInt64(packet.revision) || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<PartyCreateRequest> DecodePartyCreateRequest(
        const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyCreateRequest packet;
        std::uint8_t isPublic{};
        if (!ReadExpectedType(reader, PacketType::PartyCreateRequest)
            || !reader.ReadString(packet.title) || packet.title.size() > 96
            || !reader.ReadUInt8(isPublic) || isPublic > 1
            || !reader.Finished())
        {
            return std::nullopt;
        }
        packet.isPublic = isPublic != 0;
        return packet;
    }
    std::vector<std::uint8_t> Encode(const DungeonCompletionRequest& inPacket)
    {
        PacketWriter writer(PacketType::DungeonCompletionRequest);
        writer.WriteUInt64(inPacket.roomId);
        writer.WriteUInt8(inPacket.retry ? 1 : 0);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const DungeonCompletionResponse& inPacket)
    {
        PacketWriter writer(PacketType::DungeonCompletionResponse);
        writer.WriteUInt64(inPacket.previousRoomId);
        writer.WriteUInt8(inPacket.succeeded ? 1 : 0);
        writer.WriteUInt8(inPacket.retry ? 1 : 0);
        writer.WriteUInt64(inPacket.roomId);
        writer.WriteUInt64(inPacket.combatSeed);
        writer.WriteString(inPacket.sessionBrokerAddress);
        writer.WriteUInt16(inPacket.sessionBrokerPort);
        return writer.Finish();
    }

    std::optional<DungeonCompletionRequest> DecodeDungeonCompletionRequest(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        DungeonCompletionRequest packet;
        std::uint8_t retry{};
        if (!ReadExpectedType(reader, PacketType::DungeonCompletionRequest)
            || !reader.ReadUInt64(packet.roomId) || packet.roomId == 0
            || !reader.ReadUInt8(retry) || retry > 1 || !reader.Finished()) return std::nullopt;
        packet.retry = retry != 0;
        return packet;
    }

    std::optional<DungeonCompletionResponse> DecodeDungeonCompletionResponse(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        DungeonCompletionResponse packet;
        std::uint8_t succeeded{}, retry{};
        if (!ReadExpectedType(reader, PacketType::DungeonCompletionResponse)
            || !reader.ReadUInt64(packet.previousRoomId) || packet.previousRoomId == 0
            || !reader.ReadUInt8(succeeded) || succeeded > 1
            || !reader.ReadUInt8(retry) || retry > 1
            || !reader.ReadUInt64(packet.roomId) || !reader.ReadUInt64(packet.combatSeed)
            || !reader.ReadString(packet.sessionBrokerAddress) || packet.sessionBrokerAddress.size() > 255
            || !reader.ReadUInt16(packet.sessionBrokerPort) || !reader.Finished()) return std::nullopt;
        packet.succeeded = succeeded != 0;
        packet.retry = retry != 0;
        if (packet.succeeded && packet.retry && (packet.roomId == 0 || packet.combatSeed == 0
            || packet.sessionBrokerAddress.empty() || packet.sessionBrokerPort == 0)) return std::nullopt;
        if (packet.succeeded && !packet.retry && packet.roomId != 0) return std::nullopt;
        return packet;
    }

    std::vector<std::uint8_t> Encode(const SkillStateRequest&)
    {
        return PacketWriter(PacketType::SkillStateRequest).Finish();
    }

    std::vector<std::uint8_t> Encode(const LearnSkillRequest& inPacket)
    {
        PacketWriter writer(PacketType::LearnSkillRequest);
        writer.WriteString(inPacket.skillId);
        writer.WriteUInt32(inPacket.expectedSkillLevel);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const SkillStateResponse& inPacket)
    {
        if (inPacket.payload.size() > std::numeric_limits<std::uint16_t>::max())
            throw std::length_error("Skill state exceeds TCP string size.");
        PacketWriter writer(PacketType::SkillStateResponse);
        writer.WriteString(inPacket.payload);
        return writer.Finish();
    }

    std::optional<SkillStateRequest> DecodeSkillStateRequest(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        if (!ReadExpectedType(reader, PacketType::SkillStateRequest) || !reader.Finished()) return std::nullopt;
        return SkillStateRequest{};
    }

    std::optional<LearnSkillRequest> DecodeLearnSkillRequest(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        LearnSkillRequest packet;
        if (!ReadExpectedType(reader, PacketType::LearnSkillRequest) || !reader.ReadString(packet.skillId)
            || packet.skillId.empty() || packet.skillId.size() > 64 || !reader.ReadUInt32(packet.expectedSkillLevel)
            || packet.expectedSkillLevel > 1000000 || !reader.Finished()) return std::nullopt;
        return packet;
    }

    std::optional<SkillStateResponse> DecodeSkillStateResponse(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        SkillStateResponse packet;
        if (!ReadExpectedType(reader, PacketType::SkillStateResponse) || !reader.ReadString(packet.payload)
            || packet.payload.empty() || !reader.Finished()) return std::nullopt;
        return packet;
    }

    std::vector<std::uint8_t> Encode(const PartyDetailRequest& inPacket)
    {
        PacketWriter writer(PacketType::PartyDetailRequest);
        writer.WriteUInt64(inPacket.partyId);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyDetailResponse& inPacket)
    {
        PacketWriter writer(PacketType::PartyDetailResponse);
        writer.WriteUInt64(inPacket.partyId);
        writer.WriteUInt8(static_cast<std::uint8_t>(inPacket.result));
        writer.WriteString(inPacket.title);
        writer.WriteUInt64(inPacket.leaderPlayerId);
        writer.WriteUInt8(inPacket.isPublic ? 1 : 0);
        writer.WriteUInt8(inPacket.busy ? 1 : 0);
        writer.WriteUInt8(static_cast<std::uint8_t>(inPacket.members.size()));
        for (const auto& member : inPacket.members)
        {
            writer.WriteUInt64(member.playerId);
            writer.WriteString(member.playerName);
            writer.WriteUInt8(member.slot);
        }
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyJoinRequest& inPacket)
    {
        PacketWriter writer(PacketType::PartyJoinRequest);
        writer.WriteUInt64(inPacket.partyId);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyJoinAnswer& inPacket)
    {
        PacketWriter writer(PacketType::PartyJoinAnswer);
        writer.WriteUInt64(inPacket.requestId);
        writer.WriteUInt8(inPacket.accepted ? 1 : 0);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyJoinRequestUpdate& inPacket)
    {
        PacketWriter writer(PacketType::PartyJoinRequestUpdate);
        writer.WriteUInt64(inPacket.requestId);
        writer.WriteUInt64(inPacket.partyId);
        writer.WriteUInt64(inPacket.requesterPlayerId);
        writer.WriteString(inPacket.requesterName);
        writer.WriteUInt8(static_cast<std::uint8_t>(inPacket.state));
        writer.WriteUInt8(static_cast<std::uint8_t>(inPacket.result));
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const PartyKicked& inPacket)
    {
        PacketWriter writer(PacketType::PartyKicked);
        writer.WriteUInt64(inPacket.partyId);
        writer.WriteUInt64(inPacket.leaderPlayerId);
        return writer.Finish();
    }

    std::optional<PartyDetailRequest> DecodePartyDetailRequest(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyDetailRequest packet;
        if (!ReadExpectedType(reader, PacketType::PartyDetailRequest)
            || !reader.ReadUInt64(packet.partyId) || packet.partyId == 0 || !reader.Finished()) return std::nullopt;
        return packet;
    }

    std::optional<PartyDetailResponse> DecodePartyDetailResponse(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyDetailResponse packet;
        std::uint8_t result{}, isPublic{}, busy{}, count{};
        if (!ReadExpectedType(reader, PacketType::PartyDetailResponse)
            || !reader.ReadUInt64(packet.partyId) || packet.partyId == 0
            || !reader.ReadUInt8(result) || result > static_cast<std::uint8_t>(PartyResultCode::NotPublic)
            || !reader.ReadString(packet.title) || packet.title.size() > 96
            || !reader.ReadUInt64(packet.leaderPlayerId)
            || !reader.ReadUInt8(isPublic) || isPublic > 1
            || !reader.ReadUInt8(busy) || busy > 1
            || !reader.ReadUInt8(count) || count > 8) return std::nullopt;
        packet.result = static_cast<PartyResultCode>(result);
        packet.isPublic = isPublic != 0;
        packet.busy = busy != 0;
        std::unordered_set<std::uint64_t> ids;
        std::unordered_set<std::uint8_t> slots;
        bool leaderFound = false;
        packet.members.resize(count);
        for (auto& member : packet.members)
        {
            if (!reader.ReadUInt64(member.playerId) || member.playerId == 0
                || !reader.ReadString(member.playerName) || member.playerName.empty() || member.playerName.size() > 32
                || !reader.ReadUInt8(member.slot) || member.slot >= 8
                || !ids.emplace(member.playerId).second || !slots.emplace(member.slot).second) return std::nullopt;
            leaderFound = leaderFound || member.playerId == packet.leaderPlayerId;
        }
        if (packet.result == PartyResultCode::Succeeded)
        {
            if (!leaderFound || packet.title.empty() || !packet.isPublic) return std::nullopt;
        }
        else if (!packet.title.empty() || packet.leaderPlayerId != 0 || packet.isPublic || packet.busy || count != 0)
            return std::nullopt;
        return reader.Finished() ? std::optional<PartyDetailResponse>(std::move(packet)) : std::nullopt;
    }

    std::optional<PartyJoinRequest> DecodePartyJoinRequest(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyJoinRequest packet;
        if (!ReadExpectedType(reader, PacketType::PartyJoinRequest)
            || !reader.ReadUInt64(packet.partyId) || packet.partyId == 0 || !reader.Finished()) return std::nullopt;
        return packet;
    }

    std::optional<PartyJoinAnswer> DecodePartyJoinAnswer(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyJoinAnswer packet;
        std::uint8_t accepted{};
        if (!ReadExpectedType(reader, PacketType::PartyJoinAnswer)
            || !reader.ReadUInt64(packet.requestId) || packet.requestId == 0
            || !reader.ReadUInt8(accepted) || accepted > 1 || !reader.Finished()) return std::nullopt;
        packet.accepted = accepted != 0;
        return packet;
    }

    std::optional<PartyJoinRequestUpdate> DecodePartyJoinRequestUpdate(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyJoinRequestUpdate packet;
        std::uint8_t state{}, result{};
        if (!ReadExpectedType(reader, PacketType::PartyJoinRequestUpdate)
            || !reader.ReadUInt64(packet.requestId) || packet.requestId == 0
            || !reader.ReadUInt64(packet.partyId) || packet.partyId == 0
            || !reader.ReadUInt64(packet.requesterPlayerId) || packet.requesterPlayerId == 0
            || !reader.ReadString(packet.requesterName) || packet.requesterName.empty() || packet.requesterName.size() > 32
            || !reader.ReadUInt8(state) || state < 1 || state > 4
            || !reader.ReadUInt8(result) || result > static_cast<std::uint8_t>(PartyResultCode::NotPublic)
            || (state == 4 && result == 0) || (state != 4 && result != 0)
            || !reader.Finished()) return std::nullopt;
        packet.state = static_cast<PartyJoinRequestState>(state);
        packet.result = static_cast<PartyResultCode>(result);
        return packet;
    }

    std::optional<PartyKicked> DecodePartyKicked(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        PartyKicked packet;
        if (!ReadExpectedType(reader, PacketType::PartyKicked)
            || !reader.ReadUInt64(packet.partyId) || packet.partyId == 0
            || !reader.ReadUInt64(packet.leaderPlayerId) || packet.leaderPlayerId == 0
            || !reader.Finished()) return std::nullopt;
        return packet;
    }
}
