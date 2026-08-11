#include "RouteLookupManager.h"
#include "OpenSkyBudget.h"

#include <ArduinoJson.h>

void RouteLookupManager::Initialise()
{
    const String token = authHandler.GetValidToken(configServer.GetStoredString("opensky-id"), configServer.GetStoredString("opensky-secret"));
    const bool authenticated = !token.isEmpty();

    lookupThrottleMs = OpenSkyBudget::MS_PER_DAY / OpenSkyBudget::RouteLookupBudget(authenticated);
}

void RouteLookupManager::RequestLookup(const String& trimmedCallsign)
{
    if (trimmedCallsign.isEmpty()) return;
    if (cache.find(trimmedCallsign) != cache.end()) return;
    if (queued.find(trimmedCallsign) != queued.end()) return;

    queue.push_back(trimmedCallsign);
    queued.insert(trimmedCallsign);
    Serial.print("[INFO] Queued route lookup for ");
    Serial.println(trimmedCallsign);
}

void RouteLookupManager::Update()
{
    if (queue.empty()) return;

    const unsigned long now = millis();
    if (now - lastLookup < lookupThrottleMs) return;
    lastLookup = now;

    const String callsign = queue.front();
    queue.pop_front();
    queued.erase(callsign);

    const String token = authHandler.GetValidToken(
        configServer.GetStoredString("opensky-id"),
        configServer.GetStoredString("opensky-secret")
    );

    std::vector<std::pair<String, String>> headers = {};
    if (!token.isEmpty()) headers.push_back({ "Authorization", "Bearer " + token });

    HttpResult result = http.Get(
        "https://opensky-network.org/api/routes",
        { {"callsign", callsign} },
        headers
    );

    if (!result.success) {
        Serial.print("[WARN] OpenSky route lookup failed for ");
        Serial.print(callsign);
        Serial.print(": ");
        Serial.println(result.errorMessage);
        // don't cache - AircraftManager will re-request this callsign on a later Update()
        return;
    }

    if (result.statusCode == 404) {
        CacheRoute(callsign, RouteInfo{ true, false, "", "" });
        Serial.print("[INFO] No route on file for ");
        Serial.println(callsign);
        return;
    }

    if (result.statusCode < 200 || result.statusCode >= 300) {
        Serial.print("[WARN] OpenSky route lookup for ");
        Serial.print(callsign);
        Serial.print(" returned unexpected status ");
        Serial.println(result.statusCode);
        // don't cache - a 429/5xx/auth hiccup is transient, not a confirmed
        // "no route on file"; AircraftManager will re-request this callsign
        // on a later Update() same as the network-failure case above
        return;
    }

    const RouteInfo resolved = ParseRouteResponse(result.response);
    CacheRoute(callsign, resolved);

    // Everything else in this file only logs on failure, which makes "is
    // this actually working, just slowly (throttled + reset by reboots)?"
    // impossible to answer from the serial log alone - this line closes
    // that gap.
    Serial.print("[INFO] Route lookup for ");
    Serial.print(callsign);
    if (resolved.found) {
        Serial.print(": "); Serial.print(resolved.origin);
        Serial.print(" -> "); Serial.println(resolved.destination);
    } else {
        Serial.println(": unresolved (bad response)");
    }
}

void RouteLookupManager::PruneQueueExcept(const std::set<String>& activeCallsigns)
{
    for (auto it = queue.begin(); it != queue.end(); ) {
        if (activeCallsigns.find(*it) == activeCallsigns.end()) {
            queued.erase(*it);
            it = queue.erase(it);
        } else {
            ++it;
        }
    }
}

void RouteLookupManager::CacheRoute(const String& callsign, const RouteInfo& info)
{
    // Overwriting an existing entry doesn't need a new cacheOrder entry (and
    // shouldn't get one - that would let a callsign that keeps re-resolving
    // dodge eviction forever) - RequestLookup already guards against
    // re-queueing anything already in `cache`, so this is just defensive.
    if (cache.find(callsign) == cache.end()) {
        cacheOrder.push_back(callsign);
    }
    cache[callsign] = info;

    while (cache.size() > MAX_CACHE_ENTRIES && !cacheOrder.empty()) {
        cache.erase(cacheOrder.front());
        cacheOrder.pop_front();
    }
}

RouteInfo RouteLookupManager::ParseRouteResponse(const String& json) const
{
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok)
        return RouteInfo{ true, false, "", "" };

    JsonArray route = doc["route"];
    if (route.isNull() || route.size() == 0)
        return RouteInfo{ true, false, "", "" };

    RouteInfo info;
    info.resolved = true;
    info.found = true;
    info.origin = route[0].as<String>();
    info.destination = route[route.size() - 1].as<String>();
    return info;
}

const RouteInfo* RouteLookupManager::GetRoute(const String& trimmedCallsign) const
{
    auto it = cache.find(trimmedCallsign);
    if (it == cache.end()) return nullptr;
    return &it->second;
}
