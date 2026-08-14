#pragma once

#include <ESPAsyncWebServer.h>
#include <Preferences.h>

class ConfigurationWebServer {
private:
    AsyncWebServer server;
    // Deliberately no shared `Preferences` member here - AsyncWebServer's
    // request handlers run on their own FreeRTOS task (a different one from
    // Arduino's main loop() task, which is what calls GetStoredString()),
    // and Preferences has no internal locking. A single shared instance
    // raced between those two tasks (both able to call begin()/end())
    // caused real, observed data races. Every method below opens its own
    // local Preferences handle instead, same pattern SerialCommandManager
    // already uses correctly.

public:
    ConfigurationWebServer() : server(80) {}
    ConfigurationWebServer(int port) : server(port) {}

    void Initialise();
    [[nodiscard]] const String GetStoredString(const char* key);
};