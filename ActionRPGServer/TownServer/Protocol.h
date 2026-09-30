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

    enum class PartyOperationType : std::uint8_t
    {
        Invite = 1,
        AnswerInvitation = 2,
        Leave = 3,
        Kick = 4,
        Settings = 5,
        Create = 6
    };

    enum class PartyResultCode : std::uint8_t
    {
        Succeeded = 0,
        PlayerNotFound = 1,
        AlreadyInParty = 2,
        NotInParty = 3,
        NotLeader = 4,
        PartyFull = 5,
        AlreadyInvited = 6,
        InvitationNotFound = 7,
        InvalidTarget = 8,
        Busy = 9,
        InvalidTitle = 10
    };

    struct PartyMemberInfo
    {
        std::uint64_t playerId{};
        std::string playerName;
        std::uint8_t slot{};
    };

    struct PartyDirectoryEntry
    {
        std::uint64_t partyId{};
        std::string title;
        std::string leaderName;
        std::vector<PartyMemberInfo> members;
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
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartyInviteRequest& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartyInviteAnswer& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartyLeaveRequest& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartyKickRequest& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartyInvitation& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartySnapshot& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartyOperationResult& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartySettingsRequest& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartyDirectoryPageRequest& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartyDirectoryUnsubscribe& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartyDirectoryPage& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartyDirectoryChanged& inPacket);
    [[nodiscard]] std::vector<std::uint8_t> Encode(const PartyCreateRequest& inPacket);

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
    [[nodiscard]] std::optional<PartyInviteRequest> DecodePartyInviteRequest(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartyInviteAnswer> DecodePartyInviteAnswer(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartyLeaveRequest> DecodePartyLeaveRequest(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartyKickRequest> DecodePartyKickRequest(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartyInvitation> DecodePartyInvitation(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartySnapshot> DecodePartySnapshot(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartyOperationResult> DecodePartyOperationResult(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartySettingsRequest> DecodePartySettingsRequest(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartyDirectoryPageRequest> DecodePartyDirectoryPageRequest(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartyDirectoryUnsubscribe> DecodePartyDirectoryUnsubscribe(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartyDirectoryPage> DecodePartyDirectoryPage(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartyDirectoryChanged> DecodePartyDirectoryChanged(const std::vector<std::uint8_t>& inPacket);
    [[nodiscard]] std::optional<PartyCreateRequest> DecodePartyCreateRequest(const std::vector<std::uint8_t>& inPacket);
}
