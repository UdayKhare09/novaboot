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
idempotency key. Streaming request bodies and per-request cancellation handles
are not implemented yet, so retries do not claim those semantics.
