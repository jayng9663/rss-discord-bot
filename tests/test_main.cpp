// Minimal self-contained tests (no framework needed): ctest or ./build/rssbot_tests
#include "config.hpp"
#include "discord.hpp"
#include "feed.hpp"
#include "util.hpp"

#include <iostream>
#include <string>

using namespace rssbot;

namespace {
int failures = 0;
int checks = 0;
}  // namespace

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        ++checks;                                                                          \
        if (!(cond)) {                                                                     \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond "\n";    \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(actual, expected)                                                         \
    do {                                                                                   \
        ++checks;                                                                          \
        const auto a_ = (actual);                                                          \
        const auto e_ = (expected);                                                        \
        if (!(a_ == e_)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": " #actual "\n  expected: ["   \
                      << e_ << "]\n  actual:   [" << a_ << "]\n";                          \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

static void testRss2() {
    const std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<rss version="2.0" xmlns:atom="http://www.w3.org/2005/Atom" xmlns:dc="http://purl.org/dc/elements/1.1/"
     xmlns:content="http://purl.org/rss/1.0/modules/content/">
  <channel>
    <title>Example &amp; Co</title>
    <atom:link href="https://example.com/feed.xml" rel="self" type="application/rss+xml"/>
    <link>https://example.com/</link>
    <item>
      <title>Second post</title>
      <link>https://example.com/2</link>
      <guid isPermaLink="false">post-2</guid>
      <pubDate>Mon, 02 Nov 2026 08:53:00 +0000</pubDate>
      <dc:creator>Jay</dc:creator>
      <description><![CDATA[<p>Hello &amp; <b>welcome</b></p><p>Line two<img src="/img/a.png"></p>]]></description>
    </item>
    <item>
      <title>First post</title>
      <link>/1</link>
      <author>jay@example.com (Jay Doe)</author>
      <pubDate>Sun, 01 Nov 2026 10:00:00 PST</pubDate>
      <enclosure url="https://cdn.example.com/1.jpg" type="image/jpeg" length="1"/>
    </item>
  </channel>
</rss>)";
    std::string err;
    const auto feed = parseFeed(xml, "https://example.com/feed.xml", err);
    CHECK(feed.has_value());
    if (!feed) return;
    CHECK_EQ(feed->title, std::string("Example & Co"));
    CHECK_EQ(feed->link, std::string("https://example.com/"));
    CHECK_EQ(feed->items.size(), std::size_t{2});

    const FeedItem& a = feed->items[0];
    CHECK_EQ(a.id, std::string("post-2"));
    CHECK_EQ(a.author, std::string("Jay"));
    CHECK_EQ(a.summary, std::string("Hello & welcome\n\nLine two"));
    CHECK_EQ(a.imageUrl, std::string("https://example.com/img/a.png"));
    CHECK(a.date.has_value());
    if (a.date) CHECK_EQ(toIso8601(*a.date), std::string("2026-11-02T08:53:00Z"));

    const FeedItem& b = feed->items[1];
    CHECK_EQ(b.link, std::string("https://example.com/1"));
    CHECK_EQ(b.id, std::string("https://example.com/1"));  // no guid: falls back to link
    CHECK_EQ(b.author, std::string("Jay Doe"));
    CHECK_EQ(b.imageUrl, std::string("https://cdn.example.com/1.jpg"));
    if (b.date) CHECK_EQ(toIso8601(*b.date), std::string("2026-11-01T18:00:00Z"));
}

static void testAtom() {
    const std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom" xmlns:media="http://search.yahoo.com/mrss/">
  <title type="text">Atom Example</title>
  <link rel="self" href="https://blog.example.org/atom.xml"/>
  <link rel="alternate" href="https://blog.example.org/"/>
  <entry>
    <title>Release v1.2.0</title>
    <id>tag:blog.example.org,2026:1</id>
    <link rel="alternate" type="text/html" href="/releases/v1.2.0"/>
    <updated>2026-11-02T00:53:00-08:00</updated>
    <author><name>Octo Cat</name><email>o@example.org</email></author>
    <media:thumbnail url="https://blog.example.org/thumb.png"/>
    <content type="html">&lt;p&gt;Bug fixes &amp;amp; &lt;em&gt;speed&lt;/em&gt;&lt;/p&gt;</content>
  </entry>
</feed>)";
    std::string err;
    const auto feed = parseFeed(xml, "https://blog.example.org/atom.xml", err);
    CHECK(feed.has_value());
    if (!feed) return;
    CHECK_EQ(feed->title, std::string("Atom Example"));
    CHECK_EQ(feed->link, std::string("https://blog.example.org/"));
    CHECK_EQ(feed->items.size(), std::size_t{1});
    if (feed->items.empty()) return;
    const FeedItem& e = feed->items[0];
    CHECK_EQ(e.id, std::string("tag:blog.example.org,2026:1"));
    CHECK_EQ(e.link, std::string("https://blog.example.org/releases/v1.2.0"));
    CHECK_EQ(e.author, std::string("Octo Cat"));
    CHECK_EQ(e.summary, std::string("Bug fixes & speed"));
    CHECK_EQ(e.imageUrl, std::string("https://blog.example.org/thumb.png"));
    if (e.date) CHECK_EQ(toIso8601(*e.date), std::string("2026-11-02T08:53:00Z"));
}

static void testRdfAndErrors() {
    const std::string xml = R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns="http://purl.org/rss/1.0/">
  <channel><title>RDF</title><link>https://rdf.example/</link></channel>
  <item><title>One</title><link>https://rdf.example/1</link></item>
</rdf:RDF>)";
    std::string err;
    const auto feed = parseFeed(xml, "https://rdf.example/index.rdf", err);
    CHECK(feed.has_value());
    if (feed) CHECK_EQ(feed->items.size(), std::size_t{1});

    CHECK(!parseFeed("<html><body>nope</body></html>", "https://x/", err).has_value());
    CHECK(!err.empty());
    CHECK(!parseFeed("this is not xml <", "https://x/", err).has_value());
}

static void testText() {
    CHECK_EQ(htmlToText("<ul><li>a</li><li>b</li></ul>"), std::string("\xE2\x80\xA2 a\n\xE2\x80\xA2 b"));
    CHECK_EQ(htmlToText("x<script>alert(1)</script> y &#x41;&#66; 1 < 2"), std::string("x y AB 1 < 2"));
    CHECK_EQ(truncateChars("h\xC3\xA9llo world", 5), std::string("h\xC3\xA9ll\xE2\x80\xA6"));
    CHECK_EQ(truncateChars("short", 10), std::string("short"));
    CHECK_EQ(resolveUrl("https://a.example/blog/feed.xml", "post.html"), std::string("https://a.example/blog/post.html"));
    CHECK_EQ(resolveUrl("https://a.example/blog/feed.xml", "//cdn.example/x"), std::string("https://cdn.example/x"));
    CHECK_EQ(resolveUrl("https://a.example", "x"), std::string("https://a.example/x"));
    CHECK_EQ(resolveUrl("https://a.example/f", "mailto:me@x"), std::string("mailto:me@x"));
}

static void testDates() {
    const auto rfc = parseDate("Mon, 02 Nov 2026 00:53:00 -0800");
    const auto iso = parseDate("2026-11-02T08:53:00.250Z");
    CHECK(rfc.has_value() && iso.has_value());
    if (rfc && iso) CHECK_EQ(*rfc, *iso);
    CHECK(parseDate("2 Nov 2026 08:53 GMT").has_value());
    CHECK(!parseDate("yesterday-ish").has_value());
    CHECK(!parseDate("").has_value());
}

static void testPayload() {
    Feed feed;
    feed.title = "My Feed";
    feed.link = "https://example.com/";
    FeedItem item;
    item.title = std::string(300, 'x');
    item.link = "https://example.com/p";
    item.summary = "Summary text";
    item.imageUrl = "javascript:alert(1)";  // not http(s): must be dropped
    item.date = 0;
    FeedConfig fc;
    WebhookConfig wh;
    wh.username = "News";
    Config cfg;
    const auto p = buildPayload(feed, item, fc, wh, cfg);
    const auto& e = p["embeds"][0];
    CHECK_EQ(utf8Length(e["title"].get<std::string>()), std::size_t{256});
    CHECK_EQ(e["url"].get<std::string>(), std::string("https://example.com/p"));
    CHECK_EQ(e["author"]["name"].get<std::string>(), std::string("My Feed"));
    CHECK_EQ(e["timestamp"].get<std::string>(), std::string("1970-01-01T00:00:00Z"));
    CHECK(!e.contains("image"));
    CHECK(!p.contains("content"));
    CHECK(p["allowed_mentions"]["parse"].empty());
    CHECK_EQ(p["username"].get<std::string>(), std::string("News"));
    CHECK(!p.contains("avatar_url"));

    wh.content = "<@&123> new post";
    wh.avatarUrl = "https://cdn.example/a.png";
    const auto q = buildPayload(feed, item, fc, wh, cfg);
    CHECK_EQ(q["content"].get<std::string>(), wh.content);
    CHECK_EQ(q["avatar_url"].get<std::string>(), wh.avatarUrl);

    WebhookConfig plain;
    cfg.username = "Fallback";
    CHECK_EQ(buildPayload(feed, item, fc, plain, cfg)["username"].get<std::string>(), std::string("Fallback"));
    CHECK(!buildTestPayload(wh, cfg).contains("content"));  // tests never ping
    CHECK_EQ(q["allowed_mentions"]["parse"].size(), std::size_t{3});
}

static void testConfig() {
    using nlohmann::json;
    // Version 1 layout is upgraded: global webhook -> "default", per-feed webhook_url -> its own entry.
    const json v1 = {
        {"webhook_url", "https://discord.com/api/webhooks/1/abcdefgh"},
        {"feeds", json::array({"https://a.example/feed",
                               {{"url", "https://b.example/rss"}, {"name", "B"},
                                {"webhook_url", "https://discord.com/api/webhooks/2/zz"}, {"content", "<@&9>"}}})}};
    const Config c = configFromJson(v1, "/srv/bot/config.json");
    CHECK_EQ(c.webhooks.size(), std::size_t{2});
    CHECK_EQ(c.feeds.size(), std::size_t{2});
    if (c.feeds.size() == 2 && c.webhooks.size() == 2) {
        CHECK_EQ(c.feeds[0].webhooks.size(), std::size_t{1});
        CHECK_EQ(c.feeds[0].webhooks[0], std::string("default"));
        CHECK(c.feeds[1].webhooks[0] != "default");
        CHECK_EQ(c.webhooks[1].content, std::string("<@&9>"));
        CHECK(!c.feeds[0].id.empty());
    }
    CHECK_EQ(c.statePath(), std::string("/srv/bot/state.json"));

    // Round trip through the saved format keeps ids and links.
    const Config again = configFromJson(json::parse(configToJson(c).dump()), c.path);
    CHECK_EQ(again.feeds.size(), c.feeds.size());
    if (again.feeds.size() == 2) CHECK(again.feeds[1].webhooks == c.feeds[1].webhooks);

    // Empty config is fine with the dashboard on, an error without it.
    CHECK(configFromJson(json::object(), "x.json").feeds.empty());
    bool threw = false;
    try {
        configFromJson({{"dashboard", {{"enabled", false}}}}, "x.json");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // Validation catches bad webhooks and dangling references.
    Config bad = c;
    bad.webhooks[0].url = "https://example.com/not-a-webhook";
    threw = false;
    try { validateConfig(bad); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    bad = c;
    bad.feeds[0].webhooks = {"nope"};
    threw = false;
    try { validateConfig(bad); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    bad = c;
    bad.webhooks[0].username = "My Discord Bot";
    threw = false;
    try { validateConfig(bad); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    CHECK_EQ(maskWebhookUrl("https://discord.com/api/webhooks/123/abcdefghij"),
             std::string("https://discord.com/api/webhooks/123/abcd\xE2\x80\xA6"));
    CHECK_EQ(maskWebhookUrl("env:DISCORD_WEBHOOK_URL"), std::string("$DISCORD_WEBHOOK_URL"));
    CHECK_EQ(colorToHex(0x5865F2), std::string("#5865F2"));
    CHECK_EQ(parseColor(json("#ff6600")), 0xFF6600);
}

int main() {
    testRss2();
    testAtom();
    testRdfAndErrors();
    testText();
    testDates();
    testPayload();
    testConfig();
    std::cout << (checks - failures) << '/' << checks << " checks passed\n";
    return failures == 0 ? 0 : 1;
}
