#pragma once

#include <expected>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "novaboot/websocket/websocket.h"

namespace novaboot::messaging::stomp {

/// A STOMP 1.2 frame.  Bodies are byte strings and may contain NUL when a
/// content-length header is present.
struct Frame {
    std::string command;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    [[nodiscard]] std::string_view header(std::string_view name) const;
};

struct ParseError {
    std::string message;
};

/// A conservative local-broker authorization convention. Attach the produced
/// interceptor to Endpoint::Config after authenticating the WebSocket
/// handshake. Empty destination lists leave that operation unrestricted.
struct AuthorizationPolicy {
    bool require_authenticated_principal = false;
    std::vector<std::string> allowed_send_destinations;
    std::vector<std::string> allowed_subscribe_destinations;
};

[[nodiscard]] std::function<bool(const websocket::Session&, const Frame&)>
frame_authorizer(AuthorizationPolicy policy);

/// Incremental STOMP 1.2 frame decoder.  It accepts fragmented WebSocket text
/// messages and emits zero or more complete STOMP frames; heartbeats are
/// ignored.  Any protocol error leaves the decoder in the failed state.
class Decoder {
public:
    [[nodiscard]] std::expected<std::vector<Frame>, ParseError>
    feed(std::string_view bytes);

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    std::string buffer_;
    bool failed_ = false;
};

/// Serializes one STOMP frame. Header names and values are escaped according
/// to STOMP 1.2, and a content-length header is added when the body has NUL.
[[nodiscard]] std::string serialize(const Frame& frame);

/// In-process STOMP broker for the conventional /topic and /queue namespaces.
/// It holds safe WebSocket session handles only; delivery is queued on each
/// session's owner shard and never writes a socket from the publishing thread.
class SimpleBroker {
public:
    enum class AckMode { Auto, Client, ClientIndividual };

    [[nodiscard]] bool subscribe(std::string destination,
                                 std::string subscription_id,
                                 const websocket::SessionHandle& session,
                                 AckMode ack_mode = AckMode::Auto,
                                 std::string principal = {});
    void unsubscribe(websocket::SessionHandle::Id session_id,
                     std::string_view subscription_id);

    /// Acknowledge an unacknowledged MESSAGE. `client` mode acknowledges all
    /// earlier messages for the connection; `client-individual` only this one.
    [[nodiscard]] bool acknowledge(websocket::SessionHandle::Id session_id,
                                   std::string_view ack_id);
    /// Re-deliver an unacknowledged MESSAGE and keep it pending for a later
    /// ACK. Returns false when the ack id is unknown for this session.
    [[nodiscard]] bool negative_acknowledge(websocket::SessionHandle::Id session_id,
                                            std::string_view ack_id);

    /// Sends a STOMP MESSAGE frame to every active matching subscription and
    /// returns the number of sessions that accepted the queued delivery.
    std::size_t publish(std::string_view destination, std::string_view body,
                        const std::unordered_map<std::string, std::string>& headers = {});
    [[nodiscard]] std::size_t subscription_count(std::string_view destination) const;
    [[nodiscard]] std::size_t pending_ack_count(websocket::SessionHandle::Id session_id) const;

private:
    struct Subscription {
        websocket::SessionHandle session;
        std::string id;
        AckMode ack_mode = AckMode::Auto;
        std::string principal;
    };

    struct PendingDelivery {
        std::uint64_t sequence = 0;
        std::string ack_id;
        AckMode ack_mode = AckMode::Auto;
        Frame message;
    };

    static bool supported_destination(std::string_view destination);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<Subscription>> subscriptions_;
    std::unordered_map<std::string, std::size_t> queue_cursors_;
    std::unordered_map<websocket::SessionHandle::Id, std::vector<PendingDelivery>> pending_acks_;
    std::uint64_t next_message_id_ = 1;
    std::uint64_t next_delivery_sequence_ = 1;
};

/// Application-facing broker facade, analogous to Spring's MessagingTemplate.
class MessagingTemplate {
public:
    explicit MessagingTemplate(SimpleBroker& broker) : broker_(broker) {}

    std::size_t convert_and_send(std::string_view destination, std::string_view body,
                                 const std::unordered_map<std::string, std::string>& headers = {}) {
        return broker_.publish(destination, body, headers);
    }

private:
    SimpleBroker& broker_;
};

/// Maps application destinations (typically /chat.send) to application
/// handlers. Endpoint removes its /app application prefix before lookup.
class MessageDispatcher {
public:
    using Handler = std::function<bool(std::string_view)>;

    void add_mapping(std::string destination, Handler handler);
    [[nodiscard]] bool dispatch(std::string_view destination, std::string_view body) const;

private:
    std::unordered_map<std::string, Handler> mappings_;
};

/// Raw-WebSocket STOMP 1.2 endpoint backed by SimpleBroker.  This is the
/// transport adapter that owns CONNECT/SUBSCRIBE/SEND/UNSUBSCRIBE/DISCONNECT
/// state; the broker itself remains transport-neutral.
class Endpoint {
public:
    struct Config {
        /// Minimum period at which this server can send heartbeats.
        std::chrono::milliseconds server_outgoing_heartbeat{10'000};
        /// Minimum period at which this server expects client heartbeats.
        std::chrono::milliseconds server_incoming_heartbeat{10'000};
        /// Optional dispatcher for application SEND destinations such as /app/chat.send.
        MessageDispatcher* dispatcher = nullptr;
        /// STOMP application destination prefix, matching Spring's conventional /app.
        std::string application_destination_prefix = "/app";
        /// Runs before each decoded STOMP frame after WebSocket handshake.
        /// Return false to send ERROR and close the STOMP session.
        std::function<bool(const websocket::Session&, const Frame&)> interceptor;
    };

    explicit Endpoint(SimpleBroker& broker);
    Endpoint(SimpleBroker& broker, Config config);
    ~Endpoint();

    Endpoint(const Endpoint&) = delete;
    Endpoint& operator=(const Endpoint&) = delete;

    [[nodiscard]] websocket::Handler websocket_handler();
    void set_meter(std::shared_ptr<observability::MeterRegistry> meter);
    void begin_shutdown();
    [[nodiscard]] bool drained() const;
    void force_shutdown();

private:
    struct ConnectionState {
        Decoder decoder;
        bool connected = false;
        websocket::SessionHandle handle;
        std::string principal;
        std::vector<std::string> subscription_ids;
        std::chrono::milliseconds outgoing_heartbeat{0};
        std::chrono::milliseconds incoming_heartbeat{0};
        std::atomic<std::int64_t> last_received_ms{0};
        std::atomic<std::int64_t> last_sent_ms{0};
    };

    void on_open(websocket::Session& session);
    void on_message(websocket::Session& session, const websocket::Message& message);
    void on_close(websocket::Session& session, const websocket::CloseInfo& close);
    void process_frame(websocket::Session& session, ConnectionState& state, const Frame& frame);
    void heartbeat_loop(std::stop_token stop);
    static std::int64_t now_ms();
    void send(websocket::Session& session, Frame frame);
    void protocol_error(websocket::Session& session, std::string message);

    SimpleBroker& broker_;
    Config config_;
    mutable std::mutex mutex_;
    std::unordered_map<websocket::SessionHandle::Id, std::shared_ptr<ConnectionState>> connections_;
    std::shared_ptr<observability::MeterRegistry> meters_;
    std::atomic_bool accepting_{true};
    std::jthread heartbeat_thread_;
};

/// Optional STOMP 1.2 broker relay. Each browser WebSocket session gets one
/// upstream TCP connection; frames, receipts and acknowledgements are passed
/// through unchanged. On a dropped upstream connection the relay reconnects
/// and replays the client's CONNECT and active SUBSCRIBE frames. TLS belongs
/// at the broker-facing proxy for this intentionally dependency-free baseline.
class RelayEndpoint {
public:
    struct Config {
        std::string host;
        std::uint16_t port = 61613;
        std::chrono::milliseconds reconnect_delay{1'000};
        std::size_t max_queued_frames = 256;
        std::function<bool(const websocket::Session&, const Frame&)> interceptor;
    };

    explicit RelayEndpoint(Config config);
    ~RelayEndpoint();
    RelayEndpoint(const RelayEndpoint&) = delete;
    RelayEndpoint& operator=(const RelayEndpoint&) = delete;

    [[nodiscard]] websocket::Handler websocket_handler();
    void begin_shutdown();
    [[nodiscard]] bool drained() const;
    void force_shutdown();

private:
    struct ConnectionState;
    void on_open(websocket::Session& session);
    void on_message(websocket::Session& session, const websocket::Message& message);
    void on_close(websocket::Session& session, const websocket::CloseInfo& close);
    void relay_loop(const std::shared_ptr<ConnectionState>& state,
                    std::stop_token stop);

    Config config_;
    mutable std::mutex mutex_;
    std::unordered_map<websocket::SessionHandle::Id, std::shared_ptr<ConnectionState>> connections_;
    std::atomic_bool accepting_{true};
};

} // namespace novaboot::messaging::stomp
