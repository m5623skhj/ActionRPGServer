#include "RoomControlProtocol.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
    constexpr std::size_t MAX_PARTICIPANT_COUNT = 64;
    constexpr std::size_t MAX_ADDRESS_LENGTH = 255;

    class PacketWriter final
    {
    public:
        explicit PacketWriter(const ActionRPG::RoomControlProtocol::PacketType inType)
        {
            WriteUInt16(static_cast<std::uint16_t>(inType));
        }

        void WriteUInt8(const std::uint8_t inValue)
        {
            data.push_back(inValue);
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
            WriteUInt32(static_cast<std::uint32_t>(inValue));
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
            return offset <= data.size() && inSize <= data.size() - offset;
        }

        const std::vector<std::uint8_t>& data;
        std::size_t offset{};
    };

    bool ReadExpectedType(PacketReader& inReader, const ActionRPG::RoomControlProtocol::PacketType inExpected)
    {
        std::uint16_t type{};
        return inReader.ReadUInt16(type) && type == static_cast<std::uint16_t>(inExpected);
    }

    void WritePlayerIds(PacketWriter& inWriter,
        const std::vector<ActionRPG::RoomControlProtocol::PlayerId>& inPlayerIds)
    {
        inWriter.WriteUInt16(static_cast<std::uint16_t>(inPlayerIds.size()));
        for (const ActionRPG::RoomControlProtocol::PlayerId playerId : inPlayerIds)
        {
            inWriter.WriteUInt64(playerId);
        }
    }

    bool ReadPlayerIds(PacketReader& inReader,
        std::vector<ActionRPG::RoomControlProtocol::PlayerId>& outPlayerIds,
        const bool inAllowEmpty = false)
    {
        std::uint16_t count{};
        if (!inReader.ReadUInt16(count) || (!inAllowEmpty && count == 0) || count > MAX_PARTICIPANT_COUNT)
        {
            return false;
        }
        outPlayerIds.resize(count);
        for (ActionRPG::RoomControlProtocol::PlayerId& playerId : outPlayerIds)
        {
            if (!inReader.ReadUInt64(playerId) || playerId == 0)
            {
                return false;
            }
        }
        return true;
    }
}

namespace ActionRPG::RoomControlProtocol
{
    std::optional<PacketType> ReadPacketType(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        std::uint16_t type{};
        if (!reader.ReadUInt16(type) || type < static_cast<std::uint16_t>(PacketType::RegisterRoomServer)
            || type > static_cast<std::uint16_t>(PacketType::FinishRoomResult))
        {
            return std::nullopt;
        }
        return static_cast<PacketType>(type);
    }

    std::vector<std::uint8_t> Encode(const RegisterRoomServer& inPacket)
    {
        PacketWriter writer(PacketType::RegisterRoomServer);
        writer.WriteUInt64(inPacket.roomServerId);
        writer.WriteUInt32(inPacket.maxRoomCount);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const CreateRoom& inPacket)
    {
        PacketWriter writer(PacketType::CreateRoom);
        writer.WriteUInt64(inPacket.requestId);
        writer.WriteUInt32(inPacket.dungeonId);
        WritePlayerIds(writer, inPacket.participantPlayerIds);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const CreateRoomResult& inPacket)
    {
        PacketWriter writer(PacketType::CreateRoomResult);
        writer.WriteUInt64(inPacket.requestId);
        writer.WriteUInt8(inPacket.succeeded ? 1 : 0);
        writer.WriteUInt64(inPacket.roomId);
        writer.WriteUInt64(inPacket.combatSeed);
        writer.WriteString(inPacket.sessionBrokerAddress);
        writer.WriteUInt16(inPacket.sessionBrokerPort);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const ConfirmJoin& inPacket)
    {
        PacketWriter writer(PacketType::ConfirmJoin);
        writer.WriteUInt64(inPacket.roomId);
        writer.WriteUInt64(inPacket.playerId);
        writer.WriteUInt64(inPacket.challenge);
        writer.WriteUInt32(inPacket.characterId);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const EnterRoom& inPacket)
    {
        PacketWriter writer(PacketType::EnterRoom);
        writer.WriteUInt64(inPacket.roomId);
        writer.WriteUInt64(inPacket.playerId);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const LeaveRoom& inPacket)
    {
        PacketWriter writer(PacketType::LeaveRoom);
        writer.WriteUInt64(inPacket.roomId);
        writer.WriteUInt64(inPacket.playerId);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const RoomEnded& inPacket)
    {
        PacketWriter writer(PacketType::RoomEnded);
        writer.WriteUInt64(inPacket.roomId);
        writer.WriteUInt8(static_cast<std::uint8_t>(inPacket.reason));
        WritePlayerIds(writer, inPacket.rewardPlayerIds);
        return writer.Finish();
    }

    std::optional<RegisterRoomServer> DecodeRegisterRoomServer(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        RegisterRoomServer packet;
        if (!ReadExpectedType(reader, PacketType::RegisterRoomServer)
            || !reader.ReadUInt64(packet.roomServerId)
            || !reader.ReadUInt32(packet.maxRoomCount)
            || packet.roomServerId == 0 || packet.maxRoomCount == 0 || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<CreateRoom> DecodeCreateRoom(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        CreateRoom packet;
        if (!ReadExpectedType(reader, PacketType::CreateRoom)
            || !reader.ReadUInt64(packet.requestId)
            || !reader.ReadUInt32(packet.dungeonId)
            || !ReadPlayerIds(reader, packet.participantPlayerIds)
            || packet.requestId == 0 || packet.dungeonId == 0 || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<CreateRoomResult> DecodeCreateRoomResult(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        CreateRoomResult packet;
        std::uint8_t succeeded{};
        if (!ReadExpectedType(reader, PacketType::CreateRoomResult)
            || !reader.ReadUInt64(packet.requestId)
            || !reader.ReadUInt8(succeeded)
            || succeeded > 1
            || !reader.ReadUInt64(packet.roomId)
            || !reader.ReadUInt64(packet.combatSeed)
            || !reader.ReadString(packet.sessionBrokerAddress)
            || !reader.ReadUInt16(packet.sessionBrokerPort)
            || packet.requestId == 0 || packet.sessionBrokerAddress.size() > MAX_ADDRESS_LENGTH
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

    std::optional<ConfirmJoin> DecodeConfirmJoin(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        ConfirmJoin packet;
        if (!ReadExpectedType(reader, PacketType::ConfirmJoin)
            || !reader.ReadUInt64(packet.roomId)
            || !reader.ReadUInt64(packet.playerId)
            || !reader.ReadUInt64(packet.challenge)
            || !reader.ReadUInt32(packet.characterId) || packet.characterId == 0
            || packet.roomId == 0 || packet.playerId == 0 || packet.challenge == 0 || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<EnterRoom> DecodeEnterRoom(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        EnterRoom packet;
        if (!ReadExpectedType(reader, PacketType::EnterRoom)
            || !reader.ReadUInt64(packet.roomId)
            || !reader.ReadUInt64(packet.playerId)
            || packet.roomId == 0 || packet.playerId == 0 || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<LeaveRoom> DecodeLeaveRoom(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        LeaveRoom packet;
        if (!ReadExpectedType(reader, PacketType::LeaveRoom)
            || !reader.ReadUInt64(packet.roomId)
            || !reader.ReadUInt64(packet.playerId)
            || packet.roomId == 0 || packet.playerId == 0 || !reader.Finished())
        {
            return std::nullopt;
        }
        return packet;
    }

    std::optional<RoomEnded> DecodeRoomEnded(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        RoomEnded packet;
        std::uint8_t reason{};
        if (!ReadExpectedType(reader, PacketType::RoomEnded)
            || !reader.ReadUInt64(packet.roomId)
            || !reader.ReadUInt8(reason)
            || !ReadPlayerIds(reader, packet.rewardPlayerIds, true)
            || packet.roomId == 0
            || reason < static_cast<std::uint8_t>(RoomEndReason::Cleared)
            || reason > static_cast<std::uint8_t>(RoomEndReason::Aborted)
            || !reader.Finished())
        {
            return std::nullopt;
        }
        packet.reason = static_cast<RoomEndReason>(reason);
        return packet;
    }
    std::vector<std::uint8_t> Encode(const FinishRoom& inPacket)
    {
        PacketWriter writer(PacketType::FinishRoom);
        writer.WriteUInt64(inPacket.requestId);
        writer.WriteUInt64(inPacket.roomId);
        writer.WriteUInt8(inPacket.retry ? 1 : 0);
        WritePlayerIds(writer, inPacket.participantPlayerIds);
        return writer.Finish();
    }

    std::vector<std::uint8_t> Encode(const FinishRoomResult& inPacket)
    {
        PacketWriter writer(PacketType::FinishRoomResult);
        writer.WriteUInt64(inPacket.requestId);
        writer.WriteUInt64(inPacket.previousRoomId);
        writer.WriteUInt8(inPacket.succeeded ? 1 : 0);
        writer.WriteUInt64(inPacket.roomId);
        writer.WriteUInt64(inPacket.combatSeed);
        writer.WriteString(inPacket.sessionBrokerAddress);
        writer.WriteUInt16(inPacket.sessionBrokerPort);
        return writer.Finish();
    }

    std::optional<FinishRoom> DecodeFinishRoom(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        FinishRoom packet;
        std::uint8_t retry{};
        if (!ReadExpectedType(reader, PacketType::FinishRoom)
            || !reader.ReadUInt64(packet.requestId) || packet.requestId == 0
            || !reader.ReadUInt64(packet.roomId) || packet.roomId == 0
            || !reader.ReadUInt8(retry) || retry > 1
            || !ReadPlayerIds(reader, packet.participantPlayerIds) || !reader.Finished()) return std::nullopt;
        packet.retry = retry != 0;
        return packet;
    }

    std::optional<FinishRoomResult> DecodeFinishRoomResult(const std::vector<std::uint8_t>& inPacket)
    {
        PacketReader reader(inPacket);
        FinishRoomResult packet;
        std::uint8_t succeeded{};
        if (!ReadExpectedType(reader, PacketType::FinishRoomResult)
            || !reader.ReadUInt64(packet.requestId) || packet.requestId == 0
            || !reader.ReadUInt64(packet.previousRoomId) || packet.previousRoomId == 0
            || !reader.ReadUInt8(succeeded) || succeeded > 1
            || !reader.ReadUInt64(packet.roomId) || !reader.ReadUInt64(packet.combatSeed)
            || !reader.ReadString(packet.sessionBrokerAddress) || packet.sessionBrokerAddress.size() > MAX_ADDRESS_LENGTH
            || !reader.ReadUInt16(packet.sessionBrokerPort) || !reader.Finished()) return std::nullopt;
        packet.succeeded = succeeded != 0;
        if (packet.succeeded && packet.roomId != 0 && (packet.combatSeed == 0
            || packet.sessionBrokerAddress.empty() || packet.sessionBrokerPort == 0)) return std::nullopt;
        return packet;
    }

}
