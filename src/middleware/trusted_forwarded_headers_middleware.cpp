#include "novaboot/middleware/trusted_forwarded_headers_middleware.h"

#include <cctype>
#include <array>
#include <arpa/inet.h>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace novaboot::middleware {
namespace {

std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

bool safe_value(std::string_view value) noexcept {
    if (value.empty()) return false;
    for (const unsigned char character : value) {
        if (character < 0x21 || character == 0x7f || character == ',' || character == ';') return false;
    }
    return true;
}

bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

std::string_view unquote(std::string_view value) noexcept {
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return value;
}

bool parse_ip(std::string_view value, std::array<unsigned char, 16>& output,
              int& family) noexcept {
    std::string text(value);
    if (::inet_pton(AF_INET, text.c_str(), output.data()) == 1) {
        family = AF_INET;
        return true;
    }
    if (::inet_pton(AF_INET6, text.c_str(), output.data()) == 1) {
        family = AF_INET6;
        return true;
    }
    return false;
}

bool cidr_contains(std::string_view cidr, std::string_view peer) noexcept {
    const auto slash = cidr.find('/');
    if (slash == std::string_view::npos) return false;
    const auto prefix_text = cidr.substr(slash + 1);
    int prefix = 0;
    for (const auto character : prefix_text) {
        if (!std::isdigit(static_cast<unsigned char>(character))) return false;
        prefix = prefix * 10 + (character - '0');
    }
    std::array<unsigned char, 16> network{};
    std::array<unsigned char, 16> candidate{};
    int network_family = 0;
    int candidate_family = 0;
    if (!parse_ip(cidr.substr(0, slash), network, network_family) ||
        !parse_ip(peer, candidate, candidate_family) || network_family != candidate_family) {
        return false;
    }
    const int max_bits = network_family == AF_INET ? 32 : 128;
    if (prefix < 0 || prefix > max_bits) return false;
    const int full_bytes = prefix / 8;
    const int trailing_bits = prefix % 8;
    if (full_bytes > 0 && std::memcmp(network.data(), candidate.data(), full_bytes) != 0) {
        return false;
    }
    if (trailing_bits == 0) return true;
    const auto mask = static_cast<unsigned char>(0xffU << (8 - trailing_bits));
    return (network[full_bytes] & mask) == (candidate[full_bytes] & mask);
}

std::optional<std::string_view> forwarded_client_ip(std::string_view value) noexcept {
    value = unquote(value);
    if (value.starts_with('[')) {
        const auto closing = value.find(']');
        if (closing == std::string_view::npos) return std::nullopt;
        value = value.substr(1, closing - 1);
    } else if (value.find(':') != value.rfind(':')) {
        // Bare IPv6 address.
    } else if (const auto colon = value.find(':'); colon != std::string_view::npos) {
        value = value.substr(0, colon); // IPv4:port form.
    }
    std::array<unsigned char, 16> ignored{};
    int family = 0;
    return parse_ip(value, ignored, family) ? std::optional(value) : std::nullopt;
}

} // namespace

TrustedForwardedHeadersMiddleware::TrustedForwardedHeadersMiddleware()
    : TrustedForwardedHeadersMiddleware(Config{}) {}

TrustedForwardedHeadersMiddleware::TrustedForwardedHeadersMiddleware(Config config)
    : config_(config) {
    if (!config_.trust_all_direct_peers && config_.trusted_peer_cidrs.empty()) {
        throw std::invalid_argument(
            "TrustedForwardedHeadersMiddleware requires trusted peer CIDRs or explicit trust_all_direct_peers");
    }
}

bool TrustedForwardedHeadersMiddleware::peer_is_trusted(std::string_view peer) const noexcept {
    if (config_.trust_all_direct_peers) return true;
    for (const auto& cidr : config_.trusted_peer_cidrs) {
        if (cidr_contains(cidr, peer)) return true;
    }
    return false;
}

void TrustedForwardedHeadersMiddleware::handle(http3::Request& request,
                                                http3::Response&,
                                                context::RequestContext&, Next next) {
    if (!peer_is_trusted(request.peer_address())) {
        next();
        return;
    }
    if (const auto header = request.header("forwarded")) {
        const auto first_element = header->substr(0, header->find(','));
        std::string_view remaining = first_element;
        while (!remaining.empty()) {
            const auto separator = remaining.find(';');
            const auto part = trim(remaining.substr(0, separator));
            const auto equals = part.find('=');
            if (equals != std::string_view::npos) {
                const auto name = trim(part.substr(0, equals));
                const auto value = unquote(part.substr(equals + 1));
                if (safe_value(value)) {
                    if (config_.apply_proto && equals_ignore_case(name, "proto") &&
                        (value == "https" || value == "http")) {
                        request.set_scheme(value);
                    }
                    if (config_.apply_host && equals_ignore_case(name, "host")) {
                        request.set_authority(value);
                    }
                    if (equals_ignore_case(name, "for")) {
                        if (const auto client_ip = forwarded_client_ip(value)) {
                            request.set_client_address(*client_ip);
                        }
                    }
                }
            }
            if (separator == std::string_view::npos) break;
            remaining.remove_prefix(separator + 1);
        }
    }
    next();
}

} // namespace novaboot::middleware
