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
//   GET_CONFIG                                    -> one-line JSON snapshot
//   SET_COLOR <KEY> <RRGGBB hex>                  -> "OK" / "ERR <reason>"
//                                                     (applies immediately, no restart)
//   SET_LOCATION <lat> <lon> <radius>             -> "OK"
//                                                     (persisted; radar radius/network
//                                                     fetch only fully applies after RESTART)
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

    void HandleLine(const String& line);
    void HandleCommand(const String& line);
    void HandleCoastlineDataLine(const String& line);
};
