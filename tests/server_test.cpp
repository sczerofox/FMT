// server 的单元测试：HTTP 监听、响应信封、错误信封
#include "fmt/server/server.hpp"

#include <string>

#include <cpp-httplib/httplib.h>

#include "fmt/common/envelope.hpp"
#include "fmt_test.hpp"

FMT_TEST(Server, 启动监听并应答状态) {
    fmt::server::HttpServer server;
    // 端口传 0：让系统分配一个空闲端口，测试不会撞上 4122 被占。
    const fmt::Status started = server.start("127.0.0.1", 0, R"(D:\FMT)", nullptr);
    FMT_CHECK(fmt::ok(started));
    FMT_CHECK(server.running());
    FMT_CHECK(server.port() > 0);

    httplib::Client client("127.0.0.1", server.port());
    const auto response = client.Get("/api/status");
    FMT_CHECK(response != nullptr);
    if (response != nullptr) {
        FMT_CHECK_EQ(response->status, 200);
        const nlohmann::json body = nlohmann::json::parse(response->body);
        FMT_CHECK(body["ok"].get<bool>());
        FMT_CHECK_EQ(body["data"]["root"].get<std::string>(), std::string(R"(D:\FMT)"));
        FMT_CHECK_EQ(body["data"]["channel"].get<std::string>(), std::string("http"));
    }

    server.stop();
    FMT_CHECK(!server.running());
}

FMT_TEST(Server, 未知路由返回错误信封) {
    fmt::server::HttpServer server;
    FMT_CHECK(fmt::ok(server.start("127.0.0.1", 0, R"(C:\FMT)", nullptr)));

    httplib::Client client("127.0.0.1", server.port());

    // 尚未实现的业务路由
    const auto pending = client.Get("/api/file/list");
    FMT_CHECK(pending != nullptr);
    if (pending != nullptr) {
        FMT_CHECK_EQ(pending->status, 500);
        const nlohmann::json body = nlohmann::json::parse(pending->body);
        FMT_CHECK(!body["ok"].get<bool>());
        FMT_CHECK_EQ(body["error"]["code"].get<std::string>(), std::string("FMT-602"));
    }

    // 完全不存在的路径
    const auto missing = client.Get("/nothing/here");
    FMT_CHECK(missing != nullptr);
    if (missing != nullptr) {
        FMT_CHECK_EQ(missing->status, 404);
        const nlohmann::json body = nlohmann::json::parse(missing->body);
        FMT_CHECK(!body["ok"].get<bool>());
        FMT_CHECK_EQ(body["error"]["code"].get<std::string>(), std::string("FMT-002"));
    }

    server.stop();
}

FMT_TEST(Server, 绑不上地址时返回错误而不是崩溃) {
    fmt::server::HttpServer server;
    // 10.255.255.1 不是本机地址，绑不上。
    // 不用「同端口再绑一次」来测：Windows 的 SO_REUSEADDR 允许重复绑定同一端口
    // （与 Linux 语义相反），那样测不出失败路径。
    const fmt::Status failed = server.start("10.255.255.1", 4122, R"(D:\FMT)", nullptr);
    FMT_CHECK(!fmt::ok(failed));
    FMT_CHECK(fmt::error_of(failed)->code == fmt::ErrorCode::ServiceOperationFailed);
    FMT_CHECK(!server.running());
    FMT_CHECK_EQ(server.port(), 0);
}

FMT_TEST(Server, 信封与管道共用同一份实现) {
    const nlohmann::json good = fmt::envelope_ok(nlohmann::json{{"count", 3}});
    FMT_CHECK(good["ok"].get<bool>());
    FMT_CHECK_EQ(good["data"]["count"].get<int>(), 3);

    const nlohmann::json bad = fmt::envelope_error(fmt::make_error(fmt::ErrorCode::BucketNotFound));
    FMT_CHECK(!bad["ok"].get<bool>());
    FMT_CHECK_EQ(bad["error"]["code"].get<std::string>(), std::string("FMT-200"));
    FMT_CHECK_EQ(bad["error"]["message"].get<std::string>(), std::string("Bucket 不存在"));
}
