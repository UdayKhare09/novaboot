#include <gtest/gtest.h>
#include <array>
#include "novaboot/novaboot.h"
#include "novaboot/db/transaction.h"

using namespace novaboot;
using namespace novaboot::di;
using namespace novaboot::annotations;

// ─── Test Components ──────────────────────────────────────────────────────────

static int g_repository_calls = 0;
static int g_post_construct_calls = 0;
static int g_pre_destroy_calls = 0;
static int g_controller_calls = 0;
static int g_tx_controller_calls = 0;
static int g_tx_proxy_calls = 0;
static int g_websocket_open_calls = 0;
static int g_websocket_message_calls = 0;
static int g_websocket_close_calls = 0;
static std::string g_websocket_principal;

struct TxRouteState {
    int begins = 0;
    int commits = 0;
    int rollbacks = 0;
};

class TxRouteConnection : public novaboot::db::Connection {
public:
    explicit TxRouteConnection(std::shared_ptr<TxRouteState> state)
        : state_(std::move(state)) {}

    std::int64_t execute(std::string_view, const std::vector<novaboot::db::Parameter>& = {}) override {
        return 0;
    }

    std::unique_ptr<novaboot::db::ResultSet> query(
        std::string_view, const std::vector<novaboot::db::Parameter>& = {}) override {
        throw std::logic_error("fake transaction route connection does not support queries");
    }

    std::int64_t last_insert_id() override { return 0; }
    void begin_transaction() override { state_->begins++; }
    void commit() override { state_->commits++; }
    void rollback() override { state_->rollbacks++; }

private:
    std::shared_ptr<TxRouteState> state_;
};

class TxRouteDataSource : public novaboot::db::DataSource {
public:
    explicit TxRouteDataSource(std::shared_ptr<TxRouteState> state)
        : state_(std::move(state)),
          connection_(std::make_shared<TxRouteConnection>(state_)),
          dialect_(std::make_shared<novaboot::db::SqliteDialect>()) {}

    std::shared_ptr<novaboot::db::Connection> get_connection() override {
        return connection_;
    }

    std::shared_ptr<novaboot::db::SqlDialect> dialect() override {
        return dialect_;
    }

    void close() override {}

private:
    std::shared_ptr<TxRouteState> state_;
    std::shared_ptr<novaboot::db::Connection> connection_;
    std::shared_ptr<novaboot::db::SqlDialect> dialect_;
};

struct [[= Repository() ]] TestRepository {
    void do_db_work() {
        g_repository_calls++;
    }
};

struct [[= Service() ]] TestService {
    TestRepository& repo;
    
    explicit TestService(TestRepository& r) : repo(r) {}
    
    void do_service_work() {
        repo.do_db_work();
    }
    
    [[= PostConstruct() ]]
    void init() {
        g_post_construct_calls++;
    }
    
    [[= PreDestroy() ]]
    void destroy() {
        g_pre_destroy_calls++;
    }
};

struct [[= RestController("/api/test") ]] TestController {
    TestService& service;
    
    explicit TestController(TestService& s) : service(s) {}
    
    [[= GetMapping("/hello")]]
    void hello(http3::Request&, http3::Response&, context::RequestContext&) {
        g_controller_calls++;
        service.do_service_work();
    }
    
    [[= PostMapping("/create")]]
    void create(http3::Request&, http3::Response&, context::RequestContext&) {
        g_controller_calls += 10;
        service.do_service_work();
    }
};

struct [[= RestController("/api/tx") ]] TxRouteController {
    [[= PostMapping("/fail") ]]
    [[= Transactional() ]]
    void fail(http3::Request&, http3::Response&, context::RequestContext&) {
        g_tx_controller_calls++;
        throw std::runtime_error("route failure");
    }
};

struct [[= Service() ]] TxProxyService {
    [[= Transactional() ]]
    void committed() {
        g_tx_proxy_calls++;
    }

    [[= Transactional() ]]
    void failed() {
        g_tx_proxy_calls++;
        throw std::runtime_error("proxy failure");
    }

    void plain() {
        g_tx_proxy_calls++;
    }
};

struct [[= WebSocket("/ws/annotated") ]] AnnotatedWebSocket {
    void on_open(websocket::Session& session) {
        g_websocket_open_calls++;
        g_websocket_principal = session.principal();
    }

    void on_message(websocket::Session&, const websocket::Message&) {
        g_websocket_message_calls++;
    }

    void on_close(websocket::Session&, const websocket::CloseInfo&) {
        g_websocket_close_calls++;
    }

    websocket::HandshakeDecision authorize(const http3::Request& request) {
        if (const auto user = request.header("x-demo-user");
            user && *user == "socket-user") {
            return websocket::HandshakeDecision::allow("socket-user");
        }
        return websocket::HandshakeDecision::reject(401, "WebSocket authentication required");
    }
};

// ─── Tests ────────────────────────────────────────────────────────────────────

static_assert(has_annotation<Repository>(^^TestRepository), "TestRepository should have Repository annotation");
static_assert(has_annotation<Service>(^^TestService), "TestService should have Service annotation");
static_assert(has_annotation<RestController>(^^TestController), "TestController should have RestController annotation");
static_assert(has_annotation<WebSocket>(^^AnnotatedWebSocket), "WebSocket endpoint should have WebSocket annotation");

TEST(AnnotationsTest, WebSocketEndpointAutoRegistration) {
    g_websocket_open_calls = 0;
    g_websocket_message_calls = 0;
    g_websocket_close_calls = 0;
    g_websocket_principal.clear();

    RootContainer di_root;
    register_beans<AnnotatedWebSocket>(di_root);
    di_root.build();

    router::Router router;
    di_root.register_routes_and_advice(router);
    ASSERT_EQ(router.websocket_routes().size(), 1U);
    EXPECT_EQ(router.websocket_routes()[0], "/ws/annotated");

    auto match = router.match_websocket("/ws/annotated");
    ASSERT_NE(match.handler, nullptr);
    http3::Request request;
    request.headers().add("x-demo-user", "socket-user");
    const auto decision = match.handler->authorize(request);
    ASSERT_TRUE(decision.accepted);
    EXPECT_EQ(decision.principal, "socket-user");

    websocket::Connection connection(*match.handler, decision.principal);
    EXPECT_EQ(g_websocket_open_calls, 1);
    EXPECT_EQ(g_websocket_principal, "socket-user");

    const std::array<std::uint8_t, 8> frame{
        0x81U, 0x82U, 0x11U, 0x22U, 0x33U, 0x44U,
        static_cast<std::uint8_t>('o' ^ 0x11),
        static_cast<std::uint8_t>('k' ^ 0x22),
    };
    ASSERT_TRUE(connection.feed(frame).has_value());
    EXPECT_EQ(g_websocket_message_calls, 1);

    auto rejected_request = http3::Request{};
    const auto rejected = match.handler->authorize(rejected_request);
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.rejection_status, 401);
    di_root.shutdown();
}

TEST(AnnotationsTest, FullLifecycleAndRouteScanning) {
    g_repository_calls = 0;
    g_post_construct_calls = 0;
    g_pre_destroy_calls = 0;
    g_controller_calls = 0;

    RootContainer di_root;
    
    // Register beans using reflection scanner
    register_beans<TestRepository, TestService, TestController>(di_root);
    
    di_root.build();
    
    // Verify PostConstruct was invoked
    EXPECT_EQ(g_post_construct_calls, 1);
    
    // Verify beans exist and are wired
    EXPECT_TRUE(di_root.has<TestRepository>());
    EXPECT_TRUE(di_root.has<TestService>());
    EXPECT_TRUE(di_root.has<TestController>());
    
    auto& controller = di_root.resolve<TestController>();
    auto& service = di_root.resolve<TestService>();
    EXPECT_EQ(&controller.service, &service);
    
    // Verify routes can be registered and resolved
    router::Router router;
    register_routes<TestController>(router);
    
    // Verify the routes exist in the router
    EXPECT_EQ(router.size(), 2u);
    
    // Match GET /api/test/hello
    auto match_get = router.match(router::Method::GET, "/api/test/hello");
    ASSERT_NE(match_get.handler, nullptr);
    
    // Match POST /api/test/create
    auto match_post = router.match(router::Method::POST, "/api/test/create");
    ASSERT_NE(match_post.handler, nullptr);
    
    // Invoke handlers
    auto shard = di_root.make_shard_container();
    auto req_container = shard->make_request_container();
    context::RequestContext ctx;
    ctx.bind_container(*req_container);
    
    http3::Request req;
    http3::Response res;
    
    (*match_get.handler)(req, res, ctx);
    EXPECT_EQ(g_controller_calls, 1);
    EXPECT_EQ(g_repository_calls, 1);
    
    (*match_post.handler)(req, res, ctx);
    EXPECT_EQ(g_controller_calls, 11);
    EXPECT_EQ(g_repository_calls, 2);
    
    // Shutdown container and verify PreDestroy
    di_root.shutdown();
    EXPECT_EQ(g_pre_destroy_calls, 1);
}

TEST(AnnotationsTest, TransactionalRouteRollsBackOnException) {
    g_tx_controller_calls = 0;
    auto state = std::make_shared<TxRouteState>();
    auto ds = std::make_shared<TxRouteDataSource>(state);

    RootContainer di_root;
    di_root.register_bean<std::shared_ptr<novaboot::db::DataSource>>(
        [ds](ContainerBase&) {
            return new std::shared_ptr<novaboot::db::DataSource>(ds);
        });
    di_root.register_bean<novaboot::db::TransactionManager>(
        [](ContainerBase& c) {
            return new novaboot::db::TransactionManager(
                c.resolve<std::shared_ptr<novaboot::db::DataSource>>());
        });
    register_beans<TxRouteController>(di_root);
    di_root.build();

    router::Router router;
    register_routes<TxRouteController>(router);

    auto match = router.match(router::Method::POST, "/api/tx/fail");
    ASSERT_NE(match.handler, nullptr);

    auto shard = di_root.make_shard_container();
    auto req_container = shard->make_request_container();
    context::RequestContext ctx;
    ctx.bind_container(*req_container);

    http3::Request req;
    req.set_method("POST");
    http3::Response res;

    EXPECT_THROW((*match.handler)(req, res, ctx), std::runtime_error);
    EXPECT_EQ(g_tx_controller_calls, 1);
    EXPECT_EQ(state->begins, 1);
    EXPECT_EQ(state->commits, 0);
    EXPECT_EQ(state->rollbacks, 1);

    di_root.shutdown();
}

TEST(AnnotationsTest, ScannerRegistersTransactionalServiceProxy) {
    g_tx_proxy_calls = 0;
    auto state = std::make_shared<TxRouteState>();
    auto ds = std::make_shared<TxRouteDataSource>(state);

    RootContainer di_root;
    di_root.register_bean<std::shared_ptr<novaboot::db::DataSource>>(
        [ds](ContainerBase&) {
            return new std::shared_ptr<novaboot::db::DataSource>(ds);
        });
    di_root.register_bean<novaboot::db::TransactionManager>(
        [](ContainerBase& c) {
            return new novaboot::db::TransactionManager(
                c.resolve<std::shared_ptr<novaboot::db::DataSource>>());
        });
    register_beans<TxProxyService>(di_root);
    di_root.build();

    using Proxy = novaboot::db::TransactionalProxy<TxProxyService>;
    EXPECT_TRUE(di_root.has<TxProxyService>());
    EXPECT_TRUE(di_root.has<Proxy>());

    auto& proxy = di_root.resolve<Proxy>();
    proxy.invoke<&TxProxyService::committed>();
    EXPECT_EQ(g_tx_proxy_calls, 1);
    EXPECT_EQ(state->begins, 1);
    EXPECT_EQ(state->commits, 1);
    EXPECT_EQ(state->rollbacks, 0);

    EXPECT_THROW(proxy.invoke<&TxProxyService::failed>(), std::runtime_error);
    EXPECT_EQ(g_tx_proxy_calls, 2);
    EXPECT_EQ(state->begins, 2);
    EXPECT_EQ(state->commits, 1);
    EXPECT_EQ(state->rollbacks, 1);

    proxy.invoke<&TxProxyService::plain>();
    EXPECT_EQ(g_tx_proxy_calls, 3);
    EXPECT_EQ(state->begins, 2);
    EXPECT_EQ(state->commits, 1);
    EXPECT_EQ(state->rollbacks, 1);

    di_root.shutdown();
}
