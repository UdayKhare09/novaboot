#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <variant>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace novaboot::net {

/// Type-safe wrapper around sockaddr_in / sockaddr_in6.
/// Supports IPv4, IPv6, and comparison/hashing for use as map keys.
class Address {
public:
    Address() noexcept;

    /// Construct from IPv4 address string and port
    static Address from_ipv4(const std::string& ip, std::uint16_t port);

    /// Construct from IPv6 address string and port
    static Address from_ipv6(const std::string& ip, std::uint16_t port);

    /// Construct from any string (auto-detect v4/v6) and port
    static Address from_string(const std::string& ip, std::uint16_t port);

    /// Construct from raw sockaddr
    static Address from_sockaddr(const struct sockaddr* sa,
                                 socklen_t len) noexcept;

    /// Get the raw sockaddr pointer (for sendto/recvfrom)
    [[nodiscard]] const struct sockaddr* sockaddr_ptr() const noexcept;
    [[nodiscard]] struct sockaddr*       sockaddr_ptr_mut() noexcept;
    [[nodiscard]] socklen_t              sockaddr_len() const noexcept;
    [[nodiscard]] socklen_t*             sockaddr_len_ptr() noexcept;

    /// Get port (host byte order)
    [[nodiscard]] std::uint16_t port() const noexcept;

    /// Set port (host byte order)
    void set_port(std::uint16_t port) noexcept;

    /// Get address family (AF_INET or AF_INET6)
    [[nodiscard]] int family() const noexcept;

    /// Check if this is an IPv4 address
    [[nodiscard]] bool is_v4() const noexcept;

    /// Check if this is an IPv6 address
    [[nodiscard]] bool is_v6() const noexcept;

    /// Human-readable representation
    [[nodiscard]] std::string to_string() const;
    /// Numeric host address without a port, suitable for request peer identity.
    [[nodiscard]] std::string ip_string() const;

    // Comparison operators
    bool operator==(const Address& other) const noexcept;
    bool operator!=(const Address& other) const noexcept;
    std::strong_ordering operator<=>(const Address& other) const noexcept;

    /// Hash support for use in unordered containers
    struct Hash {
        std::size_t operator()(const Address& addr) const noexcept;
    };

private:
    union {
        struct sockaddr_in  v4_;
        struct sockaddr_in6 v6_;
        struct sockaddr     sa_;
    } storage_;
    socklen_t len_ = sizeof(struct sockaddr_in);
};

} // namespace novaboot::net
