#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace rssbot {

struct FeedItem {
    std::string id;        // guid / atom:id, falling back to the link
    std::string title;     // plain text
    std::string link;
    std::string summary;   // plain text, HTML stripped
    std::string author;
    std::string imageUrl;
    std::optional<std::time_t> date;
};

struct Feed {
    std::string title;
    std::string link;
    std::vector<FeedItem> items;
};

// Parses RSS 2.0, RSS 1.0 (RDF) and Atom 1.0. baseUrl (the feed URL) resolves relative links.
std::optional<Feed> parseFeed(const std::string& xml, const std::string& baseUrl, std::string& error);

}  // namespace rssbot
