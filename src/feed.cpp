#include "feed.hpp"

#include "util.hpp"

#include <pugixml.hpp>

#include <array>
#include <string_view>

namespace rssbot {
namespace {

// Element name without its namespace prefix ("dc:creator" -> "creator").
std::string_view localName(const char* name) {
    const std::string_view s(name);
    const auto colon = s.find(':');
    return colon == std::string_view::npos ? s : s.substr(colon + 1);
}

bool isElement(const pugi::xml_node& n, std::string_view name) {
    return n.type() == pugi::node_element && localName(n.name()) == name;
}

pugi::xml_node child(const pugi::xml_node& parent, std::string_view name) {
    for (const pugi::xml_node c : parent.children())
        if (isElement(c, name)) return c;
    return {};
}

// Text of a node. Child elements (Atom type="xhtml") are re-serialised as markup so
// htmlToText can turn them into line breaks and images can still be found.
void collectContent(const pugi::xml_node& n, std::string& out) {
    for (const pugi::xml_node c : n.children()) {
        if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) {
            out += c.value();
        } else if (c.type() == pugi::node_element) {
            out += '<';
            out += localName(c.name());
            for (const pugi::xml_attribute a : c.attributes()) {
                out += ' ';
                out += a.name();
                out += "=\"";
                for (const char ch : std::string_view(a.value())) {
                    if (ch == '"') out += "&quot;";
                    else if (ch == '&') out += "&amp;";
                    else out += ch;
                }
                out += '"';
            }
            out += '>';
            collectContent(c, out);
            out += "</";
            out += localName(c.name());
            out += '>';
        }
    }
}

std::string content(const pugi::xml_node& n) {
    std::string out;
    if (n) collectContent(n, out);
    return out;
}

std::string text(const pugi::xml_node& n) { return trim(content(n)); }

std::string findLink(const pugi::xml_node& n, const std::string& base) {
    std::string fallback;
    for (const pugi::xml_node c : n.children()) {
        if (!isElement(c, "link")) continue;
        // RSS: <link>url</link>
        if (std::string t = trim(c.text().get()); !t.empty()) return resolveUrl(base, t);
        // Atom: <link rel="alternate" href="url"/>
        const std::string href = trim(c.attribute("href").value());
        const std::string_view rel = c.attribute("rel").value();
        if (href.empty()) continue;
        if (rel.empty() || rel == "alternate") return resolveUrl(base, href);
        if (fallback.empty() && rel != "self" && rel != "enclosure" && rel != "hub" && rel != "replies")
            fallback = resolveUrl(base, href);
    }
    return fallback;
}

std::string findAuthor(const pugi::xml_node& item) {
    if (const pugi::xml_node a = child(item, "author")) {
        if (const pugi::xml_node name = child(a, "name")) return htmlToText(text(name));
        // RSS: "editor@example.com (Real Name)"
        const std::string s = text(a);
        const auto open = s.find('('), close = s.rfind(')');
        if (open != std::string::npos && close != std::string::npos && close > open)
            return trim(s.substr(open + 1, close - open - 1));
        return htmlToText(s);
    }
    if (const pugi::xml_node c = child(item, "creator")) return htmlToText(text(c));
    return {};
}

bool isImageType(std::string_view type) { return type.starts_with("image/"); }

std::string findMediaImage(const pugi::xml_node& n) {
    for (const pugi::xml_node c : n.children()) {
        if (c.type() != pugi::node_element) continue;
        const std::string_view name = localName(c.name());
        if (name == "thumbnail" && c.attribute("url")) return c.attribute("url").value();
        if (name == "content" && c.attribute("url") &&
            (std::string_view(c.attribute("medium").value()) == "image" || isImageType(c.attribute("type").value())))
            return c.attribute("url").value();
        if (name == "enclosure" && isImageType(c.attribute("type").value())) return c.attribute("url").value();
        if (name == "link" && std::string_view(c.attribute("rel").value()) == "enclosure" &&
            isImageType(c.attribute("type").value()))
            return c.attribute("href").value();
        if (name == "group") {
            if (std::string s = findMediaImage(c); !s.empty()) return s;
        }
    }
    return {};
}

// First <img src="..."> in an HTML fragment.
std::string firstImgSrc(const std::string& html) {
    const std::string lower = toLower(html);
    const auto tag = lower.find("<img");
    if (tag == std::string::npos) return {};
    const auto end = lower.find('>', tag);
    auto src = lower.find(" src=", tag);
    if (src == std::string::npos || (end != std::string::npos && src > end)) return {};
    src += 5;
    if (src >= html.size()) return {};
    const char quote = html[src];
    if (quote == '"' || quote == '\'') {
        const auto close = html.find(quote, src + 1);
        if (close == std::string::npos) return {};
        return decodeEntities(html.substr(src + 1, close - src - 1));
    }
    const auto stop = html.find_first_of(" \t\r\n>", src);
    return decodeEntities(html.substr(src, stop == std::string::npos ? std::string::npos : stop - src));
}

FeedItem parseItem(const pugi::xml_node& n, const std::string& base) {
    FeedItem item;
    item.title = htmlToText(text(child(n, "title")));
    item.link = findLink(n, base);
    item.author = findAuthor(n);

    pugi::xml_node idNode = child(n, "guid");
    if (!idNode) idNode = child(n, "id");
    item.id = text(idNode);

    for (const char* name : {"pubDate", "published", "updated", "date", "issued", "modified"}) {
        if (const pugi::xml_node d = child(n, name)) {
            item.date = parseDate(text(d));
            if (item.date) break;
        }
    }

    std::string html;
    for (const char* name : {"description", "summary", "encoded", "content"}) {
        const pugi::xml_node c = child(n, name);
        if (!c || c.attribute("url")) continue;  // skip media:content
        html = content(c);
        if (!trim(html).empty()) break;
    }
    item.summary = htmlToText(html);

    std::string image = findMediaImage(n);
    if (image.empty()) image = firstImgSrc(html);
    if (image.empty()) {
        // Look in the full content too (content:encoded often has the images).
        if (const pugi::xml_node c = child(n, "encoded")) image = firstImgSrc(content(c));
    }
    item.imageUrl = image.empty() ? image : resolveUrl(item.link.empty() ? base : item.link, image);

    if (item.id.empty()) item.id = !item.link.empty() ? item.link : item.title + "|" + text(child(n, "pubDate"));
    return item;
}

}  // namespace

std::optional<Feed> parseFeed(const std::string& xml, const std::string& baseUrl, std::string& error) {
    pugi::xml_document doc;
    const pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size(), pugi::parse_default);
    if (!result) {
        error = std::string("XML parse error: ") + result.description() + " at offset " +
                std::to_string(result.offset);
        return std::nullopt;
    }

    const pugi::xml_node root = doc.document_element();
    const std::string_view rootName = localName(root.name());
    pugi::xml_node channel, itemParent;
    std::string_view itemName = "item";
    if (rootName == "rss") {
        channel = itemParent = child(root, "channel");
    } else if (rootName == "RDF") {  // RSS 1.0: items are siblings of <channel>
        channel = child(root, "channel");
        itemParent = root;
    } else if (rootName == "feed") {  // Atom
        channel = itemParent = root;
        itemName = "entry";
    } else {
        error = "not an RSS or Atom feed (root element <" + std::string(root.name()) + ">)";
        return std::nullopt;
    }
    if (!itemParent) {
        error = "feed has no <channel>";
        return std::nullopt;
    }

    Feed feed;
    feed.title = htmlToText(text(child(channel, "title")));
    feed.link = findLink(channel, baseUrl);
    for (const pugi::xml_node c : itemParent.children())
        if (isElement(c, itemName)) feed.items.push_back(parseItem(c, baseUrl));
    return feed;
}

}  // namespace rssbot
