#include "SerialCommandManager.h"
#include "ColorConfig.h"

#include <Preferences.h>
#include <WiFi.h>
#include <algorithm>

namespace {
    // Minimal JSON string escaping for values embedded in this file's
    // hand-built JSON (see GET_CONFIG below). Only backslash and double
    // quote need escaping to keep the output valid JSON. Every other field
    // GET_CONFIG emits is either numeric, a boolean literal, or drawn from a
    // fixed key set (color names) - the OpenSky client ID is the only
    // arbitrary user-entered text that flows into it, hence the one call
    // site below.
    String EscapeJsonString(const String& value) {
        String escaped;
        escaped.reserve(value.length());
        for (size_t i = 0; i < value.length(); i++) {
            const char c = value[i];
            if (c == '\\' || c == '"') escaped += '\\';
            escaped += c;
        }
        return escaped;
    }
}

void SerialCommandManager::Update()
{
    if (receivingCoastline && millis() - lastCoastlineDataMs > COASTLINE_STALL_TIMEOUT_MS) {
        Serial.println("ERR coastline transfer stalled - abandoned, back to normal commands");
        receivingCoastline = false;
        pendingPoints.clear();
    }

    if (wifiReceiveStep != WifiReceiveStep::None && millis() - lastWifiLineMs > WIFI_STALL_TIMEOUT_MS) {
        Serial.println("ERR SET_WIFI stalled - abandoned, back to normal commands");
        wifiReceiveStep = WifiReceiveStep::None;
        pendingWifiSsid = "";
    }

    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n') {
            lineBuffer.trim();
            if (lineBuffer.length() > 0) HandleLine(lineBuffer);
            lineBuffer = "";
        }
        else if (c != '\r') {
            lineBuffer += c;
        }
    }
}

void SerialCommandManager::HandleLine(const String& line)
{
    if (receivingCoastline) HandleCoastlineDataLine(line);
    else if (wifiReceiveStep != WifiReceiveStep::None) HandleWifiDataLine(line);
    else HandleCommand(line);
}

void SerialCommandManager::HandleCommand(const String& line)
{
    if (line == "PING") {
        Serial.println("PONG");
        return;
    }

    if (line == "GET_CONFIG") {
        Preferences prefs;
        prefs.begin("config", true);
        const String lat = prefs.getString("latitude", "0");
        const String lon = prefs.getString("longitude", "0");
        const String rad = prefs.getString("radius", "1.0");
        const String openskyId = prefs.getString("opensky-id", "");
        String openskySecret = prefs.getString("opensky-secret", "");
        const String scanlineEnabled = prefs.getString("scanline", "true");
        const String infotextEnabled = prefs.getString("infotext", "true");
        const String triangleEnabled = prefs.getString("triangle", "true");
        const String coastlineEnabled = prefs.getString("coastline", "true");
        prefs.end();

        // Mask the secret exactly like ConfigurationWebServer's web page
        // does (same-length string of '*', empty if unset) - never echo the
        // real secret back over serial. SET_OPENSKY_AUTH recognises a value
        // containing '*' as this masked placeholder and leaves the stored
        // secret untouched rather than overwriting it with asterisks, so a
        // companion app can safely round-trip this field unless the user
        // actually types a new secret.
        std::fill(openskySecret.begin(), openskySecret.end(), '*');

        String json = "{\"lat\":" + lat + ",\"lon\":" + lon + ",\"radius\":" + rad;
        json += ",\"openskyId\":\"" + EscapeJsonString(openskyId) + "\"";
        json += ",\"openskySecret\":\"" + openskySecret + "\""; // always just '*' repeated (or empty) - never needs escaping
        // Live currently-connected SSID (WiFi.SSID(), not a stored preference) -
        // accurate regardless of whether it came from the captive portal,
        // hard-coded firmware constants, or SET_WIFI below. No password is
        // ever reported - see SET_WIFI's doc comment for why there's no
        // masked-round-trip convention here like there is for the OpenSky secret.
        json += ",\"wifiSsid\":\"" + EscapeJsonString(WiFi.SSID()) + "\"";
        json += ",\"toggles\":{";
        json += String("\"scanline\":") + (scanlineEnabled == "true" ? "true" : "false");
        json += String(",\"infotext\":") + (infotextEnabled == "true" ? "true" : "false");
        json += String(",\"triangle\":") + (triangleEnabled == "true" ? "true" : "false");
        json += String(",\"coastline\":") + (coastlineEnabled == "true" ? "true" : "false");
        json += "}";
        json += ",\"colors\":{";
        for (int i = 0; i < ColorConfig::COUNT; i++) {
            const auto key = static_cast<ColorConfig::Key>(i);
            const uint32_t color = ColorConfig::Get(key);
            char hex[8];
            snprintf(hex, sizeof(hex), "%06X", color & 0xFFFFFF);
            json += String("\"") + ColorConfig::NameFromKey(key) + "\":\"" + hex + "\"";
            if (i + 1 < ColorConfig::COUNT) json += ",";
        }
        json += "}}";
        Serial.println(json);
        return;
    }

    if (line.startsWith("SET_COLOR ")) {
        const String rest = line.substring(10);
        const int space = rest.indexOf(' ');
        if (space < 0) {
            Serial.println("ERR expected: SET_COLOR <KEY> <RRGGBB>");
            return;
        }

        const String keyName = rest.substring(0, space);
        const String hex = rest.substring(space + 1);

        ColorConfig::Key key;
        if (!ColorConfig::KeyFromName(keyName, key)) {
            Serial.println("ERR unknown color key: " + keyName);
            return;
        }
        if (hex.length() != 6) {
            Serial.println("ERR expected 6 hex digits, got: " + hex);
            return;
        }

        const uint32_t rgb = strtoul(hex.c_str(), nullptr, 16);
        ColorConfig::Set(key, rgb);
        Serial.println("OK");
        return;
    }

    if (line.startsWith("SET_LOCATION ")) {
        const String rest = line.substring(13);
        const int firstSpace = rest.indexOf(' ');
        const int secondSpace = firstSpace < 0 ? -1 : rest.indexOf(' ', firstSpace + 1);
        if (firstSpace < 0 || secondSpace < 0) {
            Serial.println("ERR expected: SET_LOCATION <lat> <lon> <radius>");
            return;
        }

        const double newLat = rest.substring(0, firstSpace).toDouble();
        const double newLon = rest.substring(firstSpace + 1, secondSpace).toDouble();
        const double newRad = rest.substring(secondSpace + 1).toDouble();

        Preferences prefs;
        prefs.begin("config", false);
        prefs.putString("latitude", String(newLat, 6));
        prefs.putString("longitude", String(newLon, 6));
        prefs.putString("radius", String(newRad, 6));
        prefs.end();

        // Update in-memory projection too, so a coastline push that follows
        // in the same session (same lat/lon/radius) can apply live.
        coastline.SetLocation(newLat, newLon, newRad);
        Serial.println("OK");
        return;
    }

    if (line.startsWith("SET_OPENSKY_AUTH ")) {
        const String rest = line.substring(17);
        const int space = rest.indexOf(' ');
        if (space < 0) {
            Serial.println("ERR expected: SET_OPENSKY_AUTH <clientId> <clientSecret>");
            return;
        }

        const String clientId = rest.substring(0, space);
        const String clientSecret = rest.substring(space + 1);

        Preferences prefs;
        prefs.begin("config", false);
        prefs.putString("opensky-id", clientId);
        // Same "don't overwrite with masked value" guard as
        // ConfigurationWebServer's web form (see GET_CONFIG above for where
        // the masked value comes from) - a caller that just round-trips
        // whatever GET_CONFIG returned won't accidentally blank out an
        // already-set secret.
        if (clientSecret.indexOf('*') == -1) {
            prefs.putString("opensky-secret", clientSecret);
        }
        prefs.end();

        Serial.println("OK");
        return;
    }

    if (line.startsWith("SET_TOGGLE ")) {
        const String rest = line.substring(11);
        const int space = rest.indexOf(' ');
        if (space < 0) {
            Serial.println("ERR expected: SET_TOGGLE <name> <true|false>");
            return;
        }

        const String name = rest.substring(0, space);
        const String value = rest.substring(space + 1);

        // Must match the exact preference keys ConfigurationWebServer's web
        // form writes (scanline/infotext/triangle/coastline) - AircraftManager,
        // main.cpp, and CoastlineManager all read these back with a literal
        // "true" string comparison, not a bool, so the value written here
        // has to be exactly "true" or "false".
        static const char* knownToggles[] = { "scanline", "infotext", "triangle", "coastline" };
        bool validName = false;
        for (const char* known : knownToggles) {
            if (name == known) { validName = true; break; }
        }
        if (!validName) {
            Serial.println("ERR unknown toggle: " + name);
            return;
        }
        if (value != "true" && value != "false") {
            Serial.println("ERR expected true or false, got: " + value);
            return;
        }

        Preferences prefs;
        prefs.begin("config", false);
        prefs.putString(name.c_str(), value);
        prefs.end();

        Serial.println("OK");
        return;
    }

    if (line == "SET_WIFI") {
        pendingWifiSsid = "";
        wifiReceiveStep = WifiReceiveStep::AwaitingSsid;
        lastWifiLineMs = millis();
        Serial.println("OK");
        return;
    }

    if (line.startsWith("COASTLINE_BEGIN ")) {
        const String rest = line.substring(16);
        const int p1 = rest.indexOf(' ');
        const int p2 = p1 < 0 ? -1 : rest.indexOf(' ', p1 + 1);
        const int p3 = p2 < 0 ? -1 : rest.indexOf(' ', p2 + 1);
        if (p1 < 0 || p2 < 0 || p3 < 0) {
            Serial.println("ERR expected: COASTLINE_BEGIN <count> <lat> <lon> <radius>");
            return;
        }

        const long count = rest.substring(0, p1).toInt();
        if (count <= 0 || static_cast<size_t>(count) > CoastlineManager::MAX_RUNTIME_COASTLINE_POINTS) {
            Serial.println("ERR point count out of range");
            return;
        }

        pendingLat = rest.substring(p1 + 1, p2).toDouble();
        pendingLon = rest.substring(p2 + 1, p3).toDouble();
        pendingRad = rest.substring(p3 + 1).toDouble();
        expectedPoints = static_cast<size_t>(count);
        pendingPoints.clear();
        pendingPoints.reserve(expectedPoints * 2);
        receivingCoastline = true;
        lastCoastlineDataMs = millis();
        Serial.println("OK");
        return;
    }

    if (line == "RESTART") {
        Serial.println("OK");
        ESP.restart();
        return;
    }

    Serial.println("ERR unknown command: " + line);
}

void SerialCommandManager::HandleCoastlineDataLine(const String& line)
{
    // Parsed directly off line.c_str() with strtol rather than
    // line.substring() (which heap-allocates a new String per call) - this
    // runs once per point, up to MAX_RUNTIME_COASTLINE_POINTS times per
    // transfer, and gratuitous heap churn in a loop that size is exactly the
    // kind of thing that fragments an ESP32's small heap over a long uptime.
    char* afterFirst = nullptr;
    const long latMicro = strtol(line.c_str(), &afterFirst, 10);
    if (afterFirst == nullptr || *afterFirst != ',') {
        Serial.println("ERR malformed point line (expected latMicro,lonMicro) - aborted");
        receivingCoastline = false;
        pendingPoints.clear();
        return;
    }
    const long lonMicro = strtol(afterFirst + 1, nullptr, 10);

    pendingPoints.push_back(static_cast<int32_t>(latMicro));
    pendingPoints.push_back(static_cast<int32_t>(lonMicro));
    lastCoastlineDataMs = millis();

    const size_t received = pendingPoints.size() / 2;
    if (received >= expectedPoints) {
        receivingCoastline = false;
        const bool ok = coastline.StoreCoastlinePoints(pendingPoints.data(), expectedPoints, pendingLat, pendingLon, pendingRad);
        pendingPoints.clear();
        Serial.println(ok ? ("OK " + String(expectedPoints) + " points") : "ERR failed to store coastline data");
    }
    // Acks every COASTLINE_ACK_INTERVAL points so the sender can pace itself
    // to however fast this device is actually keeping up, rather than
    // blasting the whole transfer in one uninterrupted burst - the fixed-size
    // USB-CDC receive buffer can only absorb so much unread data if loop()
    // happens to be busy (an HTTP call, a render) when bytes arrive, and no
    // buffer size is really "big enough" to rule that out on principle. This
    // is what actually bounds the risk, not the buffer size.
    else if (received % COASTLINE_ACK_INTERVAL == 0) {
        Serial.print("PROGRESS ");
        Serial.println(received);
    }
}

void SerialCommandManager::HandleWifiDataLine(const String& line)
{
    // Note: line has already been through SerialCommandManager::Update()'s
    // lineBuffer.trim() before reaching here, so leading/trailing whitespace
    // typed by a human at a terminal is stripped - a real SSID/password
    // containing meaningful internal spaces (e.g. "My Home WiFi") still
    // comes through untouched, only the outer edges are trimmed.
    lastWifiLineMs = millis();

    if (wifiReceiveStep == WifiReceiveStep::AwaitingSsid) {
        if (line.isEmpty()) {
            Serial.println("ERR SSID cannot be empty - aborted");
            wifiReceiveStep = WifiReceiveStep::None;
            return;
        }
        pendingWifiSsid = line;
        wifiReceiveStep = WifiReceiveStep::AwaitingPassword;
        Serial.println("OK");
        return;
    }

    // AwaitingPassword - line is the password (may legitimately be empty for
    // an open network).
    Preferences prefs;
    prefs.begin("config", false);
    prefs.putString("wifi-ssid", pendingWifiSsid);
    prefs.putString("wifi-password", line);
    prefs.end();

    pendingWifiSsid = "";
    wifiReceiveStep = WifiReceiveStep::None;
    Serial.println("OK");
}
