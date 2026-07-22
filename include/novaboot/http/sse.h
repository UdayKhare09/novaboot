#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "novaboot/http3/response.h"

namespace novaboot::http::sse {

/// A single Server-Sent Event. `data` is split into one `data:` field per
/// line, as required by the EventSource wire format.
struct Event {
    std::string data;
    std::optional<std::string> event;
    std::optional<std::string> id;
    std::optional<std::uint32_t> retry_milliseconds;
    std::optional<std::string> comment;
};

enum class EncodeError {
    InvalidField,
};

/// RFC-compatible SSE serialization. Event names and ids must be one line;
/// newline-bearing data is intentionally represented by multiple data fields.
[[nodiscard]] inline std::expected<std::string, EncodeError> encode(const Event& event) {
    const auto valid_single_line = [](const std::optional<std::string>& value) {
        return !value || (value->find('\r') == std::string::npos &&
                          value->find('\n') == std::string::npos);
    };
    if (!valid_single_line(event.event) || !valid_single_line(event.id)) {
        return std::unexpected(EncodeError::InvalidField);
    }

    std::string encoded;
    if (event.comment) {
        std::size_t start = 0;
        while (start <= event.comment->size()) {
            const auto end = event.comment->find('\n', start);
            auto line = event.comment->substr(start, end == std::string::npos
                ? std::string::npos : end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            encoded += ':';
            if (!line.empty()) encoded += ' ' + line;
            encoded += '\n';
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    if (event.event) encoded += "event: " + *event.event + '\n';
    if (event.id) encoded += "id: " + *event.id + '\n';
    if (event.retry_milliseconds) {
        encoded += "retry: " + std::to_string(*event.retry_milliseconds) + '\n';
    }

    std::size_t start = 0;
    while (start <= event.data.size()) {
        const auto end = event.data.find('\n', start);
        auto line = event.data.substr(start, end == std::string::npos
            ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        encoded += "data:";
        if (!line.empty()) encoded += ' ' + line;
        encoded += '\n';
        if (end == std::string::npos) break;
        start = end + 1;
    }
    encoded += '\n';
    return encoded;
}

/// Configure a response for an SSE representation. It deliberately does not
/// install a content length, allowing a future transport writer to stream it.
inline void configure_response(Response& response) {
    response.status(200)
        .header("content-type", "text/event-stream; charset=utf-8")
        .header("cache-control", "no-cache")
        .header("x-accel-buffering", "no");
}

/// A bounded, thread-safe producer queue intended for one SSE connection.
/// Publishers never block a worker: when full, `publish` returns Backpressured
/// and the application decides whether to drop, retry, or close the channel.
class Channel {
public:
    struct Limits {
        std::size_t max_pending_events = 256;
        std::size_t max_pending_bytes = 1024 * 1024;
    };

    enum class PublishResult {
        Accepted,
        Closed,
        Backpressured,
        InvalidEvent,
    };

    using Wakeup = std::function<void()>;

    Channel() : Channel(Limits{}) {}
    explicit Channel(Limits limits) : limits_(limits) {}

    [[nodiscard]] PublishResult publish(const Event& event) {
        const auto encoded = encode(event);
        if (!encoded) return PublishResult::InvalidEvent;

        Wakeup wakeup;
        {
            std::lock_guard lock(mutex_);
            if (closed_) return PublishResult::Closed;
            if (pending_.size() >= limits_.max_pending_events ||
                encoded->size() > limits_.max_pending_bytes - pending_bytes_) {
                return PublishResult::Backpressured;
            }
            pending_bytes_ += encoded->size();
            pending_.push_back(*encoded);
            wakeup = wakeup_;
        }
        if (wakeup) wakeup();
        return PublishResult::Accepted;
    }

    /// Returns the next encoded event, preserving FIFO order.
    [[nodiscard]] std::optional<std::string> take_next() {
        std::lock_guard lock(mutex_);
        if (pending_.empty()) return std::nullopt;
        auto event = std::move(pending_.front());
        pending_.pop_front();
        pending_bytes_ -= event.size();
        return event;
    }

    /// Stop accepting new events. Already queued events remain drainable.
    void close() {
        Wakeup wakeup;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            wakeup = wakeup_;
        }
        if (wakeup) wakeup();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t pending_bytes() const {
        std::lock_guard lock(mutex_);
        return pending_bytes_;
    }

    /// The server transport installs this callback to wake its owner event loop
    /// after a background publisher successfully queues an event or closes.
    void set_wakeup(Wakeup wakeup) {
        std::lock_guard lock(mutex_);
        wakeup_ = std::move(wakeup);
    }

private:
    Limits limits_;
    mutable std::mutex mutex_;
    std::deque<std::string> pending_;
    std::size_t pending_bytes_ = 0;
    bool closed_ = false;
    Wakeup wakeup_;
};

/// Start an SSE response and attach its producer channel. The HTTP transport
/// takes responsibility for draining the channel after the route returns.
inline Response& open(Response& response, std::shared_ptr<Channel> channel) {
    configure_response(response);
    return response.event_stream(std::move(channel));
}

} // namespace novaboot::http::sse
