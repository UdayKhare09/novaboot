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

namespace novaboot::router {

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

    /// Register a route directly
    void add_route(Method method, std::string_view pattern,
                   Handler handler);

    /// Look up a route for a given method and path.
    /// Returns the handler and populates path_params if found.
    /// Returns nullptr if no matching route.
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
};

} // namespace novaboot::router
