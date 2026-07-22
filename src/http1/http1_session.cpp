#include "novaboot/http1/http1_session.h"
#include <sstream>
#include <format>
#include <algorithm>
#include <utility>
#include <spdlog/spdlog.h>

namespace novaboot::http1 {

namespace {

std::string_view trim(std::string_view sv) {
    auto start = sv.find_first_not_of(" \t");
    if (start == std::string_view::npos) return {};
    auto end = sv.find_last_not_of(" \t");
    return sv.substr(start, end - start + 1);
}

std::string get_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 101: return "Switching Protocols";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

std::vector<uint8_t> serialize_upgrade_response(std::string_view accept_key,
                                                std::string_view subprotocol) {
    const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                 "Upgrade: websocket\r\n"
                                 "Connection: Upgrade\r\n"
                                 "Sec-WebSocket-Accept: " + std::string(accept_key) + "\r\n" +
                                 (subprotocol.empty() ? "" :
                                  "Sec-WebSocket-Protocol: " + std::string(subprotocol) + "\r\n") +
                                 "\r\n";
    return {response.begin(), response.end()};
}

std::vector<uint8_t> serialize_rejection_response(int status, std::string_view body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << " " << get_status_text(status) << "\r\n"
             << "Connection: close\r\n"
             << "Content-Type: text/plain\r\n"
             << "Content-Length: " << body.size() << "\r\n\r\n"
             << body;
    const auto serialized = response.str();
    return {serialized.begin(), serialized.end()};
}

void append_chunk(std::vector<uint8_t>& output, std::string_view body) {
    const auto length = std::format("{:x}", body.size());
    output.insert(output.end(), length.begin(), length.end());
    output.push_back('\r');
    output.push_back('\n');
    output.insert(output.end(), body.begin(), body.end());
    output.push_back('\r');
    output.push_back('\n');
}

// Attempts to read a line up to \r\n.
// Returns empty view and leaves offset unchanged if complete line not yet found.
std::optional<std::string_view> get_line(const std::vector<uint8_t>& buffer, std::size_t& offset) {
    for (std::size_t i = offset; i + 1 < buffer.size(); ++i) {
        if (buffer[i] == '\r' && buffer[i+1] == '\n') {
            std::string_view line(reinterpret_cast<const char*>(buffer.data() + offset), i - offset);
            offset = i + 2;
            return line;
        }
    }
    return std::nullopt;
}

} // anonymous namespace

void Http1Session::reset() {
    state_ = ParseState::RequestLine;
    current_request_ = http3::Request();
    current_request_.set_peer_address(peer_address_);
    content_length_ = 0;
}

std::optional<Http1Session::AcceptedUpgrade> Http1Session::take_upgrade_handler() {
    return std::exchange(upgraded_handler_, std::nullopt);
}

Http1Session::~Http1Session() {
    if (sse_channel_) {
        // A publisher may outlive the TCP client. Detach its owner-loop wakeup
        // before closing the channel so it cannot post into a destroyed manager.
        sse_channel_->set_wakeup({});
        sse_channel_->close();
    }
}

std::vector<uint8_t> Http1Session::take_remaining_data() {
    if (parse_offset_ >= buffer_.size()) {
        buffer_.clear();
        parse_offset_ = 0;
        return {};
    }
    std::vector<uint8_t> remaining(
        buffer_.begin() + static_cast<std::vector<uint8_t>::difference_type>(parse_offset_),
        buffer_.end());
    buffer_.clear();
    parse_offset_ = 0;
    return remaining;
}

std::expected<std::vector<uint8_t>, int> Http1Session::feed_data(
    const std::vector<uint8_t>& decrypted_data,
    std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> encrypt_callback) {

    if (sse_active_) {
        return drain_sse_outbound(std::move(encrypt_callback));
    }
    buffer_.insert(buffer_.end(), decrypted_data.begin(), decrypted_data.end());
    std::vector<uint8_t> pending_responses;

    while (parse_offset_ < buffer_.size()) {
        if (state_ == ParseState::RequestLine) {
            auto line_opt = get_line(buffer_, parse_offset_);
            if (!line_opt) break; // Need more data

            std::string_view line = *line_opt;
            if (line.empty()) continue; // skip leading empty lines

            std::size_t first_space = line.find(' ');
            std::size_t last_space = line.rfind(' ');
            if (first_space == std::string_view::npos || last_space == std::string_view::npos || first_space == last_space) {
                spdlog::warn("Invalid HTTP/1.1 request line: {}", line);
                state_ = ParseState::Error;
                return std::unexpected(-1);
            }

            std::string_view method = line.substr(0, first_space);
            std::string_view path = line.substr(first_space + 1, last_space - first_space - 1);
            std::string_view version = line.substr(last_space + 1);
            (void)version;

            current_request_.set_method(method);
            current_request_.set_path(path);
            current_request_.set_scheme("https");

            state_ = ParseState::Headers;
        }

        if (state_ == ParseState::Headers) {
            bool done = false;
            while (true) {
                auto line_opt = get_line(buffer_, parse_offset_);
                if (!line_opt) {
                    done = true;
                    break; // Need more data
                }

                std::string_view line = *line_opt;
                if (line.empty()) {
                    // Empty line marks end of headers
                    current_request_.mark_headers_complete();
                    if (websocket::is_upgrade_attempt(current_request_) && upgrade_handler_) {
                        auto upgrade_result = upgrade_handler_(current_request_);
                        if (upgrade_result.kind == UpgradeResult::Kind::Rejected) {
                            const auto raw = serialize_rejection_response(
                                upgrade_result.rejection_status,
                                upgrade_result.rejection_body);
                            const auto encrypted = encrypt_callback(raw);
                            pending_responses.insert(pending_responses.end(),
                                                     encrypted.begin(), encrypted.end());
                            keep_alive_ = false;
                            reset();
                            break;
                        }
                        if (upgrade_result.kind == UpgradeResult::Kind::Accepted &&
                            upgrade_result.handler) {
                            auto accept_key = websocket::validate_upgrade_request(current_request_);
                            if (!accept_key) {
                                const auto raw = serialize_rejection_response(
                                    400, accept_key.error().message);
                                const auto encrypted = encrypt_callback(raw);
                                pending_responses.insert(pending_responses.end(), encrypted.begin(), encrypted.end());
                                keep_alive_ = false;
                                reset();
                                break;
                            }
                            const auto raw = serialize_upgrade_response(*accept_key,
                                                                        upgrade_result.subprotocol);
                            const auto encrypted = encrypt_callback(raw);
                            pending_responses.insert(pending_responses.end(), encrypted.begin(), encrypted.end());
                            upgraded_handler_ = AcceptedUpgrade{
                                .handler = std::move(*upgrade_result.handler),
                                .principal = std::move(upgrade_result.principal),
                                .subprotocol = std::move(upgrade_result.subprotocol),
                            };
                            upgraded_ = true;
                            keep_alive_ = true;
                            break;
                        }
                    }
                    state_ = (content_length_ > 0) ? ParseState::Body : ParseState::Complete;
                    break;
                }

                std::size_t colon = line.find(':');
                if (colon == std::string_view::npos) {
                    spdlog::warn("Invalid HTTP/1.1 header: {}", line);
                    state_ = ParseState::Error;
                    return std::unexpected(-2);
                }

                std::string_view name = trim(line.substr(0, colon));
                std::string_view value = trim(line.substr(colon + 1));

                current_request_.headers().add(name, value);

                // Check for Content-Length
                std::string name_lower(name);
                std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
                if (name_lower == "content-length") {
                    try {
                        content_length_ = std::stoull(std::string(value));
                    } catch (...) {
                        state_ = ParseState::Error;
                        return std::unexpected(-3);
                    }
                } else if (name_lower == "connection") {
                    std::string value_lower(value);
                    std::transform(value_lower.begin(), value_lower.end(), value_lower.begin(), ::tolower);
                    if (value_lower == "close") {
                        keep_alive_ = false;
                    } else if (value_lower == "keep-alive") {
                        keep_alive_ = true;
                    }
                } else if (name_lower == "host") {
                    current_request_.set_authority(value);
                }
            }
            if (done) break; // Need more data
            if (upgraded_) break;
        }

        if (upgraded_) break;

        if (state_ == ParseState::Body) {
            std::size_t available = buffer_.size() - parse_offset_;
            if (available < content_length_) {
                break; // Need more body data
            }

            current_request_.append_body(buffer_.data() + parse_offset_, content_length_);
            parse_offset_ += content_length_;
            current_request_.mark_body_complete();
            state_ = ParseState::Complete;
        }

        if (state_ == ParseState::Complete) {
            http3::Response current_response;

            // Execute the handler
            handler_(current_request_, current_response);

            const auto& sse_channel = current_response.event_stream();
            const bool is_sse = static_cast<bool>(sse_channel);

            // Serialize response to HTTP/1.1 format
            std::ostringstream oss;
            oss << "HTTP/1.1 " << current_response.status_code() << " " << get_status_text(current_response.status_code()) << "\r\n";
            oss << "Alt-Svc: h3=\":4433\"; ma=86400\r\n";
            if (keep_alive_) {
                oss << "Connection: keep-alive\r\n";
            } else {
                oss << "Connection: close\r\n";
            }

            // Write custom headers
            for (const auto& entry : current_response.headers().entries()) {
                // Skip duplicating Connection or Alt-Svc
                std::string entry_lower = entry.name;
                std::transform(entry_lower.begin(), entry_lower.end(), entry_lower.begin(), ::tolower);
                if (entry_lower != "connection" && entry_lower != "alt-svc" &&
                    (!is_sse || (entry_lower != "content-length" &&
                                 entry_lower != "transfer-encoding"))) {
                    oss << entry.name << ": " << entry.value << "\r\n";
                }
            }

            if (is_sse) {
                oss << "Transfer-Encoding: chunked\r\n";
            } else if (!current_response.headers().has("content-length")) {
                oss << "Content-Length: " << current_response.body_size() << "\r\n";
            }
            oss << "\r\n";
            if (!is_sse) {
                oss << current_response.body_str();
            }

            std::string serialized = oss.str();
            std::vector<uint8_t> raw_resp(serialized.begin(), serialized.end());
            std::vector<uint8_t> encrypted = encrypt_callback(raw_resp);
            pending_responses.insert(pending_responses.end(), encrypted.begin(), encrypted.end());

            if (is_sse) {
                sse_channel_ = sse_channel;
                sse_active_ = true;
                sse_terminal_sent_ = false;
                sse_channel_->set_wakeup(sse_wakeup_);
                auto pending_events = drain_sse_outbound(encrypt_callback);
                if (!pending_events) return std::unexpected(pending_events.error());
                pending_responses.insert(pending_responses.end(),
                                         pending_events->begin(), pending_events->end());
                buffer_.clear();
                parse_offset_ = 0;
                reset();
                return pending_responses;
            }

            // Prepare for next request in buffer (if keepalive pipelining)
            reset();
        }
    }

    // Clean up consumed buffer bytes
    if (parse_offset_ > 0 && !upgraded_) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::vector<uint8_t>::difference_type>(parse_offset_));
        parse_offset_ = 0;
    }

    return pending_responses;
}

std::expected<std::vector<uint8_t>, int> Http1Session::drain_sse_outbound(
    std::function<std::vector<uint8_t>(const std::vector<uint8_t>&)> encrypt_callback) {
    if (!sse_active_ || !sse_channel_) return std::vector<uint8_t>{};

    std::vector<uint8_t> raw;
    while (auto event = sse_channel_->take_next()) {
        append_chunk(raw, *event);
    }
    if (sse_channel_->closed() && !sse_terminal_sent_) {
        static constexpr std::string_view terminal = "0\r\n\r\n";
        raw.insert(raw.end(), terminal.begin(), terminal.end());
        sse_terminal_sent_ = true;
        sse_active_ = false;
        keep_alive_ = false;
        sse_channel_->set_wakeup({});
        sse_channel_.reset();
    }
    if (raw.empty()) return raw;
    return encrypt_callback(raw);
}

} // namespace novaboot::http1
