#pragma once

#include <ctime>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rssbot {

// HTTP validators per feed URL (conditional GET).
struct FeedCache {
    std::string etag;
    std::string lastModified;
};

// What has been posted for one feed -> webhook pair.
struct TargetState {
    bool initialized = false;      // false until the first successful fetch for this pair
    std::deque<std::string> seen;  // insertion order, oldest first
    std::unordered_set<std::string> seenSet;
    std::unordered_map<std::string, std::time_t> postedAt;  // seen + actually sent (vs. skipped)

    bool isSeen(const std::string& id) const;
    void markSeen(const std::string& id, std::size_t maxKeep);
    void markPosted(const std::string& id, std::time_t when, std::size_t maxKeep);
};

// Remembers which items were already posted where, so restarts never repost.
class StateStore {
public:
    explicit StateStore(std::string path);
    void load();
    bool save() const;  // atomic: write to a temp file, then rename
    FeedCache& cache(const std::string& feedUrl);
    TargetState& target(const std::string& feedUrl, const std::string& webhookId);
    const TargetState* findTarget(const std::string& feedUrl, const std::string& webhookId) const;

private:
    std::string path_;
    std::map<std::string, FeedCache> caches_;
    std::map<std::string, TargetState> targets_;          // key: webhookId + "|" + feedUrl
    std::map<std::string, std::vector<std::string>> legacySeen_;  // version 1 files, per feed URL
};

}  // namespace rssbot
