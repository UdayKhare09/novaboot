#pragma once

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include "novaboot/core/server.h"

namespace novaboot::testing {

/// RAII wrapper for a real NovaBoot server in an integration test. It waits
/// for every shard to bind before returning and always stops/joins the server.
class LiveServer {
public:
    explicit LiveServer(std::unique_ptr<Server> server,
                        std::chrono::milliseconds startup_timeout = std::chrono::seconds{5})
        : server_(std::move(server)) {
        if (!server_) throw std::invalid_argument("LiveServer requires a Server");
        thread_ = std::thread([this] {
            try {
                server_->run();
            } catch (...) {
                startup_error_ = std::current_exception();
                startup_failed_.store(true, std::memory_order_release);
            }
        });
        const auto deadline = std::chrono::steady_clock::now() + startup_timeout;
        while (!server_->is_ready() && !startup_failed_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!server_->is_ready()) {
            stop();
            if (startup_failed_.load(std::memory_order_acquire)) {
                std::rethrow_exception(startup_error_);
            }
            throw std::runtime_error("LiveServer did not become ready before its startup timeout");
        }
    }

    ~LiveServer() { stop(); }

    LiveServer(const LiveServer&) = delete;
    LiveServer& operator=(const LiveServer&) = delete;

    [[nodiscard]] Server& server() noexcept { return *server_; }

    void stop() noexcept {
        if (server_) server_->stop();
        if (thread_.joinable()) thread_.join();
    }

private:
    std::unique_ptr<Server> server_;
    std::thread thread_;
    std::exception_ptr startup_error_;
    std::atomic_bool startup_failed_ = false;
};

} // namespace novaboot::testing
