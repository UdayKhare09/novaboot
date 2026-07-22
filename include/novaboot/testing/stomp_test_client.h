#pragma once

#include "novaboot/messaging/stomp.h"
#include "novaboot/testing/websocket_test_client.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace novaboot::testing {

/// In-process STOMP 1.2 peer built on WebSocketTestClient. It uses the real
/// endpoint and STOMP codec, making it appropriate for broker/controller tests.
class StompTestClient {
public:
    explicit StompTestClient(messaging::stomp::Endpoint& endpoint, std::string principal = {})
        : websocket_(endpoint.websocket_handler(), std::move(principal)) {}

    void send(messaging::stomp::Frame frame) {
        websocket_.send_text(messaging::stomp::serialize(frame));
    }

    void connect() {
        send({.command = "CONNECT", .headers = {{"accept-version", "1.2"}}, .body = {}});
        const auto frames = take_frames();
        if (frames.size() != 1 || frames.front().command != "CONNECTED") {
            throw std::runtime_error("STOMP test client expected CONNECTED");
        }
    }

    [[nodiscard]] std::vector<messaging::stomp::Frame> take_frames() {
        std::vector<messaging::stomp::Frame> result;
        for (const auto& frame : websocket_.take_frames()) {
            if (frame.opcode != websocket::Opcode::Text) continue;
            const auto decoded = decoder_.feed(frame.text());
            if (!decoded) throw std::runtime_error("STOMP test client could not decode endpoint frame: " +
                                                   decoded.error().message);
            result.insert(result.end(), decoded->begin(), decoded->end());
        }
        return result;
    }

    void require_receipt(std::string_view receipt_id) {
        const auto frames = take_frames();
        if (frames.size() != 1 || frames.front().command != "RECEIPT" ||
            frames.front().header("receipt-id") != receipt_id) {
            throw std::runtime_error("STOMP test client expected matching RECEIPT");
        }
    }

    [[nodiscard]] bool closed() const noexcept { return websocket_.closed(); }

private:
    WebSocketTestClient websocket_;
    messaging::stomp::Decoder decoder_;
};

} // namespace novaboot::testing
