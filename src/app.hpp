#pragma once

#include "config.hpp"
#include "discord.hpp"
#include "state.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace rssbot {

struct FeedStatus {
    std::time_t lastCheck = 0;
    bool ok = true;
    std::string message;
    std::string title;
    std::size_t items = 0;
};

struct PostRecord {
    std::time_t time = 0;
    std::string feed;
    std::string webhook;
    std::string title;
    std::string link;
};

// A manual repost started from the dashboard; runs on the App's worker thread.
struct RepostJob {
    std::string id;
    std::string feedId;
    std::string feedName;
    std::vector<std::string> itemIds;   // empty: every post in the feed
    std::vector<std::string> webhooks;  // empty: the feed's webhooks
    std::string state = "queued";       // queued, running, done, failed, cancelled
    std::string message;
    std::string current;
    std::size_t total = 0;
    std::size_t done = 0;
    std::size_t failed = 0;
    bool cancel = false;
    std::time_t created = 0;
    std::time_t finished = 0;
};

// Owns the live config, the seen-item state and the polling loop. The dashboard
// talks to it from its own threads; everything shared is guarded by mu_ and the
// network work happens outside the lock.
class App {
public:
    App(Config config, bool dryRun);
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void run(bool once);                              // blocks until g_stop (or one round)
    void requestPoll(const std::string& feedId = {});  // empty: all feeds

    Config config() const;
    // Applies mutate to a copy, validates it, writes config.json, then swaps it in.
    void updateConfig(const std::function<void(Config&)>& mutate);
    nlohmann::json status() const;

    // Manual reposts. Returns the job id; throws std::runtime_error for unknown feed/webhook.
    std::string queueRepost(const std::string& feedId, std::vector<std::string> itemIds,
                            std::vector<std::string> webhooks);
    bool cancelJob(const std::string& jobId);
    // Per item, per webhook: {"state": "posted"|"skipped"|"new", "time": unix}.
    nlohmann::json itemStatus(const std::string& feedUrl, const std::vector<std::string>& itemIds,
                              const std::vector<std::string>& webhookIds) const;

private:
    void workerLoop();
    void runJob(const std::string& jobId);
    void pollCycle(const std::set<std::string>& only);
    void pollFeed(const Config& cfg, const FeedConfig& fc, Http& http, DiscordWebhook& webhook);
    void setStatus(const std::string& feedId, bool ok, std::string message, const std::string& title = {},
                   std::size_t items = 0);

    mutable std::mutex mu_;
    Config cfg_;
    StateStore state_;
    const bool dryRun_;
    std::map<std::string, FeedStatus> status_;
    std::deque<PostRecord> posts_;
    bool pollAll_ = false;
    std::set<std::string> pollIds_;
    std::atomic<bool> pollPending_{false};
    std::atomic<bool> polling_{false};
    std::time_t lastPoll_ = 0;
    std::time_t nextPoll_ = 0;

    std::map<std::string, RepostJob> jobs_;
    std::deque<std::string> jobOrder_;  // oldest first, for pruning
    std::deque<std::string> jobQueue_;
    std::condition_variable jobCv_;
    bool workerStop_ = false;
    std::thread worker_;  // last: started after everything above exists
};

}  // namespace rssbot
