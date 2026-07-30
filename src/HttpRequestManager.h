#pragma once

#include <HTTPClient.h>
#include <vector>

struct HttpResult {
    bool success;           // Whether the request succeeded
    int statusCode;         // HTTP status code (0 if network error)
    String response;        // Response body (empty on error)
    String errorMessage;    // Error description if success == false
};

class HttpRequestManager
{
private:
    HTTPClient http;

    String BuildQueryString(const std::vector<std::pair<String, String>>& params) const;

public:
    HttpRequestManager() = default;
    ~HttpRequestManager() = default;

    // maxResponseBytes: if > 0, abort (success = false) rather than reading a body larger than this.
    // timeoutMs: if > 0, overrides the default HTTPClient timeout for this request.
    [[nodiscard]] HttpResult Get(const String& url, const std::vector<std::pair<String, String>>& params = {}, const std::vector<std::pair<String, String>>& headers = {}, size_t maxResponseBytes = 0, int timeoutMs = 0);
    [[nodiscard]] HttpResult Post(const String& url, const String& body = "", const std::vector<std::pair<String, String>>& headers = {}, size_t maxResponseBytes = 0, int timeoutMs = 0);
};