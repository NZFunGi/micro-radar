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
    std::deque<String> queue;
    std::set<String> queued;

    unsigned long lookupThrottleMs = 0;
    unsigned long lastLookup = 0;

    RouteInfo ParseRouteResponse(const String& json) const;

public:
    RouteLookupManager(ConfigurationWebServer& config, OpenSkyAuthTokenHandler& auth, HttpRequestManager& httpManager)
        : http(httpManager), authHandler(auth), configServer(config)
    {
    }
    ~RouteLookupManager() = default;

    void Initialise();
    void Update();
    void RequestLookup(const String& trimmedCallsign);
    const RouteInfo* GetRoute(const String& trimmedCallsign) const;
};
