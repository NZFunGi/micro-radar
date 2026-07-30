#pragma once

#include <vector>
#include <cstdint>
#include <HTTPClient.h>

#include "ConfigurationWebServer.h"
#include "LGFX.h"

// Supplies coastline geometry for the configured radar location and
// rasterizes it into a coarse land/sea grid for cheap per-frame drawing.
//
// Two data sources, tried in order:
//  1. EmbeddedCoastline.h - a hand-curated land-point cloud baked into flash
//     for one specific location (see that file), used whenever the currently
//     configured lat/lon is close to that baked-in location. Each entry is
//     the center of a known real-world land block; LoadEmbeddedCoastline just
//     projects each point onto the screen grid and marks that cell as land
//     directly - no segment geometry, no heap allocation (the array is
//     flash-resident and iterated in place), no network call, no boot delay.
//  2. A live Overpass fetch, streamed and parsed incrementally rather than
//     buffered whole (see FetchCoastlineWays) - used as a fallback for any
//     other location, reasoning about actual coastline ways/segments since
//     there's no pre-curated point cloud for arbitrary locations. Some
//     locations have coastlines dense enough that the raw response is
//     hundreds of KB to multiple MB, far more than fits in one contiguous
//     allocation alongside WiFi/TLS/webserver overhead on an ESP32-C3, hence
//     the incremental parse rather than a simple getString().
//
// Regenerate EmbeddedCoastline.h if the device is relocated and you want the
// fast/offline path for the new site.
class CoastlineManager
{
private:
    struct Point { int16_t x; int16_t y; };

    // Owns its own HTTPClient (rather than going through HttpRequestManager)
    // because this needs raw stream access for incremental parsing, which
    // HttpRequestManager's whole-body-in-a-String Get/Post interface doesn't
    // expose.
    HTTPClient httpClient;
    ConfigurationWebServer& configServer;

    double lat = 0.0;
    double lon = 0.0;
    double rad = 0.2;

    bool enabled = true;
    bool dataReady = false;

    static constexpr int SCREEN_SIZE = 240;
    // Fine per-pixel-ish grid: the embedded data is now an accurate
    // area-weighted majority classification computed offline for this exact
    // grid (see EmbeddedCoastline.h), not a sparse/noisy point cloud, so a
    // finer resolution is legible rather than scattered.
    static constexpr int GRID_SIZE = 60;               // 60 * CELL_PX == SCREEN_SIZE
    static constexpr int CELL_PX = SCREEN_SIZE / GRID_SIZE;
    static constexpr size_t MAX_COASTLINE_POINTS = 2000;

    std::vector<uint8_t> landGrid; // packed bitset, GRID_SIZE*GRID_SIZE bits

    // Loads a point cloud (see EmbeddedCoastline.h for the format: each entry
    // is the center of one already-decided device-grid land cell) into
    // landGrid, projecting each point with the CURRENT lat/lon/rad. Shared by
    // both LoadEmbeddedCoastline and LoadNvsCoastline/StoreCoastlinePoints so
    // there's one grid-building code path regardless of where the points
    // came from.
    void ApplyPointCloud(const int32_t* latLonMicroPairs, size_t pointCount);

    bool LoadEmbeddedCoastline();

    // Same idea as LoadEmbeddedCoastline, but for a point cloud pushed at
    // runtime by a companion app over serial (see StoreCoastlinePoints) and
    // persisted to NVS - takes priority over the compiled-in
    // EmbeddedCoastline.h data since it's specific to whatever this device is
    // actually configured for right now, not whatever location the firmware
    // happened to be built for.
    bool LoadNvsCoastline();

    // Closes small enclosed water pockets surrounded by land (a real strait or
    // channel spans across a landmass edge-to-edge; a data gap in sparse
    // source data instead shows up as a tiny isolated hole entirely
    // surrounded by land, which reads as a mistake, not a real feature).
    // Repeated majority-neighbor passes fill those small holes while leaving
    // genuinely large water bodies (harbours, the open sea) untouched, since
    // cells deep inside those have few or no land neighbors.
    void FillSmallWaterPockets();

    // Ways are stored flattened - one shared points vector plus a per-way
    // point count - rather than std::vector<std::vector<Point>>. Thousands of
    // separate small heap allocations (one per way) turned out to cost far
    // more in allocator overhead than the actual point data, and crashed with
    // std::bad_alloc on this device's ~90-130KB of free heap even though the
    // raw point data itself would easily have fit in one contiguous block.
    bool FetchCoastlineWays(std::vector<Point>& points, std::vector<uint16_t>& wayLengths);
    bool StreamParseGeometry(Stream& stream, std::vector<Point>& points, std::vector<uint16_t>& wayLengths) const;
    void BuildLandGrid(const std::vector<Point>& points, const std::vector<uint16_t>& wayLengths);
    bool IsLand(int cellX, int cellY) const;
    void SetLand(int cellX, int cellY);

public:
    CoastlineManager(ConfigurationWebServer& config)
        : configServer(config)
    {
    }
    ~CoastlineManager() = default;

    // Runtime point-cloud pushes are capped well above the current 60x60
    // grid's cell count (3600) - plenty of headroom, still small enough to
    // comfortably fit as one NVS blob.
    static constexpr size_t MAX_RUNTIME_COASTLINE_POINTS = 3000;

    // Base "ocean" tone - used both for land-cell contrast here and as the
    // whole-screen clear color in main.cpp, so the display reads as water
    // by default even before/without coastline data. Backed by ColorConfig
    // (NVS), so no longer constexpr - still cheap (in-memory cache lookup).
    static uint32_t SeaColor();
    static uint32_t LandColor();

    void Initialise(LGFX& tft);
    void Draw(LGFX_Sprite& backbuffer) const;

    // Updates the projection center/radius in memory (does NOT persist to
    // NVS - the serial command handler does that itself via the same
    // "config" keys the web config page uses). Needed so a freshly-pushed
    // coastline point cloud for a new location/radius projects correctly
    // without requiring a reboot first.
    void SetLocation(double newLat, double newLon, double newRad);

    // Called by SerialCommandManager once a full point cloud has been
    // received over USB serial. Persists it to NVS (so it survives reboots
    // and is preferred over EmbeddedCoastline.h from then on) and, if it
    // matches the currently active location/radius, applies it immediately
    // so a companion app gets a live preview with no restart needed.
    // Returns false if pointCount is 0 or exceeds MAX_RUNTIME_COASTLINE_POINTS.
    bool StoreCoastlinePoints(const int32_t* latLonMicroPairs, size_t pointCount, double genLat, double genLon, double genRad);
};
