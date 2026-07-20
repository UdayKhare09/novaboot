#include "novaboot/messaging/stomp.h"

#include <charconv>
#include <cctype>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <thread>

namespace novaboot::messaging::stomp {

namespace {

std::string escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case ':': result += "\\c"; break;
        default: result += ch; break;
        }
    }
    return result;
}

std::expected<std::string, ParseError> unescape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\') {
            result += value[i];
            continue;
        }
        if (++i == value.size()) {
            return std::unexpected(ParseError{"unterminated STOMP header escape"});
        }
        switch (value[i]) {
        case '\\': result += '\\'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 'c': result += ':'; break;
        default:
            return std::unexpected(ParseError{"invalid STOMP header escape"});
        }
    }
    return result;
}

std::size_t line_end(const std::string& data, std::size_t from) {
    return data.find('\n', from);
}

std::string_view line_without_cr(std::string_view line) {
    return line.ends_with('\r') ? line.substr(0, line.size() - 1U) : line;
}

} // namespace

std::string_view Frame::header(std::string_view name) const {
    const auto it = headers.find(std::string(name));
    return it == headers.end() ? std::string_view{} : std::string_view(it->second);
}

namespace {

std::expected<std::pair<std::uint64_t, std::uint64_t>, ParseError>
parse_heartbeat(std::string_view value) {
    if (value.empty()) return std::pair<std::uint64_t, std::uint64_t>{0, 0};
    const auto comma = value.find(',');
    if (comma == std::string_view::npos) {
        return std::unexpected(ParseError{"invalid STOMP heart-beat header"});
    }
    std::uint64_t client_outgoing = 0;
    std::uint64_t client_incoming = 0;
    const auto parse = [](std::string_view text, std::uint64_t& target) {
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), target);
        return ec == std::errc{} && ptr == text.data() + text.size();
    };
    if (!parse(value.substr(0, comma), client_outgoing) ||
        !parse(value.substr(comma + 1U), client_incoming)) {
        return std::unexpected(ParseError{"invalid STOMP heart-beat header"});
    }
    return std::pair{client_outgoing, client_incoming};
}

} // namespace

std::expected<std::vector<Frame>, ParseError> Decoder::feed(std::string_view bytes) {
    if (failed_) return std::unexpected(ParseError{"STOMP decoder is in failed state"});
    buffer_.append(bytes);
    std::vector<Frame> frames;

    while (true) {
        // STOMP heartbeats are optional LF or CRLF between frames.
        while (buffer_.starts_with('\n')) buffer_.erase(0, 1);
        while (buffer_.starts_with("\r\n")) buffer_.erase(0, 2);
        if (buffer_.empty()) break;

        const auto command_end = line_end(buffer_, 0);
        if (command_end == std::string::npos) break;
        const auto command = line_without_cr({buffer_.data(), command_end});
        if (command.empty()) {
            failed_ = true;
            return std::unexpected(ParseError{"STOMP command is empty"});
        }

        Frame frame;
        frame.command = command;
        std::size_t cursor = command_end + 1U;
        while (true) {
            const auto end = line_end(buffer_, cursor);
            if (end == std::string::npos) return frames;
            const auto line = line_without_cr({buffer_.data() + cursor, end - cursor});
            cursor = end + 1U;
            if (line.empty()) break;
            const auto colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0U) {
                failed_ = true;
                return std::unexpected(ParseError{"malformed STOMP header"});
            }
            auto name = unescape(line.substr(0, colon));
            auto value = unescape(line.substr(colon + 1U));
            if (!name || !value) {
                failed_ = true;
                return std::unexpected(!name ? name.error() : value.error());
            }
            frame.headers.insert_or_assign(std::move(*name), std::move(*value));
        }

        std::size_t body_size = 0;
        if (const auto length_it = frame.headers.find("content-length");
            length_it != frame.headers.end()) {
            const auto begin = length_it->second.data();
            const auto end = begin + length_it->second.size();
            const auto [ptr, ec] = std::from_chars(begin, end, body_size);
            if (ec != std::errc{} || ptr != end || body_size > buffer_.size() - cursor) {
                if (ec == std::errc{} && ptr == end) return frames;
                failed_ = true;
                return std::unexpected(ParseError{"invalid STOMP content-length"});
            }
            if (buffer_.size() < cursor + body_size + 1U) return frames;
            if (buffer_[cursor + body_size] != '\0') {
                failed_ = true;
                return std::unexpected(ParseError{"STOMP frame is missing NUL terminator"});
            }
            frame.body.assign(buffer_.data() + cursor, body_size);
            buffer_.erase(0, cursor + body_size + 1U);
        } else {
            const auto terminator = buffer_.find('\0', cursor);
            if (terminator == std::string::npos) return frames;
            frame.body.assign(buffer_.data() + cursor, terminator - cursor);
            buffer_.erase(0, terminator + 1U);
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

std::string serialize(const Frame& frame) {
    std::string result = frame.command;
    result += '\n';
    bool has_content_length = false;
    for (const auto& [name, value] : frame.headers) {
        has_content_length = has_content_length || name == "content-length";
        result += escape(name);
        result += ':';
        result += escape(value);
        result += '\n';
    }
    if (frame.body.contains('\0') && !has_content_length) {
        result += "content-length:" + std::to_string(frame.body.size()) + '\n';
    }
    result += '\n';
    result += frame.body;
    result += '\0';
    return result;
}

bool SimpleBroker::supported_destination(std::string_view destination) {
    return destination.starts_with("/topic/") || destination.starts_with("/queue/") ||
           destination.starts_with("/user/");
}

bool SimpleBroker::subscribe(std::string destination, std::string subscription_id,
                             const websocket::SessionHandle& session, AckMode ack_mode,
                             std::string principal) {
    if (!supported_destination(destination) || subscription_id.empty() ||
        !session.id() || !session.active()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    auto& subscriptions = subscriptions_[std::move(destination)];
    for (auto& subscription : subscriptions) {
        if (subscription.session.id() == session.id() && subscription.id == subscription_id) {
            subscription.session = session;
            subscription.ack_mode = ack_mode;
            subscription.principal = std::move(principal);
            return true;
        }
    }
    subscriptions.push_back(Subscription{
        .session = session, .id = std::move(subscription_id), .ack_mode = ack_mode,
        .principal = std::move(principal)});
    return true;
}

void SimpleBroker::unsubscribe(websocket::SessionHandle::Id session_id,
                               std::string_view subscription_id) {
    std::lock_guard lock(mutex_);
    for (auto destination = subscriptions_.begin(); destination != subscriptions_.end();) {
        auto& subscriptions = destination->second;
        std::erase_if(subscriptions, [&](const Subscription& subscription) {
            return subscription.session.id() == session_id && subscription.id == subscription_id;
        });
        if (subscriptions.empty()) {
            destination = subscriptions_.erase(destination);
        } else {
            ++destination;
        }
    }
    pending_acks_.erase(session_id);
}

bool SimpleBroker::acknowledge(websocket::SessionHandle::Id session_id,
                               std::string_view ack_id) {
    std::lock_guard lock(mutex_);
    const auto pending_it = pending_acks_.find(session_id);
    if (pending_it == pending_acks_.end()) return false;
    auto& pending = pending_it->second;
    const auto acknowledged = std::find_if(pending.begin(), pending.end(),
        [&](const PendingDelivery& delivery) { return delivery.ack_id == ack_id; });
    if (acknowledged == pending.end()) return false;
    const auto sequence = acknowledged->sequence;
    if (acknowledged->ack_mode == AckMode::Client) {
        std::erase_if(pending, [&](const PendingDelivery& delivery) {
            return delivery.sequence <= sequence;
        });
    } else {
        pending.erase(acknowledged);
    }
    if (pending.empty()) pending_acks_.erase(pending_it);
    return true;
}

bool SimpleBroker::negative_acknowledge(websocket::SessionHandle::Id session_id,
                                        std::string_view ack_id) {
    websocket::SessionHandle session;
    Frame redelivery;
    {
        std::lock_guard lock(mutex_);
        const auto pending_it = pending_acks_.find(session_id);
        if (pending_it == pending_acks_.end()) return false;
        const auto delivery = std::find_if(pending_it->second.begin(), pending_it->second.end(),
            [&](const PendingDelivery& value) { return value.ack_id == ack_id; });
        if (delivery == pending_it->second.end()) return false;
        redelivery = delivery->message;
        redelivery.headers.insert_or_assign("redelivered", "true");
        for (const auto& [_, subscriptions] : subscriptions_) {
            const auto subscription = std::find_if(subscriptions.begin(), subscriptions.end(),
                [&](const Subscription& value) {
                    return value.session.id() == session_id &&
                           value.id == redelivery.header("subscription");
                });
            if (subscription != subscriptions.end()) {
                session = subscription->session;
                break;
            }
        }
    }
    return session.active() && session.send_text(serialize(redelivery));
}

std::size_t SimpleBroker::publish(
    std::string_view destination, std::string_view body,
    const std::unordered_map<std::string, std::string>& headers) {
    if (!supported_destination(destination)) return 0;

    std::vector<Subscription> targets;
    std::string message_id;
    {
        std::lock_guard lock(mutex_);
        const auto it = subscriptions_.find(std::string(destination));
        if (it == subscriptions_.end()) return 0;
        const auto& subscriptions = it->second;
        if (destination.starts_with("/queue/")) {
            const auto cursor = queue_cursors_[std::string(destination)]++;
            for (std::size_t offset = 0; offset < subscriptions.size(); ++offset) {
                const auto& candidate = subscriptions[(cursor + offset) % subscriptions.size()];
                if (candidate.session.active()) {
                    targets.push_back(candidate);
                    break;
                }
            }
        } else if (destination.starts_with("/user/")) {
            const auto principal_start = std::string_view("/user/").size();
            const auto separator = destination.find('/', principal_start);
            if (separator == std::string_view::npos) return 0;
            const auto principal = destination.substr(principal_start, separator - principal_start);
            for (const auto& candidate : subscriptions) {
                if (candidate.principal == principal) targets.push_back(candidate);
            }
        } else {
            targets = subscriptions;
        }
        message_id = std::to_string(next_message_id_++);
    }

    std::size_t delivered = 0;
    std::vector<websocket::SessionHandle::Id> stale;
    for (const auto& target : targets) {
        if (!target.session.active()) {
            stale.push_back(target.session.id());
            continue;
        }
        Frame message{
            .command = "MESSAGE",
            .headers = headers,
            .body = std::string(body),
        };
        message.headers.insert_or_assign("destination", std::string(destination));
        message.headers.insert_or_assign("subscription", target.id);
        message.headers.insert_or_assign("message-id", message_id);
        if (target.ack_mode == AckMode::Client) {
            message.headers.insert_or_assign("ack", message_id);
        } else if (target.ack_mode == AckMode::ClientIndividual) {
            message.headers.insert_or_assign("ack", message_id);
        }
        if (target.session.send_text(serialize(message))) {
            ++delivered;
            if (target.ack_mode != AckMode::Auto) {
                std::lock_guard lock(mutex_);
                pending_acks_[target.session.id()].push_back(PendingDelivery{
                    .sequence = next_delivery_sequence_++,
                    .ack_id = message_id,
                    .ack_mode = target.ack_mode,
                    .message = message,
                });
            }
        } else if (!target.session.active()) {
            stale.push_back(target.session.id());
        }
    }

    if (!stale.empty()) {
        std::lock_guard lock(mutex_);
        for (auto destination_it = subscriptions_.begin(); destination_it != subscriptions_.end();) {
            auto& subscriptions = destination_it->second;
            std::erase_if(subscriptions, [&](const Subscription& subscription) {
                return std::ranges::find(stale, subscription.session.id()) != stale.end();
            });
            if (subscriptions.empty()) {
                destination_it = subscriptions_.erase(destination_it);
            } else {
                ++destination_it;
            }
        }
    }
    return delivered;
}

std::size_t SimpleBroker::subscription_count(std::string_view destination) const {
    std::lock_guard lock(mutex_);
    const auto it = subscriptions_.find(std::string(destination));
    return it == subscriptions_.end() ? 0U : it->second.size();
}

std::size_t SimpleBroker::pending_ack_count(websocket::SessionHandle::Id session_id) const {
    std::lock_guard lock(mutex_);
    const auto it = pending_acks_.find(session_id);
    return it == pending_acks_.end() ? 0U : it->second.size();
}

void MessageDispatcher::add_mapping(std::string destination, Handler handler) {
    if (destination.empty() || destination.front() != '/' || !handler) {
        throw std::invalid_argument("STOMP message mapping requires an absolute destination and handler");
    }
    mappings_.insert_or_assign(std::move(destination), std::move(handler));
}

bool MessageDispatcher::dispatch(std::string_view destination, std::string_view body) const {
    const auto it = mappings_.find(std::string(destination));
    return it != mappings_.end() && it->second(body);
}

Endpoint::Endpoint(SimpleBroker& broker) : Endpoint(broker, Config{}) {}

Endpoint::Endpoint(SimpleBroker& broker, Config config)
    : broker_(broker), config_(config),
      heartbeat_thread_([this](std::stop_token stop) { heartbeat_loop(stop); }) {}

Endpoint::~Endpoint() = default;

websocket::Handler Endpoint::websocket_handler() {
    return websocket::Handler{
        .on_open = [this](websocket::Session& session) { on_open(session); },
        .on_message = [this](websocket::Session& session, const websocket::Message& message) {
            on_message(session, message);
        },
        .on_close = [this](websocket::Session& session, const websocket::CloseInfo& close) {
            on_close(session, close);
        },
        .authorize = {},
    };
}

void Endpoint::on_open(websocket::Session& session) {
    const auto handle = session.handle();
    const auto now = now_ms();
    std::lock_guard lock(mutex_);
    auto state = std::make_shared<ConnectionState>();
    state->handle = handle;
    state->principal = std::string(session.principal());
    state->last_received_ms.store(now, std::memory_order_relaxed);
    state->last_sent_ms.store(now, std::memory_order_relaxed);
    connections_[handle.id()] = std::move(state);
}

void Endpoint::on_message(websocket::Session& session, const websocket::Message& message) {
    if (!message.is_text()) {
        protocol_error(session, "STOMP requires text WebSocket messages");
        return;
    }
    std::shared_ptr<ConnectionState> state;
    {
        std::lock_guard lock(mutex_);
        const auto it = connections_.find(session.handle().id());
        if (it == connections_.end()) return;
        state = it->second;
    }
    state->last_received_ms.store(now_ms(), std::memory_order_relaxed);
    const auto frames = state->decoder.feed(message.text());
    if (!frames) {
        protocol_error(session, frames.error().message);
        return;
    }
    for (const auto& frame : *frames) {
        if (session.closing()) break;
        if (config_.interceptor && !config_.interceptor(session, frame)) {
            protocol_error(session, "STOMP frame rejected by interceptor");
            break;
        }
        process_frame(session, *state, frame);
    }
}

void Endpoint::on_close(websocket::Session& session, const websocket::CloseInfo&) {
    std::shared_ptr<ConnectionState> state;
    {
        std::lock_guard lock(mutex_);
        const auto it = connections_.find(session.handle().id());
        if (it == connections_.end()) return;
        state = std::move(it->second);
        connections_.erase(it);
    }
    for (const auto& subscription_id : state->subscription_ids) {
        broker_.unsubscribe(state->handle.id(), subscription_id);
    }
}

void Endpoint::process_frame(websocket::Session& session, ConnectionState& state,
                             const Frame& frame) {
    if (!state.connected) {
        if (frame.command != "CONNECT" && frame.command != "STOMP") {
            protocol_error(session, "CONNECT or STOMP must be the first frame");
            return;
        }
        const auto versions = frame.header("accept-version");
        if (versions.empty() || versions.find("1.2") == std::string_view::npos) {
            protocol_error(session, "STOMP 1.2 is required");
            return;
        }
        const auto requested_heartbeat = parse_heartbeat(frame.header("heart-beat"));
        if (!requested_heartbeat) {
            protocol_error(session, requested_heartbeat.error().message);
            return;
        }
        const auto [client_outgoing, client_incoming] = *requested_heartbeat;
        const auto server_outgoing = static_cast<std::uint64_t>(config_.server_outgoing_heartbeat.count());
        const auto server_incoming = static_cast<std::uint64_t>(config_.server_incoming_heartbeat.count());
        const auto outgoing = client_outgoing == 0U || server_incoming == 0U
            ? 0U : std::max(client_outgoing, server_incoming);
        const auto incoming = client_incoming == 0U || server_outgoing == 0U
            ? 0U : std::max(client_incoming, server_outgoing);
        state.outgoing_heartbeat = std::chrono::milliseconds(outgoing);
        state.incoming_heartbeat = std::chrono::milliseconds(incoming);
        state.connected = true;
        send(session, Frame{.command = "CONNECTED", .headers = {
            {"version", "1.2"},
            {"heart-beat", std::to_string(server_outgoing) + "," + std::to_string(server_incoming)}}, .body = {}});
        return;
    }

    if (frame.command == "SUBSCRIBE") {
        const auto destination = frame.header("destination");
        const auto id = frame.header("id");
        const auto ack = frame.header("ack");
        const auto ack_mode = ack.empty() || ack == "auto" ? SimpleBroker::AckMode::Auto
            : ack == "client" ? SimpleBroker::AckMode::Client
            : ack == "client-individual" ? SimpleBroker::AckMode::ClientIndividual
            : static_cast<SimpleBroker::AckMode>(-1);
        if (destination.empty() || id.empty() || ack_mode == static_cast<SimpleBroker::AckMode>(-1) ||
            !broker_.subscribe(std::string(destination), std::string(id), state.handle, ack_mode,
                               state.principal)) {
            protocol_error(session, "invalid STOMP subscription");
            return;
        }
        state.subscription_ids.emplace_back(id);
    } else if (frame.command == "UNSUBSCRIBE") {
        const auto id = frame.header("id");
        if (id.empty()) {
            protocol_error(session, "UNSUBSCRIBE requires id");
            return;
        }
        broker_.unsubscribe(state.handle.id(), id);
        std::erase(state.subscription_ids, std::string(id));
    } else if (frame.command == "SEND") {
        const auto destination = frame.header("destination");
        const auto application_prefix = config_.application_destination_prefix;
        if (!application_prefix.empty() && destination.starts_with(application_prefix + "/")) {
            const auto mapping_destination = destination.substr(application_prefix.size());
            if (!config_.dispatcher || !config_.dispatcher->dispatch(mapping_destination, frame.body)) {
                protocol_error(session, "unmapped STOMP application destination");
                return;
            }
        } else if (destination.empty() || broker_.publish(destination, frame.body, frame.headers) == 0U) {
            // Zero subscribers is a valid broker publish, so only reject a
            // destination outside the supported broker namespaces.
            if (!destination.starts_with("/topic/") && !destination.starts_with("/queue/")) {
                protocol_error(session, "unsupported STOMP destination");
                return;
            }
        }
    } else if (frame.command == "ACK" || frame.command == "NACK") {
        const auto id = frame.header("id");
        const bool accepted = !id.empty() &&
            (frame.command == "ACK"
                 ? broker_.acknowledge(state.handle.id(), id)
                 : broker_.negative_acknowledge(state.handle.id(), id));
        if (!accepted) {
            protocol_error(session, "unknown STOMP acknowledgement id");
            return;
        }
    } else if (frame.command == "DISCONNECT") {
        if (const auto receipt = frame.header("receipt"); !receipt.empty()) {
            send(session, Frame{.command = "RECEIPT", .headers = {{"receipt-id", std::string(receipt)}}, .body = {}});
        }
        session.close(1000, "STOMP disconnect");
        return;
    } else {
        protocol_error(session, "unsupported STOMP command");
        return;
    }

    if (const auto receipt = frame.header("receipt"); !receipt.empty()) {
        send(session, Frame{.command = "RECEIPT", .headers = {{"receipt-id", std::string(receipt)}}, .body = {}});
    }
}

std::int64_t Endpoint::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Endpoint::heartbeat_loop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        // A short cadence keeps negotiated heartbeats accurate without giving
        // the endpoint its own event-loop affinity; sends still enter the
        // owning shard through SessionHandle.
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        const auto now = now_ms();
        std::vector<std::shared_ptr<ConnectionState>> states;
        {
            std::lock_guard lock(mutex_);
            states.reserve(connections_.size());
            for (const auto& [_, state] : connections_) states.push_back(state);
        }
        for (const auto& state : states) {
            if (!state->connected || !state->handle.active()) continue;
            const auto incoming = state->incoming_heartbeat.count();
            if (incoming > 0 && now - state->last_received_ms.load(std::memory_order_relaxed) > incoming * 2) {
                (void)state->handle.send_text(serialize(Frame{
                    .command = "ERROR", .headers = {{"message", "STOMP heartbeat timeout"}},
                    .body = "STOMP heartbeat timeout"}));
                (void)state->handle.close(1002, "STOMP heartbeat timeout");
                continue;
            }
            const auto outgoing = state->outgoing_heartbeat.count();
            if (outgoing > 0 && now - state->last_sent_ms.load(std::memory_order_relaxed) >= outgoing) {
                if (state->handle.send_text("\n")) {
                    state->last_sent_ms.store(now, std::memory_order_relaxed);
                }
            }
        }
    }
}

void Endpoint::send(websocket::Session& session, Frame frame) {
    (void)session.send_text(serialize(frame));
}

void Endpoint::protocol_error(websocket::Session& session, std::string message) {
    send(session, Frame{.command = "ERROR", .headers = {{"message", message}}, .body = message});
    (void)session.close(1002, "Invalid STOMP frame");
}

} // namespace novaboot::messaging::stomp
