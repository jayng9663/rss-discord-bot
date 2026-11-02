#include "util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

namespace rssbot {

std::atomic<bool> g_stop{false};

namespace {
std::atomic<bool> g_verbose{false};
std::mutex g_logMutex;
std::deque<std::string> g_logRing;
constexpr std::size_t kLogRingSize = 400;

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
}  // namespace

bool interruptibleSleep(std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (!g_stop.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= end) return true;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - now);
        std::this_thread::sleep_for(std::min(left, std::chrono::milliseconds(200)));
    }
    return false;
}

void setVerbose(bool verbose) { g_verbose = verbose; }

void writeLog(LogLevel level, std::string_view message) {
    if (level == LogLevel::Debug && !g_verbose) return;
    const std::lock_guard lock(g_logMutex);

    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);

    const char* tag = "INFO";
    switch (level) {
        case LogLevel::Debug: tag = "DEBUG"; break;
        case LogLevel::Info: tag = "INFO"; break;
        case LogLevel::Warn: tag = "WARN"; break;
        case LogLevel::Error: tag = "ERROR"; break;
    }
    std::string line = std::string(stamp) + " [" + tag + "] " + std::string(message);
    std::cerr << line << '\n';
    g_logRing.push_back(std::move(line));
    if (g_logRing.size() > kLogRingSize) g_logRing.pop_front();
}

std::vector<std::string> recentLogs() {
    const std::lock_guard lock(g_logMutex);
    return {g_logRing.begin(), g_logRing.end()};
}

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool isHttpUrl(std::string_view s) {
    const std::string lower = toLower(s.substr(0, 8));
    return (lower.starts_with("https://") && s.size() > 8) || (lower.starts_with("http://") && s.size() > 7);
}

std::string resolveUrl(const std::string& base, const std::string& ref) {
    if (ref.empty()) return ref;
    // Already absolute ("https:...", "mailto:...")?
    const auto colon = ref.find(':');
    const auto slash = ref.find_first_of("/?#");
    if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) return ref;

    const auto schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return ref;
    if (ref.starts_with("//")) return base.substr(0, schemeEnd + 1) + ref;

    const std::string noQuery = base.substr(0, base.find_first_of("?#"));
    const auto hostEnd = noQuery.find('/', schemeEnd + 3);
    const std::string origin = hostEnd == std::string::npos ? noQuery : noQuery.substr(0, hostEnd);
    if (ref[0] == '/') return origin + ref;
    if (ref[0] == '?' || ref[0] == '#') return noQuery + ref;
    if (hostEnd == std::string::npos) return origin + "/" + ref;
    return noQuery.substr(0, noQuery.rfind('/') + 1) + ref;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::size_t utf8Length(std::string_view s) {
    std::size_t n = 0;
    for (const char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

std::string truncateChars(std::string_view s, std::size_t maxChars) {
    if (utf8Length(s) <= maxChars) return std::string(s);
    if (maxChars == 0) return {};
    const std::size_t keep = maxChars - 1;  // leave room for the ellipsis
    std::size_t count = 0, i = 0;
    for (; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (count == keep) break;
            ++count;
        }
    }
    std::string out(s.substr(0, i));
    while (!out.empty() && isSpace(out.back())) out.pop_back();
    out += "\xE2\x80\xA6";  // U+2026
    return out;
}

namespace {

// Decodes the entity starting at s[i] == '&'. On success appends it and advances i.
bool decodeEntityAt(std::string_view s, std::size_t& i, std::string& out) {
    const auto semi = s.find(';', i);
    if (semi == std::string_view::npos || semi - i > 12 || semi == i + 1) return false;
    const std::string_view ent = s.substr(i + 1, semi - i - 1);

    if (ent[0] == '#') {
        const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
        const std::string_view digits = ent.substr(hex ? 2 : 1);
        std::uint32_t cp = 0;
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), cp, hex ? 16 : 10);
        if (ec != std::errc() || ptr != digits.data() + digits.size() || cp == 0 || cp > 0x10FFFF) return false;
        appendUtf8(out, cp == 0xA0 ? 0x20 : cp);
        i = semi + 1;
        return true;
    }

    static constexpr std::array<std::pair<std::string_view, std::uint32_t>, 20> kNamed{{
        {"amp", '&'},     {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
        {"nbsp", ' '},    {"ndash", 0x2013}, {"mdash", 0x2014}, {"hellip", 0x2026}, {"lsquo", 0x2018},
        {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"laquo", 0xAB},   {"raquo", 0xBB},
        {"copy", 0xA9},   {"reg", 0xAE},     {"trade", 0x2122}, {"bull", 0x2022},  {"middot", 0xB7},
    }};
    for (const auto& [name, cp] : kNamed) {
        if (name == ent) {
            appendUtf8(out, cp);
            i = semi + 1;
            return true;
        }
    }
    return false;
}

bool isBlockTag(std::string_view name) {
    static constexpr std::array<std::string_view, 18> kBlocks{
        "p", "div", "br", "li", "ul", "ol", "tr", "table", "blockquote", "pre",
        "h1", "h2", "h3", "h4", "h5", "h6", "hr", "section"};
    return std::find(kBlocks.begin(), kBlocks.end(), name) != kBlocks.end();
}

}  // namespace

std::string decodeEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '&' && decodeEntityAt(s, i, out)) continue;
        out += s[i++];
    }
    return out;
}

std::string htmlToText(std::string_view html) {
    const std::string lower = toLower(html);
    std::string raw;
    raw.reserve(html.size());

    for (std::size_t i = 0; i < html.size();) {
        const char c = html[i];
        if (c == '<') {
            if (lower.compare(i, 4, "<!--") == 0) {
                const auto e = lower.find("-->", i + 4);
                i = e == std::string::npos ? html.size() : e + 3;
                continue;
            }
            const char next = i + 1 < html.size() ? html[i + 1] : '\0';
            const auto end = html.find('>', i);
            if (end == std::string_view::npos ||
                !(std::isalpha(static_cast<unsigned char>(next)) || next == '/' || next == '!')) {
                raw += c;  // a literal '<', not a tag
                ++i;
                continue;
            }
            const bool closing = next == '/';
            std::size_t ns = i + (closing ? 2 : 1), ne = ns;
            while (ne < end && std::isalnum(static_cast<unsigned char>(lower[ne]))) ++ne;
            const std::string name = lower.substr(ns, ne - ns);

            if (!closing && (name == "script" || name == "style")) {
                const auto close = lower.find("</" + name, end);
                const auto closeEnd = close == std::string::npos ? close : lower.find('>', close);
                i = closeEnd == std::string::npos ? html.size() : closeEnd + 1;
                continue;
            }
            if (isBlockTag(name) && !(closing && name == "li")) raw += '\n';
            if (name == "li" && !closing) raw += "\xE2\x80\xA2 ";  // bullet
            i = end + 1;
            continue;
        }
        if (c == '&' && decodeEntityAt(html, i, raw)) continue;
        raw += c;
        ++i;
    }

    // Collapse runs of spaces, trim line ends, allow at most one blank line.
    std::string out;
    out.reserve(raw.size());
    int newlines = 0;
    bool pendingSpace = false;
    for (const char ch : raw) {
        if (ch == '\r') continue;
        if (ch == '\n') {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            if (!out.empty() && newlines < 2) out += '\n';
            ++newlines;
            pendingSpace = false;
            continue;
        }
        if (isSpace(ch)) {
            pendingSpace = true;
            continue;
        }
        if (pendingSpace && !out.empty() && out.back() != '\n') out += ' ';
        pendingSpace = false;
        newlines = 0;
        out += ch;
    }
    return trim(out);
}

namespace {

long zoneOffsetSeconds(const std::string& zone) {
    if (zone.empty()) return 0;
    if (zone[0] == '+' || zone[0] == '-') {
        std::string digits;
        for (const char c : zone.substr(1))
            if (isDigit(c)) digits += c;
        if (digits.size() < 2) return 0;
        const long hours = std::stol(digits.substr(0, 2));
        const long minutes = digits.size() >= 4 ? std::stol(digits.substr(2, 2)) : 0;
        return (zone[0] == '-' ? -1 : 1) * (hours * 3600 + minutes * 60);
    }
    static constexpr std::array<std::pair<std::string_view, int>, 13> kZones{{
        {"gmt", 0}, {"ut", 0}, {"utc", 0}, {"z", 0}, {"est", -5}, {"edt", -4}, {"cst", -6},
        {"cdt", -5}, {"mst", -7}, {"mdt", -6}, {"pst", -8}, {"pdt", -7}, {"cet", 1},
    }};
    const std::string lower = toLower(zone);
    for (const auto& [name, hours] : kZones)
        if (name == lower) return hours * 3600L;
    return 0;
}

std::optional<std::time_t> makeTime(int year, int month, int day, int hour, int minute, int second, long offset) {
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 24 || minute > 59 || second > 60)
        return std::nullopt;
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    return timegm(&tm) - offset;
}

// "Mon, 02 Nov 2026 08:53:00 +0000" (weekday and seconds optional)
std::optional<std::time_t> parseRfc822(const std::string& in) {
    std::string s = in;
    if (const auto comma = s.find(','); comma != std::string::npos) s = s.substr(comma + 1);

    int day = 0, year = 0;
    char monthBuf[16] = {}, timeBuf[16] = {}, zoneBuf[16] = {};
    const int fields = std::sscanf(s.c_str(), "%d %15s %d %15s %15s", &day, monthBuf, &year, timeBuf, zoneBuf);
    if (fields < 4) return std::nullopt;

    static constexpr std::array<std::string_view, 12> kMonths{
        "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    const std::string mon = toLower(std::string_view(monthBuf).substr(0, 3));
    const auto it = std::find(kMonths.begin(), kMonths.end(), mon);
    if (it == kMonths.end()) return std::nullopt;
    if (year < 100) year += year < 70 ? 2000 : 1900;

    int hour = 0, minute = 0, second = 0;
    if (std::sscanf(timeBuf, "%d:%d:%d", &hour, &minute, &second) < 2) return std::nullopt;
    return makeTime(year, static_cast<int>(it - kMonths.begin()) + 1, day, hour, minute, second,
                    fields >= 5 ? zoneOffsetSeconds(zoneBuf) : 0);
}

// "2026-11-02T08:53:00Z", "2026-11-02T00:53:00.123-08:00", "2026-11-02"
std::optional<std::time_t> parseIso8601(std::string_view s) {
    const auto num = [&](std::size_t pos, std::size_t len, int& out) {
        if (pos + len > s.size()) return false;
        out = 0;
        for (std::size_t k = pos; k < pos + len; ++k) {
            if (!isDigit(s[k])) return false;
            out = out * 10 + (s[k] - '0');
        }
        return true;
    };
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!num(0, 4, year) || s[4] != '-' || !num(5, 2, month) || s.size() < 10 || s[7] != '-' || !num(8, 2, day))
        return std::nullopt;

    long offset = 0;
    std::size_t p = 10;
    if (p < s.size() && (s[p] == 'T' || s[p] == 't' || s[p] == ' ')) {
        if (!num(p + 1, 2, hour) || p + 3 >= s.size() || s[p + 3] != ':' || !num(p + 4, 2, minute))
            return std::nullopt;
        p += 6;
        if (p < s.size() && s[p] == ':') {
            if (!num(p + 1, 2, second)) return std::nullopt;
            p += 3;
        }
        if (p < s.size() && (s[p] == '.' || s[p] == ',')) {
            ++p;
            while (p < s.size() && isDigit(s[p])) ++p;
        }
        while (p < s.size() && s[p] == ' ') ++p;
        if (p < s.size()) offset = zoneOffsetSeconds(std::string(s.substr(p)));
    }
    return makeTime(year, month, day, hour, minute, second, offset);
}

}  // namespace

std::optional<std::time_t> parseDate(std::string_view in) {
    const std::string s = trim(in);
    if (s.size() >= 10 && isDigit(s[0]) && s[4] == '-') return parseIso8601(s);
    if (s.empty()) return std::nullopt;
    return parseRfc822(s);
}

std::string toIso8601(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace rssbot
