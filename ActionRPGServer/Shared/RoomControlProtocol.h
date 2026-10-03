#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ActionRPG::RoomControlProtocol
{
    using RequestId = std::uint64_t;
    using RoomServerId = std::uint64_t;
    using RoomId = std::uint64_t;
    using PlayerId = std::uint64_t;

    enum class PacketType : std::uint16_t
    {
        RegisterRoomServer = 1,
        CreateRoom = 2,
        CreateRoomResult = 3,
        ConfirmJoin = 4,
        EnterRoom = 5,
        LeaveRoom = 6,
        RoomEnded = 7,
        FinishRoom = 8,
        FinishRoomResult = 9
    };

    enum class RoomEndReason : std::uint8_t
    {
        Cleared = 1,
        Aborted = 2
    };

    struct RegisterRoomServer
    {
        RoomServerId roomServerId{};
        std::uint32_t maxRoomCount{};
    };

    struct CreateRoom
    {
        RequestId requestId{};
        std::uint32_t dungeonId{};
        std::vector<PlayerId> participantPlayerIds;
    };

    struct CreateRoomResult
    {
        RequestId requestId{};
        bool succeeded{};
        RoomId roomId{};
        std::uint64_t combatSeed{};
        std::string sessionBrokerAddress;
        std::uint16_t sessionBrokerPort{};
    };

    struct FinishRoom
    {
        RequestId requestId{};
        RoomId roomId{};
        bool retry{};
        std::vector<PlayerId> participantPlayerIds;
    };

    struct FinishRoomResult
    {
        RequestId requestId{};
        RoomId previousRoomId{};
        bool succeeded{};
        RoomId roomId{};
        std::uint64_t combatSeed{};
        std::string sessionBrokerAddress;
        std::uint16_t sessionBrokerPort{};
    };

    struct ConfirmJoin
    {
        RoomId roomId{};
        PlayerId playerId{};
        std::uint64_t challenge{};
        std::uint32_t characterId{};
    };

    struct EnterRoom
    {
        RoomId roomId{};
        PlayerId playerId{};
    };

    struct LeaveRoom
    {
        RoomId roomId{};
        PlayerId playerId{};
    };

    struct RoomEnded
    {
        RoomId roomId{};
        RoomEndReason reason{ RoomEndReason::Aborted };
        std::vector<PlayerId> rewardPlayerIds;
    };

    [[nodiscard]] std::optional<PacketType> ReadPacketType(const std::vector<std::uint8_t>& inPacket);

    [[nodiscard]] std::vector<std::uint8_t> Encode(const RegisterRoomServer& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const CreateRoom& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const CreateRoomResult& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const FinishRoom& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const FinishRoomResult& inPacket);
    [[nodiscard]] std::optional<FinishRoom> DecodeFinishRoom(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<FinishRoomResult> DecodeFinishRoomResult(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const ConfirmJoin& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const EnterRoom& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const LeaveRoom& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const RoomEnded& inPacket);

    [[nodiscard]] std::optional<RegisterRoomServer> DecodeRegisterRoomServer(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<CreateRoom> DecodeCreateRoom(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<CreateRoomResult> DecodeCreateRoomResult(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<ConfirmJoin> DecodeConfirmJoin(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<EnterRoom> DecodeEnterRoom(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<LeaveRoom> DecodeLeaveRoom(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<RoomEnded> DecodeRoomEnded(const std::vector<std::uint8_t>& inPacket);
}
