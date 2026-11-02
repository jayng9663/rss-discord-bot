#pragma once

#include "app.hpp"
#include "config.hpp"

#include <memory>
#include <thread>

namespace httplib {
class Server;
}

namespace rssbot {

// Embedded web UI + JSON API (cpp-httplib) for managing webhooks and feeds.
class Dashboard {
public:
    Dashboard(App& app, DashboardConfig cfg);
    ~Dashboard();
    Dashboard(const Dashboard&) = delete;
    Dashboard& operator=(const Dashboard&) = delete;

    bool start();  // false if the address/port cannot be bound
    void stop();

private:
    void routes();

    App& app_;
    DashboardConfig cfg_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
};

}  // namespace rssbot
