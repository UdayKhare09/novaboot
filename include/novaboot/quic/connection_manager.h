#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <ngtcp2/ngtcp2.h>

#include "novaboot/net/address.h"
#include "novaboot/quic/quic_connection.h"

namespace novaboot::quic {

class TlsContext;

/// Hash function for ngtcp2_cid (Connection ID)
struct CidHash {
    std::size_t operator()(const ngtcp2_cid& cid) const noexcept {
        // FNV-1a hash
        std::size_t hash = 14695981039346656037ULL;
        for (std::size_t i = 0; i < cid.datalen; ++i) {
            hash ^= cid.data[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

/// Equality comparison for ngtcp2_cid
struct CidEqual {
    bool operator()(const ngtcp2_cid& a,
                    const ngtcp2_cid& b) const noexcept {
        return ngtcp2_cid_eq(&a, &b);
    }
};

/// Per-shard connection manager.
///
/// Maps Connection IDs to QuicConnection objects. Handles:
///   - Initial packet routing (new connection detection)
///   - CID-based connection lookup
///   - Multiple CIDs per connection (CID rotation)
///   - Connection lifecycle (creation, idle timeout, cleanup)
///
/// Designed for future connection migration support:
///   - CID → connection mapping (not address → connection)
///   - Connection owns its CIDs, not the other way around
///
/// Thread-safety: NOT thread-safe. Owned by exactly one Shard.
class ConnectionManager {
public:
    /// Callback invoked when a new connection is established
    /// (after handshake completes). The Http3Session should be
    /// created and attached to the connection at this point.
    using HandshakeCompleteCallback =
        std::function<void(QuicConnection& conn)>;

    explicit ConnectionManager(const TlsContext& tls_ctx,
                               core::EventLoop& event_loop);
    ~ConnectionManager() = default;

    // Non-copyable, non-movable
    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    /// Set the callback for handshake completion
    void set_handshake_callback(HandshakeCompleteCallback cb) {
        handshake_cb_ = std::move(cb);
    }

    /// Set the callback for sending packets
    void set_send_callback(SendPacketCallback cb) {
        send_cb_ = std::move(cb);
    }

    /// Process an incoming UDP packet.
    /// Routes to existing connection or creates a new one.
    /// Returns 0 on success, negative on error.
    int on_packet(const net::IncomingPacket& packet);

    /// Find a connection by Connection ID
    [[nodiscard]] QuicConnection* find(const ngtcp2_cid& cid);

    /// Register an additional CID for an existing connection
    void add_cid(const ngtcp2_cid& cid, QuicConnection* conn);

    /// Remove a CID mapping
    void remove_cid(const ngtcp2_cid& cid);

    /// Remove a connection and all its CIDs
    void remove_connection(QuicConnection* conn);

    /// Clean up closed/draining connections
    void cleanup();

    /// Write pending data for all connections
    void write_all();

    /// Number of active connections
    [[nodiscard]] std::size_t size() const noexcept {
        return connections_.size();
    }

private:
    /// Try to accept a new connection from an Initial packet
    QuicConnection* accept_connection(const net::IncomingPacket& packet);

    const TlsContext& tls_ctx_;
    core::EventLoop&  event_loop_;

    /// CID → QuicConnection (many-to-one: a connection can have multiple CIDs)
    std::unordered_map<ngtcp2_cid, QuicConnection*, CidHash, CidEqual>
        cid_map_;

    /// Owning storage for connections
    std::vector<std::unique_ptr<QuicConnection>> connections_;

    HandshakeCompleteCallback handshake_cb_;
    SendPacketCallback        send_cb_;
};

} // namespace novaboot::quic
