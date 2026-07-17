#pragma once

#include "novaboot/context/request_context.h"
#include "novaboot/di/container.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/router/router.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

namespace novaboot::testing {

struct TestResponse {
    int status = 200;
    std::string body;
    http3::HeaderMap headers;
};

class AppTestClient {
public:
    explicit AppTestClient(di::RootContainer& container)
        : container_(container), shard_(container_.make_shard_container()) {
        shard_->initialize();
        container.register_routes_and_advice(router_);
    }

    TestResponse get(std::string_view path) {
        return request("GET", path, {});
    }

    TestResponse post(std::string_view path, std::string_view body) {
        return request("POST", path, body);
    }

    TestResponse put(std::string_view path, std::string_view body) {
        return request("PUT", path, body);
    }

    TestResponse del(std::string_view path) {
        return request("DELETE", path, {});
    }

    TestResponse request(std::string_view method, std::string_view path, std::string_view body) {
        http3::Request req;
        req.set_method(method);
        req.set_path(path);
        req.headers().set("content-type", "application/json");
        if (!body.empty()) {
            req.append_body(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
        }

        http3::Response res;
        context::RequestContext ctx;
        auto request_container = shard_->make_request_container();
        ctx.bind_container(*request_container);

        const auto match_path = strip_query(path);
        auto match = router_.match(method, match_path);
        if (!match.handler) {
            res.status(404).json(R"({"error":"Not Found"})");
            return to_test_response(res);
        }

        req.path_params() = match.params;

        try {
            (*match.handler)(req, res, ctx);
        } catch (const std::exception& ex) {
            if (!router_.handle_exception(ex, res, ctx)) {
                res.status(500).json(R"({"error":"Internal Server Error"})");
            }
        }

        return to_test_response(res);
    }

private:
    di::RootContainer& container_;
    std::unique_ptr<di::ShardContainer> shard_;
    router::Router router_;

    static std::string_view strip_query(std::string_view path) {
        const auto pos = path.find('?');
        return pos == std::string_view::npos ? path : path.substr(0, pos);
    }

    static TestResponse to_test_response(const http3::Response& response) {
        return TestResponse{
            .status = response.status_code(),
            .body = response.body_str(),
            .headers = response.headers(),
        };
    }
};

} // namespace novaboot::testing
