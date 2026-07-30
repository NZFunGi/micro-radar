#include "ColorConfig.h"
#include "LGFX.h"

#include <Preferences.h>

namespace ColorConfig {
namespace {

    struct Entry { Key key; const char* name; uint32_t defaultColor; };

    // Defaults match the hand-picked palette this project already shipped
    // with: light sea/land tones so the fully-saturated aircraft/category
    // hues stand out against them, dark blue as the "unknown category"
    // default since most real-world OpenSky traffic reports no category at
    // all. Array order must match declaration order in Key.
    const Entry kEntries[COUNT] = {
        { SEA,         "SEA",         lgfx::color888(176, 196, 210) },
        { LAND,        "LAND",        lgfx::color888(150, 205, 140) },
        { RADAR,       "RADAR",       lgfx::color888(45, 70, 100) },
        { CAT_LIGHT,   "CAT_LIGHT",   lgfx::color888(0, 140, 200) },
        { CAT_LARGE,   "CAT_LARGE",   lgfx::color888(255, 140, 0) },
        { CAT_HEAVY,   "CAT_HEAVY",   lgfx::color888(255, 40, 40) },
        { CAT_ROTOR,   "CAT_ROTOR",   lgfx::color888(255, 0, 180) },
        { CAT_GLIDER,  "CAT_GLIDER",  lgfx::color888(110, 110, 120) },
        { CAT_UNKNOWN, "CAT_UNKNOWN", lgfx::color888(20, 70, 190) },
        { RANGE_LABEL, "RANGE_LABEL", lgfx::color888(45, 70, 100) },
    };

    uint32_t cache[COUNT];

}

void Initialise()
{
    Preferences prefs;
    prefs.begin("colors", true);
    for (const Entry& e : kEntries) {
        cache[e.key] = prefs.getUInt(e.name, e.defaultColor);
    }
    prefs.end();
}

uint32_t Get(Key key)
{
    return cache[key];
}

void Set(Key key, uint32_t color888)
{
    cache[key] = color888;

    Preferences prefs;
    prefs.begin("colors", false);
    prefs.putUInt(kEntries[key].name, color888);
    prefs.end();
}

bool KeyFromName(const String& name, Key& outKey)
{
    for (const Entry& e : kEntries) {
        if (name.equalsIgnoreCase(e.name)) {
            outKey = e.key;
            return true;
        }
    }
    return false;
}

const char* NameFromKey(Key key)
{
    return kEntries[key].name;
}

}
