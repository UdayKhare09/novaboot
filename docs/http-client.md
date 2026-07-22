# HTTP client

`novaboot::client::RestClient` is a persistent TLS client for HTTP/1.1,
HTTP/2, and HTTP/3. It reuses HTTP/1.1 connections where possible and uses the
same request API for every transport.

## Trace propagation

By default, each logical request that does not already contain `traceparent`
gets a fresh W3C Trace Context. The same context is retained when that request
is retried. Supplying `traceparent` yourself takes precedence, which lets a
service propagate the context of its current inbound request.

```cpp
auto response = client->get("/inventory");
```

Set `RestClient::Config::propagate_trace_context = false` (or the matching
builder method) only when propagation is intentionally handled elsewhere.

## Retries

NovaBoot never replays a request by default. Configure both a bounded attempt
count and a predicate that explicitly permits the replay:

```cpp
auto client = RestClient::builder()
    .host("inventory.internal")
    .protocol(novaboot::client::Protocol::HTTP2)
    .retry_policy(2, [](std::string_view method,
                         const novaboot::http3::ClientResponse& response,
                         int attempt) {
        return method == "GET" && attempt == 1 && response.status_code == 503;
    })
    .build(event_loop);
```

The predicate is called only after a completed response. It must allow only
safe requests—normally idempotent methods, or writes guarded by an application
idempotency key. Pull-based streaming bodies are deliberately not replayed:
the producer may already have consumed a non-repeatable upload.

## Streaming uploads

`async_post_stream` and `async_put_stream` pull non-empty chunks on the
client EventLoop thread; return `std::nullopt` to finish the body. HTTP/1.1
uses chunked transfer encoding, while HTTP/2 and HTTP/3 feed their native
stream data providers without first joining chunks into one request string.

```cpp
std::vector<std::string> chunks{"first ", "second"};
auto next = [chunks = std::move(chunks), index = std::size_t{0}]() mutable
    -> std::optional<std::string> {
    return index < chunks.size() ? std::optional{chunks[index++]}
                                 : std::nullopt;
};
auto response = co_await client->async_post_stream("/import", std::move(next));
```

## Cancellation

Pass an `async::CancellationToken` to a synchronous request when another
thread or shutdown path may stop waiting for it:

```cpp
novaboot::async::CancellationSource cancellation;
auto response = client->get("/inventory", {}, cancellation.token());
```

Cancellation causes `RequestCancelled` and safely completes pending callbacks
before the request task is destroyed. HTTP/2 sends `RST_STREAM(CANCEL)` and
HTTP/3 resets only the affected QUIC stream; other multiplexed requests remain
connected. HTTP/1.1 has no stream reset, so it closes and reconnects its single
connection. The same token can be passed to any async request overload.
