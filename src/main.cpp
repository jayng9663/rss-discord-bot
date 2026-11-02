#include "app.hpp"
#include "config.hpp"
#include "dashboard.hpp"
#include "util.hpp"

#include <curl/curl.h>

#include <csignal>
#include <format>
#include <iostream>
#include <memory>
#include <string>

#ifndef RSSBOT_VERSION
#define RSSBOT_VERSION "dev"
#endif

using namespace rssbot;

namespace {

struct Options {
    std::string configPath = "config.json";
    bool once = false;
    bool dryRun = false;
    bool checkOnly = false;
    bool verbose = false;
    bool noDashboard = false;
    int port = 0;
    std::string bind;
};

void printUsage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n\n"
              << "Polls RSS/Atom feeds and posts new items to Discord through webhooks,\n"
              << "with a web dashboard to manage feeds and webhooks.\n\n"
              << "Options:\n"
              << "  -c, --config PATH   config file (default: config.json; created by the dashboard)\n"
              << "  -p, --port N        dashboard port (default 8080)\n"
              << "      --bind ADDR     dashboard address (default 127.0.0.1)\n"
              << "      --no-dashboard  run without the web dashboard\n"
              << "      --once          poll every feed once, then exit (no dashboard)\n"
              << "      --dry-run       print webhook payloads instead of posting; state is not saved\n"
              << "      --check         validate the config file and exit\n"
              << "  -v, --verbose       debug logging\n"
              << "  -h, --help          show this help\n"
              << "      --version       print the version\n\n"
              << "Environment:\n"
              << "  DISCORD_WEBHOOK_URL used as the default webhook when the config has none\n";
}

void onSignal(int) { g_stop.store(true); }

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if ((arg == "-c" || arg == "--config") && hasValue) opt.configPath = argv[++i];
        else if (arg.starts_with("--config=")) opt.configPath = arg.substr(9);
        else if ((arg == "-p" || arg == "--port") && hasValue) opt.port = std::atoi(argv[++i]);
        else if (arg == "--bind" && hasValue) opt.bind = argv[++i];
        else if (arg == "--no-dashboard") opt.noDashboard = true;
        else if (arg == "--once") opt.once = true;
        else if (arg == "--dry-run") opt.dryRun = true;
        else if (arg == "--check") opt.checkOnly = true;
        else if (arg == "-v" || arg == "--verbose") opt.verbose = true;
        else if (arg == "--version") {
            std::cout << "rss-discord-bot " << RSSBOT_VERSION << '\n';
            return 0;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n\n";
            printUsage(argv[0]);
            return 2;
        }
    }
    setVerbose(opt.verbose);

    const bool wantDashboard = !opt.noDashboard && !opt.once && !opt.checkOnly;
    Config cfg;
    try {
        cfg = loadConfig(opt.configPath, /*allowMissing=*/wantDashboard);
        if (opt.port > 0) cfg.dashboard.port = opt.port;
        if (!opt.bind.empty()) cfg.dashboard.bind = opt.bind;
        if (!wantDashboard) cfg.dashboard.enabled = false;
        validateConfig(cfg);
    } catch (const std::exception& e) {
        logError(e.what());
        return 2;
    }
    if (opt.checkOnly) {
        std::cout << "config OK: " << cfg.webhooks.size() << " webhook(s), " << cfg.feeds.size()
                  << " feed(s), state file " << cfg.statePath() << '\n';
        return 0;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        logError("curl_global_init failed");
        return 1;
    }

    int rc = 0;
    {
        App app(cfg, opt.dryRun);
        std::unique_ptr<Dashboard> dashboard;
        if (cfg.dashboard.enabled) {
            dashboard = std::make_unique<Dashboard>(app, cfg.dashboard);
            if (!dashboard->start()) {
                dashboard.reset();
                if (cfg.feeds.empty()) rc = 1;
            }
        }
        if (rc == 0) {
            logInfo(std::format("rss-discord-bot {}: {} webhook(s), {} feed(s), polling every {}s{}", RSSBOT_VERSION,
                                cfg.webhooks.size(), cfg.feeds.size(), cfg.intervalSeconds,
                                opt.dryRun ? " (dry run)" : ""));
            if (cfg.feeds.empty()) logInfo("no feeds yet: add webhooks and feeds in the dashboard");
            app.run(opt.once);
        }
        if (dashboard) dashboard->stop();
    }
    curl_global_cleanup();
    logInfo("stopped");
    return rc;
}
