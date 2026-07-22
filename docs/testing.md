# Testing NovaBoot applications

`novaboot::testing::AppTestClient` runs application routes in-process. It
creates request-scoped DI state, executes registered routes and configured
middleware, and routes thrown exceptions through controller advice.

```cpp
novaboot::di::RootContainer root;
novaboot::annotations::register_beans<MyController>(root);
root.build();

novaboot::testing::AppTestClient client(root, {
    std::make_shared<novaboot::middleware::RequestIdMiddleware>(),
});

const auto response = client.post(
    "/api/items", R"({"name":"keyboard"})",
    {{"authorization", "Bearer test-token"}});

EXPECT_EQ(response.status, 201);
EXPECT_TRUE(response.body_contains(R"("name":"keyboard")"));
EXPECT_EQ(response.header("x-request-id"), "...");

root.shutdown();
```

Supported convenience methods are `get`, `post`, `put`, `patch`, and `del`.
They accept optional request headers, preserve query strings for route handling,
and return status, body, and response headers. This harness deliberately does
not bind a socket or start an io_uring shard; use focused protocol tests for
live HTTP/1.1, HTTP/2, HTTP/3, WebSocket, and TLS behavior alongside the
`LiveServer` lifecycle harness below.

## Live server lifecycle tests

`novaboot::testing::LiveServer` owns a real `Server` for an integration test.
It starts the server thread, waits until every shard has bound its TCP and UDP
listeners, and always stops and joins that thread during teardown. This avoids
fixed startup sleeps and leaves no listener behind after a failing assertion.

```cpp
auto server = Server::create()
    .bind("127.0.0.1", 4441)
    .tls("cert.pem", "key.pem")
    .workers(1)
    .build();
server->route("/health").get([](auto&, auto& response, auto&) {
    response.text("ok");
});

novaboot::testing::LiveServer live(std::move(server));
ASSERT_TRUE(live.server().is_ready());
// Exercise it with RestClient, curl, or a browser protocol test.
```

## WebSocket and STOMP endpoint tests

`WebSocketTestClient` sends correctly masked RFC 6455 client frames to a real
endpoint handler and decodes its replies. It can require text replies and close
codes without writing frame fixtures by hand. `StompTestClient` builds on it to
exercise a real local STOMP endpoint, including `CONNECTED` and `RECEIPT`.

```cpp
novaboot::testing::StompTestClient client(endpoint, "alice");
client.connect();
client.send({.command = "SUBSCRIBE", .headers = {
    {"id", "alerts"}, {"destination", "/topic/alerts"}, {"receipt", "ok"},
}});
client.require_receipt("ok");
```

These are in-process protocol clients. Keep live TLS, browser, HTTP/1.1, and
HTTP/2 interoperability tests separate from them.

## PostgreSQL integration isolation

PostgreSQL integration tests use `PostgresTestDatabase`. It creates a unique
schema for each test, configures every connection in its pool with that schema
as `search_path`, and drops the schema with `CASCADE` during teardown. Tests
therefore do not share tables or require manual cleanup when a disposable
PostgreSQL instance is available.

Set `NOVABOOT_POSTGRES_TEST_URL` to a libpq connection string or URI for CI.
When it is unset, the local Docker-development default is
`host=localhost dbname=postgres user=postgres password=postgres`.
