#include "novaboot/websocket/websocket.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <deque>
#include <limits>
#include <atomic>
#include <utility>

#include <openssl/evp.h>
#include <openssl/sha.h>

namespace novaboot::websocket {

namespace {

constexpr std::string_view websocket_guid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

bool has_token(std::string_view value, std::string_view wanted) {
    while (!value.empty()) {
        const auto comma = value.find(',');
        auto token = value.substr(0, comma);
        const auto first = token.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            const auto last = token.find_last_not_of(" \t");
            token = token.substr(first, last - first + 1);
            if (ascii_iequals(token, wanted)) return true;
        }
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
    }
    return false;
}

bool is_valid_close_code(std::uint16_t code) {
    return (code >= 1000 && code <= 1014 && code != 1004 && code != 1005 &&
            code != 1006 && code != 1015) ||
           (code >= 3000 && code <= 4999);
}

bool is_valid_opcode(std::uint8_t opcode) {
    return opcode == static_cast<std::uint8_t>(Opcode::Continuation) ||
           opcode == static_cast<std::uint8_t>(Opcode::Text) ||
           opcode == static_cast<std::uint8_t>(Opcode::Binary) ||
           opcode == static_cast<std::uint8_t>(Opcode::Close) ||
           opcode == static_cast<std::uint8_t>(Opcode::Ping) ||
           opcode == static_cast<std::uint8_t>(Opcode::Pong);
}

bool is_control(Opcode opcode) {
    return static_cast<std::uint8_t>(opcode) >= 0x8;
}

bool valid_utf8(std::span<const std::uint8_t> bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto ch = bytes[i];
        if (ch <= 0x7fU) {
            ++i;
            continue;
        }
        std::size_t continuation_count = 0;
        std::uint32_t value = 0;
        if ((ch & 0xe0U) == 0xc0U) {
            continuation_count = 1;
            value = ch & 0x1fU;
        } else if ((ch & 0xf0U) == 0xe0U) {
            continuation_count = 2;
            value = ch & 0x0fU;
        } else if ((ch & 0xf8U) == 0xf0U) {
            continuation_count = 3;
            value = ch & 0x07U;
        } else {
            return false;
        }
        if (i + continuation_count >= bytes.size()) return false;
        for (std::size_t j = 1; j <= continuation_count; ++j) {
            const auto continuation = bytes[i + j];
            if ((continuation & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (continuation & 0x3fU);
        }
        const bool overlong = (continuation_count == 1 && value < 0x80U) ||
                              (continuation_count == 2 && value < 0x800U) ||
                              (continuation_count == 3 && value < 0x10000U);
        if (overlong || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
            return false;
        }
        i += continuation_count + 1;
    }
    return true;
}

} // namespace

namespace detail {

enum class QueuedKind { Text, Binary, Close };

struct QueuedCommand {
    QueuedKind kind = QueuedKind::Text;
    std::vector<std::uint8_t> payload;
    std::uint16_t close_code = 1000;
    std::string close_reason;
};

class SharedSession {
public:
    SharedSession(Limits limits, Wakeup wakeup)
        : id_(next_id_.fetch_add(1, std::memory_order_relaxed)),
          limits_(limits),
          wakeup_(std::move(wakeup)) {}

    [[nodiscard]] SessionHandle::Id id() const noexcept { return id_; }

    [[nodiscard]] bool active() const noexcept {
        std::lock_guard lock(mutex_);
        return active_ && !closing_;
    }

    bool enqueue(QueuedKind kind, std::span<const std::uint8_t> payload) {
        Wakeup wakeup;
        bool accepted = true;
        {
            std::lock_guard lock(mutex_);
            if (!active_ || closing_) return false;
            const auto encoded_size = payload.size() + 10U;
            if (queued_bytes_ + encoded_size > limits_.max_pending_send_bytes) {
                queued_.clear();
                queued_bytes_ = 0;
                closing_ = true;
                queued_.push_back(QueuedCommand{
                    .kind = QueuedKind::Close,
                    .payload = {},
                    .close_code = 1013,
                    .close_reason = "Outbound queue limit exceeded",
                });
                accepted = false;
            } else {
                queued_.push_back(QueuedCommand{
                    .kind = kind,
                    .payload = {payload.begin(), payload.end()},
                    .close_code = 1000,
                    .close_reason = {},
                });
                queued_bytes_ += encoded_size;
            }
            wakeup = wakeup_;
        }
        if (wakeup) wakeup();
        return accepted;
    }

    bool enqueue_close(std::uint16_t code, std::string_view reason) {
        Wakeup wakeup;
        {
            std::lock_guard lock(mutex_);
            if (!active_ || closing_) return false;
            closing_ = true;
            queued_.push_back(QueuedCommand{
                .kind = QueuedKind::Close,
                .payload = {},
                .close_code = code,
                .close_reason = std::string(reason.substr(0, 123U)),
            });
            wakeup = wakeup_;
        }
        if (wakeup) wakeup();
        return true;
    }

    void mark_closing() {
        std::lock_guard lock(mutex_);
        closing_ = true;
    }

    void deactivate() {
        std::lock_guard lock(mutex_);
        active_ = false;
        closing_ = true;
        queued_.clear();
        queued_bytes_ = 0;
        wakeup_ = {};
    }

    [[nodiscard]] std::vector<QueuedCommand> take_commands() {
        std::lock_guard lock(mutex_);
        queued_bytes_ = 0;
        std::vector<QueuedCommand> commands;
        commands.reserve(queued_.size());
        while (!queued_.empty()) {
            commands.push_back(std::move(queued_.front()));
            queued_.pop_front();
        }
        return commands;
    }

private:
    inline static std::atomic<SessionHandle::Id> next_id_{1};

    SessionHandle::Id id_;
    Limits limits_;
    mutable std::mutex mutex_;
    bool active_ = true;
    bool closing_ = false;
    std::size_t queued_bytes_ = 0;
    std::deque<QueuedCommand> queued_;
    Wakeup wakeup_;
};

} // namespace detail

bool TransportBackpressure::try_reserve(std::size_t bytes, bool force) {
    std::lock_guard lock(mutex_);
    if (!force && (bytes > max_pending_bytes_ ||
                   pending_bytes_ > max_pending_bytes_ - bytes)) {
        return false;
    }
    pending_bytes_ += bytes;
    return true;
}

void TransportBackpressure::release(std::size_t bytes) {
    std::lock_guard lock(mutex_);
    pending_bytes_ = bytes >= pending_bytes_ ? 0 : pending_bytes_ - bytes;
}

std::size_t TransportBackpressure::pending_bytes() const {
    std::lock_guard lock(mutex_);
    return pending_bytes_;
}

bool SessionHandle::active() const noexcept {
    const auto shared = state_.lock();
    return shared && shared->active();
}

bool SessionHandle::send_text(std::string_view text) const {
    const auto shared = state_.lock();
    return shared && shared->enqueue(
        detail::QueuedKind::Text,
        {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

bool SessionHandle::send_binary(std::span<const std::uint8_t> bytes) const {
    const auto shared = state_.lock();
    return shared && shared->enqueue(detail::QueuedKind::Binary, bytes);
}

bool SessionHandle::close(std::uint16_t code, std::string_view reason) const {
    const auto shared = state_.lock();
    return shared && shared->enqueue_close(code, reason);
}

void SessionRegistry::add(const SessionHandle& session) {
    if (!session.id() || !session.active()) return;
    std::lock_guard lock(mutex_);
    sessions_[session.id()] = session;
}

void SessionRegistry::remove(SessionHandle::Id id) {
    std::lock_guard lock(mutex_);
    sessions_.erase(id);
}

std::size_t SessionRegistry::size() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

std::size_t SessionRegistry::broadcast_text(std::string_view text) {
    std::lock_guard lock(mutex_);
    std::size_t delivered = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (!it->second.active()) {
            it = sessions_.erase(it);
        } else {
            delivered += it->second.send_text(text) ? 1U : 0U;
            ++it;
        }
    }
    return delivered;
}

std::size_t SessionRegistry::broadcast_binary(std::span<const std::uint8_t> bytes) {
    std::lock_guard lock(mutex_);
    std::size_t delivered = 0;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (!it->second.active()) {
            it = sessions_.erase(it);
        } else {
            delivered += it->second.send_binary(bytes) ? 1U : 0U;
            ++it;
        }
    }
    return delivered;
}

bool Session::send_frame(Opcode opcode, std::span<const std::uint8_t> payload,
                         bool force) {
    const auto size = payload.size();
    const std::size_t header_size = size <= 125U ? 2U : (size <= 0xffffU ? 4U : 10U);
    if (!force && outbound_.size() + header_size + size > limits_.max_pending_send_bytes) {
        overloaded_ = true;
        close(1013, "Outbound queue limit exceeded");
        return false;
    }
    const auto frame_size = header_size + size;
    if (transport_backpressure_ &&
        !transport_backpressure_->try_reserve(frame_size, force)) {
        overloaded_ = true;
        close(1013, "Transport flow-control limit exceeded");
        return false;
    }
    outbound_.push_back(static_cast<std::uint8_t>(0x80U | static_cast<std::uint8_t>(opcode)));
    if (size <= 125U) {
        outbound_.push_back(static_cast<std::uint8_t>(size));
    } else if (size <= 0xffffU) {
        outbound_.push_back(126U);
        outbound_.push_back(static_cast<std::uint8_t>((size >> 8U) & 0xffU));
        outbound_.push_back(static_cast<std::uint8_t>(size & 0xffU));
    } else {
        outbound_.push_back(127U);
        const auto length = static_cast<std::uint64_t>(size);
        for (int shift = 56; shift >= 0; shift -= 8) {
            outbound_.push_back(static_cast<std::uint8_t>((length >> shift) & 0xffU));
        }
    }
    outbound_.insert(outbound_.end(), payload.begin(), payload.end());
    return true;
}

bool Session::send_text(std::string_view text) {
    if (!close_sent_) {
        return send_frame(Opcode::Text,
                          {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    }
    return false;
}

bool Session::send_binary(std::span<const std::uint8_t> bytes) {
    return !close_sent_ && send_frame(Opcode::Binary, bytes);
}

bool Session::close(std::uint16_t code, std::string_view reason) {
    if (close_sent_) return false;
    if (reason.size() > 123U) reason = reason.substr(0, 123U);
    std::vector<std::uint8_t> payload;
    payload.reserve(2 + reason.size());
    payload.push_back(static_cast<std::uint8_t>((code >> 8U) & 0xffU));
    payload.push_back(static_cast<std::uint8_t>(code & 0xffU));
    payload.insert(payload.end(), reason.begin(), reason.end());
    // A slow peer must still receive a close frame. Dropping stale queued
    // application messages is preferable to exceeding the memory bound.
    if (outbound_.size() + 2U + payload.size() > limits_.max_pending_send_bytes) {
        outbound_.clear();
    }
    (void)send_frame(Opcode::Close, payload, true);
    close_sent_ = true;
    if (shared_) shared_->mark_closing();
    return true;
}

SessionHandle Session::handle() const {
    return shared_ ? SessionHandle(shared_->id(), shared_) : SessionHandle{};
}

std::vector<std::uint8_t> Session::take_outbound() {
    return std::exchange(outbound_, {});
}

bool is_upgrade_attempt(const http3::Request& request) {
    const auto upgrade = request.header("upgrade");
    return upgrade && has_token(*upgrade, "websocket");
}

std::expected<std::string, ProtocolError>
validate_upgrade_request(const http3::Request& request) {
    if (request.method() != "GET") {
        return std::unexpected(ProtocolError{1002, "WebSocket upgrade requires GET"});
    }
    const auto upgrade = request.header("upgrade");
    const auto connection = request.header("connection");
    const auto version = request.header("sec-websocket-version");
    const auto key = request.header("sec-websocket-key");
    if (!upgrade || !has_token(*upgrade, "websocket") || !connection ||
        !has_token(*connection, "upgrade")) {
        return std::unexpected(ProtocolError{1002, "Invalid WebSocket upgrade headers"});
    }
    if (!version || *version != "13") {
        return std::unexpected(ProtocolError{1002, "Unsupported WebSocket version"});
    }
    if (!key) {
        return std::unexpected(ProtocolError{1002, "Missing Sec-WebSocket-Key"});
    }

    std::array<unsigned char, 32> decoded{};
    const int decoded_size = EVP_DecodeBlock(decoded.data(),
                                               reinterpret_cast<const unsigned char*>(key->data()),
                                               static_cast<int>(key->size()));
    std::size_t padding = 0;
    if (key->ends_with("==")) padding = 2;
    else if (key->ends_with("=")) padding = 1;
    if (decoded_size < 0 || static_cast<std::size_t>(decoded_size) < padding ||
        static_cast<std::size_t>(decoded_size) - padding != 16U) {
        return std::unexpected(ProtocolError{1002, "Invalid Sec-WebSocket-Key"});
    }

    const std::string source = std::string(*key) + std::string(websocket_guid);
    std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
    SHA1(reinterpret_cast<const unsigned char*>(source.data()), source.size(), digest.data());

    std::array<unsigned char, 32> encoded{};
    const int encoded_size = EVP_EncodeBlock(encoded.data(), digest.data(),
                                               static_cast<int>(digest.size()));
    return std::string(reinterpret_cast<const char*>(encoded.data()),
                       static_cast<std::size_t>(encoded_size));
}

Connection::Connection(Handler handler, std::string principal, Wakeup wakeup,
                       std::shared_ptr<TransportBackpressure> transport_backpressure)
    : handler_(std::move(handler)),
      shared_(std::make_shared<detail::SharedSession>(handler_.limits, std::move(wakeup))),
      session_(handler_.limits, std::move(principal), shared_, std::move(transport_backpressure)),
      limits_(handler_.limits) {
    if (handler_.on_open) handler_.on_open(session_);
}

Connection::~Connection() {
    if (shared_) shared_->deactivate();
}

Connection::Connection(Connection&& other) noexcept
    : handler_(std::move(other.handler_)),
      shared_(std::move(other.shared_)),
      session_(std::move(other.session_)),
      limits_(other.limits_),
      input_(std::move(other.input_)),
      fragmented_payload_(std::move(other.fragmented_payload_)),
      fragmented_opcode_(other.fragmented_opcode_),
      fragmented_(other.fragmented_),
      closed_(other.closed_),
      close_notified_(other.close_notified_) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this == &other) return *this;
    if (shared_) shared_->deactivate();
    handler_ = std::move(other.handler_);
    shared_ = std::move(other.shared_);
    session_ = std::move(other.session_);
    limits_ = other.limits_;
    input_ = std::move(other.input_);
    fragmented_payload_ = std::move(other.fragmented_payload_);
    fragmented_opcode_ = other.fragmented_opcode_;
    fragmented_ = other.fragmented_;
    closed_ = other.closed_;
    close_notified_ = other.close_notified_;
    return *this;
}

std::expected<void, ProtocolError> Connection::feed(std::span<const std::uint8_t> data) {
    if (closed_) return {};
    input_.insert(input_.end(), data.begin(), data.end());

    std::size_t offset = 0;
    while (true) {
        if (input_.size() - offset < 2U) break;
        const auto first = input_[offset];
        const auto second = input_[offset + 1U];
        const bool fin = (first & 0x80U) != 0;
        if ((first & 0x70U) != 0) {
            protocol_close(1002, "RSV bits require an extension");
            return std::unexpected(ProtocolError{1002, "RSV bits require an extension"});
        }
        const auto opcode_value = static_cast<std::uint8_t>(first & 0x0fU);
        if (!is_valid_opcode(opcode_value)) {
            protocol_close(1002, "Unsupported opcode");
            return std::unexpected(ProtocolError{1002, "Unsupported opcode"});
        }
        const auto opcode = static_cast<Opcode>(opcode_value);
        if ((second & 0x80U) == 0) {
            protocol_close(1002, "Client frames must be masked");
            return std::unexpected(ProtocolError{1002, "Client frames must be masked"});
        }

        std::uint64_t payload_size = second & 0x7fU;
        std::size_t header_size = 2;
        if (payload_size == 126U) {
            if (input_.size() - offset < 4U) break;
            payload_size = (static_cast<std::uint64_t>(input_[offset + 2U]) << 8U) |
                           input_[offset + 3U];
            header_size = 4;
        } else if (payload_size == 127U) {
            if (input_.size() - offset < 10U) break;
            if ((input_[offset + 2U] & 0x80U) != 0) {
                protocol_close(1002, "Invalid 64-bit payload length");
                return std::unexpected(ProtocolError{1002, "Invalid 64-bit payload length"});
            }
            payload_size = 0;
            for (std::size_t i = 0; i < 8; ++i) {
                payload_size = (payload_size << 8U) | input_[offset + 2U + i];
            }
            header_size = 10;
        }
        if (payload_size > limits_.max_message_bytes ||
            payload_size > std::numeric_limits<std::size_t>::max()) {
            protocol_close(1009, "Message too large");
            return std::unexpected(ProtocolError{1009, "Message too large"});
        }
        if (is_control(opcode) && (!fin || payload_size > 125U)) {
            protocol_close(1002, "Invalid control frame");
            return std::unexpected(ProtocolError{1002, "Invalid control frame"});
        }
        const std::size_t total_size = header_size + 4U + static_cast<std::size_t>(payload_size);
        if (input_.size() - offset < total_size) break;

        const auto mask_offset = offset + header_size;
        const auto payload_offset = mask_offset + 4U;
        std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_size));
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::uint8_t>(input_[payload_offset + i] ^ input_[mask_offset + (i % 4U)]);
        }
        const auto result = process_frame(fin, opcode, payload);
        offset += total_size;
        if (!result) {
            input_.erase(input_.begin(), input_.begin() + static_cast<std::vector<std::uint8_t>::difference_type>(offset));
            return result;
        }
        if (closed_) break;
    }

    if (offset > 0) {
        input_.erase(input_.begin(), input_.begin() + static_cast<std::vector<std::uint8_t>::difference_type>(offset));
    }
    return {};
}

std::expected<void, ProtocolError> Connection::process_frame(
    bool fin, Opcode opcode, std::span<const std::uint8_t> payload) {
    if (opcode == Opcode::Ping) {
        (void)session_.send_frame(Opcode::Pong, payload);
        return {};
    }
    if (opcode == Opcode::Pong) return {};
    if (opcode == Opcode::Close) {
        CloseInfo close;
        if (payload.size() == 1U) {
            protocol_close(1002, "Invalid close payload");
            return std::unexpected(ProtocolError{1002, "Invalid close payload"});
        }
        if (payload.size() >= 2U) {
            close.code = static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(payload[0]) << 8U) | payload[1]);
            if (!is_valid_close_code(close.code) ||
                !valid_utf8(payload.subspan(2))) {
                protocol_close(1002, "Invalid close payload");
                return std::unexpected(ProtocolError{1002, "Invalid close payload"});
            }
            close.reason.assign(reinterpret_cast<const char*>(payload.data() + 2U), payload.size() - 2U);
        }
        if (!session_.closing()) session_.close(close.code, close.reason);
        closed_ = true;
        if (shared_) shared_->deactivate();
        notify_close(close);
        return {};
    }

    if (opcode == Opcode::Continuation) {
        if (!fragmented_) {
            protocol_close(1002, "Unexpected continuation frame");
            return std::unexpected(ProtocolError{1002, "Unexpected continuation frame"});
        }
        if (fragmented_payload_.size() + payload.size() > limits_.max_message_bytes) {
            protocol_close(1009, "Message too large");
            return std::unexpected(ProtocolError{1009, "Message too large"});
        }
        fragmented_payload_.insert(fragmented_payload_.end(), payload.begin(), payload.end());
        if (!fin) return {};
        if (fragmented_opcode_ == Opcode::Text && !valid_utf8(fragmented_payload_)) {
            protocol_close(1007, "Invalid UTF-8 text message");
            return std::unexpected(ProtocolError{1007, "Invalid UTF-8 text message"});
        }
        if (handler_.on_message) handler_.on_message(session_, Message{fragmented_opcode_, std::move(fragmented_payload_)});
        fragmented_payload_.clear();
        fragmented_ = false;
        fragmented_opcode_ = Opcode::Continuation;
        return {};
    }

    if (fragmented_) {
        protocol_close(1002, "Expected continuation frame");
        return std::unexpected(ProtocolError{1002, "Expected continuation frame"});
    }
    if (!fin) {
        fragmented_ = true;
        fragmented_opcode_ = opcode;
        fragmented_payload_.assign(payload.begin(), payload.end());
        return {};
    }
    if (opcode == Opcode::Text && !valid_utf8(payload)) {
        protocol_close(1007, "Invalid UTF-8 text message");
        return std::unexpected(ProtocolError{1007, "Invalid UTF-8 text message"});
    }
    if (handler_.on_message) handler_.on_message(session_, Message{opcode, {payload.begin(), payload.end()}});
    return {};
}

void Connection::notify_close(const CloseInfo& close) {
    if (!close_notified_ && handler_.on_close) handler_.on_close(session_, close);
    close_notified_ = true;
}

void Connection::protocol_close(std::uint16_t code, std::string_view reason) {
    if (!session_.closing()) session_.close(code, reason);
    closed_ = true;
    if (shared_) shared_->deactivate();
    notify_close(CloseInfo{code, std::string(reason)});
}

std::vector<std::uint8_t> Connection::take_outbound() {
    return session_.take_outbound();
}

std::vector<std::uint8_t> Connection::drain_external_outbound() {
    if (!shared_) return {};
    for (auto& command : shared_->take_commands()) {
        switch (command.kind) {
        case detail::QueuedKind::Text:
            (void)session_.send_text({
                reinterpret_cast<const char*>(command.payload.data()), command.payload.size()});
            break;
        case detail::QueuedKind::Binary:
            (void)session_.send_binary(command.payload);
            break;
        case detail::QueuedKind::Close:
            (void)session_.close(command.close_code, command.close_reason);
            break;
        }
    }
    return take_outbound();
}

} // namespace novaboot::websocket
