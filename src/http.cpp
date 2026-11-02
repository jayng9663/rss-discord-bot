#include "http.hpp"

#include "util.hpp"

#include <curl/curl.h>

#include <stdexcept>
#include <string_view>

namespace rssbot {
namespace {

constexpr std::size_t kMaxBodyBytes = 16 * 1024 * 1024;

struct Sink {
    std::string* out;
    bool overflow = false;
};

std::size_t onBody(char* data, std::size_t size, std::size_t count, void* user) {
    auto* sink = static_cast<Sink*>(user);
    const std::size_t bytes = size * count;
    if (sink->out->size() + bytes > kMaxBodyBytes) {
        sink->overflow = true;
        return 0;  // aborts the transfer
    }
    sink->out->append(data, bytes);
    return bytes;
}

std::size_t onHeader(char* data, std::size_t size, std::size_t count, void* user) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(user);
    const std::size_t bytes = size * count;
    const std::string_view line(data, bytes);
    if (line.starts_with("HTTP/")) {
        headers->clear();  // a new response begins (e.g. after a redirect)
    } else if (const auto colon = line.find(':'); colon != std::string_view::npos) {
        (*headers)[toLower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return bytes;
}

}  // namespace

std::string HttpResponse::header(const std::string& lowerName) const {
    const auto it = headers.find(lowerName);
    return it == headers.end() ? std::string{} : it->second;
}

Http::Http(std::string userAgent, long timeoutSeconds)
    : handle_(curl_easy_init()), userAgent_(std::move(userAgent)), timeoutSeconds_(timeoutSeconds) {
    if (handle_ == nullptr) throw std::runtime_error("curl_easy_init failed");
}

Http::~Http() { curl_easy_cleanup(static_cast<CURL*>(handle_)); }

HttpResponse Http::get(const std::string& url, const std::vector<std::string>& headers) {
    return perform("GET", url, headers, nullptr);
}

HttpResponse Http::postJson(const std::string& url, const std::string& body) {
    return perform("POST", url, {"Content-Type: application/json"}, &body);
}

HttpResponse Http::patchJson(const std::string& url, const std::string& body) {
    return perform("PATCH", url, {"Content-Type: application/json"}, &body);
}

HttpResponse Http::perform(const char* method, const std::string& url, const std::vector<std::string>& headers,
                           const std::string* body) {
    CURL* curl = static_cast<CURL*>(handle_);
    curl_easy_reset(curl);

    HttpResponse response;
    Sink sink{&response.body};
    char errorBuffer[CURL_ERROR_SIZE] = {};

    curl_slist* headerList = nullptr;
    for (const auto& h : headers) headerList = curl_slist_append(headerList, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent_.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds_);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // any encoding curl supports
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
    if (body != nullptr) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body->size()));
        if (std::string_view(method) != "POST") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_easy_reset(curl);  // drop pointers to locals before they go out of scope
    curl_slist_free_all(headerList);

    if (rc != CURLE_OK) {
        if (sink.overflow) response.error = "response larger than 16 MiB";
        else if (errorBuffer[0] != '\0') response.error = trim(errorBuffer);
        else response.error = curl_easy_strerror(rc);
    }
    return response;
}

}  // namespace rssbot
