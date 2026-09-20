#include "Protocol.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>
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
        inWriter.WriteFloat(inMap.spawnX);
        inWriter.WriteFloat(inMap.spawnY);
        inWriter.WriteFloat(inMap.sectorSize);
        inWriter.WriteFloat(inMap.walkSpeed);
        inWriter.WriteFloat(inMap.runSpeed);
    }

    bool ReadMapInfo(PacketReader& inReader, TownProtocol::MapInfo& outMap)
    {
        constexpr std::uint16_t MAX_IMAGES = 2048;
        constexpr std::uint16_t MAX_POLYGONS = 256;
        constexpr std::uint16_t MAX_VERTICES = 2048;
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

        return readPolygons(outMap.walkablePolygons)
            && readPolygons(outMap.blockedPolygons)
            && inReader.ReadFloat(outMap.spawnX)
            && inReader.ReadFloat(outMap.spawnY)
            && inReader.ReadFloat(outMap.sectorSize)
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
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const EnterTownResponse& inPacket)
    {
        PacketWriter writer(PacketType::EnterTownResponse);
        writer.WriteUInt64(inPacket.playerId);
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

    std::optional<EnterTownRequest> DecodeEnterTownRequest(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        EnterTownRequest packet;
        if (!ReadExpectedType(reader, PacketType::EnterTownRequest)
            || !reader.ReadString(packet.playerName)
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
}
