#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "novaboot/context/request_context.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/router/path_params.h"
#include "novaboot/router/route.h"
#include "novaboot/router/json.h"

namespace novaboot::router {

class RouterGroup;

class ExceptionHandler {
public:
    virtual bool handle(const std::exception& ex, http3::Response& res, context::RequestContext& ctx) = 0;
    virtual ~ExceptionHandler() = default;
};

template<typename Ex, typename Handler>
class FluentExceptionHandler : public ExceptionHandler {
    Handler handler_;

    template<typename R>
    static void handle_result(const R& result, http3::Response& res) {
        if constexpr (requires { result.status_code(); result.headers(); }) {
            res.status(result.status_code());
            for (const auto& h : result.headers()) {
                res.header(h.first, h.second);
            }
            if constexpr (requires { result.body(); }) {
                res.json(novaboot::json::serialize(result.body()));
            }
        } else {
            res.status(200);
            res.json(novaboot::json::serialize(result));
        }
    }

public:
    explicit FluentExceptionHandler(Handler&& h) : handler_(std::forward<Handler>(h)) {}

    bool handle(const std::exception& ex, http3::Response& res, context::RequestContext& ctx) override {
        if (auto* typed_ex = dynamic_cast<const Ex*>(&ex)) {
            if constexpr (std::is_invocable_v<Handler, const Ex&, http3::Response&, context::RequestContext&>) {
                handler_(*typed_ex, res, ctx);
            } else if constexpr (std::is_invocable_v<Handler, const Ex&, context::RequestContext&>) {
                auto result = handler_(*typed_ex, ctx);
                handle_result(result, res);
            } else if constexpr (std::is_invocable_v<Handler, const Ex&>) {
                auto result = handler_(*typed_ex);
                handle_result(result, res);
            }
            return true;
        }
        return false;
    }
};

/// Radix-tree (compressed trie) URL router.
///
/// Features:
///   - O(path_length) lookup time
///   - Static paths: /api/users
///   - Path parameters: /api/users/:id
///   - Wildcard catch-all: /static/*filepath
///   - Per-method dispatch
///
/// Thread-safety: READ-ONLY after routes are registered.
/// Safe to share across shards after build().
class Router {
public:
    /// Fluent route builder for a specific path
    class RouteBuilder {
    public:
        RouteBuilder(Router& router, std::string path);

        RouteBuilder& get(Handler handler);
        RouteBuilder& post(Handler handler);
        RouteBuilder& put(Handler handler);
        RouteBuilder& del(Handler handler);
        RouteBuilder& patch(Handler handler);
        RouteBuilder& head(Handler handler);
        RouteBuilder& options(Handler handler);
        RouteBuilder& any(Handler handler);

    private:
        Router&     router_;
        std::string path_;
    };

    Router() = default;
    ~Router() = default;

    // Non-copyable, movable
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;
    Router(Router&&) = default;
    Router& operator=(Router&&) = default;

    /// Start building a route for a path (fluent API)
    RouteBuilder route(std::string_view path);

    /// Start a group of routes sharing a prefix
    RouterGroup group(std::string prefix);

    /// Register a route directly
    void add_route(Method method, std::string_view pattern,
                   Handler handler);

    /// Look up a route for a given method and path.
    /// Returns the handler and populates path_params if found.
    /// Returns nullptr if no matching route.
    template<typename Ex, typename Handler>
    Router& on_exception(Handler&& handler) {
        exception_handlers_.push_back(
            std::make_unique<FluentExceptionHandler<Ex, std::decay_t<Handler>>>(std::forward<Handler>(handler))
        );
        return *this;
    }

    void add_exception_handler(std::unique_ptr<ExceptionHandler> handler) {
        exception_handlers_.push_back(std::move(handler));
    }

    bool handle_exception(const std::exception& ex, http3::Response& res, context::RequestContext& ctx) const {
        for (const auto& handler : exception_handlers_) {
            if (handler->handle(ex, res, ctx)) {
                return true;
            }
        }
        return false;
    }

    struct MatchResult {
        Handler*    handler = nullptr;
        PathParams  params;
    };

    MatchResult match(Method method, std::string_view path) const;

    /// Convenience: match from a string method name
    MatchResult match(std::string_view method, std::string_view path) const;

    /// Number of registered routes
    [[nodiscard]] std::size_t size() const noexcept { return route_count_; }

private:
    /// Radix tree node
    struct Node {
        std::string segment;  // The path segment this node represents

        /// Handlers indexed by Method enum
        Handler handlers[static_cast<int>(Method::ANY) + 1] = {};

        /// Child nodes
        std::vector<std::unique_ptr<Node>> children;

        /// Parameter child (":param" node) — at most one
        std::unique_ptr<Node> param_child;
        std::string           param_name; // e.g., "id" from ":id"

        /// Wildcard child ("*name" node) — at most one, must be last
        std::unique_ptr<Node> wildcard_child;
        std::string           wildcard_name; // e.g., "filepath" from "*filepath"

        /// Does this node have any handler?
        [[nodiscard]] bool has_handler(Method m) const noexcept {
            return handlers[static_cast<int>(m)] != nullptr ||
                   handlers[static_cast<int>(Method::ANY)] != nullptr;
        }

        /// Get the handler for a method (falls back to ANY)
        [[nodiscard]] Handler* get_handler(Method m) {
            if (handlers[static_cast<int>(m)]) {
                return &handlers[static_cast<int>(m)];
            }
            if (handlers[static_cast<int>(Method::ANY)]) {
                return &handlers[static_cast<int>(Method::ANY)];
            }
            return nullptr;
        }
    };

    /// Insert a route pattern into the radix tree
    void insert(Node* node, std::string_view remaining,
                Method method, Handler handler);

    /// Search the radix tree for a matching path
    Handler* search(const Node* node, std::string_view remaining,
                    PathParams& params) const;

    /// Get or create the root node for lookup
    [[nodiscard]] const Node* root() const noexcept {
        return root_.get();
    }

    std::unique_ptr<Node> root_ = std::make_unique<Node>();
    std::size_t           route_count_ = 0;

    /// Flat storage for method resolution during lookup
    Method current_method_ = Method::GET;
    mutable Method lookup_method_ = Method::GET;

    std::vector<std::unique_ptr<ExceptionHandler>> exception_handlers_;
};

class RouterGroup {
private:
    Router& router_;
    std::string prefix_;
public:
    RouterGroup(Router& router, std::string prefix)
        : router_(router), prefix_(std::move(prefix)) {}

    RouterGroup group(std::string subprefix) {
        return RouterGroup(router_, join_path(prefix_, subprefix));
    }

    RouterGroup& get(std::string_view path, Handler handler) {
        router_.add_route(Method::GET, join_path(prefix_, path), std::move(handler));
        return *this;
    }

    RouterGroup& post(std::string_view path, Handler handler) {
        router_.add_route(Method::POST, join_path(prefix_, path), std::move(handler));
        return *this;
    }

    RouterGroup& put(std::string_view path, Handler handler) {
        router_.add_route(Method::PUT, join_path(prefix_, path), std::move(handler));
        return *this;
    }

    RouterGroup& del(std::string_view path, Handler handler) {
        router_.add_route(Method::DELETE_, join_path(prefix_, path), std::move(handler));
        return *this;
    }

    RouterGroup& patch(std::string_view path, Handler handler) {
        router_.add_route(Method::PATCH, join_path(prefix_, path), std::move(handler));
        return *this;
    }

    RouterGroup& head(std::string_view path, Handler handler) {
        router_.add_route(Method::HEAD, join_path(prefix_, path), std::move(handler));
        return *this;
    }

    RouterGroup& options(std::string_view path, Handler handler) {
        router_.add_route(Method::OPTIONS, join_path(prefix_, path), std::move(handler));
        return *this;
    }

    RouterGroup& any(std::string_view path, Handler handler) {
        router_.add_route(Method::ANY, join_path(prefix_, path), std::move(handler));
        return *this;
    }

private:
    static std::string join_path(const std::string& base, std::string_view rel) {
        if (rel.empty()) return base;
        if (base.empty()) return std::string(rel);
        std::string res = base;
        if (res.back() == '/' && rel.front() == '/') {
            res.pop_back();
        } else if (res.back() != '/' && rel.front() != '/') {
            res.push_back('/');
        }
        res.append(rel);
        return res;
    }
};

inline RouterGroup Router::group(std::string prefix) {
    return RouterGroup(*this, std::move(prefix));
}

} // namespace novaboot::router
