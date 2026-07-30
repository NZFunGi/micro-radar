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
    // the remainder for /states/all polling.
    constexpr float ROUTE_LOOKUP_BUDGET_SHARE = 0.2f;

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
