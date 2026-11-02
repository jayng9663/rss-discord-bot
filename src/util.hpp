#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rssbot {

// Set by the signal handler; long waits poll it so Ctrl+C / SIGTERM stop promptly.
extern std::atomic<bool> g_stop;

// Sleeps in small steps; returns false if a stop was requested before the time elapsed.
bool interruptibleSleep(std::chrono::milliseconds duration);

enum class LogLevel { Debug, Info, Warn, Error };
void setVerbose(bool verbose);
void writeLog(LogLevel level, std::string_view message);
// The most recent log lines (for the dashboard), oldest first.
std::vector<std::string> recentLogs();
inline void logDebug(std::string_view m) { writeLog(LogLevel::Debug, m); }
inline void logInfo(std::string_view m) { writeLog(LogLevel::Info, m); }
inline void logWarn(std::string_view m) { writeLog(LogLevel::Warn, m); }
inline void logError(std::string_view m) { writeLog(LogLevel::Error, m); }

std::string trim(std::string_view s);
std::string toLower(std::string_view s);

bool isHttpUrl(std::string_view s);
// Resolves a (possibly relative) link against the URL of the document it came from.
std::string resolveUrl(const std::string& base, const std::string& ref);

void appendUtf8(std::string& out, std::uint32_t codepoint);
std::size_t utf8Length(std::string_view s);
// Cuts s to at most maxChars code points, ending with an ellipsis when shortened.
std::string truncateChars(std::string_view s, std::size_t maxChars);

std::string decodeEntities(std::string_view s);
// Strips tags, decodes entities and normalises whitespace (block tags become line breaks).
std::string htmlToText(std::string_view html);

// Accepts RFC 822 (RSS pubDate) and ISO 8601 / RFC 3339 (Atom) timestamps.
std::optional<std::time_t> parseDate(std::string_view s);
std::string toIso8601(std::time_t t);

}  // namespace rssbot
