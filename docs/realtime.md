# Real-time applications

NovaBoot supports two related, but distinct, real-time programming models:

- Raw RFC 6455 WebSockets, for applications that own their message protocol.
- STOMP 1.2 over a WebSocket, for publish/subscribe applications using
  conventional `/topic`, `/queue`, and `/user` destinations.

Both use the same WebSocket session implementation and may be exposed by the
same server. WebTransport and HTTP/3 WebSockets are not implemented yet.

## Raw WebSocket endpoints

Register an endpoint programmatically when its lifecycle callbacks are best
kept close to server setup:

```cpp
novaboot::websocket::Handler chat{
    .on_open = [](novaboot::websocket::Session& session) {
        session.send_text("connected");
    },
    .on_message = [](novaboot::websocket::Session& session,
                     const novaboot::websocket::Message& message) {
        if (message.is_text()) session.send_text(message.text());
    },
};

app->websocket("/ws/chat", std::move(chat));
```

Or let DI register a singleton endpoint with `[[= WebSocket ]]`:

```cpp
struct [[= novaboot::annotations::WebSocket("/ws/chat") ]] ChatEndpoint {
    novaboot::websocket::SessionRegistry sessions;

    void on_open(novaboot::websocket::Session& session) {
        sessions.add(session.handle());
    }

    void on_message(novaboot::websocket::Session&,
                    const novaboot::websocket::Message& message) {
        if (message.is_text()) sessions.broadcast_text(message.text());
    }

    void on_close(novaboot::websocket::Session& session,
                  const novaboot::websocket::CloseInfo&) {
        sessions.remove(session.handle().id());
    }
};
```

The supported optional annotation callbacks are `on_open`, `on_message`,
`on_close`, and `authorize`. `Session` is callback-scoped. For later or
cross-thread sends, retain `SessionHandle` through `SessionRegistry`, not a
reference to `Session`.

Outbound sending is bounded per connection. `send_text`, `send_binary`, and
the corresponding handle methods return `false` when the connection is
closing or cannot accept more queued data. A slow client is closed with WebSocket
code `1013`; applications should treat a `false` return as a failed delivery.

## Authentication and origin policy

WebSocket handshakes do not use CORS middleware. Validate the `Origin` header,
cookie, bearer token, or another credential in `Handler::authorize`; reject
unknown origins explicitly in browser-facing deployments.

```cpp
.authorize = [](const novaboot::http3::Request& request) {
    if (request.header("origin") != "https://chat.example.com") {
        return novaboot::websocket::HandshakeDecision::reject(403, "origin denied");
    }
    return novaboot::websocket::HandshakeDecision::allow("alice");
},
```

The accepted principal is exposed as `Session::principal()` and is used by
STOMP `/user/{principal}/...` deliveries. Do not use `allowed_origins = {"*"}`
as a substitute for this WebSocket policy.

## STOMP controllers

STOMP endpoints are backed by an in-process `SimpleBroker`. `/topic/**`
broadcasts to all subscribers, `/queue/**` selects one competing subscriber,
and `/user/{principal}/...` only reaches sessions with that principal.

```cpp
struct ChatController {
    [[= novaboot::annotations::MessageMapping("/chat.send") ]]
    [[= novaboot::annotations::SendTo("/topic/chat") ]]
    std::string send(std::string_view body) { return std::string(body); }
};

novaboot::messaging::stomp::SimpleBroker broker;
novaboot::messaging::stomp::MessagingTemplate messaging(broker);
novaboot::messaging::stomp::MessageDispatcher dispatcher;
ChatController controller;
novaboot::annotations::register_message_mappings(dispatcher, messaging, controller);

novaboot::messaging::stomp::Endpoint endpoint(broker, {
    .dispatcher = &dispatcher,
    .interceptor = {},
});
app->stomp("/ws/stomp", endpoint);
```

By default, the endpoint reserves `/app` as the application destination
prefix. Therefore a browser sends to `/app/chat.send`, while the controller
annotation is `MessageMapping("/chat.send")`. Broker destinations such as
`/topic/chat` are sent directly to the broker; they do not invoke controllers.
Set `Endpoint::Config::application_destination_prefix` to use a different
prefix, or to an empty string to disable prefix-based controller dispatch.

`SendTo` supports a `std::string` result. A `void` mapping may perform its own
server push through `MessagingTemplate::convert_and_send`. The local broker
supports STOMP `CONNECT`/`STOMP`, `SUBSCRIBE`, `SEND`, `UNSUBSCRIBE`, `ACK`,
`NACK`, `DISCONNECT`, receipts, heartbeats, and `ERROR`. Manual ACK modes are
in-memory at-least-once delivery; they do not provide persistence,
dead-lettering, delayed retries, or database transaction coupling.

## Browser and proxy deployment

Use `wss://` in production. The browser chooses the underlying HTTP version:
the normal WebSocket JavaScript API cannot force one. NovaBoot accepts the
HTTP/1.1 Upgrade handshake and HTTP/2 extended CONNECT (RFC 8441) through the
same endpoint. An HTTP/2-capable browser may still open a distinct HTTP/1.1
WebSocket connection when an eligible HTTP/2 connection is not available.

Browsers do not reconnect WebSockets automatically. Recreate the socket after
`onclose`, resubscribe after STOMP `CONNECTED`, and use bounded exponential
backoff with jitter. Do not blindly replay non-idempotent `SEND` frames after
a disconnect: an acknowledgement may have been lost even when the broker
already processed the message.

When TLS is terminated by a proxy or load balancer, configure it to:

- forward HTTP/1.1 `Upgrade` and `Connection` headers without buffering;
- support HTTP/2 extended CONNECT when HTTP/2 WebSockets are required;
- preserve or establish a trustworthy forwarded-client identity before the
  application authorizes a handshake;
- keep idle timeouts longer than the application's WebSocket or STOMP
  heartbeat interval; and
- use sticky routing if application state is held only in a process-local
  `SessionRegistry` or `SimpleBroker`.

The in-process broker does not span server processes. Use an external relay
only when a deployment needs cross-process broker state; that relay is not
part of NovaBoot yet.

The runnable reference is `examples/websocket_chat`: `/` is raw WebSocket
chat and `/stomp.html` is STOMP chat using `/app/chat.send`.
