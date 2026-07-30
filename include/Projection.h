#pragma once

#include <utility>

// Shared lat/lon -> screen pixel transform used by every layer that draws
// onto the radar (aircraft, coastline, ...) so they stay aligned.
// Fixed north-up equirectangular-ish projection - no rotation is ever applied,
// so increasing latitude always moves toward the top of the screen.
namespace Projection {

    inline std::pair<int, int> ToScreen(double lat, double lon, double centreLat, double centreLon, double radiusDeg, int screenSize)
    {
        const double dLon = lon - centreLon;
        const double dLat = lat - centreLat;

        const double normLon = (dLon + radiusDeg) / (2.0 * radiusDeg);
        const double normLat = (dLat + radiusDeg) / (2.0 * radiusDeg);

        const int x = static_cast<int>(normLon * screenSize);
        const int y = static_cast<int>(screenSize - (normLat * screenSize));

        return { x, y };
    }

}
