#include <cstdlib>
#include <novaboot/novaboot.h>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>

/// Simple logging middleware that logs every request.
class LoggingMiddleware : public novaboot::middleware::Middleware {
public:
    void handle(novaboot::http3::Request& req,
                novaboot::http3::Response& res,
                novaboot::context::RequestContext& ctx,
                Next next) override {
        auto start = std::chrono::steady_clock::now();

        // Call downstream handler
        next();

        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start);

        spdlog::info("{} {} → {} ({} μs)",
                     req.method(), req.path(),
                     res.status_code(), elapsed.count());
    }
};

int main() {
    spdlog::set_level(spdlog::level::info);

    // Build the server
    auto app = novaboot::Server::create()
        .bind("0.0.0.0", 4433)
        .tls("cert.pem", "key.pem")
        .workers((int)std::thread::hardware_concurrency())
        .middleware(std::make_shared<LoggingMiddleware>())
        .build();

    // Register routes
    app->route("/")
        .get([](auto& req, auto& res, auto& ctx) {
            res.status(200)
               .header("content-type", "text/plain")
               .body("Welcome to NovaBoot!");
        });

    app->route("/api/hello")
        .get([](auto& req, auto& res, auto& ctx) {
            res.status(200)
               .json(R"({"message": "Hello from NovaBoot!", "protocol": "HTTP/3"})");
        });

    app->route("/api/users/:id")
        .get([](auto& req, auto& res, auto& ctx) {
            auto id = req.path_params().template get_as<int>("id");
            if (!id) {
                res.status(400).body("Invalid user ID");
                return;
            }
            res.status(200)
               .json(std::format(R"({{"id": {}, "name": "User {}"}})",
                                 *id, *id));
        });

    app->route("/api/echo")
        .post([](auto& req, auto& res, auto& ctx) {
            res.status(200)
               .header("content-type",
                       req.header("content-type")
                           .value_or("application/octet-stream"))
               .body(req.body());
        });

    app->route("/health")
        .get([](auto& req, auto& res, auto& ctx) {
            res.status(200).json(R"({"status": "ok"})");
        });

    // Run (blocks until SIGINT/SIGTERM)
    app->run();

    return 0;
}
