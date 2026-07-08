#include "novaboot/quic/connection_manager.h"

#include <algorithm>
#include <cstring>

#include <ngtcp2/ngtcp2.h>

#include <spdlog/spdlog.h>

namespace novaboot::quic {

ConnectionManager::ConnectionManager(const TlsContext& tls_ctx,
                                     core::EventLoop& event_loop)
    : tls_ctx_(tls_ctx), event_loop_(event_loop) {
}

int ConnectionManager::on_packet(const net::IncomingPacket& packet) {
    // Decode the version and connection IDs from the raw packet
    ngtcp2_version_cid vc;
    int rv = ngtcp2_pkt_decode_version_cid(
        &vc, packet.data, packet.size, NGTCP2_MIN_CIDLEN);

    if (rv != 0) {
        if (rv == NGTCP2_ERR_VERSION_NEGOTIATION) {
            // TODO: Send version negotiation packet
            spdlog::debug("Version negotiation required");
            return 0;
        }
        spdlog::debug("Failed to decode packet version/CID: {}",
                      ngtcp2_strerror(rv));
        return 0; // Drop malformed packets silently
    }

    // Look up the connection by Destination CID
    ngtcp2_cid dcid;
    ngtcp2_cid_init(&dcid, vc.dcid, vc.dcidlen);

    auto* conn = find(dcid);

    if (conn) {
        // Existing connection — feed the packet
        if (conn->is_draining()) {
            return 0; // Drop packets for draining connections
        }

        auto ts = QuicConnection::timestamp_now();
        rv = conn->on_read(packet, ts);

        if (rv != 0) {
            spdlog::debug("Connection read error, closing: {}",
                          ngtcp2_strerror(rv));
            // Mark for cleanup
        }

        // Write any pending response data
        conn->on_write();
        return rv;
    }

    // No existing connection — try to accept a new one
    conn = accept_connection(packet);
    if (!conn) {
        return 0; // Not an Initial packet or accept failed
    }

    // Feed the packet into the new connection
    auto ts = QuicConnection::timestamp_now();
    rv = conn->on_read(packet, ts);

    if (rv != 0) {
        spdlog::debug("New connection first read error: {}",
                      ngtcp2_strerror(rv));
        return rv;
    }

    // Write handshake response
    conn->on_write();
    return 0;
}

QuicConnection* ConnectionManager::accept_connection(
    const net::IncomingPacket& packet) {

    // Validate this is an Initial packet
    ngtcp2_pkt_hd hd;
    int rv = ngtcp2_accept(&hd, packet.data, packet.size);
    if (rv != 0) {
        // Not a valid Initial packet — drop silently
        return nullptr;
    }

    spdlog::debug("Accepting new QUIC connection from {}",
                  packet.remote.to_string());

    try {
        auto conn = QuicConnection::create(
            tls_ctx_, hd,
            packet.local, packet.remote,
            event_loop_, send_cb_);

        auto* conn_ptr = conn.get();

        // Register the server's CID
        cid_map_[conn_ptr->scid()] = conn_ptr;

        // Also register the client's original DCID (used in Initial packets)
        cid_map_[hd.dcid] = conn_ptr;

        connections_.push_back(std::move(conn));

        // Notify about handshake completion setup
        // The actual Http3Session is created when handshake completes
        // (via the ngtcp2 handshake_completed callback → Shard)
        if (handshake_cb_) {
            conn_ptr->set_handshake_callback(handshake_cb_);
        }

        return conn_ptr;
    } catch (const std::exception& e) {
        spdlog::error("Failed to create QUIC connection: {}", e.what());
        return nullptr;
    }
}

QuicConnection* ConnectionManager::find(const ngtcp2_cid& cid) {
    auto it = cid_map_.find(cid);
    if (it != cid_map_.end()) {
        return it->second;
    }
    return nullptr;
}

void ConnectionManager::add_cid(const ngtcp2_cid& cid,
                                QuicConnection* conn) {
    cid_map_[cid] = conn;
}

void ConnectionManager::remove_cid(const ngtcp2_cid& cid) {
    cid_map_.erase(cid);
}

void ConnectionManager::remove_connection(QuicConnection* conn) {
    // Remove all CIDs pointing to this connection
    std::erase_if(cid_map_, [conn](const auto& pair) {
        return pair.second == conn;
    });

    // Remove from owning storage
    std::erase_if(connections_, [conn](const auto& ptr) {
        return ptr.get() == conn;
    });
}

void ConnectionManager::cleanup() {
    // Collect connections to remove
    std::vector<QuicConnection*> to_remove;

    for (auto& conn : connections_) {
        if (conn->is_closed() || conn->is_draining()) {
            to_remove.push_back(conn.get());
        }
    }

    for (auto* conn : to_remove) {
        spdlog::debug("Cleaning up connection from {}",
                      conn->remote_addr().to_string());
        remove_connection(conn);
    }
}

void ConnectionManager::write_all() {
    for (auto& conn : connections_) {
        if (!conn->is_closed() && !conn->is_draining()) {
            conn->on_write();
        }
    }
}

} // namespace novaboot::quic
