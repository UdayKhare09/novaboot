# Browser cookie security

NovaBoot's cookie helpers make `Set-Cookie` fields explicit and safe to emit.
They reject header delimiters/control characters and reject `SameSite=None`
unless the cookie is also `Secure`.

```cpp
http::set_cookie(response, {
    .name = "nova_session",
    .value = session_id,
    .path = "/",
    .secure = true,
    .http_only = true,
    .same_site = http::SameSite::Lax,
});
```

`http::request_cookie(request, "nova_session")` parses request `Cookie`
fields without treating a prefix match as a valid cookie name.

## Security response headers

`SecurityHeadersMiddleware` adds defensive response headers without replacing
headers deliberately set by a handler. Its defaults include HSTS,
`X-Content-Type-Options: nosniff`, frame/referrer/permissions policies, COOP,
CORP, and `X-XSS-Protection: 0`. HSTS is emitted only for requests whose
scheme is `https`; do not disable that check merely to accommodate an untrusted
forwarded header. Header configuration rejects control characters to prevent
accidental header injection.

Content Security Policy remains opt-in because a safe policy is application
specific. Configure `content_security_policy` before enabling browser pages
with inline resources or third-party assets.

## Secure error responses

Unhandled route exceptions are logged server-side but return only
`{"error":"Internal Server Error"}` to clients. This avoids exposing database
errors, paths, or secrets. For local diagnosis only, opt in explicitly:

```cpp
auto app = Server::create()
    .tls("cert.pem", "key.pem")
    .expose_error_details(true) // never enable in production
    .build();
```

Application `on_exception`/controller-advice handlers remain responsible for
their own intentional client-facing error bodies.

## Trusted proxy headers

NovaBoot ignores `Forwarded` headers by default. At a reverse-proxy deployment,
allow only proxy peer networks and place this middleware before
scheme-sensitive middleware such as `SecurityHeadersMiddleware`:

```cpp
server.middleware(std::make_shared<middleware::TrustedForwardedHeadersMiddleware>(
    middleware::TrustedForwardedHeadersMiddleware::Config{
        .trusted_peer_cidrs = {"10.42.0.0/16", "2001:db8:42::/48"},
    }));
```

It reads the first RFC 7239 `Forwarded` element's `proto`, `host`, and valid
`for` IP only when the direct transport peer matches the configured CIDR. The
accepted `for` value becomes `Request::client_address()`; `peer_address()`
always remains the direct socket peer. It deliberately does not trust legacy
`X-Forwarded-*` headers. `trust_all_direct_peers = true` remains available only
as an explicit compatibility escape hatch and is unsafe for a publicly exposed
application port.

## JWTs in HttpOnly cookies

`JwtMiddleware` accepts `Authorization: Bearer` by default. To authenticate a
browser with a JWT cookie instead, enable it deliberately:

```cpp
auto jwt = std::make_shared<middleware::JwtMiddleware>(
    middleware::JwtMiddleware::Config{
        .allowed_algorithms = {middleware::JwtAlgorithm::HS256},
        .hmac_secret = read_secret(),
        .jwt_cookie_name = "nova_access",
    });
```

Issue `nova_access` with `Secure`, `HttpOnly`, and an appropriate `SameSite`
policy using `http::set_cookie`. A valid Bearer token has precedence whenever
both forms are sent. The same setting is also used for WebSocket handshakes
unless `websocket_cookie_name` explicitly selects a separate socket cookie.
JWTs are never accepted from query strings.

Because browsers attach authentication cookies automatically, install
`CsrfMiddleware` for unsafe browser routes that use this option. A Bearer-only
API does not need CSRF middleware.

## CSRF for browser sessions

`CsrfMiddleware` uses the double-submit-cookie pattern. On a safe request it
issues a readable `XSRF-TOKEN` cookie if one is absent. JavaScript must copy
that value into `X-XSRF-TOKEN` for every unsafe request; a missing or unequal
value receives `403` before the route runs.

```cpp
app.middleware(std::make_shared<middleware::CsrfMiddleware>());
```

The CSRF token cookie intentionally does **not** use `HttpOnly`, because the
browser client needs to read it. Keep the actual session/authentication cookie
`HttpOnly`, `Secure`, and scoped to the smallest practical path/domain.

Use this middleware for cookie-authenticated browser routes. APIs authenticated
only by an `Authorization: Bearer` header normally should not use it. Configure
the middleware with `SameSite::None` only for an HTTPS cross-site flow; NovaBoot
rejects an insecure configuration.

## Server-side sessions

`SessionManager` creates opaque IDs backed by a `SessionStore`; the default
`InMemorySessionStore` is thread-safe and appropriate for a single process.
For several application instances, `db::postgres::PostgresSessionStore` keeps
the same opaque records in an application-managed PostgreSQL table. A login
rotates an existing session before setting a fresh `Secure`, `HttpOnly`,
`SameSite=Lax` cookie. Logout invalidates the server-side entry and emits
`Max-Age=0`.

```cpp
auto sessions = std::make_shared<middleware::SessionManager>();

// In the successful login handler:
sessions->login(request, response, {.subject = user_id});

// Global middleware, with public login/logout routes left open:
server.middleware(std::make_shared<middleware::SessionMiddleware>(
    sessions, middleware::SessionMiddleware::Config{
        .allowlist_paths = {"/login", "/logout", "/actuator/health"},
    }));
server.middleware(std::make_shared<middleware::CsrfMiddleware>());
```

Create the PostgreSQL table through an explicit migration; the store never
alters schema at runtime:

```cpp
migrations.push_back(db::Migration::sql(
    12, "browser sessions",
    db::postgres::PostgresSessionStore::schema_ddl()));

auto session_store = std::make_shared<db::postgres::PostgresSessionStore>(datasource);
auto sessions = std::make_shared<middleware::SessionManager>(
    middleware::SessionManager::Config{}, session_store);
```

Records contain the opaque ID, subject, roles, scopes, and expiry only. Use
`erase_expired(now)` from scheduled maintenance to prune old rows; reads never
authenticate expired records.

`SessionMiddleware` stores `SessionPrincipal` in `RequestContext`; its roles
and scopes are enforced by `AuthorizationMiddleware` in the same way as JWT
claims. Put CSRF after session authentication and exclude/token-bootstrap
routes as appropriate.

## Domain authorization policies

`AuthorizationMiddleware` route policies can combine roles/scopes with custom
predicates. Every matching requirement must allow the request.

```cpp
middleware::AuthorizationMiddleware::Policy tenant_policy{
    .path = "/tenants/*",
    .required_scopes = {"articles:write"},
    .custom_policies = {
        [](const http3::Request& request, const context::RequestContext& context) {
            const auto* principal = context.get<middleware::JwtPrincipal>();
            return principal && request.header("x-tenant").value_or("") == principal->subject;
        },
    },
};
```

Custom policies return `false` for the normal configured `403` response. They
can also be attached to a public policy (`require_authenticated = false`) for
explicit request validation; they receive the same request context.

## Declarative controller authorization

Reflected controllers can declare the same JWT/session role and scope contract
on a class or an individual mapped method. A method annotation overrides the
class annotation; `PermitAll` explicitly opens one endpoint. Role and scope
values are whitespace- or comma-separated and default to requiring every
listed value.

```cpp
struct [[= RestController("/articles") ]]
       [[= Authorize("editor admin", "articles:write") ]]
ArticleController {
    [[= GetMapping("/drafts") ]]
    std::string drafts(); // editor AND admin, plus articles:write

    [[= GetMapping("/login-options") ]]
    [[= PermitAll() ]]
    std::string login_options();
};
```

Use `Authorize("editor admin", AuthorizationMatch::Any)` when either role is
sufficient. The annotations require a preceding `JwtMiddleware` or
`SessionMiddleware`; an absent identity receives `401`, while an identity that
does not satisfy the declared role/scope receives `403`.

## Service method authorization

For application services, inject NovaBoot's explicit DI proxy instead of the
concrete service when a method carries `Authorize`:

```cpp
struct [[= Service() ]] OrderService {
    [[= Authorize("admin", "orders:write") ]]
    void place_order(Order order);
};

// Registered automatically for a secured @Service.
middleware::AuthorizationProxy<OrderService>& orders = /* DI */;
orders.invoke<&OrderService::place_order>(order);
```

The proxy reads the request context installed by NovaBoot's middleware
pipeline. A secured call from a background job or any other context without an
identity throws `middleware::AccessDeniedException`; background work must pass
an explicit identity policy instead of inheriting one implicitly. This is the
safe C++ counterpart to Spring Security method proxies, not transparent
interception of arbitrary `service.method()` calls. When the secured service
also uses `Transactional`, the registered authorization proxy authorizes first
and then delegates to NovaBoot's transaction interceptor.

## Rate limiting

`RateLimitMiddleware` implements a bounded, per-process token bucket. It
returns `429` with `RateLimit-Limit`, `RateLimit-Remaining`, `RateLimit-Reset`,
and `Retry-After` when the bucket is empty.

```cpp
server.middleware(std::make_shared<middleware::RateLimitMiddleware>(
    middleware::RateLimitMiddleware::Config{
        .policy = {
            .name = "api-per-user",
            .algorithm = middleware::RateLimitAlgorithm::Gcra,
            .limit = 120,
            .window = std::chrono::minutes{1},
            .burst = 20,
        },
        .key_resolver = middleware::RateLimitMiddleware::authenticated_principal_key,
        .allowlist_paths = {"/actuator/health"},
    }));
```

Install it after JWT/session authentication when using
`authenticated_principal_key`; anonymous requests then share one bucket. The
safe default is a single global bucket. To limit by direct client or a
CIDR-validated `Forwarded: for=`, configure
`RateLimitMiddleware::client_address_key` (and install trusted forwarding
middleware first). This limiter is
local to one process, so global multi-instance limits belong in a shared edge
or distributed limiter. The middleware depends on a small `RateLimitStore`
interface; its default is `InMemoryRateLimitStore`. The middleware configures
the `RateLimitPolicy` algorithm (`TokenBucket`, `FixedWindow`,
`SlidingWindow`, or `Gcra`), while a store only executes/persists that policy
atomically. A future Redis/Valkey store can implement the same
`acquire(key, policy, now)` contract without changing application code or the
middleware pipeline. Give each policy a distinct `name` when middleware
instances share a store.

## In-flight concurrency limits

`ConcurrencyLimitMiddleware` bounds active downstream request work instead of
request frequency. It releases its permit even if a later middleware or route
handler throws, and rejects excess work with `429`.

```cpp
server.middleware(std::make_shared<middleware::ConcurrencyLimitMiddleware>(
    middleware::ConcurrencyLimitMiddleware::Config{
        .max_concurrent = 64,
        .allowlist_paths = {"/actuator/health"},
    }));
```

The default is one global pool. Supply a trusted application identity resolver
to use separate pools, and place it after authentication if the resolver uses
the request principal. This is deliberately a local in-flight limit; a
distributed concurrency quota requires a lease-aware shared implementation.
