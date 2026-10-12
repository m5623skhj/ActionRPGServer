#pragma once

#include "PlayerSkillCatalog.h"
#include <cmath>
#include <limits>

namespace ActionRPG::Combat
{
    /// Data durations are combat seconds; persistent cooldowns and UI remaining time use real seconds.
    struct CombatTempo
    {
        float scale{1.0f};

        [[nodiscard]] static CombatTempo Parse(const PlayerSkills::Catalog::Json& inValue)
        { return {static_cast<float>(PlayerSkills::Catalog::Number(inValue, 0.1, 1.0))}; }

        [[nodiscard]] double RealSeconds(double inCombatSeconds) const
        { return inCombatSeconds / static_cast<double>(scale); }

        [[nodiscard]] float RealDurationSeconds(float inCombatSeconds) const
        {
            const double seconds = RealSeconds(inCombatSeconds);
            PlayerSkills::Catalog::Require(std::isfinite(seconds) && seconds >= 0
                && seconds <= std::numeric_limits<float>::max(), "Combat duration exceeds real-time float range.");
            return static_cast<float>(seconds);
        }

        /// Preserve the V6 ledger's millisecond bounds; never silently clamp a configured cooldown.
        [[nodiscard]] std::uint32_t ItemCooldownMilliseconds(float inCombatSeconds) const
        {
            // Retain existing item-data millisecond rounding when scale is one.
            const double milliseconds = std::ceil(inCombatSeconds * 1000.0f / scale);
            PlayerSkills::Catalog::Require(std::isfinite(milliseconds) && milliseconds >= 1 && milliseconds <= 3600000,
                "Dungeon item cooldown exceeds the persistent millisecond contract.");
            return static_cast<std::uint32_t>(milliseconds);
        }
    };
}
