#include "novaboot/middleware/content_negotiation_middleware.h"

#include "novaboot/http/content_negotiation.h"

namespace novaboot::middleware {

void ContentNegotiationMiddleware::handle(http3::Request& request,
                                           http3::Response& response,
                                           context::RequestContext&,
                                           Next next) {
    next();

    if (response.status_code() == 204 || response.status_code() == 304 ||
        response.body_size() == 0) {
        return;
    }
    const auto content_type = response.headers().get("content-type");
    if (!content_type || http::accepts_content_type(request, *content_type)) {
        return;
    }

    response.status(406)
        .text("Not Acceptable");
    response.headers().set("vary", "Accept");
}

} // namespace novaboot::middleware
