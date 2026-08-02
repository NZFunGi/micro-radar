#pragma once

#include <algorithm>

// Shared OpenSky daily request quota constants. AircraftManager's /states/all
// polling and RouteLookupManager's /api/routes lookups draw from the same
// per-day pool, so both derive their timing from this single source instead
// of independently guessing a safe rate.
namespace OpenSkyBudget {

    constexpr long MS_PER_DAY = 24L * 60 * 60 * 1000;
    constexpr int ANONYMOUS_TOKENS_PER_DAY = 400;
    constexpr int AUTHED_TOKENS_PER_DAY = 4000;
    constexpr int TOKEN_BUFFER = 3;

    // Fraction of the daily quota reserved for /api/routes lookups, leaving
    // the remainder for /states/all polling. At 0.2 (the original value),
    // even the authenticated rate is only one route lookup roughly every
    // ~1.8 minutes - in an area with more than a handful of distinct aircraft
    // passing through, RouteLookupManager's queue (strict FIFO, see
    // RouteLookupManager.cpp) falls permanently behind: a specific flight can
    // sit queued for 20+ minutes, and if it leaves the configured radar
    // radius before its turn comes up, PruneQueueExcept silently drops it
    // without ever resolving. Raised to 0.4 to roughly halve that wait
    // (~55s authenticated); /states/all polling still gets 60% of the daily
    // budget, which barely matters visually since TrackedAircraft already
    // smoothly interpolates aircraft position between polls rather than
    // jumping. If a given location is busy enough that lookups still can't
    // keep up, raise this further - the trade-off is purely against position
    // polling frequency, not correctness.
    constexpr float ROUTE_LOOKUP_BUDGET_SHARE = 0.4f;

    inline int DailyRequestBudget(bool authenticated)
    {
        const int total = authenticated ? AUTHED_TOKENS_PER_DAY : ANONYMOUS_TOKENS_PER_DAY;
        return total - TOKEN_BUFFER;
    }

    inline int StatesAllBudget(bool authenticated)
    {
        const int total = DailyRequestBudget(authenticated);
        return std::max(1, static_cast<int>(total * (1.0f - ROUTE_LOOKUP_BUDGET_SHARE)));
    }

    inline int RouteLookupBudget(bool authenticated)
    {
        const int total = DailyRequestBudget(authenticated);
        return std::max(1, static_cast<int>(total * ROUTE_LOOKUP_BUDGET_SHARE));
    }

}
