#pragma once

#include "Protocol.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace TownServer::Domain
{
    using PlayerId = std::uint64_t;

    /// Network-independent player identity used by TownServer content logic.
    class Player final
    {
    public:
        Player(PlayerId inPlayerId, std::string inName, TownProtocol::Vector2 inPosition);

        [[nodiscard]] PlayerId GetId() const noexcept;
        [[nodiscard]] const std::string& GetName() const noexcept;
        [[nodiscard]] TownProtocol::Vector2 GetPosition() const noexcept;
        [[nodiscard]] TownProtocol::Vector2 GetVelocity() const noexcept;
        [[nodiscard]] std::uint32_t GetLastProcessedInput() const noexcept;

        void SetMovementInput(const TownProtocol::MoveInput& inInput,
            std::chrono::steady_clock::time_point inReceivedTime) noexcept;
        void Simulate(float inDeltaSeconds, float inWalkSpeed, float inRunSpeed,
            std::chrono::steady_clock::time_point inNow) noexcept;
        void SetPosition(TownProtocol::Vector2 inPosition) noexcept;
        void StopMovement() noexcept;

    private:
        PlayerId playerId;
        std::string name;
        TownProtocol::Vector2 position;
        TownProtocol::Vector2 velocity;
        std::int8_t directionX{};
        std::int8_t directionY{};
        std::uint32_t lastProcessedInput{};
        std::chrono::steady_clock::time_point lastInputTime{};
        bool running{};
    };
}
