#include "novaboot/net/address.h"

#include <compare>
#include <format>
#include <stdexcept>

namespace novaboot::net {

Address::Address() noexcept {
    std::memset(&storage_, 0, sizeof(storage_));
    storage_.v4_.sin_family = AF_INET;
    len_ = sizeof(struct sockaddr_in);
}

Address Address::from_ipv4(const std::string& ip, std::uint16_t port) {
    Address addr;
    std::memset(&addr.storage_, 0, sizeof(addr.storage_));
    addr.storage_.v4_.sin_family = AF_INET;
    addr.storage_.v4_.sin_port   = htons(port);
    addr.len_ = sizeof(struct sockaddr_in);

    if (::inet_pton(AF_INET, ip.c_str(), &addr.storage_.v4_.sin_addr) != 1) {
        throw std::invalid_argument(
            std::format("Invalid IPv4 address: {}", ip));
    }
    return addr;
}

Address Address::from_ipv6(const std::string& ip, std::uint16_t port) {
    Address addr;
    std::memset(&addr.storage_, 0, sizeof(addr.storage_));
    addr.storage_.v6_.sin6_family = AF_INET6;
    addr.storage_.v6_.sin6_port   = htons(port);
    addr.len_ = sizeof(struct sockaddr_in6);

    if (::inet_pton(AF_INET6, ip.c_str(), &addr.storage_.v6_.sin6_addr) != 1) {
        throw std::invalid_argument(
            std::format("Invalid IPv6 address: {}", ip));
    }
    return addr;
}

Address Address::from_string(const std::string& ip, std::uint16_t port) {
    // Try IPv4 first, then IPv6
    struct in_addr v4_test;
    if (::inet_pton(AF_INET, ip.c_str(), &v4_test) == 1) {
        return from_ipv4(ip, port);
    }

    struct in6_addr v6_test;
    if (::inet_pton(AF_INET6, ip.c_str(), &v6_test) == 1) {
        return from_ipv6(ip, port);
    }

    throw std::invalid_argument(
        std::format("Invalid IP address: {}", ip));
}

Address Address::from_sockaddr(const struct sockaddr* sa,
                               socklen_t len) noexcept {
    Address addr;
    std::memcpy(&addr.storage_, sa, len);
    addr.len_ = len;
    return addr;
}

const struct sockaddr* Address::sockaddr_ptr() const noexcept {
    return &storage_.sa_;
}

struct sockaddr* Address::sockaddr_ptr_mut() noexcept {
    return &storage_.sa_;
}

socklen_t Address::sockaddr_len() const noexcept {
    return len_;
}

socklen_t* Address::sockaddr_len_ptr() noexcept {
    return &len_;
}

std::uint16_t Address::port() const noexcept {
    if (storage_.sa_.sa_family == AF_INET6) {
        return ntohs(storage_.v6_.sin6_port);
    }
    return ntohs(storage_.v4_.sin_port);
}

int Address::family() const noexcept {
    return storage_.sa_.sa_family;
}

bool Address::is_v4() const noexcept {
    return storage_.sa_.sa_family == AF_INET;
}

bool Address::is_v6() const noexcept {
    return storage_.sa_.sa_family == AF_INET6;
}

std::string Address::to_string() const {
    char buf[INET6_ADDRSTRLEN] = {};

    if (is_v4()) {
        ::inet_ntop(AF_INET, &storage_.v4_.sin_addr, buf, sizeof(buf));
        return std::format("{}:{}", buf, port());
    } else {
        ::inet_ntop(AF_INET6, &storage_.v6_.sin6_addr, buf, sizeof(buf));
        return std::format("[{}]:{}", buf, port());
    }
}

bool Address::operator==(const Address& other) const noexcept {
    if (family() != other.family()) return false;
    if (len_ != other.len_) return false;
    return std::memcmp(&storage_, &other.storage_, len_) == 0;
}

bool Address::operator!=(const Address& other) const noexcept {
    return !(*this == other);
}

auto Address::operator<=>(const Address& other) const noexcept
    -> std::strong_ordering {
    if (auto cmp = family() <=> other.family(); cmp != 0) {
        return cmp;
    }
    int result = std::memcmp(&storage_, &other.storage_, len_);
    if (result < 0) return std::strong_ordering::less;
    if (result > 0) return std::strong_ordering::greater;
    return std::strong_ordering::equal;
}

std::size_t Address::Hash::operator()(const Address& addr) const noexcept {
    // FNV-1a hash over the raw sockaddr bytes
    std::size_t hash = 14695981039346656037ULL;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&addr.storage_);
    for (socklen_t i = 0; i < addr.len_; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace novaboot::net
