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

// Middleware
#include "novaboot/middleware/middleware.h"
#include "novaboot/middleware/pipeline.h"

// Context
#include "novaboot/context/request_context.h"
