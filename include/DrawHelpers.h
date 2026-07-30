#pragma once

#include "LGFX.h"
#include "ColorUtils.h"

// The default bitmap font has no bold variant, so this fakes one the usual
// lightweight way: draw the string twice, offset by a single pixel, using
// whatever color/size is already set on the sprite. Cheap, keeps the same
// font metrics/spacing as everywhere else (unlike swapping to a bigger
// free-font family), and reads noticeably heavier at this display's size.
inline void DrawBoldString(LGFX_Sprite& sprite, const String& text, int32_t x, int32_t y)
{
    sprite.drawString(text, x, y);
    sprite.drawString(text, x + 1, y);
}

inline void DrawBoldCentreString(LGFX_Sprite& sprite, const String& text, int32_t x, int32_t y)
{
    sprite.drawCentreString(text, x, y);
    sprite.drawCentreString(text, x + 1, y);
}

// Draws a fan-shaped radar sweep highlight from (x0,y0) toward (x1,y1),
// fading from dimColor (right at the sweep line) to brightColor (at the
// trailing edge of the fan), plus one extra full-brightColor line marking
// the sweep's leading edge. dimColor is typically the background color, so
// the sweep fades into whatever's behind it rather than toward black.
inline void DrawScanLines(LGFX_Sprite& buf, const int x0, const int y0, const int x1, const int y1, const int thickness, const int spacing, uint32_t dimColor, uint32_t brightColor)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrt(dx * dx + dy * dy);

    // perpendicular unit vector
    float px = -dy / len;
    float py = dx / len;

    for (int i = 0; i <= thickness; i++) {
        // 1.0 at centre, 0.0 at edges
        float t = i / (float)(thickness);

        buf.drawLine(
            x0, y0,
            x1 + (px * (i * spacing)), y1 + (py * (i * spacing)),
            ColorUtils::BlendToward(brightColor, dimColor, t)
        );
    }

    buf.drawLine(
        x0, y0,
        x1 + (px * (thickness * spacing)), y1 + (py * (thickness * spacing)),
        brightColor
    );
}