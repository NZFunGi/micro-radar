#pragma once

#include "LGFX.h"

// Generic color-blending helpers shared by the aircraft/label palette
// (CategoryColors.h) and any drawing helper that needs to fade a color
// toward another (e.g. a radar sweep fading into the background).
namespace ColorUtils {

    // alpha=1 -> color, alpha=0 -> target. Blends toward `target` rather than
    // toward black so things fade into whatever's actually behind them
    // (sea, land, ...) instead of just getting darker.
    inline uint32_t BlendToward(uint32_t color, uint32_t target, float alpha)
    {
        auto lerp = [&](int shift) {
            const uint8_t from = (color >> shift) & 0xFF;
            const uint8_t to = (target >> shift) & 0xFF;
            return static_cast<uint8_t>(from * alpha + to * (1.0f - alpha));
        };
        return lgfx::color888(lerp(16), lerp(8), lerp(0));
    }

}
