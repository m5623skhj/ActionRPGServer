#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TownProtocol
{
    enum class PacketType : std::uint16_t
    {
        EnterTownRequest = 1,
        EnterTownResponse = 2,
        MoveInput = 3,
        PlayerAppear = 4,
        PlayerMove = 5,
        PlayerDisappear = 6
    };

    struct Vector2
    {
        float x{};
        float y{};
    };

    using Polygon = std::vector<Vector2>;

    struct MapImage
    {
        std::string asset;
        float x{};
        float y{};
        float width{};
        float height{};
    };

    struct MapInfo
    {
        std::string mapId;
        float worldLeft{};
        float worldTop{};
        float worldRight{};
        float worldBottom{};
        std::vector<MapImage> images;
        std::vector<Polygon> walkablePolygons;
        std::vector<Polygon> blockedPolygons;
        float spawnX{};
        float spawnY{};
        float sectorWidth{};
        float sectorHeight{};
        float walkSpeed{};
        float runSpeed{};
    };

    struct EnterTownRequest
    {
        std::string playerName;
    };

    struct EnterTownResponse
    {
        std::uint64_t playerId{};
        MapInfo map;
    };

    struct MoveInput
    {
        std::uint32_t sequence{};
        std::int8_t directionX{};
        std::int8_t directionY{};
        bool running{};
    };

    struct PlayerAppear
    {
        std::uint64_t playerId{};
        std::string playerName;
        Vector2 position;
        Vector2 velocity;
    };

    struct PlayerMove
    {
        std::uint64_t playerId{};
        std::uint32_t serverTick{};
        std::uint32_t lastProcessedInput{};
        Vector2 position;
        Vector2 velocity;
    };

    struct PlayerDisappear
    {
        std::uint64_t playerId{};
    };

    [[nodiscard]] std::optional<PacketType> ReadPacketType(const std::vector<std::uint8_t>& inPacket);

    [[nodiscard]] std::vector<std::uint8_t> Encode(const EnterTownRequest& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const EnterTownResponse& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const MoveInput& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PlayerAppear& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PlayerMove& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PlayerDisappear& inPacket);

    [[nodiscard]] std::optional<EnterTownRequest> DecodeEnterTownRequest(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<EnterTownResponse> DecodeEnterTownResponse(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<MoveInput> DecodeMoveInput(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PlayerAppear> DecodePlayerAppear(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PlayerMove> DecodePlayerMove(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PlayerDisappear> DecodePlayerDisappear(const std::vector<std::uint8_t>& inPacket);
}
