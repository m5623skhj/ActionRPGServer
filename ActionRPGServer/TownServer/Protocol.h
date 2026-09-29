#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace TownProtocol
{
    struct Vector2
    {
        float x{};
        float y{};
    };

    using Polygon = std::vector<Vector2>;

    enum class TransitionActionType : std::uint8_t
    {
        MapTransfer = 1,
        DungeonSelection = 2
    };

    struct EntryPoint
    {
        std::string id;
        Vector2 position;
    };

    struct TransitionZone
    {
        std::string id;
        Polygon polygon;
        TransitionActionType actionType = TransitionActionType::MapTransfer;
        std::string targetMapId;
        std::string targetEntryPointId;
        std::string dungeonGroupId;
    };

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
        std::vector<EntryPoint> entryPoints;
        std::vector<TransitionZone> transitionZones;
        float spawnX{};
        float spawnY{};
        float sectorWidth{};
        float sectorHeight{};
        float walkSpeed{};
        float runSpeed{};
    };

    struct DungeonOption
    {
        std::uint32_t dungeonId{};
        std::string name;
        std::string levelRange;
        std::string description;
    };

}

#include "TownPacket.generated.h"

namespace TownProtocol
{
    [[nodiscard]] std::optional<PacketType> ReadPacketType(const std::vector<std::uint8_t>& inPacket);

    [[nodiscard]] std::vector<std::uint8_t> Encode(const EnterTownRequest& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const EnterTownResponse& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const MoveInput& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PlayerAppear& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PlayerMove& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PlayerDisappear& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const ConfirmDungeonJoin& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const EnterDungeonRequest& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const EnterDungeonResponse& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const MapChanged& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const DungeonSelectionOpen& inPacket);

    [[nodiscard]] std::optional<EnterTownRequest> DecodeEnterTownRequest(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<EnterTownResponse> DecodeEnterTownResponse(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<MoveInput> DecodeMoveInput(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PlayerAppear> DecodePlayerAppear(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PlayerMove> DecodePlayerMove(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PlayerDisappear> DecodePlayerDisappear(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<ConfirmDungeonJoin> DecodeConfirmDungeonJoin(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<EnterDungeonRequest> DecodeEnterDungeonRequest(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<EnterDungeonResponse> DecodeEnterDungeonResponse(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<MapChanged> DecodeMapChanged(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<DungeonSelectionOpen> DecodeDungeonSelectionOpen(const std::vector<std::uint8_t>& inPacket);
}
