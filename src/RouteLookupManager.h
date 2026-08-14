#pragma once

#include <deque>
#include <set>
#include <cstring>

#include "HttpRequestManager.h"
#include "OpenSkyAuthTokenHandler.h"
#include "ConfigurationWebServer.h"

// Fixed-size buffers throughout - callsigns are documented as up to 8
// characters (OpenSky's states/all spec), airport codes are IATA (3 chars)
// or ICAO (4 chars) - so RouteInfo and RouteLookupManager's whole cache
// (see the comment on `cache` below) can be plain fixed-size arrays with
// zero heap allocation, rather than String/std::map.
struct RouteInfo {
    bool resolved = false;   // lookup attempted and completed (found or confirmed unknown)
    bool found = false;      // true only if a route was actually returned
    char origin[5] = {0};        // airport code - IATA where known, else ICAO (see ParseOpenSkyRouteResponse)
    char destination[5] = {0};   // airport code - IATA where known, else ICAO (see ParseOpenSkyRouteResponse)
};

// Looks up flight origin/destination for a callsign, trying hexdb.io
// (Josh Douch's free callsign-route API - https://hexdb.io, no key
// required) first, and only falling back to OpenSky's own /api/routes for
// callsigns it doesn't have. hexdb.io's own rate limit (1000 req/5min) is
// far more generous than OpenSky's shared daily quota (see
// OpenSkyBudget.h) and completely separate from it, so routing most
// lookups through hexdb.io first means they never touch the budget that
// also has to cover position polling. The trade-off, and the reason this
// isn't a straight replacement: hexdb.io is known to miss roughly half of
// real routes and can be stale - OpenSky's data is more authoritative when
// available, just slower to reach for.
//
// Each unique callsign is only ever looked up once (cached, bounded to
// MAX_CACHE_ENTRIES - see `cache`) regardless of which source resolved it,
// and lookups are drained from two independently-throttled queues so a
// burst of newly-seen aircraft can't burst-fetch either source.
class RouteLookupManager
{
private:
    HttpRequestManager& http;
    OpenSkyAuthTokenHandler& authHandler;
    ConfigurationWebServer& configServer;

    // Route cache - a fixed-size array of fixed-size entries, not a
    // std::map<String, RouteInfo>. An earlier version used exactly that,
    // with an insertion-order std::deque<String> to evict the oldest entry
    // once the map exceeded 300 - that bounded the cache's *size* but not
    // its *churn*: every eviction+insert cycle still freed and
    // re-allocated an rb-tree node plus several String heap buffers, and
    // that malloc/free churn, sustained continuously as new callsigns are
    // seen day after day in a busy area, was enough on its own to
    // fragment the ESP32's heap badly - reproducing the exact same "loses
    // aircraft, then locks up after a day or two" symptom the size cap was
    // meant to fix, just as slowly.
    //
    // This version allocates the whole cache once, at compile time -
    // every update after that is an in-place overwrite of a fixed-size
    // struct, never a heap allocation. Lookups are a linear scan over up
    // to MAX_CACHE_ENTRIES short fixed-size comparisons, which is
    // negligible cost at the rate this is actually called (at most a few
    // times per second, throttled - see hexdbThrottleMs/openSkyThrottleMs).
    struct CacheEntry {
        char callsign[9] = {0}; // up to 8 chars + null terminator
        bool used = false;
        RouteInfo info;
    };
    static constexpr size_t MAX_CACHE_ENTRIES = 300;
    CacheEntry cache[MAX_CACHE_ENTRIES];
    // Round-robin write position: CacheRoute() only advances this when
    // filling a slot for a genuinely new callsign (never on overwriting an
    // existing one), so the slot it currently points to always holds
    // whichever currently-cached entry was written longest ago - the same
    // oldest-first eviction order the old deque-based version had, just
    // without ever touching the heap to get it.
    size_t nextCacheSlot = 0;

    // Fresh callsigns, not yet tried against hexdb.io. Bounded in practice
    // by PruneQueueExcept to whatever's currently on screen (typically a
    // handful of aircraft), so - unlike the cache above - these are low
    // enough volume that the ordinary String/deque/set churn here isn't a
    // realistic fragmentation risk.
    std::deque<String> queue;
    std::set<String> queued;

    // Callsigns hexdb.io definitively missed (a real 404, or an
    // unparseable/empty route on a 200), waiting for an OpenSky lookup
    // slot - a separate queue from the one above so a callsign already past
    // its hexdb.io attempt doesn't get sent through hexdb.io again.
    std::deque<String> openSkyFallbackQueue;
    std::set<String> openSkyFallbackQueued;

    // Two independent throttle clocks - hexdb.io's own generous limit has
    // nothing to do with OpenSky's shared budget, so gating hexdb.io
    // lookups at OpenSky's much slower pace would waste exactly the
    // benefit of trying it first.
    unsigned long hexdbThrottleMs = 0;
    unsigned long lastHexdbLookup = 0;
    unsigned long openSkyThrottleMs = 0;
    unsigned long lastOpenSkyLookup = 0;

    void AttemptHexdb(const String& callsign);
    void AttemptOpenSky(const String& callsign);
    RouteInfo ParseHexdbRouteResponse(const String& text) const;
    RouteInfo ParseOpenSkyRouteResponse(const String& json) const;
    void LogRouteResult(const char* source, const String& callsign, const RouteInfo& info) const;

    CacheEntry* FindCacheEntry(const String& callsign);
    const CacheEntry* FindCacheEntry(const String& callsign) const;

    // Inserts/overwrites callsign in `cache`, evicting the oldest slot
    // first if this is a new callsign and the cache is full - see the
    // comment on `cache`/`nextCacheSlot`. The single write path to `cache`.
    void CacheRoute(const String& callsign, const RouteInfo& info);

public:
    RouteLookupManager(ConfigurationWebServer& config, OpenSkyAuthTokenHandler& auth, HttpRequestManager& httpManager)
        : http(httpManager), authHandler(auth), configServer(config)
    {
    }
    ~RouteLookupManager() = default;

    void Initialise();
    void Update();
    void RequestLookup(const String& trimmedCallsign);

    // Drops any not-yet-looked-up entry (from either queue) whose callsign
    // isn't in activeCallsigns. Intended to be called once per
    // AircraftManager fetch cycle with the set of callsigns currently on
    // screen, so a queue backlog only ever holds lookups for aircraft that
    // are still actually visible, instead of aging out currently-visible
    // aircraft behind ones that left long ago. Already-cached routes are
    // untouched - a plane that reappears with the same callsign still
    // benefits from the earlier lookup.
    void PruneQueueExcept(const std::set<String>& activeCallsigns);

    const RouteInfo* GetRoute(const String& trimmedCallsign) const;
};
