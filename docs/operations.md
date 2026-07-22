# Operations and OpenTelemetry compatibility

NovaBoot's optional `actuator` module exposes a small operations surface while
keeping the base framework free of an observability SDK dependency.

```cpp
#include "novaboot/novaboot.h"

auto actuator = std::make_shared<novaboot::actuator::Actuator>(
    novaboot::actuator::Config{
        .base_path = "/actuator",
        .expose_metrics = true,
        .expose_prometheus = true,
        .expose_config = true,
        .expose_loggers = true,
        .expose_observations = true,
        .require_authorization = true,
        .authorizer = [](const novaboot::http3::Request& request) {
            return request.header("x-management-token") == "local-dev-token";
        },
    });

actuator->add_info("application", "catalog");
actuator->add_info("version", "1.0.0");
actuator->add_config_source("application.toml", {
    {"server.port", "4435"},
    {"database.password", "not returned by the endpoint"},
});
actuator->add_health_contributor("database", [] {
    return novaboot::actuator::Health{
        novaboot::actuator::HealthStatus::Up,
        {{"database", "postgres"}},
    };
});

auto app = novaboot::Server::create()
    .bind("0.0.0.0", 443)
    .tls("cert.pem", "key.pem")
    .actuator(actuator)
    .build();
```

When attached through the server builder, the module registers these routes:

- `GET /actuator/health`
- `GET /actuator/health/liveness`
- `GET /actuator/health/readiness`
- `GET /actuator/info`
- `GET /actuator/metrics` when explicitly enabled
- `GET /actuator/prometheus` when explicitly enabled
- `GET /actuator/configprops` when explicitly enabled
- `GET /actuator/loggers` and `GET`/`POST /actuator/loggers/ROOT` when
  explicitly enabled
- `GET /actuator/observations` when explicitly enabled

The server switches readiness from `OUT_OF_SERVICE` to `UP` after its shards
start, and to `OUT_OF_SERVICE` when graceful shutdown begins. Health
contributors are synchronous and should be fast; use an already-cached health
state rather than doing a long network operation in a request handler.

`require_authorization` defaults to `false` for local development. Production
applications should require an explicit authorizer and only expose management
routes on a trusted network or management listener.

`/actuator/configprops` reports the effective configuration sources explicitly
supplied with `add_config_source`. Its response masks values whose names include
`password`, `secret`, `token`, or `credential`, as well as values ending in
`.key` or `_key`. It is not an automatic dump of process environment variables
or arbitrary TOML files; this prevents accidental secret disclosure while the
configuration precedence system is still being completed.

The root log level can be changed with an authorized request:

```http
POST /actuator/loggers/ROOT
Content-Type: application/json

{"configuredLevel":"debug"}
```

Allowed values are `trace`, `debug`, `info`, `warn`, `error`, `critical`, and
`off`. Logger management is disabled by default and uses the same explicit
management authorization policy as the other sensitive endpoints.

## Database metrics and health

Database instrumentation is explicit so applications can decide which data
sources are observed:

```cpp
auto observed_source = std::make_shared<novaboot::db::ObservingDataSource>(
    postgres_source, actuator->meters(), "postgresql");

actuator->add_health_contributor(
    "database", novaboot::db::health_contributor(*observed_source));
```

`ObservingDataSource` exports connection-acquisition timing and
`db.client.operation.duration`. Labels include only `db.system`, the normalized
operation verb, and outcome; SQL text and bound values are never metrics.
`health_contributor` performs `SELECT 1` synchronously, so configure bounded
database acquisition/query timeouts before exposing it on a high-traffic health
endpoint.

To measure DI startup, attach the same registry before building the container:

```cpp
di_root.observe(actuator->meters());
di_root.build();
```

This records container build duration plus registration and singleton gauges.

The registry can also retain completed application spans and structured events:

```cpp
actuator->meters()->record_span({.name = "order.reserve"});
actuator->meters()->record_event({.name = "order.reserved"});
```

These records form NovaBoot's exporter-neutral tracing contract. They are kept
in-process today (with the most recent 1,024 spans and events retained); the
optional OTLP adapter will map them to OpenTelemetry spans and logs/events.

The HTTP observation middleware automatically records completed
`http.server.request` spans with the propagated trace ID. `/actuator/observations`
is a small guarded development inspection endpoint for retained span names,
durations, error state, and event count; do not expose it publicly because the
full OTLP exporter is the production telemetry path.

## Signal contract

The in-process `MeterRegistry` is vendor-neutral. The HTTP middleware records
OpenTelemetry semantic-convention names:

- `http.server.active_requests`
- `http.server.request.duration` (`s`)
- `http.server.request.body.size` (`By`)
- `http.server.response.body.size` (`By`)

HTTP labels are limited to method, response status, and scheme. NovaBoot does
not use raw URL paths as metric labels, preventing unbounded metric-cardinality
from resource IDs and query strings.

`/actuator/prometheus` is a Prometheus text-format scrape endpoint. It exports
counters, up/down counters as gauges, gauges, and histograms. Histogram output
includes bounded buckets plus `_sum` and `_count`; duration uses a `_seconds`
suffix and sizes use `_bytes`. Configure Prometheus to scrape the endpoint,
then select its datasource in Grafana. Keep the route protected or bind it only
on a trusted management network in production.

The middleware extracts valid W3C `traceparent` and `tracestate` request
headers, starts a server span context, emits the propagated headers on the
response, and stores `trace_id`, `span_id`, and `parent_span_id` in the request
context. Trace identifiers are generated using OpenSSL when an inbound context
is absent.

This is compatibility, not an embedded OpenTelemetry SDK: no OTLP exporter is
linked by default. The next optional integration can adapt `MeterRegistry` and
the trace context to `opentelemetry-cpp` once applications intentionally supply
that dependency and exporter configuration.

## Request correlation

Add `RequestIdMiddleware` before request logging to return a safe
`x-request-id` response header and make the ID available as
`RequestContext::get_string("request_id")`:

```cpp
.middleware(std::make_shared<novaboot::middleware::RequestIdMiddleware>())
.middleware(std::make_shared<novaboot::middleware::RequestLoggingMiddleware>())
```

By default, a printable inbound `x-request-id` is retained for edge-to-service
correlation; malformed or overlong values are replaced. Generated IDs use
OpenSSL randomness. When present, request logging includes both `request_id`
and the OpenTelemetry-compatible `trace_id`.
