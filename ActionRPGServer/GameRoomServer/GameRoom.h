#pragma once

#include "../Shared/RoomControlProtocol.h"
#include "DungeonDefinition.h"
#include "DungeonProtocol.h"
#include "CombatDefinition.h"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace GameRoomServer
{
    class GameRoom final : public std::enable_shared_from_this<GameRoom>
    {
    public:
        using PlayerId = ActionRPG::RoomControlProtocol::PlayerId;
        using RoomId = ActionRPG::RoomControlProtocol::RoomId;
        using EnterResultHandler = std::function<void(bool)>;
        using LeaveResultHandler = std::function<void(bool, bool)>;
        using EmptyHandler = std::function<void(RoomId)>;
        using SnapshotHandler = std::function<void(std::shared_ptr<const std::string>)>;

        GameRoom(asio::io_context& inIoContext, RoomId inRoomId, std::uint32_t inDungeonId,
            std::uint64_t inCombatSeed, std::vector<PlayerId> inExpectedPlayerIds,
            std::chrono::milliseconds inEnterTimeout, EmptyHandler inEmptyHandler,
            std::shared_ptr<const DungeonDefinition> inDefinition,
            std::shared_ptr<const CombatDefinition> inCombatDefinition, EmptyHandler inClearHandler);

        void Start();
        void Stop();
        void TryEnter(PlayerId inPlayerId, EnterResultHandler inResultHandler);
        void Leave(PlayerId inPlayerId, LeaveResultHandler inResultHandler);
        void RemoveUnannouncedPlayer(PlayerId inPlayerId, std::function<void(bool)> inResultHandler);
        void CompleteDungeon(std::function<void(std::vector<PlayerId>)> inResultHandler);
        [[nodiscard]] std::shared_ptr<const std::string> GetWorldFor(PlayerId inPlayerId) const;
        void UpdateInput(PlayerId inPlayerId, ActionRPG::DungeonProtocol::DungeonMoveInput inInput,
            std::function<void(ActionRPG::DungeonProtocol::DungeonPlayerState)> inHandler);
        void SubmitAction(PlayerId inPlayerId, ActionRPG::DungeonProtocol::DungeonActionInput inInput,
            std::function<void(ActionRPG::DungeonProtocol::DungeonActionResult)> inHandler);
        void GetCombatSnapshot(PlayerId inPlayerId, SnapshotHandler inHandler);

        [[nodiscard]] RoomId GetRoomId() const noexcept;
        [[nodiscard]] std::uint64_t GetCombatSeed() const noexcept;
        [[nodiscard]] const DungeonDefinition& GetDefinition() const noexcept { return *definition; }

    private:
        enum class State
        {
            WaitingForPlayers,
            Running,
            Cleared,
            Stopped
        };

        void HandleEnterTimeout();
        void StartDungeon();
        void ScheduleTick();
        void UpdatePlayers(float inDeltaSeconds);
        void UpdateCombat(float inDeltaSeconds);

        enum class Reaction { None, Hit, Falling, Down, Rising, Dead };
        enum class ShotPhase { None, Prepare, Fire, Recover };
        struct ActorState
        {
            std::uint32_t hp{};
            float height{}, verticalSpeed{}, reactionSeconds{};
            Reaction reaction{ Reaction::None };
        };

        struct PlayerState
        {
            std::string mapId;
            DungeonPoint position;
            std::int8_t directionX{};
            std::int8_t directionY{};
            bool running{};
            std::uint32_t sequence{};
            float walkSpeed{ 280.0f };
            float runSpeed{ 480.0f };
            bool warpArmed{ true };
            std::chrono::steady_clock::time_point lastInput{};
            ActorState actor;
            bool facingLeft{}, airAttack{}, worldReady{}, jumpPreparing{};
            std::uint32_t actionSequence{}, shotCount{}, pendingShots{}, airShotCount{};
            ShotPhase shotPhase{ ShotPhase::None };
            float shotSeconds{}, jumpSeconds{};
        };

        // Definitions are shared read-only; each room owns HP and the current AI node.
        // Future updates to these instances must run on this room's strand.
        struct MonsterState
        {
            std::shared_ptr<const MonsterDefinition> definition;
            std::string mapId;
            std::string placementId;
            std::string aiNodeId;
            DungeonPoint position;
            bool facingLeft{};
            ActorState actor;
            DungeonPoint spawnPosition;
            PlayerId targetId{}, skillTargetId{};
            float stateSeconds{}, actionSeconds{};
            bool actionStarted{}, actionComplete{}, hitApplied{};
            std::unordered_map<std::string, float> cooldowns;
        };

        struct ProjectileState
        {
            std::uint64_t id{};
            PlayerId ownerId{};
            std::string mapId;
            DungeonPoint position;
            float height{}, direction{}, heightDirection{}, remainingDistance{};
        };

        void UpdateActor(ActorState& inActor, float inDeltaSeconds);
        void ApplyDamage(ActorState& inActor, std::uint32_t inDamage, bool inAirborne);
        void UpdateMonster(MonsterState& inMonster, float inDeltaSeconds);
        bool EvaluateCondition(const MonsterState& inMonster, const nlohmann::json& inCondition) const;
        void EnterNode(MonsterState& inMonster, const std::string& inNodeId);
        bool AdvanceNode(MonsterState& inMonster, const std::string& inTrigger);
        void UpdateShots(PlayerId inPlayerId, PlayerState& inPlayer, float inDeltaSeconds);
        void UpdateProjectiles(float inDeltaSeconds);
        void CheckClear();
        [[nodiscard]] bool IsMapCleared(const std::string& inMapId) const;
        [[nodiscard]] static const char* ReactionName(Reaction inReaction);
        [[nodiscard]] DungeonPoint MoveOnMap(const std::string& inMapId, DungeonPoint inPosition,
            DungeonPoint inTarget, float inDistance) const;

        asio::strand<asio::io_context::executor_type> strand;
        asio::steady_timer enterTimer;
        asio::steady_timer tickTimer;
        RoomId roomId;
        std::uint32_t dungeonId;
        std::uint64_t combatSeed;
        const std::shared_ptr<const DungeonDefinition> definition;
        const std::shared_ptr<const CombatDefinition> combatDefinition;
        nlohmann::json dungeonWorld;
        std::unordered_map<std::uint64_t, MonsterState> monsters;
        std::unordered_map<std::string, std::vector<std::uint64_t>> monsterIdsByMap;
        std::unordered_map<PlayerId, PlayerState> players;
        std::unordered_map<PlayerId, std::shared_ptr<const std::string>> initialWorlds;
        std::unordered_set<PlayerId> expectedPlayers;
        std::unordered_set<PlayerId> enteredPlayers;
        std::chrono::milliseconds enterTimeout;
        EmptyHandler emptyHandler;
        EmptyHandler clearHandler;
        std::vector<ProjectileState> projectiles;
        std::unordered_set<std::uint64_t> bosses;
        std::uint64_t nextProjectileId{ 1 };
        bool clearRequested{};
        State state = State::WaitingForPlayers;
        std::uint64_t serverTick{};
    };
}
