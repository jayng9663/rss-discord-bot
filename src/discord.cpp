#include "discord.hpp"

#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace rssbot {
namespace {

using nlohmann::json;
using std::chrono::milliseconds;

// Discord limits: title 256, description 4096, author 256, footer 2048, content 2000,
// username 80, and 6000 characters for all embed text combined (we stay well below).
constexpr std::size_t kTitleMax = 256;
constexpr std::size_t kAuthorMax = 256;
constexpr std::size_t kFooterMax = 256;
constexpr std::size_t kContentMax = 2000;
constexpr std::size_t kUsernameMax = 80;
constexpr int kMaxAttempts = 6;

milliseconds secondsToMs(double seconds) {
    seconds = std::clamp(seconds, 0.0, 600.0);
    return milliseconds(static_cast<long long>(std::ceil(seconds * 1000.0)));
}

milliseconds retryAfter(const HttpResponse& r) {
    double seconds = 1.0;
    const json body = json::parse(r.body, nullptr, false);
    if (!body.is_discarded() && body.is_object() && body.contains("retry_after") && body["retry_after"].is_number()) {
        seconds = body["retry_after"].get<double>();
    } else if (const std::string h = r.header("retry-after"); !h.empty()) {
        try {
            seconds = std::stod(h);
        } catch (const std::exception&) {
        }
    }
    return secondsToMs(seconds) + milliseconds(250);
}

milliseconds backoff(int attempt) { return milliseconds(1000LL << std::min(attempt, 6)); }

// Wait out an exhausted bucket before the next request instead of provoking a 429.
void respectBucket(const HttpResponse& r) {
    if (r.header("x-ratelimit-remaining") != "0") return;
    try {
        const double resetAfter = std::stod(r.header("x-ratelimit-reset-after"));
        logDebug(std::format("rate-limit bucket empty, waiting {:.2f}s", resetAfter));
        interruptibleSleep(secondsToMs(resetAfter));
    } catch (const std::exception&) {
    }
}

std::string jsonString(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// "Unknown Webhook (HTTP 404)" from a Discord error body.
std::string describeError(const HttpResponse& r) {
    if (!r.error.empty()) return r.error;
    const json body = json::parse(r.body, nullptr, false);
    std::string message;
    if (!body.is_discarded() && body.is_object()) {
        message = jsonString(body, "message");
        if (const auto errors = body.find("errors"); errors != body.end())
            message += " " + truncateChars(errors->dump(), 300);
    }
    if (message.empty()) message = truncateChars(trim(r.body), 300);
    return std::format("{} (HTTP {})", message.empty() ? "request failed" : message, r.status);
}

std::optional<WebhookInfo> parseInfo(const HttpResponse& r, std::string& error) {
    if (!r.error.empty() || r.status < 200 || r.status >= 300) {
        error = describeError(r);
        return std::nullopt;
    }
    const json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "unexpected response from Discord";
        return std::nullopt;
    }
    WebhookInfo info;
    info.id = jsonString(j, "id");
    info.name = jsonString(j, "name");
    info.guildId = jsonString(j, "guild_id");
    info.channelId = jsonString(j, "channel_id");
    if (const std::string hash = jsonString(j, "avatar"); !hash.empty() && !info.id.empty())
        info.avatarUrl = "https://cdn.discordapp.com/avatars/" + info.id + "/" + hash + ".png?size=256";
    return info;
}

}  // namespace

std::string withQuery(const std::string& webhookUrl, const std::string& threadId) {
    std::string url = webhookUrl;
    url += url.find('?') == std::string::npos ? '?' : '&';
    url += "wait=true";
    if (!threadId.empty()) url += "&thread_id=" + threadId;
    return url;
}

json buildPayload(const Feed& feed, const FeedItem& item, const FeedConfig& fc, const WebhookConfig& wh,
                  const Config& cfg) {
    json embed = json::object();
    embed["title"] = truncateChars(item.title.empty() ? std::string("(untitled)") : item.title, kTitleMax);
    if (isHttpUrl(item.link)) embed["url"] = item.link;
    if (fc.summaryLength > 0 && !item.summary.empty())
        embed["description"] = truncateChars(item.summary, static_cast<std::size_t>(fc.summaryLength));
    embed["color"] = fc.color;

    const std::string source = !fc.name.empty() ? fc.name : feed.title;
    if (!source.empty()) {
        json author = {{"name", truncateChars(source, kAuthorMax)}};
        if (isHttpUrl(feed.link)) author["url"] = feed.link;
        embed["author"] = author;
    }
    if (!item.author.empty()) embed["footer"] = {{"text", truncateChars(item.author, kFooterMax)}};
    if (item.date) embed["timestamp"] = toIso8601(*item.date);
    if (fc.showImage && isHttpUrl(item.imageUrl)) embed["image"] = {{"url", item.imageUrl}};

    json payload = json::object();
    payload["embeds"] = json::array({embed});
    const std::string username = !wh.username.empty() ? wh.username : cfg.username;
    const std::string avatar = !wh.avatarUrl.empty() ? wh.avatarUrl : cfg.avatarUrl;
    if (!username.empty()) payload["username"] = truncateChars(username, kUsernameMax);
    if (isHttpUrl(avatar)) payload["avatar_url"] = avatar;
    if (!wh.content.empty()) {
        // Mentions come only from the operator's own "content" text, never from feed data.
        payload["content"] = truncateChars(wh.content, kContentMax);
        payload["allowed_mentions"] = {{"parse", json::array({"roles", "users", "everyone"})}};
    } else {
        payload["allowed_mentions"] = {{"parse", json::array()}};
    }
    return payload;
}

json buildTestPayload(const WebhookConfig& wh, const Config& cfg) {
    Feed feed;
    feed.title = "rss-discord-bot";
    FeedItem item;
    item.title = "\xE2\x9C\x85 Webhook connected";
    item.summary = "New feed posts sent through \"" + (wh.name.empty() ? wh.id : wh.name) +
                   "\" will appear in this channel looking like this.";
    item.date = std::time(nullptr);
    FeedConfig fc;
    fc.color = 0x23A55A;
    WebhookConfig quiet = wh;
    quiet.content.clear();  // don't ping a role for a test
    return buildPayload(feed, item, fc, quiet, cfg);
}

std::optional<WebhookInfo> fetchWebhookInfo(Http& http, const std::string& url, std::string& error) {
    if (url.empty()) {
        error = "webhook URL is empty (environment variable not set?)";
        return std::nullopt;
    }
    return parseInfo(http.get(url), error);
}

std::optional<WebhookInfo> updateDiscordWebhook(Http& http, const std::string& url, const std::string& name,
                                                const std::string& avatarDataUri, std::string& error) {
    json body = json::object();
    if (!name.empty()) body["name"] = truncateChars(name, kUsernameMax);
    if (!avatarDataUri.empty()) body["avatar"] = avatarDataUri;
    return parseInfo(http.patchJson(url, body.dump()), error);
}

SendResult DiscordWebhook::send(const std::string& webhookUrl, const std::string& threadId, const json& payload,
                                std::string* errorOut) {
    const std::string url = withQuery(webhookUrl, threadId);
    // Feeds sometimes contain invalid UTF-8; replace it instead of throwing.
    const std::string body = payload.dump(-1, ' ', false, json::error_handler_t::replace);
    std::string lastError = "stopped";

    for (int attempt = 1; attempt <= kMaxAttempts && !g_stop; ++attempt) {
        const HttpResponse r = http_.postJson(url, body);

        if (!r.error.empty()) {
            lastError = r.error;
            logWarn(std::format("webhook request failed: {} (attempt {}/{})", r.error, attempt, kMaxAttempts));
            interruptibleSleep(backoff(attempt));
            continue;
        }
        if (r.status >= 200 && r.status < 300) {
            respectBucket(r);
            return SendResult::Sent;
        }
        lastError = describeError(r);
        if (r.status == 429) {
            const milliseconds wait = retryAfter(r);
            logWarn(std::format("rate limited by Discord, retrying in {} ms", wait.count()));
            interruptibleSleep(wait);
            continue;
        }
        if (r.status >= 500) {
            logWarn(std::format("Discord returned HTTP {} (attempt {}/{})", r.status, attempt, kMaxAttempts));
            interruptibleSleep(backoff(attempt));
            continue;
        }
        if (errorOut != nullptr) *errorOut = lastError;
        if (r.status == 400 || r.status == 413) {
            logError("Discord rejected the message: " + lastError);
            return SendResult::Rejected;
        }
        // 401/403/404: the webhook was deleted or the token is wrong. Keep the item.
        logError("webhook error: " + lastError + " - is the webhook URL still valid?");
        return SendResult::RetryLater;
    }
    if (errorOut != nullptr) *errorOut = lastError;
    return SendResult::RetryLater;
}

}  // namespace rssbot
