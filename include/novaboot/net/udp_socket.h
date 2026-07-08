#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "novaboot/net/address.h"
#include "novaboot/net/packet.h"

namespace novaboot::net {

/// Configuration for UdpSocket
struct UdpSocketConfig {
    Address bind_address;

    /// Enable SO_REUSEPORT (required for thread-per-core model)
    bool reuse_port = true;

    /// Enable SO_REUSEADDR
    bool reuse_addr = true;

    /// Enable UDP GRO (Generic Receive Offload) for batched receives
    bool enable_gro = true;

    /// Enable UDP GSO (Generic Segmentation Offload) for batched sends
    bool enable_gso = true;

    /// Enable IP_PKTINFO to know which local addr received the packet
    bool enable_pktinfo = true;

    /// Enable ECN (Explicit Congestion Notification)
    bool enable_ecn = true;

    /// Receive buffer size (SO_RCVBUF)
    int recv_buffer_size = 2 * 1024 * 1024; // 2MB

    /// Send buffer size (SO_SNDBUF)
    int send_buffer_size = 2 * 1024 * 1024; // 2MB

    /// Maximum number of messages for recvmmsg/sendmmsg batching
    int max_batch_size = 64;
};

/// High-performance UDP socket for QUIC traffic.
///
/// Features:
///   - SO_REUSEPORT for thread-per-core binding
///   - Non-blocking I/O
///   - recvmmsg() for batched receives
///   - sendmmsg() with GSO for batched sends
///   - GRO for coalesced receives
///   - ECN support for congestion notification
///   - IP_PKTINFO for local address detection
class UdpSocket {
public:
    enum class Error {
        SocketCreate,
        SetOption,
        Bind,
        WouldBlock,
        Closed,
        SystemError,
    };

    ~UdpSocket();

    // Non-copyable, movable
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    /// Create and bind a UDP socket
    static std::expected<UdpSocket, Error> create(const UdpSocketConfig& config);

    /// Get the raw file descriptor (for epoll registration)
    [[nodiscard]] int fd() const noexcept { return fd_; }

    /// Receive multiple datagrams via recvmmsg()
    /// Returns the number of messages received, or an error.
    /// Fills the packets vector with received data.
    std::expected<int, Error> recv_batch(
        std::vector<IncomingPacket>& packets,
        std::span<std::uint8_t> buffer);

    /// Send multiple datagrams via sendmmsg()
    /// Returns the number of messages sent, or an error.
    std::expected<int, Error> send_batch(
        std::span<OutgoingPacket> packets);

    /// Send a single datagram via sendmsg()
    /// Supports GSO via ancillary data.
    std::expected<std::size_t, Error> send_one(const OutgoingPacket& packet);

    /// Receive a single datagram via recvmsg()
    std::expected<IncomingPacket, Error> recv_one(
        std::span<std::uint8_t> buffer);

    /// Check if GSO is supported on this socket
    [[nodiscard]] bool gso_supported() const noexcept { return gso_supported_; }

    /// Check if GRO is supported on this socket
    [[nodiscard]] bool gro_supported() const noexcept { return gro_supported_; }

    /// Close the socket
    void close();

private:
    UdpSocket() = default;
    explicit UdpSocket(int fd) : fd_(fd) {}

    void configure_socket(const UdpSocketConfig& config);

    int  fd_             = -1;
    bool gso_supported_  = false;
    bool gro_supported_  = false;
    bool pktinfo_enabled_ = false;
    bool ecn_enabled_    = false;
    int  max_batch_size_ = 64;
};

} // namespace novaboot::net
