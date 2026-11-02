#pragma once

#include <map>
#include <string>
#include <vector>

namespace rssbot {

struct HttpResponse {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;  // keys are lower-case
    std::string error;                           // set when the transfer itself failed

    std::string header(const std::string& lowerName) const;
};

// Thin libcurl wrapper. One instance reuses a single easy handle (keep-alive), so it is
// not thread-safe; the bot is single-threaded.
class Http {
public:
    explicit Http(std::string userAgent, long timeoutSeconds = 30);
    ~Http();
    Http(const Http&) = delete;
    Http& operator=(const Http&) = delete;

    HttpResponse get(const std::string& url, const std::vector<std::string>& headers = {});
    HttpResponse postJson(const std::string& url, const std::string& body);
    HttpResponse patchJson(const std::string& url, const std::string& body);

private:
    HttpResponse perform(const char* method, const std::string& url, const std::vector<std::string>& headers,
                         const std::string* body);

    void* handle_;
    std::string userAgent_;
    long timeoutSeconds_;
};

}  // namespace rssbot
