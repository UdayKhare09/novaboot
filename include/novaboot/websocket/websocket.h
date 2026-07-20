#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "novaboot/http3/request.h"

namespace novaboot::websocket {

namespace detail { class SharedSession; }

/// Thread-safe byte budget owned by a transport adapter.  HTTP/2 retains
/// WebSocket DATA while its flow-control window is exhausted; this budget
/// makes that retained data part of Session's normal backpressure decision.
class TransportBackpressure {
public:
    explicit TransportBackpressure(std::size_t max_pending_bytes)
        : max_pending_bytes_(max_pending_bytes) {}

    [[nodiscard]] bool try_reserve(std::size_t bytes, bool force = false);
    void release(std::size_t bytes);
    [[nodiscard]] std::size_t pending_bytes() const;

private:
    const std::size_t max_pending_bytes_;
    mutable std::mutex mutex_;
    std::size_t pending_bytes_ = 0;
};

/// RFC 6455 frame opcodes supported by NovaBoot's raw WebSocket transport.
enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

struct Message {
    Opcode opcode = Opcode::Text;
    std::vector<std::uint8_t> payload;

    [[nodiscard]] bool is_text() const noexcept { return opcode == Opcode::Text; }
    [[nodiscard]] bool is_binary() const noexcept { return opcode == Opcode::Binary; }
    [[nodiscard]] std::string_view text() const noexcept {
        return {reinterpret_cast<const char*>(payload.data()), payload.size()};
    }
};

struct CloseInfo {
    std::uint16_t code = 1000;
    std::string reason;
};

/// A protocol error is converted into an RFC 6455 close frame before the TCP
/// connection is drained.
struct ProtocolError {
    std::uint16_t close_code = 1002;
    std::string message;
};

/// Per-endpoint transport limits. Hitting the outbound limit closes a slow
/// connection with 1013 (Try Again Later) rather than growing memory without
/// bound.
struct Limits {
    std::size_t max_message_bytes = 1024U * 1024U;
    std::size_t max_pending_send_bytes = 1024U * 1024U;
};

using Wakeup = std::function<void()>;

/// Thread-safe handle to one WebSocket session. Sends are queued and executed
/// on the connection's owning shard; callers never write a socket directly.
class SessionHandle {
public:
    using Id = std::uint64_t;

    SessionHandle() = default;

    [[nodiscard]] Id id() const noexcept { return id_; }
    [[nodiscard]] bool active() const noexcept;
    bool send_text(std::string_view text) const;
    bool send_binary(std::span<const std::uint8_t> bytes) const;
    bool close(std::uint16_t code = 1000, std::string_view reason = {}) const;

private:
    friend class Session;
    friend class SessionRegistry;
    SessionHandle(Id id, std::weak_ptr<detail::SharedSession> state)
        : id_(id), state_(std::move(state)) {}

    Id id_ = 0;
    std::weak_ptr<detail::SharedSession> state_;
};

/// Application-owned collection of session handles. It is safe to use from
/// service threads and automatically prunes closed sessions during broadcasts.
class SessionRegistry {
public:
    void add(const SessionHandle& session);
    void remove(SessionHandle::Id id);
    [[nodiscard]] std::size_t size() const;
    std::size_t broadcast_text(std::string_view text);
    std::size_t broadcast_binary(std::span<const std::uint8_t> bytes);

private:
    mutable std::mutex mutex_;
    std::unordered_map<SessionHandle::Id, SessionHandle> sessions_;
};

/// A per-connection application view. It is valid only while the callback that
/// received it is running; long-lived cross-thread sends are intentionally
/// deferred until the session registry/backpressure design is added.
class Session {
public:
    /// Returns false if the connection is closing or its outbound queue is
    /// full. Queue overflow sends close code 1013 and discards queued data.
    bool send_text(std::string_view text);
    bool send_binary(std::span<const std::uint8_t> bytes);
    bool close(std::uint16_t code = 1000, std::string_view reason = {});

    [[nodiscard]] bool closing() const noexcept { return close_sent_; }
    [[nodiscard]] bool overloaded() const noexcept { return overloaded_; }
    [[nodiscard]] std::string_view principal() const noexcept { return principal_; }
    [[nodiscard]] SessionHandle handle() const;
    [[nodiscard]] std::size_t pending_send_bytes() const noexcept {
        return outbound_.size();
    }

private:
    friend class Connection;

    explicit Session(Limits limits, std::string principal,
                     std::shared_ptr<detail::SharedSession> shared,
                     std::shared_ptr<TransportBackpressure> transport_backpressure)
        : limits_(limits),
          principal_(std::move(principal)),
          shared_(std::move(shared)),
          transport_backpressure_(std::move(transport_backpressure)) {}

    bool send_frame(Opcode opcode, std::span<const std::uint8_t> payload,
                    bool force = false);
    [[nodiscard]] std::vector<std::uint8_t> take_outbound();

    std::vector<std::uint8_t> outbound_;
    Limits limits_;
    std::string principal_;
    std::shared_ptr<detail::SharedSession> shared_;
    std::shared_ptr<TransportBackpressure> transport_backpressure_;
    bool close_sent_ = false;
    bool overloaded_ = false;
};

/// Result of endpoint-specific authentication/authorization before accepting
/// the Upgrade request. The principal is copied into Session for the lifetime
/// of the WebSocket connection.
struct HandshakeDecision {
    bool accepted = true;
    int rejection_status = 403;
    std::string rejection_body = "Forbidden";
    std::string principal;

    [[nodiscard]] static HandshakeDecision allow(std::string principal = {}) {
        return HandshakeDecision{.accepted = true, .principal = std::move(principal)};
    }

    [[nodiscard]] static HandshakeDecision reject(
        int status = 403, std::string body = "Forbidden") {
        return HandshakeDecision{
            .accepted = false,
            .rejection_status = status,
            .rejection_body = std::move(body),
            .principal = {},
        };
    }
};

/// Application lifecycle callbacks for a raw WebSocket endpoint.
struct Handler {
    std::function<void(Session&)> on_open{};
    std::function<void(Session&, const Message&)> on_message{};
    std::function<void(Session&, const CloseInfo&)> on_close{};
    std::function<HandshakeDecision(const http3::Request&)> authorize{};
    Limits limits{};
};

/// Returns true when the request intends to switch protocols to WebSocket.
[[nodiscard]] bool is_upgrade_attempt(const http3::Request& request);

/// Validates an RFC 6455 opening handshake and returns Sec-WebSocket-Accept.
[[nodiscard]] std::expected<std::string, ProtocolError>
validate_upgrade_request(const http3::Request& request);

/// Stateful RFC 6455 frame parser and server-side frame writer.
class Connection {
public:
    explicit Connection(Handler handler, std::string principal = {},
                        Wakeup wakeup = {},
                        std::shared_ptr<TransportBackpressure> transport_backpressure = {});
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    /// Consume decrypted bytes from the peer. Client frames must be masked.
    /// Any automatic pong/close frames and callback sends are collected for
    /// take_outbound().
    std::expected<void, ProtocolError> feed(std::span<const std::uint8_t> data);

    [[nodiscard]] std::vector<std::uint8_t> take_outbound();
    /// Drain cross-thread SessionHandle sends on the owner shard.
    [[nodiscard]] std::vector<std::uint8_t> drain_external_outbound();
    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    std::expected<void, ProtocolError> process_frame(
        bool fin, Opcode opcode, std::span<const std::uint8_t> payload);
    void notify_close(const CloseInfo& close);
    void protocol_close(std::uint16_t code, std::string_view reason);

    Handler handler_;
    std::shared_ptr<detail::SharedSession> shared_;
    Session session_;
    Limits limits_;
    std::vector<std::uint8_t> input_;
    std::vector<std::uint8_t> fragmented_payload_;
    Opcode fragmented_opcode_ = Opcode::Continuation;
    bool fragmented_ = false;
    bool closed_ = false;
    bool close_notified_ = false;
};

} // namespace novaboot::websocket
