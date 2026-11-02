#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace rssbot {

// A Discord webhook: one channel in one server. Feeds can post to several of them.
struct WebhookConfig {
    std::string id;
    std::string name;       // label in the dashboard, e.g. "Gaming server #news"
    std::string url;        // https://discord.com/api/webhooks/ID/TOKEN, or "env:VARIABLE"
    std::string username;   // display name on posts (empty: global default)
    std::string avatarUrl;  // display image on posts (empty: global default / webhook avatar)
    std::string threadId;   // optional: post into a thread or forum post
    std::string content;    // optional text above every embed, e.g. a role ping "<@&123>"
    std::string guildId;    // filled in from Discord, informational
    std::string channelId;

    std::string resolvedUrl() const;  // expands "env:VARIABLE"; empty if unset
};

struct FeedConfig {
    std::string id;
    std::string name;  // embed author; defaults to the feed's own title
    std::string url;
    std::vector<std::string> webhooks;  // WebhookConfig ids
    int color = 0x5865F2;
    int summaryLength = 350;  // 0 hides the description
    bool showImage = true;
    bool enabled = true;
};

struct DashboardConfig {
    bool enabled = true;
    std::string bind = "127.0.0.1";
    int port = 8080;
    std::string password;  // HTTP Basic auth; required to allow non-localhost access
};

struct Config {
    std::string path;  // the config file this came from / is saved to
    std::string username = "RSS";
    std::string avatarUrl;
    std::string stateFile = "state.json";
    std::string userAgent = "rss-discord-bot/2.0 (+libcurl)";
    int intervalSeconds = 300;
    int maxPostsPerPoll = 5;
    int firstRunPosts = 1;   // newest items posted when a feed/webhook pair is new
    int postDelayMs = 1000;  // pause between webhook posts
    int defaultColor = 0x5865F2;
    DashboardConfig dashboard;
    std::vector<WebhookConfig> webhooks;
    std::vector<FeedConfig> feeds;

    std::string statePath() const;  // stateFile resolved relative to the config file
    const WebhookConfig* findWebhook(const std::string& id) const;
    WebhookConfig* findWebhook(const std::string& id);
    FeedConfig* findFeed(const std::string& id);
};

// Missing file + allowMissing: returns an empty config (set up through the dashboard).
Config loadConfig(const std::string& path, bool allowMissing);
// Parses and validates. Also upgrades the old single "webhook_url" layout.
Config configFromJson(const nlohmann::json& j, const std::string& path);
nlohmann::ordered_json configToJson(const Config& c);
void validateConfig(const Config& c);  // throws std::runtime_error
void saveConfig(const Config& c);      // atomic, file mode 0600 (it holds webhook tokens)

int parseColor(const nlohmann::json& value);  // 0xRRGGBB, "#RRGGBB" or "0xRRGGBB"
std::string colorToHex(int color);
std::string newId(std::string_view prefix);
bool isWebhookUrl(const std::string& url);
std::string maskWebhookUrl(const std::string& url);  // keeps the id, hides most of the token

}  // namespace rssbot
