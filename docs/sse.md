# Server-Sent Events

`novaboot::http::sse` provides the wire-format and bounded producer channel
used by one SSE connection:

```cpp
using novaboot::http::sse::Channel;
using novaboot::http::sse::Event;

Channel channel({.max_pending_events = 128, .max_pending_bytes = 256 * 1024});
channel.publish(Event{
    .data = R"({"status":"ready"})",
    .event = "status",
    .id = "42",
});
```

Events are encoded using `data:`, `event:`, `id:`, `retry:`, and comment
fields. Multiline data becomes multiple `data:` fields. Event names and IDs
reject CR/LF to prevent framing injection.

The channel is thread-safe and non-blocking for publishers. It enforces both
event-count and byte-count limits; `Backpressured` means the application must
drop, retry later, or close the connection. `close()` prevents new events while
allowing already queued events to drain, and wakes the transport owner.

Use `open(response, channel)` in a route to set the headers and attach the
channel. HTTP/1.1 emits a chunked persistent event stream; HTTP/2 defers and
resumes DATA frames; HTTP/3 defers and resumes its QUIC stream. Each transport
drains queued events on its owner loop after a publish/close wakeup.

If the peer disconnects first, NovaBoot detaches the owner-loop wakeup and
closes the channel. A publisher holding its `shared_ptr` then receives `Closed`
instead of accessing a stale transport.

Closing a channel ends the response cleanly for each transport. A peer
disconnect or server-session teardown detaches the wakeup and closes the
channel, so retained publishers observe `Closed`. Live TLS integration coverage
exercises finite event streams through NovaBoot's HTTP/2 and HTTP/3 client
paths, including final stream completion.
