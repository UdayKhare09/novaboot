#pragma once

#include <functional>
#include <string_view>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"

namespace novaboot::router {

/// Route handler function signature
using Handler =
    std::move_only_function<void(http3::Request&, http3::Response&,
                                 context::RequestContext&)>;

/// HTTP method enum
enum class Method {
    GET,
    POST,
    PUT,
    DELETE_,
    PATCH,
    HEAD,
    OPTIONS,
    ANY,  // Matches all methods
};

/// Convert method string to enum
Method method_from_string(std::string_view method);

/// Convert method enum to string
std::string_view method_to_string(Method method);

/// A single route definition (method + path + handler)
struct Route {
    Method  method;
    std::string pattern;
    Handler handler;
};

} // namespace novaboot::router
