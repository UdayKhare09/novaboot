#pragma once

/// @file novaboot.h
/// @brief NovaBoot — Production-grade HTTP/3-only C++ framework
///
/// Umbrella header that includes all public API headers.

// Core
#include "novaboot/core/server.h"
#include "novaboot/core/event_loop.h"

// HTTP/3
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/http3/header_map.h"

// Router
#include "novaboot/router/router.h"
#include "novaboot/router/route.h"
#include "novaboot/router/path_params.h"
#include "novaboot/router/response_entity.h"
#include "novaboot/router/json.h"

// Validation
#include "novaboot/validation/validation.h"


// Middleware
#include "novaboot/middleware/middleware.h"
#include "novaboot/middleware/pipeline.h"
#include "novaboot/middleware/cors_middleware.h"
#include "novaboot/middleware/request_logging_middleware.h"
#include "novaboot/middleware/jwt_middleware.h"
#include "novaboot/middleware/security_headers_middleware.h"
#include "novaboot/middleware/body_size_limit_middleware.h"
#include "novaboot/middleware/compression_middleware.h"
#include "novaboot/middleware/authorization_middleware.h"

// Context
#include "novaboot/context/request_context.h"

// Client (HTTP/3 REST client)
#include "novaboot/http3/client_response.h"
#include "novaboot/async/task.h"
#include "novaboot/client/rest_client.h"

// Configuration & Data sources
#include "novaboot/config/app_config.h"
#include "novaboot/data/pgsql/pgsql_data_source.h"
#include "novaboot/data/redis/redis_data_source.h"
#include "novaboot/data/crud_repository.h"
#include "novaboot/data/cache_repository.h"
#include "novaboot/data/caching_crud_repository.h"

namespace novaboot::http {
using ClientResponse = http3::ClientResponse;
}
