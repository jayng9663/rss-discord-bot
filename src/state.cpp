#include "state.hpp"

#include "util.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>

namespace rssbot {

bool TargetState::isSeen(const std::string& id) const { return seenSet.contains(id); }

void TargetState::markSeen(const std::string& id, std::size_t maxKeep) {
    if (!seenSet.insert(id).second) return;
    seen.push_back(id);
    while (seen.size() > maxKeep) {
        seenSet.erase(seen.front());
        postedAt.erase(seen.front());
        seen.pop_front();
    }
}

void TargetState::markPosted(const std::string& id, std::time_t when, std::size_t maxKeep) {
    markSeen(id, maxKeep);
    if (seenSet.contains(id)) postedAt[id] = when;
}

StateStore::StateStore(std::string path) : path_(std::move(path)) {}

void StateStore::load() {
    std::ifstream in(path_);
    if (!in) return;  // first run
    const auto j = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        logWarn("state file " + path_ + " is unreadable, starting fresh");
        return;
    }
    const auto readSeen = [](const nlohmann::json& obj, auto&& fn) {
        if (const auto s = obj.find("seen"); s != obj.end() && s->is_array())
            for (const auto& id : *s)
                if (id.is_string()) fn(id.get<std::string>());
    };

    if (const auto feeds = j.find("feeds"); feeds != j.end() && feeds->is_object()) {
        for (const auto& entry : feeds->items()) {
            const auto& fj = entry.value();
            if (!fj.is_object()) continue;
            FeedCache& cache = caches_[entry.key()];
            if (const auto e = fj.find("etag"); e != fj.end() && e->is_string()) cache.etag = e->get<std::string>();
            if (const auto m = fj.find("last_modified"); m != fj.end() && m->is_string())
                cache.lastModified = m->get<std::string>();
            readSeen(fj, [&](std::string id) { legacySeen_[entry.key()].push_back(std::move(id)); });
        }
    }
    if (const auto targets = j.find("targets"); targets != j.end() && targets->is_object()) {
        for (const auto& entry : targets->items()) {
            if (!entry.value().is_object()) continue;
            TargetState& st = targets_[entry.key()];
            st.initialized = true;
            readSeen(entry.value(), [&](const std::string& id) { st.markSeen(id, SIZE_MAX); });
            if (const auto p = entry.value().find("posted"); p != entry.value().end() && p->is_object())
                for (const auto& e : p->items())
                    if (e.value().is_number_integer() && st.seenSet.contains(e.key()))
                        st.postedAt[e.key()] = e.value().get<std::time_t>();
        }
    }
}

bool StateStore::save() const {
    nlohmann::json feeds = nlohmann::json::object();
    for (const auto& [url, cache] : caches_)
        feeds[url] = {{"etag", cache.etag}, {"last_modified", cache.lastModified}};
    nlohmann::json targets = nlohmann::json::object();
    for (const auto& [key, st] : targets_)
        if (st.initialized) targets[key] = {{"seen", st.seen}, {"posted", st.postedAt}};
    const nlohmann::json j = {{"version", 2}, {"feeds", feeds}, {"targets", targets}};

    std::error_code ec;
    const std::filesystem::path target(path_);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);

    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
        out.flush();
        if (!out) {
            logError("cannot write state file " + tmp);
            return false;
        }
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        logError("cannot replace state file " + path_ + ": " + ec.message());
        return false;
    }
    return true;
}

FeedCache& StateStore::cache(const std::string& feedUrl) { return caches_[feedUrl]; }

const TargetState* StateStore::findTarget(const std::string& feedUrl, const std::string& webhookId) const {
    const auto it = targets_.find(webhookId + "|" + feedUrl);
    return it == targets_.end() ? nullptr : &it->second;
}

TargetState& StateStore::target(const std::string& feedUrl, const std::string& webhookId) {
    const std::string key = webhookId + "|" + feedUrl;
    auto it = targets_.find(key);
    if (it != targets_.end()) return it->second;
    TargetState& st = targets_[key];
    // Upgrade from a version 1 state file: that history belonged to the single webhook.
    if (const auto legacy = legacySeen_.find(feedUrl); legacy != legacySeen_.end()) {
        for (const auto& id : legacy->second) st.markSeen(id, SIZE_MAX);
        st.initialized = true;
    }
    return st;
}

}  // namespace rssbot
