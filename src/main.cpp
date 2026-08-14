#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>

#include "LGFX.h"
#include "WiFiManagerHelpers.h"
#include "ConfigurationWebServer.h"
#include "HttpRequestManager.h"
#include "OpenSkyAuthTokenHandler.h"
#include "RouteLookupManager.h"
#include "AircraftManager.h"
#include "CoastlineManager.h"
#include "CategoryColors.h"
#include "ColorConfig.h"
#include "ColorUtils.h"
#include "DrawHelpers.h"
#include "GeoUnits.h"
#include "SerialCommandManager.h"
#include "models/Aircraft.h"
#include "models/TrackedAircraft.h"

// Optional hard-coded Wi-Fi credentials. Leave both blank to skip pre-baking them and use the setup hotspot instead.
const char* preconfiguredWifiSsid = "";
const char* preconfiguredWifiPassword = "";

constexpr int SCREEN_SIZE = 240;
constexpr int SCREEN_SIZE_DIV_2 = (SCREEN_SIZE / 2);

LGFX tft;
LGFX_Sprite backbuffer(&tft);

WiFiManager wm;
ConfigurationWebServer configServer;
HttpRequestManager http;
OpenSkyAuthTokenHandler authHandler(http);
RouteLookupManager routeManager(configServer, authHandler, http);

AircraftManager aircraftManager(configServer, authHandler, http, tft, routeManager);
CoastlineManager coastlineManager(configServer);
SerialCommandManager serialCommands(coastlineManager);

// Cached at boot rather than read from Preferences/NVS on every single
// render frame (loop() has no frame limiter, so that was tens of flash
// reads + String allocations per second, every second, for the device's
// entire uptime). Safe to cache: every way either of these can actually
// change (SET_TOGGLE/SET_LOCATION over serial, or the web config page's
// POST /save) either explicitly restarts the device itself or is always
// followed by a RESTART as part of the established protocol - see
// SerialCommandManager.h's command list - so a stale in-memory copy
// between boots was never actually possible to begin with.
bool cachedScanlineEnabled = true;
double cachedRadiusDeg = 0.2;

void setup()
{
  // Must be called before Serial.begin() to take effect. The default (256
  // bytes) is fine for ordinary short commands/log lines, but the coastline
  // push protocol sends a whole batch of points (tens of KB) in one burst -
  // any brief stall elsewhere in loop() (an HTTP poll, a render) while that's
  // streaming in silently drops bytes once a 256-byte buffer fills, which is
  // exactly what was wedging SerialCommandManager's coastline receive state.
  Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  // delay(1000); // avoids immediate serial output being cut off - uncomment if needed

  // load the adjustable color palette before anything ever draws with it
  ColorConfig::Initialise();

  // initialise LGFX + screen
  tft.init();
  tft.invertDisplay(true);
  pinMode(3, OUTPUT);
  digitalWrite(3, HIGH);

  backbuffer.setColorDepth(8);
  backbuffer.createSprite(SCREEN_SIZE, SCREEN_SIZE);

  // establish WiFi connection
  tft.fillScreen(lgfx::color888(0, 0, 0));
  tft.setTextColor(lgfx::color888(0, 255, 0));
  tft.drawCentreString("Connecting to WiFi...", SCREEN_SIZE / 2, SCREEN_SIZE / 2);

  WiFiManagerHelpers::ConfigureWiFiManager(wm, tft);

  // Credentials set via the companion app's SET_WIFI (see
  // SerialCommandManager::HandleWifiDataLine) take priority over the
  // hard-coded constants above, since they're the more recently and
  // deliberately configured source - both are just a faster path to a known
  // network than the WiFiManager captive portal below, which remains the
  // fallback either way.
  const String storedWifiSsid = configServer.GetStoredString("wifi-ssid");
  if (!storedWifiSsid.isEmpty()) {
    WiFi.begin(storedWifiSsid.c_str(), configServer.GetStoredString("wifi-password").c_str());
    WiFi.waitForConnectResult();
  } else if (strlen(preconfiguredWifiSsid) > 0) {
    WiFi.begin(preconfiguredWifiSsid, preconfiguredWifiPassword);
    WiFi.waitForConnectResult();
  }

  wm.autoConnect(WiFiManagerHelpers::WiFiManagerName);

  // begin background server for configuration
  configServer.Initialise();

  // initialise aircraft + route lookup managers
  routeManager.Initialise();
  aircraftManager.Initialise();

  // fetch + rasterize coastline for the configured location (one-time, shows its own loading message)
  coastlineManager.Initialise(tft);

  // see the comment on these globals' declaration for why this is safe to
  // read once here rather than every loop() frame
  const String scanlineStored = configServer.GetStoredString("scanline");
  cachedScanlineEnabled = scanlineStored.isEmpty() || scanlineStored == "true";
  cachedRadiusDeg = configServer.GetStoredString("radius").toDouble();
}

void loop()
{
  aircraftManager.Update();
  routeManager.Update();
  serialCommands.Update();

  // draw cycle - clear to the "ocean" tone so the display reads as water by
  // default, even before coastline data has loaded or if it's disabled
  backbuffer.fillScreen(CoastlineManager::SeaColor());

  coastlineManager.Draw(backbuffer);

  if (cachedScanlineEnabled) {
    DrawScanLines(backbuffer,
      SCREEN_SIZE_DIV_2 - 1,
      SCREEN_SIZE_DIV_2 - 1,
      SCREEN_SIZE_DIV_2 - 1 + (std::cos(millis() / 3000.0f) * SCREEN_SIZE_DIV_2),
      SCREEN_SIZE_DIV_2 - 1 + (std::sin(millis() / 3000.0f) * SCREEN_SIZE_DIV_2),
      20, 5,
      CoastlineManager::SeaColor(),
      CategoryColors::RadarColor()
    );
  }

  aircraftManager.Draw(backbuffer);

  // Range label - drawn last so it always sits on top of the coastline,
  // scan lines, and aircraft rather than getting drawn over by any of them.
  // Centred top - close enough to the screen's edge to need the margin
  // check, but a short "90km"-style string comfortably clears the round
  // display's visible circle here (see DrawRadarCircles's OUTER radius).
  const String rangeLabel = String(cachedRadiusDeg * GeoUnits::KM_PER_DEGREE, 0) + "km";
  backbuffer.setTextSize(1);
  backbuffer.setTextColor(ColorConfig::Get(ColorConfig::RANGE_LABEL));
  DrawBoldCentreString(backbuffer, rangeLabel, SCREEN_SIZE_DIV_2, 12);

  backbuffer.pushSprite(0, 0);
}

