#pragma once

#include "LGFX.h"
#include "ColorUtils.h"
#include "ColorConfig.h"

// Maps OpenSky's ADS-B emitter category field to a display color, plus the
// rest of the app's shared palette (radar chrome, label blending) that lives
// alongside it so there's one place to tune the overall look. Actual color
// values live in ColorConfig (NVS-backed, adjustable at runtime over serial)
// - this just owns the category -> color-slot mapping.
namespace CategoryColors {

    // Most real-world OpenSky feeds report category 0 (no info) for the vast
    // majority of traffic, so CAT_UNKNOWN is the color most aircraft will
    // actually render in.
    inline uint32_t CategoryColor(int category)
    {
        switch (category) {
            case 2: case 3:            return ColorConfig::Get(ColorConfig::CAT_LIGHT);   // light / small
            case 4: case 5:            return ColorConfig::Get(ColorConfig::CAT_LARGE);   // large / high vortex large
            case 6:                    return ColorConfig::Get(ColorConfig::CAT_HEAVY);   // heavy
            case 8:                    return ColorConfig::Get(ColorConfig::CAT_ROTOR);   // rotorcraft
            case 9: case 10: case 12:  return ColorConfig::Get(ColorConfig::CAT_GLIDER);  // glider / lighter-than-air / ultralight
            default:                   return ColorConfig::Get(ColorConfig::CAT_UNKNOWN); // unknown / no info / other
        }
    }

    // Shared "instrument chrome" color for the radar rings and sweep -
    // distinct from every aircraft hue above so aircraft still read as the
    // foreground signal, not part of the background chart.
    inline uint32_t RadarColor() { return ColorConfig::Get(ColorConfig::RADAR); }

    // A light, neutral tone roughly between the coastline's sea and land
    // colors, used as the blend target for label text so it reads as
    // translucent/sunk-into-the-background rather than fully opaque. Fixed
    // rather than independently configurable - it's a derived blend target,
    // not a primary palette choice.
    constexpr uint32_t LABEL_BACKGROUND_TONE = lgfx::color888(190, 205, 190);

}
