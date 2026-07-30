#include "CoastlineManager.h"
#include "Projection.h"
#include "EmbeddedCoastline.h"
#include "ColorConfig.h"

#include <algorithm>
#include <Preferences.h>

namespace {

    // Squared distance from point (px,py) to the segment (x1,y1)-(x2,y2), plus
    // the 2D cross product of the segment direction against the vector from
    // (x1,y1) to (px,py) (used to tell which side of the segment the point
    // falls on). All-integer arithmetic (int64 to keep every intermediate
    // product safely in range): the ESP32-C3 has no hardware FPU, so every
    // float multiply/add in this function would otherwise run through slow
    // software emulation - across 3600 grid cells x every segment, that was
    // the difference between finishing in about a second and taking minutes.
    // Integer multiply/divide are native (RISC-V "M" extension), so this is
    // the same clamped point-on-segment projection, just without float ops.
    int64_t PointSegmentDistSq(int32_t px, int32_t py, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int64_t& crossOut)
    {
        const int32_t dx = x2 - x1;
        const int32_t dy = y2 - y1;
        const int64_t lenSq = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;

        int64_t closestX, closestY;
        if (lenSq == 0) {
            closestX = x1;
            closestY = y1;
        }
        else {
            const int64_t num = static_cast<int64_t>(px - x1) * dx + static_cast<int64_t>(py - y1) * dy;
            if (num <= 0) {
                closestX = x1;
                closestY = y1;
            }
            else if (num >= lenSq) {
                closestX = x2;
                closestY = y2;
            }
            else {
                closestX = x1 + (num * dx) / lenSq;
                closestY = y1 + (num * dy) / lenSq;
            }
        }

        const int64_t ddx = px - closestX;
        const int64_t ddy = py - closestY;

        crossOut = static_cast<int64_t>(dx) * (py - y1) - static_cast<int64_t>(dy) * (px - x1);
        return ddx * ddx + ddy * ddy;
    }

    // Un-chunks an HTTP/1.1 chunked-transfer body on the fly, exposing a plain
    // byte stream via read(). Overpass always sends chunked responses (no
    // Content-Length), and the raw stream from HTTPClient::getStreamPtr() is
    // NOT de-chunked - without this, chunk-size headers ("1a3f\r\n...") would
    // land in the middle of the JSON. Passes straight through untouched when
    // the response isn't chunked.
    class DechunkingStream : public Stream {
    public:
        DechunkingStream(Stream& raw, bool chunked) : raw(raw), chunked(chunked) {}

        int available() override
        {
            if (!chunked) return raw.available();
            return (remainingInChunk > 0 || raw.available()) && !finished ? 1 : 0;
        }

        int read() override
        {
            if (!chunked) return raw.read();
            if (finished) return -1;
            if (remainingInChunk == 0 && !AdvanceToNextChunk()) return -1;

            const int b = raw.read();
            if (b >= 0) remainingInChunk--;
            return b;
        }

        // Not used by StreamParseGeometry (which only calls read()), but required
        // to satisfy the Stream/Print interface.
        int peek() override { return chunked ? -1 : raw.peek(); }
        size_t write(uint8_t) override { return 0; }

    private:
        Stream& raw;
        bool chunked;
        long remainingInChunk = 0;
        bool finished = false;
        bool firstChunk = true;

        bool AdvanceToNextChunk()
        {
            if (!firstChunk) {
                // consume the trailing CRLF left over from the previous chunk's data
                raw.read();
                raw.read();
            }
            firstChunk = false;

            String sizeLine = raw.readStringUntil('\n');
            sizeLine.trim();
            if (sizeLine.length() == 0) { finished = true; return false; }

            const long size = strtol(sizeLine.c_str(), nullptr, 16);
            if (size <= 0) { finished = true; return false; } // terminal 0-length chunk

            remainingInChunk = size;
            return true;
        }
    };

}

void CoastlineManager::Initialise(LGFX& tft)
{
    lat = configServer.GetStoredString("latitude").toDouble();
    lon = configServer.GetStoredString("longitude").toDouble();
    rad = configServer.GetStoredString("radius").toDouble();

    const String coastlineEnabled = configServer.GetStoredString("coastline");
    if (!coastlineEnabled.isEmpty()) enabled = coastlineEnabled == "true";

    if (!enabled) return;

    if (LoadNvsCoastline()) {
        Serial.println("[INFO] Using coastline data pushed from companion app (no network fetch needed).");
        dataReady = true;
        return;
    }

    if (LoadEmbeddedCoastline()) {
        Serial.println("[INFO] Using embedded coastline data (no network fetch needed).");
        dataReady = true;
        return;
    }

    tft.fillScreen(lgfx::color888(0, 0, 0));
    tft.setTextColor(lgfx::color888(0, 255, 0));
    tft.drawCentreString("Loading coastline...", SCREEN_SIZE / 2, SCREEN_SIZE / 2);

    std::vector<Point> points;
    std::vector<uint16_t> wayLengths;
    if (!FetchCoastlineWays(points, wayLengths)) {
        Serial.println("[WARN] No coastline data available this boot - rendering sea-only.");
        return;
    }

    BuildLandGrid(points, wayLengths);
    dataReady = true;
}

void CoastlineManager::ApplyPointCloud(const int32_t* latLonMicroPairs, size_t pointCount)
{
    // This is a plain point cloud (each entry is the center of one already-
    // decided device-grid land cell), not coastline ways - so there's no
    // segment geometry to build here. Just project each point onto the
    // screen grid and mark that cell as land directly.
    landGrid.assign((GRID_SIZE * GRID_SIZE + 7) / 8, 0);

    for (size_t i = 0; i < pointCount; i++) {
        const double pointLat = latLonMicroPairs[i * 2] / 1e6;
        const double pointLon = latLonMicroPairs[i * 2 + 1] / 1e6;

        const auto [x, y] = Projection::ToScreen(pointLat, pointLon, lat, lon, rad, SCREEN_SIZE);
        const int cx = x / CELL_PX;
        const int cy = y / CELL_PX;
        if (cx >= 0 && cx < GRID_SIZE && cy >= 0 && cy < GRID_SIZE)
            SetLand(cx, cy);
    }

    FillSmallWaterPockets();
}

bool CoastlineManager::LoadEmbeddedCoastline()
{
    // Only use the embedded data if we're still configured for (roughly) the
    // same location AND radius it was baked for - otherwise fall through to
    // NVS/live fetch. Each entry here is already a per-device-cell majority
    // land/sea decision computed for one specific radius (see
    // EmbeddedCoastline.h) - a meaningfully different radius would reproject
    // these points into the wrong cells rather than just being
    // coarser/finer, so it's not safe to reuse loosely.
    constexpr double LOCATION_TOLERANCE_DEG = 0.05; // ~5km
    constexpr double RADIUS_TOLERANCE_DEG = 0.1;
    if (std::abs(lat - EMBEDDED_COASTLINE_LAT) > LOCATION_TOLERANCE_DEG ||
        std::abs(lon - EMBEDDED_COASTLINE_LON) > LOCATION_TOLERANCE_DEG ||
        std::abs(rad - EMBEDDED_COASTLINE_RADIUS_USED) > RADIUS_TOLERANCE_DEG) {
        return false;
    }

    // EMBEDDED_LAND_POINTS is flash-resident (static const) and iterated in
    // place here - no heap allocation for the source data itself.
    ApplyPointCloud(EMBEDDED_LAND_POINTS, EMBEDDED_LAND_POINT_COUNT);
    return true;
}

bool CoastlineManager::LoadNvsCoastline()
{
    Preferences prefs;
    prefs.begin("coastdata", true);
    const uint32_t count = prefs.getUInt("count", 0);
    if (count == 0) {
        prefs.end();
        return false;
    }

    const double genLat = prefs.getDouble("genLat", 0.0);
    const double genLon = prefs.getDouble("genLon", 0.0);
    const double genRad = prefs.getDouble("genRad", 0.0);

    // Same reasoning as LoadEmbeddedCoastline: only reuse this data if it was
    // generated for (roughly) the location/radius we're configured for now.
    constexpr double LOCATION_TOLERANCE_DEG = 0.05;
    constexpr double RADIUS_TOLERANCE_DEG = 0.1;
    if (std::abs(lat - genLat) > LOCATION_TOLERANCE_DEG ||
        std::abs(lon - genLon) > LOCATION_TOLERANCE_DEG ||
        std::abs(rad - genRad) > RADIUS_TOLERANCE_DEG) {
        prefs.end();
        return false;
    }

    std::vector<int32_t> raw(static_cast<size_t>(count) * 2);
    const size_t expectedBytes = raw.size() * sizeof(int32_t);
    const size_t got = prefs.getBytes("points", raw.data(), expectedBytes);
    prefs.end();
    if (got != expectedBytes) return false;

    ApplyPointCloud(raw.data(), count);
    return true;
}

void CoastlineManager::SetLocation(double newLat, double newLon, double newRad)
{
    lat = newLat;
    lon = newLon;
    rad = newRad;
}

bool CoastlineManager::StoreCoastlinePoints(const int32_t* latLonMicroPairs, size_t pointCount, double genLat, double genLon, double genRad)
{
    if (pointCount == 0 || pointCount > MAX_RUNTIME_COASTLINE_POINTS) return false;

    Preferences prefs;
    prefs.begin("coastdata", false);
    // A previous push can leave a differently-sized "points" blob behind -
    // clearing first means there's never a stale blob whose size disagrees
    // with the freshly-written "count", which is exactly the kind of
    // mismatch that made nvs_get_blob fail outright on a later boot rather
    // than just reading old data.
    prefs.clear();
    prefs.putUInt("count", static_cast<uint32_t>(pointCount));
    prefs.putDouble("genLat", genLat);
    prefs.putDouble("genLon", genLon);
    prefs.putDouble("genRad", genRad);
    prefs.putBytes("points", latLonMicroPairs, pointCount * 2 * sizeof(int32_t));
    prefs.end();

    // If this matches where we're currently pointed, apply it right away so
    // a companion app gets a live preview without needing a reboot first.
    constexpr double LOCATION_TOLERANCE_DEG = 0.05;
    constexpr double RADIUS_TOLERANCE_DEG = 0.1;
    if (std::abs(lat - genLat) <= LOCATION_TOLERANCE_DEG &&
        std::abs(lon - genLon) <= LOCATION_TOLERANCE_DEG &&
        std::abs(rad - genRad) <= RADIUS_TOLERANCE_DEG) {
        ApplyPointCloud(latLonMicroPairs, pointCount);
        enabled = true;
        dataReady = true;
    }

    return true;
}

void CoastlineManager::FillSmallWaterPockets()
{
    constexpr int PASSES = 2;
    constexpr float LAND_NEIGHBOR_FILL_RATIO = 0.7f;

    for (int pass = 0; pass < PASSES; pass++) {
        std::vector<uint8_t> next = landGrid;

        for (int cy = 0; cy < GRID_SIZE; cy++) {
            for (int cx = 0; cx < GRID_SIZE; cx++) {
                if (IsLand(cx, cy)) continue;

                int landNeighbors = 0, totalNeighbors = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        const int nx = cx + dx, ny = cy + dy;
                        if (nx < 0 || nx >= GRID_SIZE || ny < 0 || ny >= GRID_SIZE) continue;
                        totalNeighbors++;
                        if (IsLand(nx, ny)) landNeighbors++;
                    }
                }

                if (totalNeighbors > 0 && landNeighbors >= totalNeighbors * LAND_NEIGHBOR_FILL_RATIO) {
                    const int idx = cy * GRID_SIZE + cx;
                    next[idx / 8] |= static_cast<uint8_t>(1 << (idx % 8));
                }
            }
        }

        landGrid = std::move(next);
    }
}

bool CoastlineManager::FetchCoastlineWays(std::vector<Point>& points, std::vector<uint16_t>& wayLengths)
{
    // Cap the query area independent of the full aircraft radar radius. This is
    // no longer about our own memory (StreamParseGeometry handles arbitrarily
    // large responses), it's about keeping Overpass's own server-side query
    // time reasonable - a dense coastline over a large area can be expensive
    // enough for Overpass to generate that its own infrastructure times out
    // (observed: HTTP 504 at the full 1.5 degree radius here), independent of
    // anything on our end.
    constexpr double QUERY_RADIUS_CAP = 0.1; // degrees, ~11km - confirmed reliable directly against Overpass
    const double queryRad = std::min(rad, QUERY_RADIUS_CAP);

    const double south = lat - queryRad, west = lon - queryRad, north = lat + queryRad, east = lon + queryRad;
    const String bbox = String(south, 6) + "," + String(west, 6) + "," + String(north, 6) + "," + String(east, 6);

    const String query = "[out:json][timeout:25];way[\"natural\"=\"coastline\"](" + bbox + ");out geom(" + bbox + ");";

    // Streamed and parsed incrementally (see StreamParseGeometry), so unlike
    // the old getString()-based approach, peak memory here doesn't scale with
    // response size - a dense coastline returning hundreds of KB costs no
    // more RAM than a sparse one. A couple of retries remain as a defensive
    // measure against ordinary transient network/TLS hiccups, not as a
    // workaround for a memory ceiling.
    constexpr int MAX_ATTEMPTS = 2;

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        httpClient.begin("https://overpass-api.de/api/interpreter");
        httpClient.addHeader("Content-Type", "text/plain");
        httpClient.setTimeout(35000); // must comfortably exceed the query's own [timeout:25]

        const int statusCode = httpClient.POST(query);

        if (statusCode != 200) {
            Serial.print("[WARN] Overpass coastline request failed (attempt ");
            Serial.print(attempt);
            Serial.print("/");
            Serial.print(MAX_ATTEMPTS);
            Serial.print("): HTTP ");
            Serial.println(statusCode);
            httpClient.end();
            continue;
        }

        const bool chunked = httpClient.header("Transfer-Encoding").equalsIgnoreCase("chunked");
        DechunkingStream stream(*httpClient.getStreamPtr(), chunked);

        points.clear();
        wayLengths.clear();
        points.reserve(MAX_COASTLINE_POINTS); // one allocation, capped up front
        const bool ok = StreamParseGeometry(stream, points, wayLengths);
        httpClient.end();

        if (ok) return true;

        Serial.print("[WARN] Overpass coastline response parse failed (attempt ");
        Serial.print(attempt);
        Serial.print("/");
        Serial.print(MAX_ATTEMPTS);
        Serial.println(")");
    }

    Serial.println("[WARN] Overpass coastline fetch failed after all retries.");
    return false;
}

// Minimal streaming scanner tailored to Overpass's `out geom` JSON shape:
//   { ..., "elements": [ { ..., "geometry": [ {"lat":n,"lon":n}, null, ... ] }, ... ] }
// Extracts lat/lon pairs as they arrive rather than building any in-memory
// document - peak memory is the shared points vector (reserved up front,
// capped at MAX_COASTLINE_POINTS) plus a couple of small string buffers,
// regardless of how large the response is. Other fields (ids, node lists,
// bounds, tags) are scanned over and discarded without ever being stored.
bool CoastlineManager::StreamParseGeometry(Stream& stream, std::vector<Point>& points, std::vector<uint16_t>& wayLengths) const
{
    int depth = 0;
    bool inString = false, escapeNext = false;
    String strBuf, numBuf, lastKey;
    bool inNumber = false;

    bool inElements = false;
    int elementsDepth = -1;

    bool inGeometry = false;
    int geometryDepth = -1;
    size_t wayStartIndex = 0; // index into `points` where the current way began

    bool inNode = false;
    int nodeDepth = -1;
    bool haveLat = false, haveLon = false;
    double curLat = 0, curLon = 0;

    bool sawElements = false;

    auto finalizeNumber = [&]() {
        if (!inNumber) return;
        inNumber = false;
        if (inNode && (lastKey == "lat" || lastKey == "lon")) {
            const double v = strtod(numBuf.c_str(), nullptr);
            if (lastKey == "lat") { curLat = v; haveLat = true; }
            else { curLon = v; haveLon = true; }
        }
        numBuf = "";
    };

    while (points.size() < MAX_COASTLINE_POINTS) {
        const int c = stream.read();
        if (c < 0) break; // connection ended / stalled past the client timeout
        const char ch = static_cast<char>(c);

        if (inString) {
            if (escapeNext) { strBuf += ch; escapeNext = false; }
            else if (ch == '\\') escapeNext = true;
            else if (ch == '"') { inString = false; lastKey = strBuf; strBuf = ""; }
            else strBuf += ch;
            continue;
        }

        const bool numberChar = isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E';
        if (inNumber && !numberChar) {
            finalizeNumber();
            // fall through - ch itself (e.g. ',', '}', ']') still needs handling below
        }
        else if (numberChar) {
            inNumber = true;
            numBuf += ch;
            continue;
        }

        switch (ch) {
            case '"':
                inString = true;
                strBuf = "";
                break;

            case '{':
                depth++;
                if (inGeometry && !inNode && depth == geometryDepth + 1) {
                    inNode = true;
                    nodeDepth = depth;
                    haveLat = haveLon = false;
                }
                break;

            case '}':
                if (inNode && depth == nodeDepth) {
                    if (haveLat && haveLon && points.size() < MAX_COASTLINE_POINTS) {
                        const auto [x, y] = Projection::ToScreen(curLat, curLon, lat, lon, rad, SCREEN_SIZE);
                        points.push_back({ static_cast<int16_t>(x), static_cast<int16_t>(y) });
                    }
                    inNode = false;
                }
                depth--;
                break;

            case '[':
                depth++;
                if (!inGeometry && lastKey == "geometry") {
                    inGeometry = true;
                    geometryDepth = depth;
                    wayStartIndex = points.size();
                }
                else if (!inElements && lastKey == "elements") {
                    inElements = true;
                    sawElements = true;
                    elementsDepth = depth;
                }
                break;

            case ']':
                if (inGeometry && depth == geometryDepth) {
                    inGeometry = false;
                    const size_t wayPointCount = points.size() - wayStartIndex;
                    if (wayPointCount >= 2) wayLengths.push_back(static_cast<uint16_t>(wayPointCount));
                    else points.resize(wayStartIndex); // too short to be useful - drop its stray point(s)
                }
                if (inElements && depth == elementsDepth) {
                    inElements = false;
                    depth--;
                    return true; // elements array fully closed - a well-formed response, even if empty
                }
                depth--;
                break;

            default:
                break; // whitespace, ':', ',' - no-op
        }
    }

    // Stopped early because we hit MAX_COASTLINE_POINTS mid-stream, not because
    // the document ended - still a legitimate success (we deliberately capped
    // it), not a truncation. Only a genuine disconnect before "elements" even
    // opened counts as a real failure.
    return points.size() >= MAX_COASTLINE_POINTS || sawElements;
}

void CoastlineManager::BuildLandGrid(const std::vector<Point>& points, const std::vector<uint16_t>& wayLengths)
{
    landGrid.assign((GRID_SIZE * GRID_SIZE + 7) / 8, 0);

    for (int cy = 0; cy < GRID_SIZE; cy++) {
        for (int cx = 0; cx < GRID_SIZE; cx++) {
            const int32_t px = cx * CELL_PX + CELL_PX / 2;
            const int32_t py = cy * CELL_PX + CELL_PX / 2;

            bool found = false;
            int64_t bestDistSq = 0;
            int64_t bestCross = 0;

            size_t offset = 0;
            for (const uint16_t wayLen : wayLengths) {
                for (size_t i = 0; i + 1 < wayLen; i++) {
                    const Point& a = points[offset + i];
                    const Point& b = points[offset + i + 1];
                    int64_t cross;
                    const int64_t distSq = PointSegmentDistSq(px, py, a.x, a.y, b.x, b.y, cross);
                    if (!found || distSq < bestDistSq) {
                        bestDistSq = distSq;
                        bestCross = cross;
                        found = true;
                    }
                }
                offset += wayLen;
            }

            if (!found) continue; // no coastline segments at all - treat as sea

            // If the nearest segment is still far away (sparse data - a big gap
            // between two points on the same way, or between unrelated ways),
            // its direction says nothing reliable about this cell - extrapolating
            // land/sea across a large empty gap produced systematically wrong
            // results (whole quadrants misclassified) when verified against
            // ground truth. Only trust segments that are actually nearby;
            // anything farther defaults to sea, same as "no data at all".
            constexpr int64_t MAX_SEGMENT_DIST = 30; // screen pixels - wider now that cells themselves are 12px
            constexpr int64_t MAX_SEGMENT_DIST_SQ = MAX_SEGMENT_DIST * MAX_SEGMENT_DIST;
            if (bestDistSq > MAX_SEGMENT_DIST_SQ) continue;

            // OSM coastline convention: land is on the left of the way's node order,
            // sea on the right, in standard (east=+x, north=+y) geographic orientation.
            // Our screen projection flips the Y axis to put north at the top of the
            // screen, which mirrors chirality - so the sign check here is inverted
            // relative to the naive formula. If land/sea ever render swapped for a
            // real location, flip this comparison (< to >).
            if (bestCross < 0)
                SetLand(cx, cy);
        }
    }
}

bool CoastlineManager::IsLand(int cellX, int cellY) const
{
    const int idx = cellY * GRID_SIZE + cellX;
    return (landGrid[idx / 8] >> (idx % 8)) & 1;
}

void CoastlineManager::SetLand(int cellX, int cellY)
{
    const int idx = cellY * GRID_SIZE + cellX;
    landGrid[idx / 8] |= static_cast<uint8_t>(1 << (idx % 8));
}

uint32_t CoastlineManager::SeaColor() { return ColorConfig::Get(ColorConfig::SEA); }
uint32_t CoastlineManager::LandColor() { return ColorConfig::Get(ColorConfig::LAND); }

void CoastlineManager::Draw(LGFX_Sprite& backbuffer) const
{
    if (!enabled || !dataReady) return;

    const uint32_t landColor = LandColor();
    for (int cy = 0; cy < GRID_SIZE; cy++) {
        for (int cx = 0; cx < GRID_SIZE; cx++) {
            if (IsLand(cx, cy))
                backbuffer.fillRect(cx * CELL_PX, cy * CELL_PX, CELL_PX, CELL_PX, landColor);
        }
    }
}
