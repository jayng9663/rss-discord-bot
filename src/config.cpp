#include "config.hpp"

#include "util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <stdexcept>

namespace rssbot {
namespace {

using nlohmann::json;

std::string getString(const json& j, const char* key, const std::string& fallback) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (!it->is_string()) throw std::runtime_error(std::string("\"") + key + "\" must be a string");
    return it->get<std::string>();
}

int getInt(const json& j, const char* key, int fallback) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (!it->is_number_integer()) throw std::runtime_error(std::string("\"") + key + "\" must be an integer");
    return it->get<int>();
}

bool getBool(const json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (!it->is_boolean()) throw std::runtime_error(std::string("\"") + key + "\" must be true or false");
    return it->get<bool>();
}

int getColor(const json& j, const char* key, int fallback) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    return parseColor(*it);
}

WebhookConfig webhookFromJson(const json& wj) {
    if (!wj.is_object()) throw std::runtime_error("each webhook must be an object");
    WebhookConfig w;
    w.id = getString(wj, "id", "");
    w.name = getString(wj, "name", "");
    w.url = trim(getString(wj, "url", ""));
    w.username = getString(wj, "username", "");
    w.avatarUrl = getString(wj, "avatar_url", "");
    w.threadId = getString(wj, "thread_id", "");
    w.content = getString(wj, "content", "");
    w.guildId = getString(wj, "guild_id", "");
    w.channelId = getString(wj, "channel_id", "");
    if (w.id.empty()) w.id = newId("wh");
    return w;
}

}  // namespace

int parseColor(const json& v) {
    if (v.is_number_integer()) return v.get<int>() & 0xFFFFFF;
    if (v.is_string()) {
        std::string s = trim(v.get<std::string>());
        if (s.starts_with('#')) s.erase(0, 1);
        else if (s.starts_with("0x") || s.starts_with("0X")) s.erase(0, 2);
        try {
            std::size_t used = 0;
            const int value = std::stoi(s, &used, 16);
            if (used == s.size() && !s.empty()) return value & 0xFFFFFF;
        } catch (const std::exception&) {
        }
    }
    throw std::runtime_error("colour must be a number or a \"#RRGGBB\" string");
}

std::string colorToHex(int color) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%06X", static_cast<unsigned>(color) & 0xFFFFFFu);
    return buf;
}

std::string newId(std::string_view prefix) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id(prefix);
    id += '-';
    for (int i = 0; i < 8; ++i) id += kHex[rng() & 15u];
    return id;
}

bool isWebhookUrl(const std::string& url) {
    return url.starts_with("https://") && url.find("/api/webhooks/") != std::string::npos &&
           url.find_first_of(" \t\r\n") == std::string::npos;
}

std::string maskWebhookUrl(const std::string& url) {
    if (url.starts_with("env:")) return "$" + url.substr(4);
    const auto p = url.find("/api/webhooks/");
    if (p == std::string::npos) return "(invalid URL)";
    const auto idEnd = url.find('/', p + 14);
    if (idEnd == std::string::npos) return url.substr(0, p + 14) + "\xE2\x80\xA6";
    return url.substr(0, idEnd + 1) + url.substr(idEnd + 1, 4) + "\xE2\x80\xA6";
}

std::string WebhookConfig::resolvedUrl() const {
    if (url.starts_with("env:")) {
        const char* value = std::getenv(url.c_str() + 4);
        return value != nullptr ? trim(value) : std::string{};
    }
    return url;
}

std::string Config::statePath() const {
    std::filesystem::path p(stateFile);
    if (p.is_relative()) p = std::filesystem::path(path).parent_path() / p;
    return p.string();
}

const WebhookConfig* Config::findWebhook(const std::string& id) const {
    for (const auto& w : webhooks)
        if (w.id == id) return &w;
    return nullptr;
}

WebhookConfig* Config::findWebhook(const std::string& id) {
    for (auto& w : webhooks)
        if (w.id == id) return &w;
    return nullptr;
}

FeedConfig* Config::findFeed(const std::string& id) {
    for (auto& f : feeds)
        if (f.id == id) return &f;
    return nullptr;
}

Config configFromJson(const json& j, const std::string& path) {
    if (!j.is_object()) throw std::runtime_error("top level must be an object");

    Config c;
    c.path = path;
    c.username = getString(j, "username", c.username);
    c.avatarUrl = getString(j, "avatar_url", c.avatarUrl);
    c.stateFile = getString(j, "state_file", c.stateFile);
    c.userAgent = getString(j, "user_agent", c.userAgent);
    c.intervalSeconds = std::max(30, getInt(j, "interval_seconds", c.intervalSeconds));
    c.maxPostsPerPoll = std::max(1, getInt(j, "max_posts_per_poll", c.maxPostsPerPoll));
    c.firstRunPosts = std::max(0, getInt(j, "first_run_posts", c.firstRunPosts));
    c.postDelayMs = std::max(0, getInt(j, "post_delay_ms", c.postDelayMs));
    c.defaultColor = getColor(j, "color", c.defaultColor);

    if (const auto d = j.find("dashboard"); d != j.end() && !d->is_null()) {
        if (!d->is_object()) throw std::runtime_error("\"dashboard\" must be an object");
        c.dashboard.enabled = getBool(*d, "enabled", c.dashboard.enabled);
        c.dashboard.bind = getString(*d, "bind", c.dashboard.bind);
        c.dashboard.port = getInt(*d, "port", c.dashboard.port);
        c.dashboard.password = getString(*d, "password", c.dashboard.password);
    }

    if (const auto w = j.find("webhooks"); w != j.end() && !w->is_null()) {
        if (!w->is_array()) throw std::runtime_error("\"webhooks\" must be an array");
        for (std::size_t i = 0; i < w->size(); ++i) {
            try {
                c.webhooks.push_back(webhookFromJson((*w)[i]));
            } catch (const std::exception& e) {
                throw std::runtime_error("webhooks[" + std::to_string(i) + "]: " + e.what());
            }
        }
    }

    // Version 1 layout: a single global "webhook_url" (or DISCORD_WEBHOOK_URL).
    const auto addDefault = [&](const std::string& url, const std::string& name) {
        if (c.findWebhook("default") != nullptr) return;
        WebhookConfig w;
        w.id = "default";
        w.name = name;
        w.url = url;
        c.webhooks.insert(c.webhooks.begin(), w);
    };
    if (const std::string legacy = trim(getString(j, "webhook_url", "")); !legacy.empty()) {
        addDefault(legacy, "Default");
    } else if (const char* env = std::getenv("DISCORD_WEBHOOK_URL"); c.webhooks.empty() && env && *env) {
        addDefault("env:DISCORD_WEBHOOK_URL", "Default (DISCORD_WEBHOOK_URL)");
    }

    if (const auto feeds = j.find("feeds"); feeds != j.end() && !feeds->is_null()) {
        if (!feeds->is_array()) throw std::runtime_error("\"feeds\" must be an array");
        for (std::size_t i = 0; i < feeds->size(); ++i) {
            const json& f = (*feeds)[i];
            FeedConfig fc;
            fc.color = c.defaultColor;
            bool explicitTargets = false;
            try {
                if (f.is_string()) {
                    fc.url = trim(f.get<std::string>());
                } else if (f.is_object()) {
                    fc.id = getString(f, "id", "");
                    fc.url = trim(getString(f, "url", ""));
                    fc.name = getString(f, "name", "");
                    fc.color = getColor(f, "color", fc.color);
                    fc.summaryLength = std::clamp(getInt(f, "summary_length", fc.summaryLength), 0, 4096);
                    fc.showImage = getBool(f, "show_image", fc.showImage);
                    fc.enabled = getBool(f, "enabled", fc.enabled);
                    if (const auto t = f.find("webhooks"); t != f.end() && !t->is_null()) {
                        if (!t->is_array()) throw std::runtime_error("\"webhooks\" must be an array of webhook ids");
                        explicitTargets = true;
                        for (const auto& id : *t)
                            if (id.is_string()) fc.webhooks.push_back(id.get<std::string>());
                    } else if (const std::string own = trim(getString(f, "webhook_url", "")); !own.empty()) {
                        // Version 1 per-feed webhook: turn it into a webhook entry.
                        WebhookConfig w;
                        w.id = newId("wh");
                        w.name = fc.name.empty() ? fc.url : fc.name;
                        w.url = own;
                        w.username = getString(f, "username", "");
                        w.avatarUrl = getString(f, "avatar_url", "");
                        w.threadId = getString(f, "thread_id", "");
                        w.content = getString(f, "content", "");
                        c.webhooks.push_back(w);
                        fc.webhooks.push_back(w.id);
                        explicitTargets = true;
                    }
                } else {
                    throw std::runtime_error("must be a URL string or an object");
                }
            } catch (const std::exception& e) {
                throw std::runtime_error("feeds[" + std::to_string(i) + "]: " + e.what());
            }
            if (!explicitTargets) {
                if (c.findWebhook("default") != nullptr) fc.webhooks.push_back("default");
                else if (c.webhooks.size() == 1) fc.webhooks.push_back(c.webhooks.front().id);
            }
            if (fc.id.empty()) fc.id = newId("feed");
            c.feeds.push_back(std::move(fc));
        }
    }

    validateConfig(c);
    return c;
}

void validateConfig(const Config& c) {
    if (c.intervalSeconds < 30) throw std::runtime_error("poll interval must be at least 30 seconds");
    if (c.maxPostsPerPoll < 1) throw std::runtime_error("max posts per poll must be at least 1");
    if (c.firstRunPosts < 0 || c.postDelayMs < 0) throw std::runtime_error("negative values are not allowed");
    if (!c.avatarUrl.empty() && !isHttpUrl(c.avatarUrl))
        throw std::runtime_error("default image URL must start with http:// or https://");
    if (c.dashboard.port < 1 || c.dashboard.port > 65535) throw std::runtime_error("dashboard port must be 1-65535");

    std::set<std::string> webhookIds;
    for (const auto& w : c.webhooks) {
        const std::string label = w.name.empty() ? w.id : w.name;
        if (w.id.empty() || !webhookIds.insert(w.id).second)
            throw std::runtime_error("duplicate or empty webhook id \"" + w.id + "\"");
        if (!w.url.starts_with("env:") && !isWebhookUrl(w.url))
            throw std::runtime_error("webhook \"" + label +
                                     "\": URL must look like https://discord.com/api/webhooks/ID/TOKEN");
        if (!w.avatarUrl.empty() && !isHttpUrl(w.avatarUrl))
            throw std::runtime_error("webhook \"" + label + "\": image URL must start with http:// or https://");
        if (utf8Length(w.username) > 80)
            throw std::runtime_error("webhook \"" + label + "\": display name is longer than 80 characters");
        const std::string lower = toLower(w.username);
        if (lower.find("discord") != std::string::npos || lower.find("clyde") != std::string::npos)
            throw std::runtime_error("webhook \"" + label + "\": Discord does not allow \"discord\" or \"clyde\" in the display name");
        if (!w.threadId.empty() && w.threadId.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("webhook \"" + label + "\": thread ID must be a number");
    }

    std::set<std::string> feedIds;
    for (const auto& f : c.feeds) {
        const std::string label = f.name.empty() ? f.url : f.name;
        if (f.id.empty() || !feedIds.insert(f.id).second)
            throw std::runtime_error("duplicate or empty feed id \"" + f.id + "\"");
        if (!isHttpUrl(f.url)) throw std::runtime_error("feed \"" + label + "\": URL must start with http:// or https://");
        for (const auto& id : f.webhooks)
            if (!webhookIds.contains(id))
                throw std::runtime_error("feed \"" + label + "\" uses unknown webhook \"" + id + "\"");
    }
    if (!c.dashboard.enabled && c.feeds.empty())
        throw std::runtime_error("no feeds configured (add some to \"feeds\" or enable the dashboard)");
}

nlohmann::ordered_json configToJson(const Config& c) {
    using oj = nlohmann::ordered_json;
    oj j;
    j["username"] = c.username;
    j["avatar_url"] = c.avatarUrl;
    j["interval_seconds"] = c.intervalSeconds;
    j["max_posts_per_poll"] = c.maxPostsPerPoll;
    j["first_run_posts"] = c.firstRunPosts;
    j["post_delay_ms"] = c.postDelayMs;
    j["color"] = colorToHex(c.defaultColor);
    j["state_file"] = c.stateFile;
    j["user_agent"] = c.userAgent;
    j["dashboard"] = {{"enabled", c.dashboard.enabled},
                      {"bind", c.dashboard.bind},
                      {"port", c.dashboard.port},
                      {"password", c.dashboard.password}};
    oj webhooks = oj::array();
    for (const auto& w : c.webhooks) {
        webhooks.push_back({{"id", w.id},
                            {"name", w.name},
                            {"url", w.url},
                            {"username", w.username},
                            {"avatar_url", w.avatarUrl},
                            {"thread_id", w.threadId},
                            {"content", w.content},
                            {"guild_id", w.guildId},
                            {"channel_id", w.channelId}});
    }
    j["webhooks"] = webhooks;
    oj feeds = oj::array();
    for (const auto& f : c.feeds) {
        feeds.push_back({{"id", f.id},
                         {"name", f.name},
                         {"url", f.url},
                         {"webhooks", f.webhooks},
                         {"color", colorToHex(f.color)},
                         {"summary_length", f.summaryLength},
                         {"show_image", f.showImage},
                         {"enabled", f.enabled}});
    }
    j["feeds"] = feeds;
    return j;
}

void saveConfig(const Config& c) {
    namespace fs = std::filesystem;
    const fs::path target(c.path);
    std::error_code ec;
    if (target.has_parent_path()) fs::create_directories(target.parent_path(), ec);

    const std::string tmp = c.path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp);
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        out << configToJson(c).dump(2, ' ', false, nlohmann::ordered_json::error_handler_t::replace) << '\n';
        out.flush();
        if (!out) throw std::runtime_error("cannot write " + tmp);
    }
    fs::rename(tmp, target, ec);
    if (ec) throw std::runtime_error("cannot replace " + c.path + ": " + ec.message());
}

Config loadConfig(const std::string& path, bool allowMissing) {
    std::ifstream in(path);
    if (!in) {
        if (!allowMissing || std::filesystem::exists(path)) throw std::runtime_error("cannot open config file: " + path);
        return configFromJson(json::object(), path);
    }
    try {
        const json j = json::parse(in, nullptr, true, /*ignore_comments=*/true);
        return configFromJson(j, path);
    } catch (const std::exception& e) {
        throw std::runtime_error("config " + path + ": " + e.what());
    }
}

}  // namespace rssbot
