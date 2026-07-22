#pragma once

#include "novaboot/websocket/websocket.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace novaboot::testing {

struct WebSocketTestFrame {
    websocket::Opcode opcode = websocket::Opcode::Text;
    bool fin = true;
    std::vector<std::uint8_t> payload;

    [[nodiscard]] std::string_view text() const {
        return {reinterpret_cast<const char*>(payload.data()), payload.size()};
    }
    [[nodiscard]] std::uint16_t close_code() const {
        if (payload.size() < 2) return 1005;
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(payload[0]) << 8U) | payload[1]);
    }
};

/// An in-process RFC 6455 peer for endpoint tests. It sends correctly masked
/// client frames and decodes the server's unmasked frames. It intentionally
/// does not replace HTTP Upgrade/TLS/browser integration coverage.
class WebSocketTestClient {
public:
    explicit WebSocketTestClient(websocket::Handler handler, std::string principal = {})
        : connection_(std::move(handler), std::move(principal)) {}

    void send_text(std::string_view text) { send(websocket::Opcode::Text, text); }
    void send_binary(std::span<const std::uint8_t> data) {
        send(websocket::Opcode::Binary,
             std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
    }
    void send_ping(std::string_view payload = {}) { send(websocket::Opcode::Ping, payload); }
    void close(std::uint16_t code = 1000, std::string_view reason = {}) {
        std::vector<std::uint8_t> payload{
            static_cast<std::uint8_t>((code >> 8U) & 0xffU),
            static_cast<std::uint8_t>(code & 0xffU),
        };
        payload.insert(payload.end(), reason.begin(), reason.end());
        send(websocket::Opcode::Close,
             std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
    }

    [[nodiscard]] std::vector<WebSocketTestFrame> take_frames() {
        // Background producers (for example STOMP heartbeats) enqueue through
        // SessionHandle, just as they do on a real shard. Drain those commands
        // before inspecting endpoint-owned outbound bytes.
        return decode(connection_.drain_external_outbound());
    }
    [[nodiscard]] bool closed() const noexcept { return connection_.closed(); }

    /// Throws std::runtime_error with a concise diagnostic on mismatch.
    void require_single_text(std::string_view expected) {
        const auto frames = take_frames();
        if (frames.size() != 1 || frames.front().opcode != websocket::Opcode::Text ||
            frames.front().text() != expected) {
            throw std::runtime_error("WebSocket test client expected one matching text frame");
        }
    }

    /// Throws std::runtime_error unless the next outbound frame closes with code.
    void require_close(std::uint16_t expected_code) {
        const auto frames = take_frames();
        if (frames.empty() || frames.front().opcode != websocket::Opcode::Close ||
            frames.front().close_code() != expected_code) {
            throw std::runtime_error("WebSocket test client expected matching close frame");
        }
    }

private:
    static std::vector<std::uint8_t> masked_frame(websocket::Opcode opcode,
                                                   std::string_view payload) {
        constexpr std::array<std::uint8_t, 4> mask{0x11, 0x22, 0x33, 0x44};
        std::vector<std::uint8_t> frame;
        frame.push_back(static_cast<std::uint8_t>(0x80U | static_cast<std::uint8_t>(opcode)));
        const auto size = payload.size();
        if (size < 126U) {
            frame.push_back(static_cast<std::uint8_t>(0x80U | size));
        } else if (size <= 0xffffU) {
            frame.push_back(0x80U | 126U);
            frame.push_back(static_cast<std::uint8_t>((size >> 8U) & 0xffU));
            frame.push_back(static_cast<std::uint8_t>(size & 0xffU));
        } else {
            frame.push_back(0x80U | 127U);
            const auto length = static_cast<std::uint64_t>(size);
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(static_cast<std::uint8_t>((length >> shift) & 0xffU));
            }
        }
        frame.insert(frame.end(), mask.begin(), mask.end());
        for (std::size_t index = 0; index < size; ++index) {
            frame.push_back(static_cast<std::uint8_t>(
                static_cast<unsigned char>(payload[index]) ^ mask[index % mask.size()]));
        }
        return frame;
    }

    static std::vector<WebSocketTestFrame> decode(const std::vector<std::uint8_t>& input) {
        std::vector<WebSocketTestFrame> frames;
        std::size_t offset = 0;
        while (offset < input.size()) {
            if (input.size() - offset < 2) throw std::runtime_error("Truncated server WebSocket frame");
            const auto first = input[offset++];
            const auto second = input[offset++];
            if ((second & 0x80U) != 0U) throw std::runtime_error("Server WebSocket frame must not be masked");
            std::uint64_t length = second & 0x7fU;
            if (length == 126U) {
                if (input.size() - offset < 2) throw std::runtime_error("Truncated WebSocket length");
                length = (static_cast<std::uint64_t>(input[offset]) << 8U) | input[offset + 1U];
                offset += 2;
            } else if (length == 127U) {
                if (input.size() - offset < 8) throw std::runtime_error("Truncated WebSocket length");
                length = 0;
                for (int index = 0; index < 8; ++index) length = (length << 8U) | input[offset++];
            }
            if (length > input.size() - offset) throw std::runtime_error("Truncated WebSocket payload");
            WebSocketTestFrame frame{
                .opcode = static_cast<websocket::Opcode>(first & 0x0fU),
                .fin = (first & 0x80U) != 0U,
                .payload = std::vector<std::uint8_t>(
                    input.begin() + static_cast<std::vector<std::uint8_t>::difference_type>(offset),
                    input.begin() + static_cast<std::vector<std::uint8_t>::difference_type>(offset + length)),
            };
            offset += static_cast<std::size_t>(length);
            frames.push_back(std::move(frame));
        }
        return frames;
    }

    void send(websocket::Opcode opcode, std::string_view payload) {
        const auto frame = masked_frame(opcode, payload);
        const auto result = connection_.feed(frame);
        if (!result) throw std::runtime_error("WebSocket endpoint rejected test frame: " + result.error().message);
    }

    websocket::Connection connection_;
};

} // namespace novaboot::testing
