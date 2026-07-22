#pragma once

#include <filesystem>

#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"

namespace novaboot::http {

/// Serves one regular file below `root` with conservative cache, conditional,
/// and single-byte-range semantics. Returns false when the request is not a
/// safe resolvable file below the configured root.
[[nodiscard]] bool serve_static_resource(const std::filesystem::path& root,
                                         const http3::Request& request,
                                         Response& response,
                                         bool head_only = false);

} // namespace novaboot::http
