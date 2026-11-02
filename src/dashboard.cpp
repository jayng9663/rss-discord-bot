#include "dashboard.hpp"

#include "dashboard_html.hpp"  // generated from web/index.html
#include "discord.hpp"
#include "feed.hpp"
#include "http.hpp"
#include "util.hpp"

#include <httplib.h>

#include <algorithm>
#include <format>
#include <functional>
#include <stdexcept>

namespace rssbot {
namespace {

using nlohmann::json;
using Request = httplib::Request;
using Response = httplib::Response;
using Handler = std::function<void(const Request&, Response&)>;

struct ApiError : std::runtime_error {
    int status;
    ApiError(int s, const std::string& message) : std::runtime_error(message), status(s) {}
};

void sendJson(Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
}

Handler wrap(Handler fn) {
    return [fn = std::move(fn)](const Request& req, Response& res) {
        try {
            fn(req, res);
        } catch (const ApiError& e) {
            sendJson(res, {{"error", e.what()}}, e.status);
        } catch (const std::exception& e) {
            sendJson(res, {{"error", e.what()}}, 400);
        }
    };
}

json parseBody(const Request& req) {
    if (req.body.empty()) return json::object();
    json j = json::parse(req.body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw ApiError(400, "request body must be a JSON object");
    return j;
}

std::string str(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return trim(it->get<std::string>());
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    throw ApiError(400, std::string("\"") + key + "\" must be a string");
}

int integer(const json& j, const char* key, int fallback) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (it->is_number_integer()) return it->get<int>();
    if (it->is_string()) {
        try {
            return std::stoi(it->get<std::string>());
        } catch (const std::exception&) {
        }
    }
    throw ApiError(400, std::string("\"") + key + "\" must be a whole number");
}

bool boolean(const json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) return fallback;
    if (!it->is_boolean()) throw ApiError(400, std::string("\"") + key + "\" must be true or false");
    return it->get<bool>();
}

std::string hostOnly(const std::string& host) {
    if (host.starts_with('[')) return host.substr(0, host.find(']') + 1);
    return host.substr(0, host.find(':'));
}

bool isLoopbackHost(const std::string& host) {
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string base64Decode(std::string_view in) {
    static constexpr std::string_view kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned value = 0;
    int bits = -8;
    for (const char c : in) {
        if (c == '=') break;
        const auto p = kChars.find(c);
        if (p == std::string_view::npos) continue;
        value = ((value << 6) | static_cast<unsigned>(p)) & 0xFFFFFFu;
        bits += 6;
        if (bits >= 0) {
            out += static_cast<char>((value >> bits) & 0xFFu);
            bits -= 8;
        }
    }
    return out;
}

json webhookJson(const WebhookConfig& w, const Config& c) {
    const auto uses = std::ranges::count_if(c.feeds, [&](const FeedConfig& f) {
        return std::ranges::find(f.webhooks, w.id) != f.webhooks.end();
    });
    return {{"id", w.id},
            {"name", w.name},
            {"url_masked", maskWebhookUrl(w.url)},
            {"from_env", w.url.starts_with("env:")},
            {"username", w.username},
            {"avatar_url", w.avatarUrl},
            {"thread_id", w.threadId},
            {"content", w.content},
            {"guild_id", w.guildId},
            {"channel_id", w.channelId},
            {"feeds", uses}};
}

json feedJson(const FeedConfig& f) {
    return {{"id", f.id},
            {"name", f.name},
            {"url", f.url},
            {"webhooks", f.webhooks},
            {"color", colorToHex(f.color)},
            {"summary_length", f.summaryLength},
            {"show_image", f.showImage},
            {"enabled", f.enabled}};
}

// Webhook URLs are secrets: the API only ever returns them masked.
json configJson(const Config& c) {
    json webhooks = json::array();
    for (const auto& w : c.webhooks) webhooks.push_back(webhookJson(w, c));
    json feeds = json::array();
    for (const auto& f : c.feeds) feeds.push_back(feedJson(f));
    return {{"settings",
             {{"username", c.username},
              {"avatar_url", c.avatarUrl},
              {"interval_seconds", c.intervalSeconds},
              {"max_posts_per_poll", c.maxPostsPerPoll},
              {"first_run_posts", c.firstRunPosts},
              {"post_delay_ms", c.postDelayMs},
              {"color", colorToHex(c.defaultColor)}}},
            {"dashboard",
             {{"bind", c.dashboard.bind}, {"port", c.dashboard.port}, {"password_set", !c.dashboard.password.empty()}}},
            {"config_path", c.path},
            {"webhooks", webhooks},
            {"feeds", feeds}};
}

void applyFeedFields(FeedConfig& f, const json& b) {
    if (b.contains("name")) f.name = str(b, "name");
    if (b.contains("url")) f.url = str(b, "url");
    if (b.contains("webhooks")) {
        if (!b["webhooks"].is_array()) throw ApiError(400, "\"webhooks\" must be a list of webhook ids");
        f.webhooks.clear();
        for (const auto& id : b["webhooks"])
            if (id.is_string() && std::ranges::find(f.webhooks, id.get<std::string>()) == f.webhooks.end())
                f.webhooks.push_back(id.get<std::string>());
    }
    if (b.contains("color") && !b["color"].is_null()) f.color = parseColor(b["color"]);
    f.summaryLength = std::clamp(integer(b, "summary_length", f.summaryLength), 0, 4096);
    f.showImage = boolean(b, "show_image", f.showImage);
    f.enabled = boolean(b, "enabled", f.enabled);
}

// Fills a webhook from a dashboard form. Talks to Discord to verify the URL, to learn
// the server/channel, and (for an uploaded image) to set the webhook's avatar.
void applyWebhookFields(WebhookConfig& w, const json& b, const Config& cfg) {
    const std::string url = str(b, "url");
    bool urlChanged = false;
    if (!url.empty()) {
        if (!url.starts_with("env:") && !isWebhookUrl(url))
            throw ApiError(400, "That is not a Discord webhook URL (https://discord.com/api/webhooks/ID/TOKEN)");
        urlChanged = url != w.url;
        w.url = url;
    }
    if (w.url.empty()) throw ApiError(400, "Webhook URL is required");
    if (b.contains("name")) w.name = str(b, "name");
    if (b.contains("username")) w.username = str(b, "username");
    if (b.contains("avatar_url")) w.avatarUrl = str(b, "avatar_url");
    if (b.contains("thread_id")) w.threadId = str(b, "thread_id");
    if (b.contains("content")) w.content = str(b, "content");

    // Check the form before touching Discord.
    Config probe = cfg;
    probe.webhooks = {w};
    probe.feeds.clear();
    probe.dashboard.enabled = true;
    validateConfig(probe);

    const std::string avatarData = str(b, "avatar_data");
    if (!avatarData.empty() && !avatarData.starts_with("data:image/png;base64,") &&
        !avatarData.starts_with("data:image/jpeg;base64,") && !avatarData.starts_with("data:image/gif;base64,"))
        throw ApiError(400, "The image must be a PNG, JPEG or GIF");

    Http http(cfg.userAgent, 20);
    std::string error;
    if (urlChanged || !avatarData.empty()) {
        const auto info = fetchWebhookInfo(http, w.resolvedUrl(), error);
        if (!info) throw ApiError(400, "Discord did not accept this webhook: " + error);
        w.guildId = info->guildId;
        w.channelId = info->channelId;
        if (w.name.empty()) w.name = info->name;
        if (w.username.empty()) w.username = info->name;
        if (w.avatarUrl.empty() && avatarData.empty()) w.avatarUrl = info->avatarUrl;
    }
    if (!avatarData.empty()) {
        const auto info = updateDiscordWebhook(http, w.resolvedUrl(), w.username, avatarData, error);
        if (!info) throw ApiError(502, "Discord refused the image: " + error);
        w.avatarUrl = info->avatarUrl;
    }
}

const std::string kWebhookPath = R"(/api/webhooks/([A-Za-z0-9_-]+))";
const std::string kFeedPath = R"(/api/feeds/([A-Za-z0-9_-]+))";

}  // namespace

Dashboard::Dashboard(App& app, DashboardConfig cfg)
    : app_(app), cfg_(std::move(cfg)), server_(std::make_unique<httplib::Server>()) {
    routes();
}

Dashboard::~Dashboard() { stop(); }

bool Dashboard::start() {
    if (!server_->bind_to_port(cfg_.bind, cfg_.port)) {
        logError(std::format("dashboard: cannot listen on {}:{} (port in use?)", cfg_.bind, cfg_.port));
        return false;
    }
    thread_ = std::thread([this] { server_->listen_after_bind(); });
    const bool anyAddress = cfg_.bind == "0.0.0.0" || cfg_.bind == "::";
    logInfo(std::format("dashboard: http://{}:{}/{}", anyAddress ? "localhost" : cfg_.bind, cfg_.port,
                        cfg_.password.empty() ? "" : " (password protected)"));
    if (anyAddress && cfg_.password.empty())
        logWarn("dashboard: listening on all interfaces without a password; only localhost requests are accepted");
    return true;
}

void Dashboard::stop() {
    if (server_) server_->stop();
    if (thread_.joinable()) thread_.join();
}

void Dashboard::routes() {
    httplib::Server& s = *server_;
    s.set_payload_max_length(8 * 1024 * 1024);

    // Auth, DNS-rebinding and CSRF protection for every request.
    s.set_pre_routing_handler([this](const Request& req, Response& res) {
        if (!cfg_.password.empty()) {
            const std::string auth = req.get_header_value("Authorization");
            bool ok = false;
            if (auth.starts_with("Basic ")) {
                const std::string decoded = base64Decode(auth.substr(6));
                const auto colon = decoded.find(':');
                ok = colon != std::string::npos && constantTimeEquals(decoded.substr(colon + 1), cfg_.password);
            }
            if (!ok) {
                res.status = 401;
                res.set_header("WWW-Authenticate", "Basic realm=\"rss-discord-bot\", charset=\"UTF-8\"");
                res.set_content("Authentication required", "text/plain");
                return httplib::Server::HandlerResponse::Handled;
            }
        } else if (!isLoopbackHost(hostOnly(req.get_header_value("Host")))) {
            res.status = 403;
            res.set_content("Without a dashboard password only http://localhost and http://127.0.0.1 are allowed.\n"
                            "Set \"dashboard.password\" in config.json to open the dashboard from other machines.\n",
                            "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        }
        // Browsers cannot send this header cross-site without a CORS preflight, which we never allow.
        if (req.method != "GET" && req.method != "HEAD" && req.get_header_value("X-Requested-With") != "rssbot") {
            sendJson(res, {{"error", "missing X-Requested-With header"}}, 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    s.set_post_routing_handler([](const Request&, Response& res) {
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("Referrer-Policy", "no-referrer");
        res.set_header("Cache-Control", "no-store");
        res.set_header("Content-Security-Policy",
                       "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                       "img-src https: http: data: blob:; connect-src 'self'; frame-ancestors 'none'; "
                       "base-uri 'none'; form-action 'none'");
    });

    s.Get("/", [](const Request&, Response& res) {
        res.set_content(kDashboardHtml, sizeof(kDashboardHtml) - 1, "text/html; charset=utf-8");
    });

    s.Get("/api/status", wrap([this](const Request&, Response& res) { sendJson(res, app_.status()); }));
    s.Get("/api/config", wrap([this](const Request&, Response& res) { sendJson(res, configJson(app_.config())); }));

    s.Put("/api/settings", wrap([this](const Request& req, Response& res) {
        const json b = parseBody(req);
        app_.updateConfig([&](Config& c) {
            if (b.contains("username")) c.username = str(b, "username");
            if (b.contains("avatar_url")) c.avatarUrl = str(b, "avatar_url");
            c.intervalSeconds = integer(b, "interval_seconds", c.intervalSeconds);
            c.maxPostsPerPoll = integer(b, "max_posts_per_poll", c.maxPostsPerPoll);
            c.firstRunPosts = integer(b, "first_run_posts", c.firstRunPosts);
            c.postDelayMs = integer(b, "post_delay_ms", c.postDelayMs);
            if (b.contains("color") && !b["color"].is_null()) c.defaultColor = parseColor(b["color"]);
        });
        sendJson(res, configJson(app_.config()));
    }));

    s.Post("/api/poll", wrap([this](const Request& req, Response& res) {
        app_.requestPoll(str(parseBody(req), "feed_id"));
        sendJson(res, {{"ok", true}});
    }));

    // --- webhooks ---------------------------------------------------------------
    s.Post("/api/webhooks/inspect", wrap([this](const Request& req, Response& res) {
        const json b = parseBody(req);
        const Config cfg = app_.config();
        std::string url = str(b, "url");
        if (url.empty()) {
            const WebhookConfig* w = cfg.findWebhook(str(b, "id"));
            if (w == nullptr) throw ApiError(400, "Paste a webhook URL first");
            url = w->resolvedUrl();
        } else if (!isWebhookUrl(url)) {
            throw ApiError(400, "That is not a Discord webhook URL (https://discord.com/api/webhooks/ID/TOKEN)");
        }
        Http http(cfg.userAgent, 20);
        std::string error;
        const auto info = fetchWebhookInfo(http, url, error);
        if (!info) throw ApiError(400, "Discord did not accept this webhook: " + error);
        sendJson(res, {{"name", info->name},
                       {"avatar_url", info->avatarUrl},
                       {"guild_id", info->guildId},
                       {"channel_id", info->channelId}});
    }));

    s.Post("/api/webhooks", wrap([this](const Request& req, Response& res) {
        const json b = parseBody(req);
        WebhookConfig w;
        w.id = newId("wh");
        applyWebhookFields(w, b, app_.config());
        app_.updateConfig([&](Config& c) { c.webhooks.push_back(w); });
        logInfo(std::format("dashboard: added webhook \"{}\"", w.name));
        sendJson(res, webhookJson(w, app_.config()), 201);
    }));

    s.Put(kWebhookPath, wrap([this](const Request& req, Response& res) {
        const std::string id = req.matches[1];
        const json b = parseBody(req);
        const Config cfg = app_.config();
        const WebhookConfig* existing = cfg.findWebhook(id);
        if (existing == nullptr) throw ApiError(404, "unknown webhook");
        WebhookConfig w = *existing;
        applyWebhookFields(w, b, cfg);
        app_.updateConfig([&](Config& c) {
            WebhookConfig* target = c.findWebhook(id);
            if (target == nullptr) throw ApiError(404, "unknown webhook");
            *target = w;
        });
        sendJson(res, webhookJson(w, app_.config()));
    }));

    s.Delete(kWebhookPath, wrap([this](const Request& req, Response& res) {
        const std::string id = req.matches[1];
        app_.updateConfig([&](Config& c) {
            const auto removed = std::erase_if(c.webhooks, [&](const WebhookConfig& w) { return w.id == id; });
            if (removed == 0) throw ApiError(404, "unknown webhook");
            for (auto& f : c.feeds) std::erase(f.webhooks, id);
        });
        sendJson(res, {{"ok", true}});
    }));

    s.Post(kWebhookPath + "/test", wrap([this](const Request& req, Response& res) {
        const Config cfg = app_.config();
        const WebhookConfig* w = cfg.findWebhook(req.matches[1]);
        if (w == nullptr) throw ApiError(404, "unknown webhook");
        const std::string url = w->resolvedUrl();
        if (url.empty()) throw ApiError(400, "This webhook has no URL (environment variable not set)");
        Http http(cfg.userAgent, 20);
        DiscordWebhook webhook(http);
        std::string error;
        if (webhook.send(url, w->threadId, buildTestPayload(*w, cfg), &error) != SendResult::Sent)
            throw ApiError(502, "Discord did not accept the test message: " + error);
        sendJson(res, {{"ok", true}});
    }));

    // --- feeds ------------------------------------------------------------------
    s.Post("/api/feeds/preview", wrap([this](const Request& req, Response& res) {
        const std::string url = str(parseBody(req), "url");
        if (!isHttpUrl(url)) throw ApiError(400, "Enter an http:// or https:// feed URL");
        const Config cfg = app_.config();
        Http http(cfg.userAgent, 20);
        const HttpResponse r = http.get(url, {"Accept: application/rss+xml, application/atom+xml, application/xml;q=0.9, */*;q=0.5"});
        if (!r.error.empty()) throw ApiError(400, "Could not fetch the feed: " + r.error);
        if (r.status < 200 || r.status >= 300) throw ApiError(400, std::format("Could not fetch the feed: HTTP {}", r.status));
        std::string error;
        const auto feed = parseFeed(r.body, url, error);
        if (!feed) throw ApiError(400, error);
        json items = json::array();
        for (std::size_t i = 0; i < feed->items.size() && i < 5; ++i) {
            const FeedItem& it = feed->items[i];
            items.push_back({{"title", it.title},
                             {"link", it.link},
                             {"summary", truncateChars(it.summary, 4096)},
                             {"image", it.imageUrl},
                             {"author", it.author},
                             {"date", it.date ? json(*it.date) : json(nullptr)}});
        }
        sendJson(res, {{"title", feed->title}, {"link", feed->link}, {"count", feed->items.size()}, {"items", items}});
    }));

    s.Post("/api/feeds", wrap([this](const Request& req, Response& res) {
        const json b = parseBody(req);
        FeedConfig f;
        f.id = newId("feed");
        f.color = app_.config().defaultColor;
        applyFeedFields(f, b);
        app_.updateConfig([&](Config& c) { c.feeds.push_back(f); });
        logInfo(std::format("dashboard: added feed {}", f.url));
        app_.requestPoll(f.id);
        sendJson(res, feedJson(f), 201);
    }));

    s.Put(kFeedPath, wrap([this](const Request& req, Response& res) {
        const std::string id = req.matches[1];
        const json b = parseBody(req);
        FeedConfig updated;
        app_.updateConfig([&](Config& c) {
            FeedConfig* f = c.findFeed(id);
            if (f == nullptr) throw ApiError(404, "unknown feed");
            applyFeedFields(*f, b);
            updated = *f;
        });
        if (updated.enabled) app_.requestPoll(id);
        sendJson(res, feedJson(updated));
    }));

    // Every post currently in the feed, with posted/skipped/new per webhook.
    s.Get(kFeedPath + "/items", wrap([this](const Request& req, Response& res) {
        const std::string id = req.matches[1];
        Config cfg = app_.config();
        const FeedConfig* f = cfg.findFeed(id);
        if (f == nullptr) throw ApiError(404, "unknown feed");
        Http http(cfg.userAgent, 20);
        const HttpResponse r = http.get(f->url, {"Accept: application/rss+xml, application/atom+xml, application/xml;q=0.9, */*;q=0.5"});
        if (!r.error.empty()) throw ApiError(502, "Could not fetch the feed: " + r.error);
        if (r.status < 200 || r.status >= 300) throw ApiError(502, std::format("Could not fetch the feed: HTTP {}", r.status));
        std::string error;
        const auto feed = parseFeed(r.body, f->url, error);
        if (!feed) throw ApiError(502, error);

        std::vector<std::string> ids;
        for (const FeedItem& it : feed->items) ids.push_back(it.id);
        const json status = app_.itemStatus(f->url, ids, f->webhooks);
        json items = json::array();
        for (std::size_t i = 0; i < feed->items.size(); ++i) {
            const FeedItem& it = feed->items[i];
            items.push_back({{"id", it.id},
                             {"title", it.title},
                             {"link", it.link},
                             {"summary", truncateChars(it.summary, 4096)},
                             {"image", it.imageUrl},
                             {"author", it.author},
                             {"date", it.date ? json(*it.date) : json(nullptr)},
                             {"status", status[i]}});
        }
        sendJson(res, {{"title", feed->title}, {"link", feed->link}, {"count", items.size()}, {"items", items}});
    }));

    // Repost one/some posts ("items": [ids]) or all of them ("all": true), optionally to chosen webhooks.
    s.Post(kFeedPath + "/repost", wrap([this](const Request& req, Response& res) {
        const json b = parseBody(req);
        const auto strings = [&](const char* key) {
            std::vector<std::string> out;
            if (const auto it = b.find(key); it != b.end() && !it->is_null()) {
                if (!it->is_array()) throw ApiError(400, std::string("\"") + key + "\" must be a list");
                for (const auto& v : *it)
                    if (v.is_string()) out.push_back(v.get<std::string>());
            }
            return out;
        };
        const bool all = boolean(b, "all", false);
        std::vector<std::string> items = strings("items");
        if (!all && items.empty()) throw ApiError(400, "choose a post to repost, or \"all\"");
        if (all) items.clear();
        const std::string job = app_.queueRepost(req.matches[1], std::move(items), strings("webhooks"));
        sendJson(res, {{"job_id", job}}, 202);
    }));

    s.Post(R"(/api/jobs/([A-Za-z0-9_-]+)/cancel)", wrap([this](const Request& req, Response& res) {
        if (!app_.cancelJob(req.matches[1])) throw ApiError(404, "unknown job");
        sendJson(res, {{"ok", true}});
    }));

    s.Delete(kFeedPath, wrap([this](const Request& req, Response& res) {
        const std::string id = req.matches[1];
        app_.updateConfig([&](Config& c) {
            if (std::erase_if(c.feeds, [&](const FeedConfig& f) { return f.id == id; }) == 0)
                throw ApiError(404, "unknown feed");
        });
        sendJson(res, {{"ok", true}});
    }));
}

}  // namespace rssbot
