#pragma once

#include "config.hpp"
#include "feed.hpp"
#include "http.hpp"

#include <nlohmann/json.hpp>

#include <optional>

namespace rssbot {

enum class SendResult {
    Sent,
    RetryLater,  // network/5xx/auth trouble: keep the item and try again next poll
    Rejected,    // Discord refused this payload (HTTP 400): skip the item
};

// What Discord reports about a webhook (GET/PATCH on the webhook URL; no bot token needed).
struct WebhookInfo {
    std::string id;
    std::string name;
    std::string avatarUrl;
    std::string guildId;
    std::string channelId;
};

// Builds an execute-webhook payload with one embed, within Discord's field limits.
nlohmann::json buildPayload(const Feed& feed, const FeedItem& item, const FeedConfig& fc, const WebhookConfig& wh,
                            const Config& cfg);
nlohmann::json buildTestPayload(const WebhookConfig& wh, const Config& cfg);

std::optional<WebhookInfo> fetchWebhookInfo(Http& http, const std::string& url, std::string& error);
// Changes the webhook's own name/avatar on Discord. avatarDataUri: "data:image/png;base64,...".
std::optional<WebhookInfo> updateDiscordWebhook(Http& http, const std::string& url, const std::string& name,
                                                const std::string& avatarDataUri, std::string& error);
std::string withQuery(const std::string& webhookUrl, const std::string& threadId);

class DiscordWebhook {
public:
    explicit DiscordWebhook(Http& http) : http_(http) {}
    // Handles 429 rate limits (retry_after) and the per-webhook bucket headers.
    SendResult send(const std::string& webhookUrl, const std::string& threadId, const nlohmann::json& payload,
                    std::string* errorOut = nullptr);

private:
    Http& http_;
};

}  // namespace rssbot
