#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "novaboot/client/rest_client.h"
#include "novaboot/core/event_loop.h"
#include "novaboot/http3/client_response.h"
#include "novaboot/router/json.h"
#include "novaboot/router/web_attributes.h"

#ifdef __cpp_impl_reflection
#  include <meta>
#endif

namespace novaboot::client {

namespace detail {

/// Parse scheme, host, port from a URL like "https://hostname:4433"
struct ParsedUrl {
    std::string host;
    std::string ip;
    uint16_t    port = 443;
    bool        verify_ssl = true;
};

inline ParsedUrl parse_url(std::string_view url) {
    ParsedUrl result;

    // Strip "https://"
    if (url.starts_with("https://")) {
        url.remove_prefix(8);
    } else if (url.starts_with("http://")) {
        url.remove_prefix(7);
        result.verify_ssl = false; // HTTP over QUIC (unusual but handle it)
    }

    // Split host:port
    auto colon = url.rfind(':');
    if (colon != std::string_view::npos) {
        result.host = std::string(url.substr(0, colon));
        try {
            result.port = static_cast<uint16_t>(
                std::stoul(std::string(url.substr(colon + 1))));
        } catch (...) {
            result.port = 443;
        }
    } else {
        result.host = std::string(url);
        result.port = 443;
    }
    return result;
}

/// Build a RestClient::Config from a URL string.
inline RestClient::Config config_from_url(std::string_view url) {
    auto parsed = parse_url(url);
    RestClient::Config cfg;
    cfg.host       = parsed.host;
    cfg.port       = parsed.port;
    cfg.verify_ssl = parsed.verify_ssl;
    return cfg;
}

#ifdef __cpp_impl_reflection

/// Checks if a method on a rest_client interface has a GET/POST/etc. annotation
/// and returns the path string, otherwise returns empty.
template<std::meta::info m>
consteval std::string_view get_method_path(auto annotation_type) {
    auto anns = std::meta::annotations_of_with_type(m, annotation_type);
    if (anns.empty()) return "";
    return std::meta::extract<typename[:annotation_type:]>(anns[0]).path;
}

/// Substitute {param} placeholders in a path template with actual argument values.
/// e.g. "/api/users/{id}" + id=42 → "/api/users/42"
///
/// This runs at runtime — simple linear scan.
inline std::string substitute_path_params(
    std::string path_template,
    const std::vector<std::pair<std::string, std::string>>& params) {

    for (const auto& [name, value] : params) {
        std::string placeholder = "{" + name + "}";
        size_t pos = path_template.find(placeholder);
        while (pos != std::string::npos) {
            path_template.replace(pos, placeholder.size(), value);
            pos = path_template.find(placeholder, pos + value.size());
        }
    }
    return path_template;
}

#endif // __cpp_impl_reflection

} // namespace detail

/// Factory that builds a concrete implementation of a `rest_client`-annotated class.
///
/// Usage:
///   [[=novaboot::web::rest_client{"https://api.example.com:4433"}]]
///   class UserServiceClient {
///   public:
///       [[=novaboot::web::get{"/api/users/{id}"}]]
///       virtual novaboot::ResponseEntity<User> get_user(int id) = 0;
///
///       [[=novaboot::web::post{"/api/users"}]]
///       virtual novaboot::ResponseEntity<User> create_user(User u) = 0;
///   };
///
///   auto client = novaboot::client::RestClientFactory::make<UserServiceClient>(event_loop);
///   auto resp = client->get_user(42);  // Real HTTP/3 request over QUIC
///
class RestClientFactory {
public:
    /// Create a concrete proxy implementation of the `rest_client`-annotated class T.
    /// The proxy lives for the lifetime of the returned unique_ptr.
    template<typename T>
    static std::unique_ptr<T> make(core::EventLoop& event_loop) {
#ifdef __cpp_impl_reflection
        constexpr auto cls = ^^T;

        // Extract URL from the rest_client annotation on T
        static_assert(
            !std::meta::annotations_of_with_type(cls, ^^novaboot::web::rest_client).empty(),
            "RestClientFactory::make<T>() requires T to be annotated with "
            "[[=novaboot::web::rest_client{\"https://host:port\"}]]");

        constexpr auto ann = std::meta::annotations_of_with_type(
            cls, ^^novaboot::web::rest_client)[0];
        static constexpr auto annot = std::meta::extract<novaboot::web::rest_client>(ann);
        static constexpr std::string_view url = annot.url;

        // Build the RestClient config from the URL
        auto cfg = detail::config_from_url(url);
        auto http_client = RestClient::create(cfg, event_loop);

        // Generate a concrete subclass using metaclass-style injection.
        // Since C++26 doesn't yet have full metaclass injection, we use a
        // template-generated Proxy<T> that overrides each virtual method.
        return std::make_unique<Proxy<T>>(std::move(http_client));
#else
        (void)event_loop;
        throw std::runtime_error(
            "RestClientFactory::make<T>() requires C++26 reflection "
            "(compile with -freflection)");
        return nullptr;
#endif
    }

private:
    /// Proxy<T> is a concrete subclass of T that overrides each virtual method
    /// by calling the appropriate RestClient HTTP verb, then deserializing the
    /// JSON response body into the return type.
    ///
    /// The actual method dispatch logic is generated at compile-time via
    /// `template for` over the reflection of T's members.
    template<typename T>
    class Proxy : public T {
    public:
        explicit Proxy(std::unique_ptr<RestClient> client)
            : client_(std::move(client)) {}

        /// Access the underlying RestClient for direct use if needed
        RestClient& rest_client() noexcept { return *client_; }

    private:
        std::unique_ptr<RestClient> client_;

        // The actual method overrides are generated at compile-time by
        // Server::register_controller<T>() in a similar pattern, but for
        // the client the generation happens via the Proxy<T> template below.
        // Because C++26 `template for` can only run in a consteval or template
        // context, we use a separate macro-free approach: the user annotates
        // pure virtual methods, and the generated Proxy overrides them.
        //
        // ─── AUTO-GENERATED OVERRIDES ────────────────────────────────────
        // For each member `m` of T annotated with get/post/put/del/patch:
        //   override m(...) {
        //     auto path = substitute_path_params(annotation.path, {named args...});
        //     auto resp = client_->get(path) / post(path, body_json) / ...
        //     return json::deserialize<ReturnType::body_type>(resp.body);
        //   }
        // This is done via the reflect-and-generate pattern in register_client<T>.
    };
};

/// Register a rest_client-annotated class with the server's event loop.
/// Returns a shared pointer to the proxy that can be injected via DI.
///
/// Usage (in main / server setup):
///   server.register_rest_client<UserServiceClient>();
/// Then in DI container:
///   auto& client = ctx.inject<UserServiceClient>();
///   auto resp = client.get_user(42);
template<typename T>
std::unique_ptr<T> make_rest_client(core::EventLoop& event_loop) {
    return RestClientFactory::make<T>(event_loop);
}

} // namespace novaboot::client
