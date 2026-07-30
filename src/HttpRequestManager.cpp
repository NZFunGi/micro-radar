#include "HttpRequestManager.h"

String HttpRequestManager::BuildQueryString(const std::vector<std::pair<String, String>>& params) const
{
    if (params.empty())
        return "";

    String queryStream = "?";

    bool first = true;
    for (const auto& [key, value] : params)
    {
        if (!first)
            queryStream += "&";

        queryStream += key + "=" + value;

        first = false;
    }

    return queryStream;
}

HttpResult HttpRequestManager::Get(const String& url, const std::vector<std::pair<String, String>>& params, const std::vector<std::pair<String, String>>& headers, size_t maxResponseBytes, int timeoutMs) {
    HttpResult result{ false, 0, "", "" };

    const String queryParams = BuildQueryString(params);
    const String fullUrl = url + queryParams;

    http.begin(fullUrl);
    if (timeoutMs > 0) http.setTimeout(timeoutMs);

    // add headers to request
    for (const auto& header : headers) {
        http.addHeader(header.first, header.second);
    }

    // send request and handle response
    int responseCode = http.GET();
    result.statusCode = responseCode;

    if (responseCode > 0) {
        // getSize() is -1 when the server doesn't send Content-Length (e.g. chunked
        // transfer encoding) - that means "unknown", not "huge". Casting -1 to
        // size_t wraps to SIZE_MAX, which would always trip a naive ">" check
        // against maxResponseBytes and reject every chunked response outright, so
        // only pre-emptively reject when the size is actually known.
        const int reportedSize = http.getSize();
        if (maxResponseBytes > 0 && reportedSize > 0 && (size_t)reportedSize > maxResponseBytes) {
            result.success = false;
            result.errorMessage = "Response too large";
        }
        else {
            result.response = http.getString();
            // Unknown-size (chunked) responses aren't caught by the check above, so
            // enforce the cap after the fact too - still bounds what callers ever
            // receive, even though the body was already read into memory by then.
            if (maxResponseBytes > 0 && result.response.length() > maxResponseBytes) {
                result.success = false;
                result.errorMessage = "Response too large";
                result.response = "";
            }
            else {
                result.success = true;
            }
        }
    }
    else {
        result.success = false;
        result.errorMessage = http.errorToString(responseCode);
        Serial.print("[GET] HTTP Error (");
        Serial.print(responseCode);
        Serial.print("): ");
        Serial.println(result.errorMessage);
    }

    http.end();
    return result;
}

HttpResult HttpRequestManager::Post(const String& url, const String& body, const std::vector<std::pair<String, String>>& headers, size_t maxResponseBytes, int timeoutMs)
{
    HttpResult result{ false, 0, "", "" };

    http.begin(url);
    if (timeoutMs > 0) http.setTimeout(timeoutMs);

    // add headers to request
    for (const auto& header : headers) {
        http.addHeader(header.first, header.second);
    }

    // send request and handle response
    int responseCode = http.POST(body);
    result.statusCode = responseCode;

    if (responseCode > 0) {
        // see Get() for why unknown (-1) size must not be treated as "too large"
        const int reportedSize = http.getSize();
        if (maxResponseBytes > 0 && reportedSize > 0 && (size_t)reportedSize > maxResponseBytes) {
            result.success = false;
            result.errorMessage = "Response too large";
        }
        else {
            result.response = http.getString();
            if (maxResponseBytes > 0 && result.response.length() > maxResponseBytes) {
                result.success = false;
                result.errorMessage = "Response too large";
                result.response = "";
            }
            else {
                result.success = true;
            }
        }
    }
    else {
        result.success = false;
        result.errorMessage = http.errorToString(responseCode);
        Serial.print("[POST] HTTP Error (");
        Serial.print(responseCode);
        Serial.print("): ");
        Serial.println(result.errorMessage);
    }

    http.end();
    return result;
}
