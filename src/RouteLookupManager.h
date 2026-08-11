#pragma once

#include <map>
#include <deque>
#include <set>

#include "HttpRequestManager.h"
#include "OpenSkyAuthTokenHandler.h"
#include "ConfigurationWebServer.h"

struct RouteInfo {
    bool resolved = false;   // lookup attempted and completed (found or confirmed unknown)
    bool found = false;      // true only if a route was actually returned
    String origin;           // airport code - IATA where known (see ParseOpenSkyRouteResponse), else ICAO
    String destination;      // airport code - IATA where known (see ParseOpenSkyRouteResponse), else ICAO
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
// available, just slower to reach for. (This dual-source approach, the
// throttle tuning below, and the ICAO->IATA table are all ported from a
// sibling project, touch-radar, which validated them on real hardware.)
//
// Each unique callsign is only ever looked up once (cached, bounded to
// MAX_CACHE_ENTRIES - see CacheRoute) regardless of which source resolved
// it, and lookups are drained from two independently-throttled queues so a
// burst of newly-seen aircraft can't burst-fetch either source.
class RouteLookupManager
{
private:
    HttpRequestManager& http;
    OpenSkyAuthTokenHandler& authHandler;
    ConfigurationWebServer& configServer;

    std::map<String, RouteInfo> cache;
    // Insertion order of `cache`'s keys, oldest first - lets CacheRoute()
    // evict the oldest entry once the cache hits MAX_CACHE_ENTRIES, so a
    // callsign seen once and never again doesn't stay in memory for the
    // device's entire uptime (a real leak that exhausted the ESP32's heap
    // after a day or two in a busy area, before this cap existed).
    std::deque<String> cacheOrder;
    static constexpr size_t MAX_CACHE_ENTRIES = 300;

    // Fresh callsigns, not yet tried against hexdb.io.
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

    // Inserts/overwrites callsign in `cache`, evicting the oldest entry
    // first if this would exceed MAX_CACHE_ENTRIES - see the comment on
    // `cacheOrder`. The single write path to `cache` from either source.
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
