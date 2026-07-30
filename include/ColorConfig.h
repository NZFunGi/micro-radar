#pragma once

#include <Arduino.h>
#include <cstdint>

// Runtime-adjustable color palette (sea/land/radar/aircraft categories),
// backed by NVS (Preferences) so a companion desktop app can push new colors
// over USB serial (see SerialCommandManager) without a firmware rebuild, and
// they survive reboots. Everything else in the app should read colors
// through here rather than hardcoding them.
namespace ColorConfig {

    enum Key : uint8_t {
        SEA = 0,
        LAND,
        RADAR,
        CAT_LIGHT,
        CAT_LARGE,
        CAT_HEAVY,
        CAT_ROTOR,
        CAT_GLIDER,
        CAT_UNKNOWN,
        RANGE_LABEL,
        COUNT
    };

    // Loads all colors from NVS, falling back to the built-in defaults for
    // any key that's never been explicitly set. Call once from setup(),
    // before anything reads a color.
    void Initialise();

    // Cheap in-memory lookup (no NVS access) - safe to call every frame.
    uint32_t Get(Key key);

    // Updates the in-memory value immediately, so the very next drawn frame
    // reflects it with no restart needed, and persists it to NVS.
    void Set(Key key, uint32_t color888);

    // Name lookup for the serial command protocol, e.g. "SEA", "CAT_HEAVY".
    bool KeyFromName(const String& name, Key& outKey);
    const char* NameFromKey(Key key);
}
