#include "RouteLookupManager.h"
#include "OpenSkyBudget.h"
#include "EmbeddedAirportCodes.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cstring>

namespace {
    // hexdb.io's own documented limit is 1000 requests/5min - 400ms would be
    // comfortably under that alone, but a sibling project (touch-radar)
    // confirmed on real hardware that a 400ms-spaced burst of 30+ queued
    // lookups right after boot (a busy traffic period queues one per
    // newly-seen aircraft) was sustained TLS connect/handshake/teardown
    // churn frequent enough to fragment internal RAM and starve an
    // unrelated feature's own TLS handshake ("SSL - Memory allocation
    // failed", every single attempt, across 180s of logging). Widened to
    // give the heap allocator room to coalesce free blocks between
    // requests; costs a slower initial fill of route info after boot, not
    // a functional loss.
    constexpr unsigned long HEXDB_THROTTLE_MS = 2000;

    // Bounds-safe copy into a fixed-size buffer - always null-terminates,
    // silently truncates anything that doesn't fit (only ever expected for
    // malformed/unexpected upstream data, since callsigns and airport codes
    // are documented short - see RouteLookupManager.h).
    void CopyToFixed(char* dest, size_t destSize, const String& src)
    {
        const size_t n = std::min(src.length(), destSize - 1);
        memcpy(dest, src.c_str(), n);
        dest[n] = '\0';
    }
}

void RouteLookupManager::Initialise()
{
    const String token = authHandler.GetValidToken(configServer.GetStoredString("opensky-id"), configServer.GetStoredString("opensky-secret"));
    const bool authenticated = !token.isEmpty();

    openSkyThrottleMs = OpenSkyBudget::MS_PER_DAY / OpenSkyBudget::RouteLookupBudget(authenticated);
    hexdbThrottleMs = HEXDB_THROTTLE_MS;
}

void RouteLookupManager::RequestLookup(const String& trimmedCallsign)
{
    if (trimmedCallsign.isEmpty()) return;
    // Callsigns are documented as up to 8 chars (OpenSky's states/all spec) -
    // cache.callsign only has room for that; silently ignore anything
    // longer rather than risk a malformed/unexpected upstream value
    // overflowing a fixed buffer.
    if (trimmedCallsign.length() > 8) return;
    if (FindCacheEntry(trimmedCallsign) != nullptr) return;
    if (queued.find(trimmedCallsign) != queued.end()) return;
    // Already past its hexdb.io attempt, waiting on the OpenSky fallback -
    // don't re-queue it into the fresh (hexdb.io-first) queue too.
    if (openSkyFallbackQueued.find(trimmedCallsign) != openSkyFallbackQueued.end()) return;

    queue.push_back(trimmedCallsign);
    queued.insert(trimmedCallsign);
    Serial.print("[INFO] Queued route lookup for ");
    Serial.println(trimmedCallsign);
}

void RouteLookupManager::Update()
{
    const unsigned long now = millis();

    // At most one HTTP request per Update() call - prefers draining the
    // hexdb.io queue (cheap, doesn't touch OpenSky's budget) over the
    // OpenSky fallback queue when both have a ready throttle slot.
    if (!queue.empty() && (now - lastHexdbLookup >= hexdbThrottleMs)) {
        lastHexdbLookup = now;
        const String callsign = queue.front();
        queue.pop_front();
        queued.erase(callsign);
        AttemptHexdb(callsign);
        return;
    }

    if (!openSkyFallbackQueue.empty() && (now - lastOpenSkyLookup >= openSkyThrottleMs)) {
        lastOpenSkyLookup = now;
        const String callsign = openSkyFallbackQueue.front();
        openSkyFallbackQueue.pop_front();
        openSkyFallbackQueued.erase(callsign);
        AttemptOpenSky(callsign);
    }
}

void RouteLookupManager::AttemptHexdb(const String& callsign)
{
    // IATA endpoint specifically - hexdb.io returns IATA codes natively
    // here, no conversion needed (unlike the OpenSky fallback below, which
    // only ever has ICAO available).
    HttpResult result = http.Get("https://hexdb.io/callsign-route-iata", { {"callsign", callsign} }, {});

    if (!result.success) {
        Serial.print("[WARN] hexdb.io route lookup failed for ");
        Serial.print(callsign);
        Serial.print(": ");
        Serial.println(result.errorMessage);
        // Transient - don't cache, don't fall back. AircraftManager will
        // re-request this callsign on a later Update() if it's still
        // visible, and RequestLookup() will put it straight back in the
        // hexdb.io queue (it never made it into the OpenSky one).
        return;
    }

    if (result.statusCode == 200) {
        const RouteInfo info = ParseHexdbRouteResponse(result.response);
        if (info.found) {
            CacheRoute(callsign, info);
            LogRouteResult("hexdb.io", callsign, info);
            return;
        }
    }

    // Definitive miss (404, or a 200 with no usable route) - hexdb.io is
    // known to miss roughly half of real routes (see RouteLookupManager.h),
    // so this is the expected common case, not a bug. Fall back to OpenSky
    // rather than caching "not found" outright.
    if (openSkyFallbackQueued.find(callsign) == openSkyFallbackQueued.end()) {
        openSkyFallbackQueue.push_back(callsign);
        openSkyFallbackQueued.insert(callsign);
    }
}

void RouteLookupManager::AttemptOpenSky(const String& callsign)
{
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
        // don't cache - AircraftManager will re-request this callsign on a
        // later Update(), which (since it's not in either queue/queued set
        // any more) goes straight back through the hexdb.io attempt first,
        // same as any other fresh request.
        return;
    }

    if (result.statusCode == 404) {
        CacheRoute(callsign, RouteInfo{ true, false, {0}, {0} });
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
        // "no route on file"
        return;
    }

    const RouteInfo info = ParseOpenSkyRouteResponse(result.response);
    CacheRoute(callsign, info);
    LogRouteResult("OpenSky", callsign, info);
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

    for (auto it = openSkyFallbackQueue.begin(); it != openSkyFallbackQueue.end(); ) {
        if (activeCallsigns.find(*it) == activeCallsigns.end()) {
            openSkyFallbackQueued.erase(*it);
            it = openSkyFallbackQueue.erase(it);
        } else {
            ++it;
        }
    }
}

RouteLookupManager::CacheEntry* RouteLookupManager::FindCacheEntry(const String& callsign)
{
    for (auto& entry : cache) {
        if (entry.used && strcmp(callsign.c_str(), entry.callsign) == 0) return &entry;
    }
    return nullptr;
}

const RouteLookupManager::CacheEntry* RouteLookupManager::FindCacheEntry(const String& callsign) const
{
    for (const auto& entry : cache) {
        if (entry.used && strcmp(callsign.c_str(), entry.callsign) == 0) return &entry;
    }
    return nullptr;
}

void RouteLookupManager::CacheRoute(const String& callsign, const RouteInfo& info)
{
    if (CacheEntry* existing = FindCacheEntry(callsign)) {
        existing->info = info;
        return;
    }

    // New callsign - claim the next round-robin slot (silently overwriting
    // whatever was cached there before, if the cache is already full - see
    // the comment on `nextCacheSlot`).
    CacheEntry& slot = cache[nextCacheSlot];
    CopyToFixed(slot.callsign, sizeof(slot.callsign), callsign);
    slot.used = true;
    slot.info = info;
    nextCacheSlot = (nextCacheSlot + 1) % MAX_CACHE_ENTRIES;
}

// hexdb.io's response here is plain text (e.g. "DUB-LHR"), not JSON -
// confirmed by actually calling the endpoint directly, not from its own
// docs, which show a JSON-wrapped example that doesn't match what it
// actually returns for this particular endpoint. Multi-leg routes come
// back the same "-"-joined way OpenSky's array does, so the same
// first/last convention applies.
RouteInfo RouteLookupManager::ParseHexdbRouteResponse(const String& text) const
{
    String route = text;
    route.trim();

    const int firstDash = route.indexOf('-');
    const int lastDash = route.lastIndexOf('-');
    if (route.isEmpty() || firstDash < 0)
        return RouteInfo{ true, false, {0}, {0} };

    RouteInfo info;
    info.resolved = true;
    info.found = true;
    CopyToFixed(info.origin, sizeof(info.origin), route.substring(0, firstDash));
    CopyToFixed(info.destination, sizeof(info.destination), route.substring(lastDash + 1));
    return info;
}

// OpenSky's /api/routes returns ICAO codes only (confirmed against its
// docs) - converted to IATA here via the embedded lookup table so a route
// shown to the user reads the same way regardless of which source actually
// resolved it. Falls back to the raw ICAO code if this specific airport
// isn't in that table (a real possibility - the table only covers airports
// that have both an ICAO and IATA code assigned in the first place).
RouteInfo RouteLookupManager::ParseOpenSkyRouteResponse(const String& json) const
{
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok)
        return RouteInfo{ true, false, {0}, {0} };

    JsonArray route = doc["route"];
    if (route.isNull() || route.size() == 0)
        return RouteInfo{ true, false, {0}, {0} };

    auto toIata = [](const String& icao) -> String {
        if (icao.length() != 4) return icao;
        const char* iata = EmbeddedAirportLookupIata(icao.c_str());
        return iata ? String(iata) : icao;
    };

    RouteInfo info;
    info.resolved = true;
    info.found = true;
    CopyToFixed(info.origin, sizeof(info.origin), toIata(route[0].as<String>()));
    CopyToFixed(info.destination, sizeof(info.destination), toIata(route[route.size() - 1].as<String>()));
    return info;
}

void RouteLookupManager::LogRouteResult(const char* source, const String& callsign, const RouteInfo& info) const
{
    Serial.print("[INFO] Route lookup for ");
    Serial.print(callsign);
    Serial.print(" (");
    Serial.print(source);
    Serial.print(")");
    if (info.found) {
        Serial.print(": "); Serial.print(info.origin);
        Serial.print(" -> "); Serial.println(info.destination);
    } else {
        Serial.println(": unresolved (bad response)");
    }
}

const RouteInfo* RouteLookupManager::GetRoute(const String& trimmedCallsign) const
{
    const CacheEntry* entry = FindCacheEntry(trimmedCallsign);
    return entry ? &entry->info : nullptr;
}
