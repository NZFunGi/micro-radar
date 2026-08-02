#pragma once

#include <Arduino.h>
#include <vector>
#include <cstdint>

#include "CoastlineManager.h"

// Text-based command protocol read over the same USB serial connection
// already used for flashing/logging, so a companion desktop app (or a human
// typing into a plain serial terminal) can push live config changes without
// a firmware rebuild.
//
// Commands are newline-terminated, one per line:
//   PING                                          -> "PONG"
//   GET_CONFIG                                    -> one-line JSON snapshot: lat/lon/radius,
//                                                     the OpenSky client ID (plaintext) and
//                                                     client secret (masked with '*' at its
//                                                     stored length, same convention as the
//                                                     on-device web config page), the
//                                                     currently-connected WiFi SSID (no
//                                                     password - see SET_WIFI), the
//                                                     scanline/infotext/triangle/coastline
//                                                     display toggles, and the color palette
//   SET_COLOR <KEY> <RRGGBB hex>                  -> "OK" / "ERR <reason>"
//                                                     (applies immediately, no restart)
//   SET_LOCATION <lat> <lon> <radius>             -> "OK"
//                                                     (persisted; radar radius/network
//                                                     fetch only fully applies after RESTART)
//   SET_OPENSKY_AUTH <clientId> <clientSecret>    -> "OK"
//                                                     (persisted; only fully applies after
//                                                     RESTART, since the daily request budget
//                                                     is computed once at boot from whether
//                                                     credentials are present - see
//                                                     OpenSkyBudget.h. clientSecret may be the
//                                                     masked value GET_CONFIG returned - if it
//                                                     contains '*' the stored secret is left
//                                                     unchanged, exactly like the web config
//                                                     page's "don't overwrite with masked
//                                                     value" handling)
//   SET_TOGGLE <name> <true|false>                -> "OK" / "ERR unknown toggle: <name>"
//                                                     name is one of: scanline, infotext,
//                                                     triangle, coastline (persisted; only
//                                                     fully applies after RESTART, since these
//                                                     are read once at boot)
//   SET_WIFI                                      -> "OK" (now expects the SSID as the next line)
//   <ssid>                                         -> "OK" (now expects the password as the next line)
//   <password>                                     -> "OK"
//                                                     (persisted; only fully applies after RESTART.
//                                                     Split across three lines rather than
//                                                     space-delimited arguments like every other
//                                                     SET_* command, since SSIDs/passwords
//                                                     routinely contain spaces - each line's
//                                                     entire content, verbatim, becomes the SSID
//                                                     or password, so nothing needs escaping.
//                                                     Same "switch into a stateful multi-line
//                                                     mode" shape as COASTLINE_BEGIN below, just
//                                                     for exactly two lines instead of a batch of
//                                                     points)
//   COASTLINE_BEGIN <count> <lat> <lon> <radius>  -> "OK" / "ERR <reason>"
//   <latMicro>,<lonMicro>                            (repeated `count` times - the device
//                                                     sends "PROGRESS <n>" every
//                                                     COASTLINE_ACK_INTERVAL points, which a
//                                                     well-behaved sender should wait for
//                                                     before sending more, rather than
//                                                     blasting the whole transfer at once)
//                                                  -> "OK <n> points" once complete
//                                                     (applies immediately if lat/lon/radius
//                                                     match the device's current config)
//   RESTART                                       -> restarts the device
class SerialCommandManager {
public:
    explicit SerialCommandManager(CoastlineManager& coastline) : coastline(coastline) {}

    // Call once per loop() iteration. Non-blocking - only acts once a full
    // line has actually arrived.
    void Update();

private:
    CoastlineManager& coastline;
    String lineBuffer;

    // Coastline point transfer state - COASTLINE_BEGIN switches into this
    // mode, after which subsequent lines are treated as data points rather
    // than commands until `expectedPoints` have arrived.
    bool receivingCoastline = false;
    size_t expectedPoints = 0;
    double pendingLat = 0.0, pendingLon = 0.0, pendingRad = 0.0;
    std::vector<int32_t> pendingPoints;
    unsigned long lastCoastlineDataMs = 0;

    // If a transfer stalls or a bug on either end drops it mid-stream, this
    // caps how long we'll sit in "receiving" mode believing more data is
    // still coming - past this, treat it as abandoned and go back to normal
    // command parsing rather than wedging the serial interface until reboot.
    static constexpr unsigned long COASTLINE_STALL_TIMEOUT_MS = 10000;

    // How often (in points received) to send a "PROGRESS <n>" ack during a
    // coastline transfer - see the class comment above.
    static constexpr size_t COASTLINE_ACK_INTERVAL = 100;

    // SET_WIFI transfer state - see the class comment above. Same stall-abandon
    // reasoning as the coastline transfer, just a much shorter window since
    // there are only ever two lines to wait for.
    enum class WifiReceiveStep { None, AwaitingSsid, AwaitingPassword };
    WifiReceiveStep wifiReceiveStep = WifiReceiveStep::None;
    String pendingWifiSsid;
    unsigned long lastWifiLineMs = 0;
    static constexpr unsigned long WIFI_STALL_TIMEOUT_MS = 10000;

    void HandleLine(const String& line);
    void HandleCommand(const String& line);
    void HandleCoastlineDataLine(const String& line);
    void HandleWifiDataLine(const String& line);
};
