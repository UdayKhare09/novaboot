#pragma once

#include "novaboot/context/request_context.h"
#include "novaboot/di/container.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/router/router.h"
#include "novaboot/router/json.h"
#include "novaboot/middleware/pipeline.h"

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace novaboot::testing {

struct TestResponse {
    int status = 200;
    std::string body;
    http3::HeaderMap headers;

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const {
        return headers.get(name);
    }
    [[nodiscard]] bool body_contains(std::string_view value) const {
        return body.find(value) != std::string::npos;
    }
};

using TestHeaders = std::vector<std::pair<std::string, std::string>>;

class AppTestClient {
public:
    explicit AppTestClient(di::RootContainer& container,
                           std::vector<std::shared_ptr<middleware::Middleware>> middlewares = {})
        : container_(container), shard_(container_.make_shard_container()) {
        shard_->initialize();
        container.register_routes_and_advice(router_);
        for (auto& middleware : middlewares) pipeline_.add(std::move(middleware));
    }

    TestResponse get(std::string_view path, TestHeaders headers = {}) {
        return request("GET", path, {}, std::move(headers));
    }

    TestResponse post(std::string_view path, std::string_view body, TestHeaders headers = {}) {
        return request("POST", path, body, std::move(headers));
    }

    TestResponse put(std::string_view path, std::string_view body, TestHeaders headers = {}) {
        return request("PUT", path, body, std::move(headers));
    }

    TestResponse patch(std::string_view path, std::string_view body, TestHeaders headers = {}) {
        return request("PATCH", path, body, std::move(headers));
    }
    TestResponse del(std::string_view path, TestHeaders headers = {}) {
        return request("DELETE", path, {}, std::move(headers));
    }

    TestResponse request(std::string_view method, std::string_view path,
                         std::string_view body, TestHeaders headers = {}) {
        http3::Request req;
        req.set_method(method);
        req.set_path(path);
        req.headers().set("content-type", "application/json");
        for (const auto& [name, value] : headers) req.headers().set(name, value);
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
            pipeline_.execute(req, res, ctx, *match.handler);
        } catch (const json::BindingException& error) {
            res.status(400).json(bad_request_json("Invalid JSON request body", error.errors()));
        } catch (const validation::ValidationException& error) {
            res.status(400).json(bad_request_json("Validation failed", error.errors()));
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
    middleware::Pipeline pipeline_;

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

    static std::string bad_request_json(std::string_view message,
                                        const std::vector<std::string>& errors) {
        std::string body = R"({"error":"Bad Request","message":")" +
                           std::string(message) + R"(","errors":[)";
        bool first = true;
        for (const auto& error : errors) {
            if (!first) body += ',';
            first = false;
            body += json::serialize(error);
        }
        return body + "]}";
    }
};

} // namespace novaboot::testing
