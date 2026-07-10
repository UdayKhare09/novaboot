#pragma once

#include <string>

#include "novaboot/http3/header_map.h"

namespace novaboot::http3 {

/// Response received by an HTTP/3 client after a completed request.
struct ClientResponse {
    int       status_code = 0;
    HeaderMap headers;
    std::string body;

    /// True if the response was successfully received (status != 0)
    [[nodiscard]] bool ok() const noexcept { return status_code >= 200 && status_code < 300; }
};

} // namespace novaboot::http3
