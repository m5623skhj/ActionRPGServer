#pragma once

#include "../Shared/RoomControlProtocol.h"
#include "DungeonDefinition.h"
#include "DungeonProtocol.h"
#include "CombatDefinition.h"

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace GameRoomServer
{
    class GameRoom final : public std::enable_shared_from_this<GameRoom>
    {
    public:
        static constexpr std::uint16_t COMBAT_PROTOCOL_VERSION = 3;
        static constexpr std::uint32_t TICK_RATE = 30;
        static constexpr std::uint32_t SNAPSHOT_RATE = 15;
        static constexpr float TICK_SECONDS = 1.0f / TICK_RATE;
        static constexpr auto TICK_INTERVAL = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / TICK_RATE));
        using PlayerId = ActionRPG::RoomControlProtocol::PlayerId;
        using RoomId = ActionRPG::RoomControlProtocol::RoomId;
        using EnterResultHandler = std::function<void(bool)>;
        using LeaveResultHandler = std::function<void(bool, bool)>;
        using EmptyHandler = std::function<void(RoomId)>;
        using SnapshotHandler = std::function<void(std::shared_ptr<const std::string>)>;
        struct RealtimeFrame
        {
            std::string mapId, payload;
            std::uint64_t sequence{}, tick{}, timeMs{};
            std::uint8_t state{};
        };
        using RealtimeHandler = std::function<void(std::uint32_t, std::shared_ptr<const RealtimeFrame>)>;

        GameRoom(asio::io_context& inIoContext, RoomId inRoomId, std::uint32_t inDungeonId,
            std::uint64_t inCombatSeed, std::vector<PlayerId> inExpectedPlayerIds,
            std::chrono::milliseconds inEnterTimeout, EmptyHandler inEmptyHandler,
            std::shared_ptr<const DungeonDefinition> inDefinition,
            std::shared_ptr<const CombatDefinition> inCombatDefinition, EmptyHandler inClearHandler,
            std::function<void(std::vector<PlayerId>)> inStartedHandler);

        void Start();
        void Stop();
        void TryEnter(PlayerId inPlayerId, std::uint32_t inCharacterId, std::string inProgression, EnterResultHandler inResultHandler);
        void UpdatePlayerProgress(PlayerId inPlayerId, std::string inProgression);
        void Leave(PlayerId inPlayerId, LeaveResultHandler inResultHandler);
        void RemoveUnannouncedPlayer(PlayerId inPlayerId, std::function<void(bool)> inResultHandler);
        void ValidateCompletion(std::vector<PlayerId> inParticipants, std::function<void(bool)> inHandler);
        [[nodiscard]] std::uint32_t GetDungeonId() const noexcept { return dungeonId; }
        void CompleteDungeon(std::function<void(std::vector<PlayerId>)> inResultHandler);
        [[nodiscard]] std::shared_ptr<const std::string> GetWorldFor(PlayerId inPlayerId) const;
        void UpdateInput(PlayerId inPlayerId, ActionRPG::DungeonProtocol::DungeonMoveInput inInput,
            std::function<void(ActionRPG::DungeonProtocol::DungeonPlayerState)> inHandler);
        void SubmitAction(PlayerId inPlayerId, ActionRPG::DungeonProtocol::DungeonActionInput inInput,
            std::function<void(ActionRPG::DungeonProtocol::DungeonActionResult)> inHandler);
        void SubmitSkill(PlayerId inPlayerId, ActionRPG::DungeonProtocol::DungeonSkillInput inInput,
            std::function<void(ActionRPG::DungeonProtocol::DungeonActionResult)> inHandler);
        void GetCombatSnapshot(PlayerId inPlayerId, SnapshotHandler inHandler);
        void SubscribeRealtime(PlayerId inPlayerId, std::weak_ptr<const std::uint8_t> inLease,
            RealtimeHandler inHandler);

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
        void UpdatePlayers();
        void UpdateCombat(float inDeltaSeconds);
        void PublishRealtime();
        [[nodiscard]] std::shared_ptr<const RealtimeFrame> BuildRealtimeFrame(const std::string& inMapId,
            std::uint64_t inTimeMs) const;

        enum class Reaction { None, Hit, Falling, Down, Rising, Dead };
        enum class ShotPhase { None, Prepare, Fire, Recover };
        struct ActorState
        {
            std::uint32_t hp{};
            float height{}, verticalSpeed{}, reactionSeconds{};
            Reaction reaction{ Reaction::None };
            std::uint32_t reactionSequence{};
            float hitRecovery{}, reactionDurationSeconds{}, hitstopRemainingSeconds{};
            std::uint32_t hitstopSequence{};
            std::uint64_t hitstopStartTimeMs{};
            float hitstopDurationSeconds{};
            float actionDeltaSeconds{}; // Prepared once per room tick; never serialized.
        };

        struct ActiveSkill
        {
            std::string id;
            bool airborne{}, facingLeft{}, eventApplied{};
            float seconds{};
            std::unordered_set<std::uint64_t> hitIds;
            std::uint32_t skillLevel{};
        };
        struct ActiveBuff
        {
            std::string id, stat;
            float multiplier{}, remainingSeconds{};
        };
        struct SlideState
        {
            bool active{};
            std::uint32_t sequence{}, damage{};
            float seconds{}, durationSeconds{}, directionX{}, directionY{}, speed{}, hitstopSeconds{};
            std::unordered_set<std::uint64_t> hitIds;
        };
        struct BufferedAction
        {
            std::uint8_t action{};
            bool facingLeft{};
            float remainingSeconds{};
        };
        struct PlayerState
        {
            std::string mapId;
            DungeonPoint position;
            std::int8_t directionX{};
            std::int8_t directionY{};
            bool running{};
            std::uint32_t sequence{};
            std::uint32_t mapEpoch{ 1 };
            float walkSpeed{ 280.0f };
            float runSpeed{ 480.0f };
            bool warpArmed{ true };
            std::chrono::steady_clock::time_point lastInput{};
            ActorState actor;
            bool facingLeft{}, airAttack{}, worldReady{}, jumpPreparing{};
            std::uint32_t actionSequence{}, shotCount{}, pendingShots{}, airShotCount{};
            std::uint32_t shotSequence{}, jumpSequence{};
            ShotPhase shotPhase{ ShotPhase::None };
            float shotSeconds{}, jumpSeconds{};
            float shotInputRemainingSeconds{};
            std::deque<BufferedAction> bufferedActions;
            std::uint32_t characterId{}, skillSequence{};
            std::string lastSkillId;
            bool lastSkillAirborne{};
            float lastSkillSeconds{};
            std::optional<ActiveSkill> skill;
            std::unordered_map<std::string, float> skillCooldowns;
            std::vector<ActiveBuff> buffs;
            SlideState slide;
            ActionRPG::PlayerSkills::CharacterProgression progression;
        };

        // Definitions are shared read-only; each room owns HP and the current AI node.
        // Future updates to these instances must run on this room's strand.
        struct MonsterState
        {
            std::shared_ptr<const MonsterDefinition> definition;
            std::string mapId;
            std::string placementId;
            std::string aiNodeId;
            std::uint32_t actionSequence{ 1 };
            DungeonPoint position;
            bool facingLeft{};
            ActorState actor;
            DungeonPoint spawnPosition;
            PlayerId targetId{}, skillTargetId{};
            float stateSeconds{}, actionSeconds{};
            std::uint64_t behaviorSeed{};
            DungeonPoint pursuitDirection;
            float movementMultiplier{ 1.0f }, stopDistanceMultiplier{ 1.0f }, cooldownMultiplier{ 1.0f };
            float attackDelaySeconds{}, attackWaitSeconds{}, separationBudget{};
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
            std::string skillId;
            float directionY{}, speed{}, radius{}, ageSeconds{};
            std::uint32_t damage{};
            float hitstopSeconds{};
        };

        void PrepareActorTimes(float inDeltaSeconds);
        [[nodiscard]] static float ActionDelta(const ActorState& inActor);
        void CancelHitstop(ActorState& inActor);
        void UpdateActor(ActorState& inActor, float inDeltaSeconds);
        void ApplyDamage(ActorState& inActor, std::uint32_t inDamage, bool inAirborne,
            ActorState* inAttacker, float inHitstopSeconds);
        void UpdateMonster(MonsterState& inMonster, float inDeltaSeconds, float inWorldDeltaSeconds);
        void InitializeMonsterBehavior(MonsterState& inMonster, std::uint64_t inInstanceId);
        void SeparateMonsters(float inDeltaSeconds);
        bool EvaluateCondition(const MonsterState& inMonster, const nlohmann::json& inCondition) const;
        void EnterNode(MonsterState& inMonster, const std::string& inNodeId);
        bool AdvanceNode(MonsterState& inMonster, const std::string& inTrigger);
        void UpdateShots(PlayerId inPlayerId, PlayerState& inPlayer, float inDeltaSeconds);
        [[nodiscard]] static bool IsShotFacingLocked(const PlayerState& inPlayer);
        static void ResetShotState(PlayerState& inPlayer);
        bool TryQueueAction(PlayerState& inPlayer, std::uint8_t inAction, bool inFacingLeft);
        void UpdateBufferedActions(PlayerState& inPlayer, float inDeltaSeconds);
        bool TryStartSlide(PlayerState& inPlayer, const ActionRPG::DungeonProtocol::DungeonActionInput& inInput);
        void UpdateSlide(PlayerState& inPlayer, float inDeltaSeconds);
        void HitSlideContacts(PlayerState& inPlayer, DungeonPoint inStart, DungeonPoint inEnd);
        static void StopSlide(PlayerState& inPlayer);
        void UpdateProjectiles(float inDeltaSeconds);
        void UpdateSkillTimers(PlayerState& inPlayer, float inDeltaSeconds);
        void UpdateSkills(PlayerId inPlayerId, PlayerState& inPlayer, float inDeltaSeconds, bool inStarting = false);
        [[nodiscard]] static float BuffMultiplier(const PlayerState& inPlayer, const char* inStat);
        void CheckClear();
        [[nodiscard]] bool IsMapCleared(const std::string& inMapId) const;
        [[nodiscard]] static const char* ReactionName(Reaction inReaction);
        [[nodiscard]] DungeonPoint MoveOnMap(const std::string& inMapId, DungeonPoint inPosition,
            DungeonPoint inTarget, float inDistance) const;

        asio::strand<asio::io_context::executor_type> strand;
        asio::steady_timer enterTimer;
        asio::steady_timer tickTimer;
        std::chrono::steady_clock::time_point nextTickAt{};
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
        std::function<void(std::vector<PlayerId>)> startedHandler;
        std::vector<ProjectileState> projectiles;
        std::unordered_set<std::uint64_t> bosses;
        std::uint64_t nextProjectileId{ 1 };
        bool clearRequested{};
        bool hitstopChanged{};
        State state = State::WaitingForPlayers;
        std::uint64_t serverTick{};
        struct RealtimeSubscriber
        {
            std::weak_ptr<const std::uint8_t> lease;
            RealtimeHandler handler;
        };
        std::unordered_map<PlayerId, RealtimeSubscriber> realtimeSubscribers;
        std::uint64_t realtimeSequence{};
    };
}
