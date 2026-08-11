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
    String origin;           // ICAO airport code, e.g. "NZAA"
    String destination;      // ICAO airport code, e.g. "YMML"
};

// Looks up flight origin/destination via OpenSky's /api/routes endpoint.
// Each unique callsign is only ever looked up once (cached for the process
// lifetime), and lookups are drained from a queue at a throttled rate so a
// burst of newly-seen aircraft can't burst-fetch and blow the shared daily
// OpenSky request quota (see OpenSkyBudget.h).
class RouteLookupManager
{
private:
    HttpRequestManager& http;
    OpenSkyAuthTokenHandler& authHandler;
    ConfigurationWebServer& configServer;

    std::map<String, RouteInfo> cache;
    // Insertion order of `cache`'s keys, oldest first - lets CacheRoute() evict
    // the oldest entry once the cache hits MAX_CACHE_ENTRIES. Without this,
    // `cache` was a genuine unbounded leak: every distinct callsign ever seen
    // stayed in it for the entire uptime, which is fine for an hour of testing
    // but exhausts the ESP32's ~300KB heap after a day or two in a busy area -
    // manifesting first as JSON parses starting to fail for lack of a
    // contiguous allocation (aircraft silently disappearing from the states/all
    // response), then a full lockup once the heap is critically fragmented.
    std::deque<String> cacheOrder;
    static constexpr size_t MAX_CACHE_ENTRIES = 300;
    std::deque<String> queue;
    std::set<String> queued;

    unsigned long lookupThrottleMs = 0;
    unsigned long lastLookup = 0;

    RouteInfo ParseRouteResponse(const String& json) const;

    // Inserts/overwrites callsign in `cache`, evicting the oldest entry first
    // if this would exceed MAX_CACHE_ENTRIES - see the comment on `cacheOrder`.
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

    // Drops any not-yet-looked-up entry from the queue whose callsign isn't
    // in activeCallsigns. Intended to be called once per AircraftManager
    // fetch cycle with the set of callsigns currently on screen, so a queue
    // backlog (unavoidable in busy areas given the throttle above) only ever
    // holds lookups for aircraft that are still actually visible, instead of
    // aging out currently-visible aircraft behind ones that left long ago.
    // Already-cached routes are untouched - a plane that reappears with the
    // same callsign still benefits from the earlier lookup.
    void PruneQueueExcept(const std::set<String>& activeCallsigns);

    const RouteInfo* GetRoute(const String& trimmedCallsign) const;
};
