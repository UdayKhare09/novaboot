#pragma once

#include <vector>
#include <string>
#include <functional>
#include <expected>
#include <optional>
#include <utility>
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/http/sse.h"
#include "novaboot/websocket/websocket.h"

namespace novaboot::http1 {

class Http1Session {
public:
    struct UpgradeResult {
        enum class Kind {
            NotWebSocketRoute,
            Accepted,
            Rejected,
        };

        Kind kind = Kind::NotWebSocketRoute;
        std::optional<websocket::Handler> handler;
        std::string principal;
        std::string subprotocol;
        int rejection_status = 403;
        std::string rejection_body = "Forbidden";

        [[nodiscard]] static UpgradeResult accept(
            websocket::Handler handler, std::string principal = {},
            std::string subprotocol = {}) {
            return UpgradeResult{
                .kind = Kind::Accepted,
                .handler = std::move(handler),
                .principal = std::move(principal),
                .subprotocol = std::move(subprotocol),
            };
        }

        [[nodiscard]] static UpgradeResult reject(
            int status = 403, std::string body = "Forbidden") {
            return UpgradeResult{
                .kind = Kind::Rejected,
                .handler = std::nullopt,
                .principal = {},
                .subprotocol = {},
                .rejection_status = status,
                .rejection_body = std::move(body),
            };
        }
    };

    struct AcceptedUpgrade {
        websocket::Handler handler;
        std::string principal;
        std::string subprotocol;
    };

    enum class ParseState {
        RequestLine,
        Headers,
        Body,
        Complete,
        Error
    };

    using RequestHandler = std::function<void(http3::Request&, http3::Response&)>;
    using UpgradeHandler = std::function<UpgradeResult(http3::Request&)>;

    explicit Http1Session(RequestHandler handler, UpgradeHandler upgrade_handler = {})
        : handler_(std::move(handler)), upgrade_handler_(std::move(upgrade_handler)) {}
    Http1Session(RequestHandler handler, UpgradeHandler upgrade_handler,
                 http::sse::Channel::Wakeup sse_wakeup)
        : handler_(std::move(handler)), upgrade_handler_(std::move(upgrade_handler)),
          sse_wakeup_(std::move(sse_wakeup)) {}
    ~Http1Session();

    Http1Session(const Http1Session&) = delete;
    Http1Session& operator=(const Http1Session&) = delete;
    Http1Session(Http1Session&&) noexcept = default;
    Http1Session& operator=(Http1Session&&) noexcept = default;

    /// Feed decrypted incoming network bytes.
    /// Returns any encrypted response data that should be sent to the network.
    std::expected<std::vector<uint8_t>, int> feed_data(
        const std::vector<uint8_t>& decrypted_data,
        std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> encrypt_callback);

    /// Drain pending chunks from an active SSE response. Invoked on the TCP
    /// owner loop after Channel::publish or Channel::close wakes it.
    std::expected<std::vector<uint8_t>, int> drain_sse_outbound(
        std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> encrypt_callback);

    [[nodiscard]] bool keep_alive() const noexcept { return keep_alive_; }
    [[nodiscard]] bool upgraded() const noexcept { return upgraded_; }
    [[nodiscard]] bool sse_active() const noexcept { return sse_active_; }
    void set_peer_address(std::string_view peer_address) {
        peer_address_ = std::string(peer_address);
        current_request_.set_peer_address(peer_address_);
    }
    [[nodiscard]] std::optional<AcceptedUpgrade> take_upgrade_handler();
    [[nodiscard]] std::vector<uint8_t> take_remaining_data();

private:
    bool parse_next_request();
    void reset();

    RequestHandler handler_;
    UpgradeHandler upgrade_handler_;
    std::vector<uint8_t> buffer_;
    std::size_t parse_offset_ = 0;
    ParseState state_ = ParseState::RequestLine;

    // Currently parsing request
    http3::Request current_request_;
    std::size_t content_length_ = 0;
    bool keep_alive_ = true;
    bool upgraded_ = false;
    std::optional<AcceptedUpgrade> upgraded_handler_;
    std::shared_ptr<http::sse::Channel> sse_channel_;
    http::sse::Channel::Wakeup sse_wakeup_;
    bool sse_active_ = false;
    bool sse_terminal_sent_ = false;
    std::string peer_address_;
};

} // namespace novaboot::http1
