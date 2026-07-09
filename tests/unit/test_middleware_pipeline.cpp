#include <gtest/gtest.h>
#include "novaboot/middleware/pipeline.h"
#include "novaboot/middleware/middleware.h"
#include "novaboot/http3/request.h"
#include "novaboot/http3/response.h"
#include "novaboot/context/request_context.h"

using namespace novaboot;
using namespace novaboot::middleware;

class TraceMiddleware : public Middleware {
public:
    explicit TraceMiddleware(std::string name, std::vector<std::string>& trace)
        : name_(std::move(name)), trace_(trace) {}

    void handle(http3::Request&, http3::Response&,
                context::RequestContext&, Next next) override {
        trace_.push_back(name_ + "-pre");
        next();
        trace_.push_back(name_ + "-post");
    }

private:
    std::string name_;
    std::vector<std::string>& trace_;
};

class ShortCircuitMiddleware : public Middleware {
public:
    explicit ShortCircuitMiddleware(std::vector<std::string>& trace)
        : trace_(trace) {}

    void handle(http3::Request&, http3::Response& res,
                context::RequestContext&, Next /*next*/) override {
        trace_.push_back("sc-pre");
        res.status(403).body("Forbidden");
        // Do NOT call next()
        trace_.push_back("sc-post");
    }

private:
    std::vector<std::string>& trace_;
};

TEST(MiddlewarePipelineTest, ExecutionOrder) {
    std::vector<std::string> trace;
    Pipeline pipeline;
    pipeline.add(std::make_shared<TraceMiddleware>("mw1", trace));
    pipeline.add(std::make_shared<TraceMiddleware>("mw2", trace));

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;

    router::Handler final_handler = [&trace](http3::Request&, http3::Response& final_res, context::RequestContext&) {
        trace.push_back("handler");
        final_res.status(200).body("OK");
    };

    pipeline.execute(req, res, ctx, final_handler);

    ASSERT_EQ(trace.size(), 5);
    EXPECT_EQ(trace[0], "mw1-pre");
    EXPECT_EQ(trace[1], "mw2-pre");
    EXPECT_EQ(trace[2], "handler");
    EXPECT_EQ(trace[3], "mw2-post");
    EXPECT_EQ(trace[4], "mw1-post");
    EXPECT_EQ(res.status_code(), 200);
}

TEST(MiddlewarePipelineTest, ShortCircuit) {
    std::vector<std::string> trace;
    Pipeline pipeline;
    pipeline.add(std::make_shared<TraceMiddleware>("mw1", trace));
    pipeline.add(std::make_shared<ShortCircuitMiddleware>(trace));
    pipeline.add(std::make_shared<TraceMiddleware>("mw2", trace)); // should be skipped

    http3::Request req;
    http3::Response res;
    context::RequestContext ctx;

    bool handler_called = false;
    router::Handler final_handler = [&handler_called](http3::Request&, http3::Response&, context::RequestContext&) {
        handler_called = true;
    };

    pipeline.execute(req, res, ctx, final_handler);

    EXPECT_FALSE(handler_called);
    ASSERT_EQ(trace.size(), 4);
    EXPECT_EQ(trace[0], "mw1-pre");
    EXPECT_EQ(trace[1], "sc-pre");
    EXPECT_EQ(trace[2], "sc-post");
    EXPECT_EQ(trace[3], "mw1-post");
    EXPECT_EQ(res.status_code(), 403);
    EXPECT_EQ(res.body_data(), "Forbidden");
}
