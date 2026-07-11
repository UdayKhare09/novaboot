#pragma once

#include <vector>
#include <string>
#include <functional>
#include <expected>
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"

namespace novaboot::http1 {

class Http1Session {
public:
    enum class ParseState {
        RequestLine,
        Headers,
        Body,
        Complete,
        Error
    };

    using RequestHandler = std::function<void(http3::Request&, http3::Response&)>;

    explicit Http1Session(RequestHandler handler) : handler_(std::move(handler)) {}
    ~Http1Session() = default;

    /// Feed decrypted incoming network bytes.
    /// Returns any encrypted response data that should be sent to the network.
    std::expected<std::vector<uint8_t>, int> feed_data(
        const std::vector<uint8_t>& decrypted_data,
        std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> encrypt_callback);

    [[nodiscard]] bool keep_alive() const noexcept { return keep_alive_; }

private:
    bool parse_next_request();
    void reset();

    RequestHandler handler_;
    std::vector<uint8_t> buffer_;
    std::size_t parse_offset_ = 0;
    ParseState state_ = ParseState::RequestLine;

    // Currently parsing request
    http3::Request current_request_;
    std::size_t content_length_ = 0;
    bool keep_alive_ = true;
};

} // namespace novaboot::http1
