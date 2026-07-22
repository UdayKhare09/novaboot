#pragma once

/// @file novaboot.h
/// @brief NovaBoot — Production-grade HTTP/3-only C++ framework
///
/// Umbrella header that includes all public API headers.

// Core
#include "novaboot/core/server.h"
#include "novaboot/core/event_loop.h"
#include "novaboot/core/shutdown.h"
#include "novaboot/core/http_drain.h"
#include "novaboot/core/error_response.h"

// HTTP/3
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/http/static_resource.h"
#include "novaboot/http/multipart.h"
#include "novaboot/http/content_negotiation.h"
#include "novaboot/http/sse.h"
#include "novaboot/testing/websocket_test_client.h"
#include "novaboot/testing/stomp_test_client.h"
#include "novaboot/testing/live_server.h"
#include "novaboot/http3/header_map.h"
#include "novaboot/http3/cookie.h"

// Router
#include "novaboot/router/router.h"
#include "novaboot/router/route.h"
#include "novaboot/router/path_params.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/router/json.h"

// API documentation
#include "novaboot/openapi/openapi.h"

// Real-time (raw WebSocket over HTTP/1.1 initially)
#include "novaboot/websocket/websocket.h"
#include "novaboot/messaging/stomp.h"

// Operations / observability
#include "novaboot/actuator/actuator.h"
#include "novaboot/observability/observation.h"
#include "novaboot/observability/trace_context.h"
#include "novaboot/observability/http_observation_middleware.h"

// Validation
#include "novaboot/validation/validation.h"

// Annotations
#include "novaboot/annotations/annotations.h"
#include "novaboot/annotations/scanner.h"


// Middleware
#include "novaboot/middleware/middleware.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/middleware/cors_middleware.h"
#include "novaboot/middleware/request_logging_middleware.h"
#include "novaboot/middleware/request_id_middleware.h"
#include "novaboot/middleware/csrf_middleware.h"
#include "novaboot/middleware/session_middleware.h"
#include "novaboot/middleware/rate_limit_middleware.h"
#include "novaboot/middleware/concurrency_limit_middleware.h"
#include "novaboot/middleware/trusted_forwarded_headers_middleware.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/middleware/security_headers_middleware.h"
#include "novaboot/middleware/body_size_limit_middleware.h"
#include "novaboot/middleware/compression_middleware.h"
#include "novaboot/middleware/content_negotiation_middleware.h"
#include "novaboot/middleware/authorization_middleware.h"
#include "novaboot/middleware/declarative_authorization.h"

// Context
#include "novaboot/context/request_context.h"

// Client (HTTP/3 REST client)
#include "novaboot/http3/client_response.h"
#include "novaboot/async/cancellation.h"
#include "novaboot/async/task.h"
#include "novaboot/client/rest_client.h"

// Configuration & Data sources
#include "novaboot/config/app_config.h"
#include "novaboot/db/observability.h"
#include "novaboot/db/health.h"


namespace novaboot::http {
using ClientResponse = http3::ClientResponse;
}
