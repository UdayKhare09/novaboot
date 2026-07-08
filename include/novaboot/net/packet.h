#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "novaboot/net/address.h"

namespace novaboot::net {

/// Maximum QUIC packet size we'll handle.
/// QUIC packets should be at most 1200 bytes for initial packets,
/// but can be up to ~1500 for established connections (PMTU).
/// With GRO, the kernel may coalesce multiple packets into one buffer.
static constexpr std::size_t kMaxPacketSize = 65536; // For GRO coalesced
static constexpr std::size_t kMaxUdpPayload = 1500;

/// Incoming UDP packet received from the network.
/// The data buffer is NOT owned — it points into the receive buffer
/// managed by the UdpSocket / arena allocator.
struct IncomingPacket {
    /// Raw packet data (does not own memory)
    const std::uint8_t* data = nullptr;
    std::size_t         size = 0;

    /// Source address (who sent this)
    Address remote;

    /// Local address (which of our addresses received this)
    Address local;

    /// Kernel timestamp (nanoseconds since boot, from SO_TIMESTAMPNS)
    std::uint64_t timestamp_ns = 0;

    /// ECN (Explicit Congestion Notification) codepoint
    std::uint32_t ecn = 0;

    /// For GRO: the segment size (0 if not using GRO)
    std::uint16_t gro_segment_size = 0;

    [[nodiscard]] std::span<const std::uint8_t> as_span() const noexcept {
        return {data, size};
    }
};

/// Outgoing UDP packet to send to the network.
struct OutgoingPacket {
    /// Packet data buffer
    std::uint8_t* data = nullptr;
    std::size_t   size = 0;

    /// Destination address
    Address remote;

    /// Source address (for connected sockets or specific local addr)
    Address local;

    /// ECN marking to set
    std::uint32_t ecn = 0;

    /// GSO segment size (0 to disable GSO for this send)
    std::uint16_t gso_segment_size = 0;

    [[nodiscard]] std::span<std::uint8_t> as_span() noexcept {
        return {data, size};
    }

    [[nodiscard]] std::span<const std::uint8_t> as_span() const noexcept {
        return {data, size};
    }
};

} // namespace novaboot::net
