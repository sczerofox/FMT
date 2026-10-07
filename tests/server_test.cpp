// server 的单元测试：HTTP 监听、响应信封、错误信封
#include "fmt/server/server.hpp"

#include <string>

#include <cpp-httplib/httplib.h>

#include "fmt/common/envelope.hpp"
#include "fmt/common/string.hpp"
#include "fmt/core/app.hpp"
#include "fmt/core/path.hpp"
#include "fmt/service/commands.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

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

FMT_TEST(Server, Bucket路由与状态码) {
    fmt_test::TempDir temp("server-bucket");
    const auto root = temp / "FMT";

    auto context = fmt::initialize_service_context(root);
    FMT_CHECK(fmt::ok(context));
    fmt::AppContext& app = *std::get<std::unique_ptr<fmt::AppContext>>(context);

    // HTTP 与管道共用这一份业务实现
    auto handler = [&app](const std::string& operation, const nlohmann::json& args) {
        return fmt::service::execute_business(app, operation, args);
    };

    fmt::server::HttpServer server;
    FMT_CHECK(fmt::ok(server.start("127.0.0.1", 0, fmt::path_to_utf8(root), app.logger.get(),
                                   handler)));
    httplib::Client client("127.0.0.1", server.port());

    // 创建
    const auto created = client.Post("/api/bucket", R"({"name":"工作"})", "application/json");
    FMT_CHECK(created != nullptr);
    if (created != nullptr) {
        FMT_CHECK_EQ(created->status, 200);
        const nlohmann::json body = nlohmann::json::parse(created->body);
        FMT_CHECK(body["ok"].get<bool>());
        FMT_CHECK_EQ(body["data"]["bucket"].get<std::string>(), std::string("工作"));
    }

    // 列表
    const auto listed = client.Get("/api/bucket");
    FMT_CHECK(listed != nullptr);
    if (listed != nullptr) {
        FMT_CHECK_EQ(listed->status, 200);
        const nlohmann::json body = nlohmann::json::parse(listed->body);
        FMT_CHECK_EQ(body["data"]["buckets"].size(), std::size_t{1});
    }

    // 路径参数里的中文：客户端编码，服务端解码
    const auto one = client.Get("/api/bucket/" + fmt::url_encode("工作"));
    FMT_CHECK(one != nullptr);
    if (one != nullptr) {
        FMT_CHECK_EQ(one->status, 200);
        const nlohmann::json body = nlohmann::json::parse(one->body);
        FMT_CHECK_EQ(body["data"]["bucket"].get<std::string>(), std::string("工作"));
        FMT_CHECK(body["data"]["is_current"].get<bool>());
    }

    // 不存在 -> 404 + FMT-200
    const auto missing = client.Get("/api/bucket/nope");
    FMT_CHECK(missing != nullptr);
    if (missing != nullptr) {
        FMT_CHECK_EQ(missing->status, 404);
        FMT_CHECK_EQ(nlohmann::json::parse(missing->body)["error"]["code"].get<std::string>(),
                     std::string("FMT-200"));
    }

    // 名称非法 -> 400 + FMT-202
    const auto bad = client.Post("/api/bucket", R"({"name":"a/b"})", "application/json");
    FMT_CHECK(bad != nullptr);
    if (bad != nullptr) {
        FMT_CHECK_EQ(bad->status, 400);
        FMT_CHECK_EQ(nlohmann::json::parse(bad->body)["error"]["code"].get<std::string>(),
                     std::string("FMT-202"));
    }

    // use
    const auto used =
        client.Post("/api/bucket/" + fmt::url_encode("工作") + "/use", "", "application/json");
    FMT_CHECK(used != nullptr);
    if (used != nullptr) {
        FMT_CHECK_EQ(used->status, 200);
    }

    // delete
    const auto removed = client.Delete("/api/bucket/" + fmt::url_encode("工作"));
    FMT_CHECK(removed != nullptr);
    if (removed != nullptr) {
        FMT_CHECK_EQ(removed->status, 200);
        const nlohmann::json body = nlohmann::json::parse(removed->body);
        FMT_CHECK(body["ok"].get<bool>());
        FMT_CHECK_EQ(body["data"]["moved_to"].get<std::string>().rfind("trash/", 0), std::size_t{0});
    }

    server.stop();
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
