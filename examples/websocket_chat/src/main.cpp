#include "novaboot/novaboot.h"

#include <cstdlib>
#include <string>

#include <spdlog/spdlog.h>

using namespace novaboot;
using namespace novaboot::annotations;
using namespace novaboot::di;

/// A minimal real-time endpoint. `SessionRegistry` holds only safe handles;
/// it never writes a socket from this component's thread.
struct [[= WebSocket("/ws/chat") ]] ChatEndpoint {
    websocket::SessionRegistry sessions;

    void on_open(websocket::Session& session) {
        sessions.add(session.handle());
        session.send_text("Connected to NovaBoot chat");
    }

    void on_message(websocket::Session& session,
                    const websocket::Message& message) {
        if (!message.is_text()) {
            session.send_text("Only text messages are supported by this demo");
            return;
        }
        sessions.broadcast_text(std::string(message.text()));
    }

    void on_close(websocket::Session& session,
                  const websocket::CloseInfo&) {
        sessions.remove(session.handle().id());
    }
};

/// Spring-style STOMP controller.  Client SEND frames addressed to
/// /app/chat.send are dispatched here and the return value is published to
/// /topic/chat for every subscribed client.
struct StompChatController {
    [[= MessageMapping("/chat.send") ]]
    [[= SendTo("/topic/chat") ]]
    std::string send(std::string_view message) {
        return std::string(message);
    }
};

int main() {
    spdlog::set_level(spdlog::level::info);

    RootContainer di_root;
    register_beans<ChatEndpoint>(di_root);
    di_root.build();

    messaging::stomp::SimpleBroker stomp_broker;
    messaging::stomp::MessagingTemplate messaging(stomp_broker);
    messaging::stomp::MessageDispatcher message_dispatcher;
    StompChatController stomp_controller;
    register_message_mappings(message_dispatcher, messaging, stomp_controller);
    messaging::stomp::Endpoint stomp_endpoint(stomp_broker, {
        .dispatcher = &message_dispatcher,
        .interceptor = {},
    });

    auto app = Server::create()
        .bind("0.0.0.0", 4436)
        .tls("cert.pem", "key.pem")
        .workers(1)
        .di_container(di_root)
        .static_resources("examples/websocket_chat/src/resources/static")
        .build();
    app->stomp("/ws/stomp", stomp_endpoint);

    app->run();
    di_root.shutdown();
    return EXIT_SUCCESS;
}
