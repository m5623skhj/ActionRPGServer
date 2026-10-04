#include "Player.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace TownServer::Domain
{
    Player::Player(const PlayerId inPlayerId, std::string inName, const std::uint32_t inCharacterId,
        const TownProtocol::Vector2 inPosition)
        : playerId(inPlayerId),
          name(std::move(inName)),
          characterId(inCharacterId),
          position(inPosition),
          lastInputTime(std::chrono::steady_clock::now())
    {
    }

    PlayerId Player::GetId() const noexcept
    {
        return playerId;
    }

    const std::string& Player::GetName() const noexcept
    {
        return name;
    }

    std::uint32_t Player::GetCharacterId() const noexcept
    {
        return characterId;
    }

    TownProtocol::Vector2 Player::GetPosition() const noexcept
    {
        return position;
    }

    TownProtocol::Vector2 Player::GetVelocity() const noexcept
    {
        return velocity;
    }

    std::uint32_t Player::GetLastProcessedInput() const noexcept
    {
        return lastProcessedInput;
    }

    void Player::SetMovementInput(const TownProtocol::MoveInput& inInput,
        const std::chrono::steady_clock::time_point inReceivedTime) noexcept
    {
        if (inInput.sequence <= lastProcessedInput)
        {
            return;
        }

        directionX = inInput.directionX;
        directionY = inInput.directionY;
        lastProcessedInput = inInput.sequence;
        lastInputTime = inReceivedTime;
    }

    void Player::Simulate(const float inDeltaSeconds, const float inWalkSpeed,
        const std::chrono::steady_clock::time_point inNow) noexcept
    {
        constexpr auto INPUT_TIMEOUT = std::chrono::milliseconds(500);
        // Preserve movement before the timeout even when this integration interval crosses its deadline.
        const float movementSeconds = std::clamp(
            std::chrono::duration<float>(lastInputTime + INPUT_TIMEOUT - inNow).count() + inDeltaSeconds,
            0.0f, inDeltaSeconds);

        float x = static_cast<float>(directionX);
        float y = static_cast<float>(directionY);
        const float lengthSquared = x * x + y * y;
        if (lengthSquared > 0.0f)
        {
            const float inverseLength = 1.0f / std::sqrt(lengthSquared);
            x *= inverseLength;
            y *= inverseLength;
        }

        velocity = TownProtocol::Vector2{ x * inWalkSpeed, y * inWalkSpeed };
        position.x += velocity.x * movementSeconds;
        position.y += velocity.y * movementSeconds;
        if (inNow - lastInputTime >= INPUT_TIMEOUT)
        {
            directionX = 0;
            directionY = 0;
            velocity = {};
        }
    }

    void Player::SetPosition(const TownProtocol::Vector2 inPosition) noexcept
    {
        position = inPosition;
    }

    void Player::StopMovement() noexcept
    {
        directionX = 0;
        directionY = 0;
        velocity = {};
    }
}
