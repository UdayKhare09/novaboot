# Graceful shutdown

NovaBoot uses a bounded shutdown coordinator. Register external subsystems
while building the server:

```cpp
.shutdown_timeout(std::chrono::seconds(5))
.shutdown_participant({
    .name = "background-worker",
    .stop_accepting = [&] { worker.stop_accepting(); },
    .drained = [&] { return worker.idle(); },
    .force_close = [&] { worker.cancel(); },
})
```

On `Server::stop()`, NovaBoot first changes Actuator readiness to
`OUT_OF_SERVICE`, calls every `stop_accepting` hook, waits until all `drained`
hooks return true or the deadline expires, then invokes `force_close` only for
remaining participants before stopping shards.

The server installs an HTTP admission gate automatically. Once draining starts,
new HTTP/1.1, HTTP/2, and HTTP/3 route requests receive `503`; requests already
inside the middleware pipeline are allowed to return before the deadline.
Server-owned WebSockets and registered STOMP endpoints receive close code
`1001` and participate in the same wait.

Use the participant contract for database pools and background work so their
own bounded shutdown behavior is included in the same deadline.
