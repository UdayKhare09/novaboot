#include "novaboot/router/router.h"

#include <algorithm>
#include <stdexcept>

#include <spdlog/spdlog.h>

namespace novaboot::router {

// ─── Method conversion ──────────────────────────────────────────────────────

Method method_from_string(std::string_view method) {
    if (method == "GET")     return Method::GET;
    if (method == "POST")    return Method::POST;
    if (method == "PUT")     return Method::PUT;
    if (method == "DELETE")  return Method::DELETE_;
    if (method == "PATCH")   return Method::PATCH;
    if (method == "HEAD")    return Method::HEAD;
    if (method == "OPTIONS") return Method::OPTIONS;
    return Method::ANY;
}

std::string_view method_to_string(Method method) {
    switch (method) {
        case Method::GET:     return "GET";
        case Method::POST:    return "POST";
        case Method::PUT:     return "PUT";
        case Method::DELETE_: return "DELETE";
        case Method::PATCH:   return "PATCH";
        case Method::HEAD:    return "HEAD";
        case Method::OPTIONS: return "OPTIONS";
        case Method::ANY:     return "*";
    }
    return "UNKNOWN";
}

// ─── RouteBuilder ────────────────────────────────────────────────────────────

Router::RouteBuilder::RouteBuilder(Router& router, std::string path)
    : router_(router), path_(std::move(path)) {
}

Router::RouteBuilder& Router::RouteBuilder::get(Handler handler) {
    router_.add_route(Method::GET, path_, std::move(handler));
    return *this;
}

Router::RouteBuilder& Router::RouteBuilder::post(Handler handler) {
    router_.add_route(Method::POST, path_, std::move(handler));
    return *this;
}

Router::RouteBuilder& Router::RouteBuilder::put(Handler handler) {
    router_.add_route(Method::PUT, path_, std::move(handler));
    return *this;
}

Router::RouteBuilder& Router::RouteBuilder::del(Handler handler) {
    router_.add_route(Method::DELETE_, path_, std::move(handler));
    return *this;
}

Router::RouteBuilder& Router::RouteBuilder::patch(Handler handler) {
    router_.add_route(Method::PATCH, path_, std::move(handler));
    return *this;
}

Router::RouteBuilder& Router::RouteBuilder::head(Handler handler) {
    router_.add_route(Method::HEAD, path_, std::move(handler));
    return *this;
}

Router::RouteBuilder& Router::RouteBuilder::options(Handler handler) {
    router_.add_route(Method::OPTIONS, path_, std::move(handler));
    return *this;
}

Router::RouteBuilder& Router::RouteBuilder::any(Handler handler) {
    router_.add_route(Method::ANY, path_, std::move(handler));
    return *this;
}

// ─── Router ──────────────────────────────────────────────────────────────────

Router::RouteBuilder Router::route(std::string_view path) {
    return RouteBuilder(*this, std::string(path));
}

void Router::add_route(Method method, std::string_view pattern,
                       Handler handler) {
    if (pattern.empty() || pattern[0] != '/') {
        throw std::invalid_argument("Route pattern must start with '/'");
    }

    // Strip the leading '/' for insertion
    auto remaining = pattern.substr(1);
    insert(root_.get(), remaining, method, std::move(handler));
    ++route_count_;

    spdlog::debug("Route registered: {} {}", method_to_string(method),
                  pattern);
}

void Router::insert(Node* node, std::string_view remaining,
                    Method method, Handler handler) {
    // Base case: no more path to consume
    if (remaining.empty()) {
        node->handlers[static_cast<int>(method)] = std::move(handler);
        return;
    }

    // Extract the next segment (up to the next '/')
    auto slash_pos = remaining.find('/');
    std::string_view segment;
    std::string_view rest;

    if (slash_pos == std::string_view::npos) {
        segment = remaining;
        rest    = {};
    } else {
        segment = remaining.substr(0, slash_pos);
        rest    = remaining.substr(slash_pos + 1);
    }

    // ─── Wildcard segment (*name) ────────────────────────────────────
    if (!segment.empty() && segment[0] == '*') {
        if (!node->wildcard_child) {
            node->wildcard_child = std::make_unique<Node>();
            node->wildcard_name  = std::string(segment.substr(1));
        }
        node->wildcard_child->handlers[static_cast<int>(method)] =
            std::move(handler);
        return;
    }

    // ─── Parameter segment (:name) ───────────────────────────────────
    if (!segment.empty() && segment[0] == ':') {
        if (!node->param_child) {
            node->param_child = std::make_unique<Node>();
            node->param_name  = std::string(segment.substr(1));
        }
        insert(node->param_child.get(), rest, method, std::move(handler));
        return;
    }

    // ─── Static segment ─────────────────────────────────────────────
    // Look for an existing child with the same segment
    for (auto& child : node->children) {
        if (child->segment == segment) {
            insert(child.get(), rest, method, std::move(handler));
            return;
        }
    }

    // No matching child — create a new one
    auto child = std::make_unique<Node>();
    child->segment = std::string(segment);
    auto* child_ptr = child.get();
    node->children.push_back(std::move(child));
    insert(child_ptr, rest, method, std::move(handler));
}

Router::MatchResult Router::match(Method method,
                                  std::string_view path) const {
    if (path.empty() || path[0] != '/') {
        return {};
    }

    lookup_method_ = method;
    PathParams params;
    auto remaining = path.substr(1);

    Handler* handler = search(root_.get(), remaining, params);

    if (handler) {
        return MatchResult{handler, std::move(params)};
    }
    return {};
}

Router::MatchResult Router::match(std::string_view method,
                                  std::string_view path) const {
    return match(method_from_string(method), path);
}

Handler* Router::search(const Node* node, std::string_view remaining,
                        PathParams& params) const {
    // Base case: consumed entire path
    if (remaining.empty()) {
        return const_cast<Node*>(node)->get_handler(lookup_method_);
    }

    // Extract the next segment
    auto slash_pos = remaining.find('/');
    std::string_view segment;
    std::string_view rest;

    if (slash_pos == std::string_view::npos) {
        segment = remaining;
        rest    = {};
    } else {
        segment = remaining.substr(0, slash_pos);
        rest    = remaining.substr(slash_pos + 1);
    }

    // ─── Try static children first (highest priority) ────────────────
    for (const auto& child : node->children) {
        if (child->segment == segment) {
            auto* h = search(child.get(), rest, params);
            if (h) return h;
        }
    }

    // ─── Try parameter child ─────────────────────────────────────────
    if (node->param_child) {
        params.set(node->param_name, segment);
        auto* h = search(node->param_child.get(), rest, params);
        if (h) return h;
        // Backtrack if parameter match didn't lead to a handler
        // (in practice this rarely happens with well-formed routes)
    }

    // ─── Try wildcard child (lowest priority, consumes rest) ─────────
    if (node->wildcard_child) {
        // Wildcard captures everything remaining
        params.set(node->wildcard_name, remaining);
        return const_cast<Node*>(node->wildcard_child.get())
            ->get_handler(lookup_method_);
    }

    return nullptr;
}

} // namespace novaboot::router
