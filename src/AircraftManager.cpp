#include "AircraftManager.h"
#include "Projection.h"
#include "OpenSkyBudget.h"
#include "CategoryColors.h"
#include "ColorUtils.h"
#include "CoastlineManager.h"

constexpr int SCREEN_SIZE = 240;
constexpr int SCREEN_SIZE_DIV_2 = (SCREEN_SIZE / 2);

#include <ArduinoJson.h>
#include <set>

void AircraftManager::Initialise()
{
    // get centre point + radius
    lat = configServer.GetStoredString("latitude").toDouble();
    lon = configServer.GetStoredString("longitude").toDouble();
    rad = configServer.GetStoredString("radius").toDouble();

    // configuration
    const String renderText = configServer.GetStoredString("infotext");
    const String renderTris = configServer.GetStoredString("triangle");
    if (!renderText.isEmpty()) displayInfoText = renderText == "true" ? true : false;
    if (!renderTris.isEmpty()) displayTriangles = renderTris == "true" ? true : false;

    // calculate how often we can call OpenSky /states/all before being rate limited,
    // reserving a share of the daily quota for RouteLookupManager's /api/routes calls
    const String token = authHandler.GetValidToken(configServer.GetStoredString("opensky-id"), configServer.GetStoredString("opensky-secret"));
    const bool authenticated = !token.isEmpty();

    fetchInterval = OpenSkyBudget::MS_PER_DAY / OpenSkyBudget::StatesAllBudget(authenticated);
}

void AircraftManager::Update()
{
    unsigned long now = millis();

    // fetch cycle
    if (now - lastFetch >= fetchInterval) {
        lastFetch = now;

        // auth
        const String token = authHandler.GetValidToken(
            configServer.GetStoredString("opensky-id"),
            configServer.GetStoredString("opensky-secret")
        );

        std::vector<std::pair<String, String>> headers = {};
        if (!token.isEmpty()) headers.push_back({ "Authorization", "Bearer " + token });

        // request
        HttpResult result = http.Get(
            "https://opensky-network.org/api/states/all",
            {
              {"lamin", String(lat - rad)},
              {"lamax", String(lat + rad)},
              {"lomin", String(lon - rad)},
              {"lomax", String(lon + rad)}
            },
            headers
        );

        // If request failed, skip this update
        if (!result.success) {
            Serial.print("[WARN] OpenSky API request failed: ");
            Serial.println(result.errorMessage);
            return;
        }

        // track
        JsonDocument doc;
        deserializeJson(doc, result.response);
        auto aircraft = JsonParser::ParseArray<Aircraft>(doc["states"]);
        now = millis(); // override with post-parse timestamp

        // Collected alongside the RequestLookup calls below so we can tell
        // RouteLookupManager which callsigns are actually still in view once
        // the loop's done - see the PruneQueueExcept call below.
        std::set<String> activeCallsigns;

        for (auto& ac : aircraft) {
            auto it = trackedAircraft.find(ac.icao24);
            if (it == trackedAircraft.end())
                trackedAircraft.emplace(ac.icao24, TrackedAircraft{ ac, now });
            else
                it->second.Update(ac, now);

            String trimmedCallsign = ac.callsign;
            trimmedCallsign.trim();
            routeManager.RequestLookup(trimmedCallsign);
            if (!trimmedCallsign.isEmpty()) activeCallsigns.insert(trimmedCallsign);
        }

        // A queued route lookup is only useful if the aircraft it's for is
        // still on screen by the time its turn comes up - the lookup rate is
        // throttled to share OpenSky's daily quota with position polling
        // (see OpenSkyBudget.h), so in a busy area the queue can otherwise
        // fill up with callsigns that have long since left the tracked
        // radius, permanently crowding out aircraft that are actually
        // visible right now. Dropping anything not in this fetch's active
        // set keeps the queue relevant to the current screen instead of
        // whatever happened to be in view whenever it was first queued.
        routeManager.PruneQueueExcept(activeCallsigns);

        // remove any planes that disappeared from the feed
        for (auto it = trackedAircraft.begin(); it != trackedAircraft.end(); ) {
            bool aircraftPresent = std::any_of(aircraft.begin(), aircraft.end(), [&](const Aircraft& ac) { return ac.icao24 == it->first; });
            if (!aircraftPresent)
                it = trackedAircraft.erase(it);
            else
                ++it;
        }
    }
}

void AircraftManager::Draw(LGFX_Sprite& backbuffer)
{
    DrawRadarCircles(backbuffer);

    for (auto& [icao, tracked] : trackedAircraft) {
        if (tracked.state.onGround) continue;

        tracked.Tick();
        auto [predLat, predLon] = tracked.GetDisplayPosition();
        auto [x, y] = ProjectCoordinateToScreen(predLat, predLon);

        if (displayInfoText)
            DrawAircraftInfo(backbuffer, x, y, tracked);

        if (displayTriangles)
            DrawAircraftTriangle(backbuffer, x, y, tracked);
        else
            backbuffer.fillCircle(x, y, 3, CategoryColors::CategoryColor(tracked.state.category));
    }
}

void AircraftManager::DrawRadarCircles(LGFX_Sprite& backbuffer) const
{
    constexpr int CENTRE = SCREEN_SIZE_DIV_2 - 1;
    constexpr int OUTER = SCREEN_SIZE_DIV_2 - 1;

    const uint32_t sea = CoastlineManager::SeaColor();
    const uint32_t radar = CategoryColors::RadarColor();
    backbuffer.drawCircle(CENTRE, CENTRE, OUTER, radar);
    backbuffer.drawCircle(CENTRE, CENTRE, (OUTER / 3) * 2, ColorUtils::BlendToward(radar, sea, 0.55f));
    backbuffer.drawCircle(CENTRE, CENTRE, OUTER / 3, ColorUtils::BlendToward(radar, sea, 0.28f));

    // N/S and E/W crosshair, same dimness as the middle ring so it reads as
    // background structure rather than competing with aircraft/coastline.
    const uint32_t crosshair = ColorUtils::BlendToward(radar, sea, 0.55f);
    backbuffer.drawLine(CENTRE, CENTRE - OUTER, CENTRE, CENTRE + OUTER, crosshair);
    backbuffer.drawLine(CENTRE - OUTER, CENTRE, CENTRE + OUTER, CENTRE, crosshair);
}

std::pair<int, int> AircraftManager::ProjectCoordinateToScreen(float predLat, float predLon) const
{
    return Projection::ToScreen(predLat, predLon, lat, lon, rad, SCREEN_SIZE);
}

void AircraftManager::DrawAircraftInfo(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const
{
    const int lineHeight = tft.fontHeight() + 1;

    String trimmedCallsign = tracked.state.callsign;
    trimmedCallsign.trim();

    const RouteInfo* route = routeManager.GetRoute(trimmedCallsign);
    const bool haveRoute = route != nullptr && route->resolved && route->found;

    backbuffer.setTextSize(1);
    backbuffer.setTextColor(ColorUtils::BlendToward(CategoryColors::CategoryColor(tracked.state.category), CategoryColors::LABEL_BACKGROUND_TONE, 0.72f));
    backbuffer.drawString(trimmedCallsign, x + 5, y + 5);

    // No route data yet (still queued/throttled) or genuinely not found for
    // this callsign - just show the flight number rather than a couple of
    // "O: --" / "D: --" placeholder lines that add clutter without info.
    if (haveRoute) {
        backbuffer.drawString("O: " + route->origin, x + 5, y + 5 + lineHeight);
        backbuffer.drawString("D: " + route->destination, x + 5, y + 5 + lineHeight * 2);
    }
}

void AircraftManager::DrawAircraftTriangle(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const
{
    const float dx = std::sin(radians(tracked.state.trueTrack));
    const float dy = -std::cos(radians(tracked.state.trueTrack));
    const float px = -dy;
    const float py = dx;

    constexpr float TRIANGLE_LENGTH = 6.0f;
    constexpr float TRIANGLE_WIDTH = 3.0f;

    const float tipX = x + dx * TRIANGLE_LENGTH;
    const float tipY = y + dy * TRIANGLE_LENGTH;
    const float leftX = x - dx * TRIANGLE_LENGTH * 0.5f + px * TRIANGLE_WIDTH * 0.5f;
    const float leftY = y - dy * TRIANGLE_LENGTH * 0.5f + py * TRIANGLE_WIDTH * 0.5f;
    const float rightX = x - dx * TRIANGLE_LENGTH * 0.5f - px * TRIANGLE_WIDTH * 0.5f;
    const float rightY = y - dy * TRIANGLE_LENGTH * 0.5f - py * TRIANGLE_WIDTH * 0.5f;

    backbuffer.fillTriangle(tipX, tipY, leftX, leftY, rightX, rightY, CategoryColors::CategoryColor(tracked.state.category));
}