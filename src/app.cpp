#include "app.hpp"

#include "feed.hpp"
#include "http.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <thread>
#include <unordered_set>
#include <vector>

#ifndef RSSBOT_VERSION
#define RSSBOT_VERSION "dev"
#endif

namespace rssbot {

App::App(Config config, bool dryRun) : cfg_(std::move(config)), state_(cfg_.statePath()), dryRun_(dryRun) {
    state_.load();
    worker_ = std::thread([this] { workerLoop(); });
}

App::~App() {
    {
        const std::lock_guard lock(mu_);
        workerStop_ = true;
        for (auto& [id, job] : jobs_) job.cancel = true;
    }
    jobCv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

Config App::config() const {
    const std::lock_guard lock(mu_);
    return cfg_;
}

void App::updateConfig(const std::function<void(Config&)>& mutate) {
    const std::lock_guard lock(mu_);
    Config next = cfg_;
    mutate(next);
    validateConfig(next);
    saveConfig(next);
    cfg_ = std::move(next);
}

void App::requestPoll(const std::string& feedId) {
    const std::lock_guard lock(mu_);
    if (feedId.empty()) pollAll_ = true;
    else pollIds_.insert(feedId);
    pollPending_ = true;
}

void App::setStatus(const std::string& feedId, bool ok, std::string message, const std::string& title,
                    std::size_t items) {
    const std::lock_guard lock(mu_);
    FeedStatus& s = status_[feedId];
    s.lastCheck = std::time(nullptr);
    s.ok = ok;
    s.message = std::move(message);
    if (!title.empty() || items > 0) {
        s.title = title;
        s.items = items;
    }
}

nlohmann::json App::status() const {
    const std::lock_guard lock(mu_);
    nlohmann::json feeds = nlohmann::json::object();
    for (const auto& [id, s] : status_) {
        feeds[id] = {{"last_check", s.lastCheck}, {"ok", s.ok}, {"message", s.message},
                     {"title", s.title},          {"items", s.items}};
    }
    nlohmann::json posts = nlohmann::json::array();
    for (const auto& p : posts_)
        posts.push_back({{"time", p.time}, {"feed", p.feed}, {"webhook", p.webhook}, {"title", p.title}, {"link", p.link}});
    nlohmann::json jobs = nlohmann::json::array();
    for (auto it = jobOrder_.rbegin(); it != jobOrder_.rend(); ++it) {
        const RepostJob& j = jobs_.at(*it);
        jobs.push_back({{"id", j.id},           {"feed_id", j.feedId},   {"feed", j.feedName},
                        {"state", j.state},     {"total", j.total},      {"done", j.done},
                        {"failed", j.failed},   {"message", j.message},  {"current", j.current},
                        {"created", j.created}, {"finished", j.finished}, {"items", j.itemIds.size()}});
    }
    return {{"version", RSSBOT_VERSION},
            {"jobs", jobs},
            {"dry_run", dryRun_},
            {"polling", polling_.load()},
            {"last_poll", lastPoll_},
            {"next_poll", polling_ ? 0 : nextPoll_},
            {"interval_seconds", cfg_.intervalSeconds},
            {"feeds", feeds},
            {"posts", posts},
            {"logs", recentLogs()}};
}

void App::run(bool once) {
    bool all = true;
    std::set<std::string> only;
    while (!g_stop) {
        pollCycle(all ? std::set<std::string>{} : only);
        if (once) break;

        const int interval = config().intervalSeconds;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(interval);
        {
            const std::lock_guard lock(mu_);
            nextPoll_ = std::time(nullptr) + interval;
        }
        while (!g_stop && !pollPending_ && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

        const std::lock_guard lock(mu_);
        if (pollPending_ && std::chrono::steady_clock::now() < deadline) {
            all = pollAll_;
            only = std::move(pollIds_);
        } else {
            all = true;
            only.clear();
        }
        pollIds_.clear();
        pollAll_ = false;
        pollPending_ = false;
    }
}

void App::pollCycle(const std::set<std::string>& only) {
    const Config cfg = config();
    {
        const std::lock_guard lock(mu_);
        lastPoll_ = std::time(nullptr);
    }
    polling_ = true;
    Http http(cfg.userAgent);
    DiscordWebhook webhook(http);
    for (const FeedConfig& fc : cfg.feeds) {
        if (g_stop) break;
        if (only.empty() ? !fc.enabled : !only.contains(fc.id)) continue;
        try {
            pollFeed(cfg, fc, http, webhook);
        } catch (const std::exception& e) {
            logError(std::format("[{}] {}", fc.name.empty() ? fc.url : fc.name, e.what()));
            setStatus(fc.id, false, e.what());
        }
    }
    if (!dryRun_) {
        const std::lock_guard lock(mu_);
        state_.save();
    }
    polling_ = false;
}

void App::pollFeed(const Config& cfg, const FeedConfig& fc, Http& http, DiscordWebhook& webhook) {
    const std::string label = fc.name.empty() ? fc.url : fc.name;

    std::vector<const WebhookConfig*> targets;
    for (const auto& id : fc.webhooks)
        if (const WebhookConfig* w = cfg.findWebhook(id)) targets.push_back(w);
    if (targets.empty()) {
        setStatus(fc.id, false, "no webhook selected");
        return;
    }

    std::vector<std::string> headers{
        "Accept: application/rss+xml, application/atom+xml, application/xml;q=0.9, text/xml;q=0.8, */*;q=0.5"};
    {
        const std::lock_guard lock(mu_);
        bool allInitialized = true;
        for (const WebhookConfig* t : targets) allInitialized = allInitialized && state_.target(fc.url, t->id).initialized;
        // A newly added webhook needs the full feed, so only ask "changed?" when every target has history.
        const FeedCache& cache = state_.cache(fc.url);
        if (allInitialized && !dryRun_) {
            if (!cache.etag.empty()) headers.push_back("If-None-Match: " + cache.etag);
            if (!cache.lastModified.empty()) headers.push_back("If-Modified-Since: " + cache.lastModified);
        }
    }

    const HttpResponse r = http.get(fc.url, headers);
    if (!r.error.empty()) {
        logWarn(std::format("[{}] fetch failed: {}", label, r.error));
        setStatus(fc.id, false, "fetch failed: " + r.error);
        return;
    }
    if (r.status == 304) {
        logDebug(std::format("[{}] not modified", label));
        setStatus(fc.id, true, "up to date");
        return;
    }
    if (r.status < 200 || r.status >= 300) {
        logWarn(std::format("[{}] fetch failed: HTTP {}", label, r.status));
        setStatus(fc.id, false, std::format("fetch failed: HTTP {}", r.status));
        return;
    }

    std::string error;
    const auto feed = parseFeed(r.body, fc.url, error);
    if (!feed) {
        logWarn(std::format("[{}] {}", label, error));
        setStatus(fc.id, false, error);
        return;
    }

    // Keep enough history that items still listed in the feed are never forgotten.
    const std::size_t keep = std::max<std::size_t>(1000, feed->items.size() * 3);
    bool complete = true;
    std::size_t posted = 0;
    std::string failure;

    for (const WebhookConfig* t : targets) {
        if (g_stop) {
            complete = false;
            break;
        }
        const std::string whName = t->name.empty() ? t->id : t->name;
        const std::string url = t->resolvedUrl();
        if (url.empty()) {
            failure = std::format("webhook \"{}\" has no URL ({} is not set)", whName, t->url.substr(4));
            logWarn(std::format("[{}] {}", label, failure));
            complete = false;
            continue;
        }

        std::vector<const FeedItem*> fresh;
        bool firstRun = false;
        {
            const std::lock_guard lock(mu_);
            const TargetState& ts = state_.target(fc.url, t->id);
            std::unordered_set<std::string> inThisDocument;
            for (const FeedItem& item : feed->items)
                if (!ts.isSeen(item.id) && inThisDocument.insert(item.id).second) fresh.push_back(&item);
            firstRun = !ts.initialized && !dryRun_;
        }

        // Post oldest first so the channel reads chronologically.
        const bool allDated = std::ranges::all_of(fresh, [](const FeedItem* i) { return i->date.has_value(); });
        if (allDated) std::ranges::stable_sort(fresh, {}, [](const FeedItem* i) { return *i->date; });
        else std::ranges::reverse(fresh);  // feeds list newest first

        const std::size_t limit = static_cast<std::size_t>(firstRun ? cfg.firstRunPosts : cfg.maxPostsPerPoll);
        {
            const std::lock_guard lock(mu_);
            TargetState& ts = state_.target(fc.url, t->id);
            if (fresh.size() > limit) {
                const std::size_t skip = fresh.size() - limit;
                for (std::size_t k = 0; k < skip; ++k) ts.markSeen(fresh[k]->id, keep);
                if (firstRun)
                    logInfo(std::format("[{} -> {}] first run: marked {} existing item(s) as seen", label, whName, skip));
                else
                    logInfo(std::format("[{} -> {}] {} new items, skipping the {} oldest (max_posts_per_poll = {})",
                                        label, whName, fresh.size(), skip, limit));
                fresh.erase(fresh.begin(), fresh.begin() + static_cast<std::ptrdiff_t>(skip));
            }
            ts.initialized = true;
        }

        for (const FeedItem* item : fresh) {
            if (g_stop) {
                complete = false;
                break;
            }
            const auto payload = buildPayload(*feed, *item, fc, *t, cfg);
            if (dryRun_) {
                std::cout << "--- " << label << " -> " << whName << '\n'
                          << payload.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
                const std::lock_guard lock(mu_);
                state_.target(fc.url, t->id).markSeen(item->id, keep);
                continue;
            }
            std::string sendError;
            const SendResult res = webhook.send(url, t->threadId, payload, &sendError);
            if (res == SendResult::RetryLater) {
                failure = std::format("posting to \"{}\" failed: {}", whName, sendError);
                logWarn(std::format("[{}] {}; will retry next poll", label, failure));
                complete = false;
                break;
            }
            {
                const std::lock_guard lock(mu_);
                state_.target(fc.url, t->id).markSeen(item->id, keep);
                if (res == SendResult::Sent) {
                    state_.target(fc.url, t->id).markPosted(item->id, std::time(nullptr), keep);
                    posts_.push_front({std::time(nullptr), label, whName, item->title, item->link});
                    if (posts_.size() > 100) posts_.pop_back();
                }
                state_.save();
            }
            if (res == SendResult::Sent) {
                ++posted;
                logInfo(std::format("[{} -> {}] posted: {}", label, whName, item->title));
            } else {
                logWarn(std::format("[{} -> {}] skipped an item Discord would not accept: {}", label, whName, item->title));
            }
            interruptibleSleep(std::chrono::milliseconds(cfg.postDelayMs));
        }
    }

    if (complete) {
        // Only remember validators once everything in this version of the feed is handled.
        const std::lock_guard lock(mu_);
        FeedCache& cache = state_.cache(fc.url);
        cache.etag = r.header("etag");
        cache.lastModified = r.header("last-modified");
    }
    std::string message = !complete ? (failure.empty() ? std::string("interrupted") : failure)
                          : posted > 0 ? std::format("posted {} item{}", posted, posted == 1 ? "" : "s")
                                       : std::string("up to date");
    setStatus(fc.id, complete, std::move(message), feed->title, feed->items.size());
}

// ---- manual reposts ---------------------------------------------------------

std::string App::queueRepost(const std::string& feedId, std::vector<std::string> itemIds,
                             std::vector<std::string> webhooks) {
    std::string id;
    {
        const std::lock_guard lock(mu_);
        FeedConfig* fc = cfg_.findFeed(feedId);
        if (fc == nullptr) throw std::runtime_error("unknown feed");
        for (const auto& w : webhooks)
            if (cfg_.findWebhook(w) == nullptr) throw std::runtime_error("unknown webhook \"" + w + "\"");
        if (webhooks.empty() && fc->webhooks.empty()) throw std::runtime_error("this feed has no webhook to post to");

        RepostJob job;
        job.id = id = newId("job");
        job.feedId = feedId;
        job.feedName = fc->name.empty() ? fc->url : fc->name;
        job.itemIds = std::move(itemIds);
        job.webhooks = std::move(webhooks);
        job.created = std::time(nullptr);
        jobs_[id] = std::move(job);
        jobOrder_.push_back(id);
        jobQueue_.push_back(id);
        // Keep the last 30 jobs for the dashboard.
        while (jobOrder_.size() > 30) {
            const RepostJob& old = jobs_.at(jobOrder_.front());
            if (old.state == "queued" || old.state == "running") break;
            jobs_.erase(jobOrder_.front());
            jobOrder_.pop_front();
        }
    }
    jobCv_.notify_all();
    return id;
}

bool App::cancelJob(const std::string& jobId) {
    const std::lock_guard lock(mu_);
    const auto it = jobs_.find(jobId);
    if (it == jobs_.end()) return false;
    RepostJob& job = it->second;
    if (job.state == "queued") {
        std::erase(jobQueue_, jobId);
        job.state = "cancelled";
        job.finished = std::time(nullptr);
    } else if (job.state == "running") {
        job.cancel = true;
    }
    return true;
}

nlohmann::json App::itemStatus(const std::string& feedUrl, const std::vector<std::string>& itemIds,
                               const std::vector<std::string>& webhookIds) const {
    const std::lock_guard lock(mu_);
    nlohmann::json out = nlohmann::json::array();
    for (const auto& id : itemIds) {
        nlohmann::json row = nlohmann::json::object();
        for (const auto& w : webhookIds) {
            std::string state = "new";
            std::time_t when = 0;
            if (const TargetState* ts = state_.findTarget(feedUrl, w)) {
                if (const auto p = ts->postedAt.find(id); p != ts->postedAt.end()) {
                    state = "posted";
                    when = p->second;
                } else if (ts->isSeen(id)) {
                    state = "skipped";
                }
            }
            row[w] = {{"state", state}, {"time", when}};
        }
        out.push_back(std::move(row));
    }
    return out;
}

void App::workerLoop() {
    std::unique_lock lock(mu_);
    while (true) {
        jobCv_.wait(lock, [&] { return workerStop_ || !jobQueue_.empty(); });
        if (workerStop_) return;
        const std::string id = jobQueue_.front();
        jobQueue_.pop_front();
        lock.unlock();
        try {
            runJob(id);
        } catch (const std::exception& e) {
            const std::lock_guard guard(mu_);
            RepostJob& job = jobs_.at(id);
            job.state = "failed";
            job.message = e.what();
            job.finished = std::time(nullptr);
        }
        lock.lock();
    }
}

void App::runJob(const std::string& jobId) {
    const Config cfg = config();
    std::string feedId;
    std::vector<std::string> itemIds, webhookIds;
    {
        const std::lock_guard lock(mu_);
        RepostJob& job = jobs_.at(jobId);
        if (job.cancel) {
            job.state = "cancelled";
            job.finished = std::time(nullptr);
            return;
        }
        job.state = "running";
        feedId = job.feedId;
        itemIds = job.itemIds;
        webhookIds = job.webhooks;
    }
    const auto finish = [&](const std::string& state, const std::string& message) {
        const std::lock_guard lock(mu_);
        RepostJob& job = jobs_.at(jobId);
        job.state = state;
        if (!message.empty()) job.message = message;
        job.current.clear();
        job.finished = std::time(nullptr);
    };

    const FeedConfig* fc = nullptr;
    for (const auto& f : cfg.feeds)
        if (f.id == feedId) fc = &f;
    if (fc == nullptr) return finish("failed", "the feed was deleted");

    std::vector<const WebhookConfig*> targets;
    for (const auto& id : webhookIds.empty() ? fc->webhooks : webhookIds)
        if (const WebhookConfig* w = cfg.findWebhook(id)) targets.push_back(w);
    if (targets.empty()) return finish("failed", "no webhook to post to");

    Http http(cfg.userAgent);
    DiscordWebhook webhook(http);
    const HttpResponse r = http.get(fc->url, {"Accept: application/rss+xml, application/atom+xml, application/xml;q=0.9, */*;q=0.5"});
    if (!r.error.empty()) return finish("failed", "fetch failed: " + r.error);
    if (r.status < 200 || r.status >= 300) return finish("failed", std::format("fetch failed: HTTP {}", r.status));
    std::string error;
    const auto feed = parseFeed(r.body, fc->url, error);
    if (!feed) return finish("failed", error);

    std::vector<const FeedItem*> items;
    std::unordered_set<std::string> want(itemIds.begin(), itemIds.end()), added;
    for (const FeedItem& item : feed->items)
        if ((want.empty() || want.contains(item.id)) && added.insert(item.id).second) items.push_back(&item);
    if (items.empty()) return finish("failed", "those posts are no longer in the feed");

    const bool allDated = std::ranges::all_of(items, [](const FeedItem* i) { return i->date.has_value(); });
    if (allDated) std::ranges::stable_sort(items, {}, [](const FeedItem* i) { return *i->date; });
    else std::ranges::reverse(items);

    const std::string label = fc->name.empty() ? (feed->title.empty() ? fc->url : feed->title) : fc->name;
    const std::size_t keep = std::max<std::size_t>(1000, feed->items.size() * 3);
    {
        const std::lock_guard lock(mu_);
        jobs_.at(jobId).total = items.size() * targets.size();
    }
    logInfo(std::format("[{}] reposting {} post(s) to {} webhook(s)", label, items.size(), targets.size()));

    for (const FeedItem* item : items) {
        for (const WebhookConfig* t : targets) {
            {
                const std::lock_guard lock(mu_);
                RepostJob& job = jobs_.at(jobId);
                if (job.cancel || workerStop_ || g_stop) {
                    job.state = "cancelled";
                    job.current.clear();
                    job.finished = std::time(nullptr);
                    logInfo(std::format("[{}] repost cancelled after {} of {}", label, job.done, job.total));
                    return;
                }
                job.current = item->title;
            }
            const std::string whName = t->name.empty() ? t->id : t->name;
            const std::string url = t->resolvedUrl();
            bool ok = false;
            std::string sendError;
            if (url.empty()) {
                sendError = "webhook \"" + whName + "\" has no URL";
            } else {
                const auto payload = buildPayload(*feed, *item, *fc, *t, cfg);
                if (dryRun_) {
                    std::cout << "--- repost " << label << " -> " << whName << '\n'
                              << payload.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << std::endl;
                    ok = true;
                } else {
                    ok = webhook.send(url, t->threadId, payload, &sendError) == SendResult::Sent;
                }
            }
            {
                const std::lock_guard lock(mu_);
                RepostJob& job = jobs_.at(jobId);
                if (ok) {
                    ++job.done;
                    state_.target(fc->url, t->id).markPosted(item->id, std::time(nullptr), keep);
                    posts_.push_front({std::time(nullptr), label + " (repost)", whName, item->title, item->link});
                    if (posts_.size() > 100) posts_.pop_back();
                    if (!dryRun_) state_.save();
                } else {
                    ++job.failed;
                    job.message = sendError;
                }
            }
            if (ok) logInfo(std::format("[{} -> {}] reposted: {}", label, whName, item->title));
            else logWarn(std::format("[{} -> {}] repost failed: {}", label, whName, sendError));
            interruptibleSleep(std::chrono::milliseconds(cfg.postDelayMs));
        }
    }

    std::size_t done = 0, failed = 0;
    std::string lastError;
    {
        const std::lock_guard lock(mu_);
        const RepostJob& job = jobs_.at(jobId);
        done = job.done;
        failed = job.failed;
        lastError = job.message;
    }
    if (failed == 0) finish("done", std::format("reposted {}", done));
    else finish(done == 0 ? "failed" : "done", std::format("{} sent, {} failed: {}", done, failed, lastError));
}

}  // namespace rssbot
