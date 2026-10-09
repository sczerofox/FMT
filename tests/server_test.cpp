// server 的单元测试：HTTP 监听、响应信封、错误信封
#include "fmt/server/server.hpp"

#include <string>

#include <cpp-httplib/httplib.h>

#include "fmt/common/envelope.hpp"
#include "fmt/common/string.hpp"
#include "fmt/core/app.hpp"
#include "fmt/core/path.hpp"
#include "fmt/file/file.hpp"
#include "fmt/bucket/bucket.hpp"
#include "fmt/service/commands.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

namespace {

// 测试用的固定 token。HTTP 层要求注入校验器（没注入一律 401），
// 这里给一个只认这一个 token 的校验器；测 401 的用例会显式不带它。
constexpr const char* kTestToken = "test-token-0123456789abcdef";

fmt::server::TokenVerifier test_verifier() {
    return [](const std::string& token) -> std::string {
        return token == kTestToken ? std::string("user") : std::string{};
    };
}

// 默认带上 token 头的客户端（每个请求都带，省得每处手写）
httplib::Client test_client(const fmt::server::HttpServer& server) {
    httplib::Client client("127.0.0.1", server.port());
    client.set_default_headers({{"X-FMT-Token", kTestToken}});
    client.set_default_headers({{"X-FMT-Token", kTestToken}});
    return client;
}

}  // namespace
FMT_TEST(Server, 启动监听并应答状态) {
    fmt::server::HttpServer server;
    // 端口传 0：让系统分配一个空闲端口，测试不会撞上 4122 被占。
    const fmt::Status started = server.start("127.0.0.1", 0, R"(D:\FMT)", nullptr, {}, test_verifier());
    FMT_CHECK(fmt::ok(started));
    FMT_CHECK(server.running());
    FMT_CHECK(server.port() > 0);

    httplib::Client client("127.0.0.1", server.port());
    client.set_default_headers({{"X-FMT-Token", kTestToken}});
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
    FMT_CHECK(fmt::ok(server.start("127.0.0.1", 0, R"(C:\FMT)", nullptr, {}, test_verifier())));

    httplib::Client client("127.0.0.1", server.port());
    client.set_default_headers({{"X-FMT-Token", kTestToken}});

    // 已知模块但没这个接口 → **501 Not Implemented**
    //（原来是 500，等于说「服务器坏了」；而这只是「这个接口没做」）
    const auto pending = client.Get("/api/file/list");
    FMT_CHECK(pending != nullptr);
    if (pending != nullptr) {
        FMT_CHECK_EQ(pending->status, 501);
        const nlohmann::json body = nlohmann::json::parse(pending->body);
        FMT_CHECK(!body["ok"].get<bool>());
        FMT_CHECK_EQ(body["error"]["code"].get<std::string>(), std::string("FMT-602"));
    }

    // share 整组还没实现：同样 501，不是 500
    const auto share = client.Get("/api/share/x");
    FMT_CHECK(share != nullptr);
    if (share != nullptr) {
        FMT_CHECK_EQ(share->status, 501);
    }

    // 完全打错的 /api 路径 → **404 Not Found** + FMT-017
    const auto unknown = client.Get("/api/nosuch");
    FMT_CHECK(unknown != nullptr);
    if (unknown != nullptr) {
        FMT_CHECK_EQ(unknown->status, 404);
        const nlohmann::json body = nlohmann::json::parse(unknown->body);
        FMT_CHECK_EQ(body["error"]["code"].get<std::string>(), std::string("FMT-017"));
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
    const fmt::Status failed = server.start("10.255.255.1", 4122, R"(D:\FMT)", nullptr, {}, test_verifier());
    FMT_CHECK(!fmt::ok(failed));
    FMT_CHECK(fmt::error_of(failed)->code == fmt::ErrorCode::ServiceOperationFailed);
    FMT_CHECK(!server.running());
    FMT_CHECK_EQ(server.port(), 0);
}

FMT_TEST(Server, 管理接口要token且桶路由已下线) {
    fmt_test::TempDir temp("server-auth");
    const auto root = temp / "root";
    fmt::server::HttpServer server;
    FMT_CHECK(fmt::ok(server.start("127.0.0.1", 0, fmt::path_to_utf8(root), nullptr, {}, test_verifier())));

    // ① 不带 token：管理接口一律 401 + FMT-018
    {
        httplib::Client anonymous("127.0.0.1", server.port());
        const auto response = anonymous.Get("/api/file");
        FMT_CHECK(response != nullptr);
        if (response != nullptr) {
            FMT_CHECK_EQ(response->status, 401);
            const nlohmann::json body = nlohmann::json::parse(response->body);
            FMT_CHECK_EQ(body["error"]["code"].get<std::string>(), std::string("FMT-018"));
        }
    }

    // ② token 不对：同样 401
    {
        httplib::Client wrong("127.0.0.1", server.port());
        wrong.set_default_headers({{"X-FMT-Token", "not-the-token"}});
        const auto response = wrong.Get("/api/file");
        FMT_CHECK(response != nullptr && response->status == 401);
    }

    // ③ Authorization: Bearer 也要认
    {
        httplib::Client bearer("127.0.0.1", server.port());
        bearer.set_default_headers({{"Authorization", std::string("Bearer ") + kTestToken}});
        const auto response = bearer.Get("/api/status");
        FMT_CHECK(response != nullptr && response->status == 200);
    }

    // ④ 健康检查不需要 token（唯一的公开读接口）
    {
        httplib::Client anonymous("127.0.0.1", server.port());
        const auto response = anonymous.Get("/api/ping");
        FMT_CHECK(response != nullptr && response->status == 200);
    }

    // ⑤ 桶接口已下线（用户明确不要 HTTP 桶接口）：404「没有这个接口」，不是 501
    {
        httplib::Client client("127.0.0.1", server.port());
        client.set_default_headers({{"X-FMT-Token", kTestToken}});
        for (const char* path : {"/api/bucket", "/api/bucket/x"}) {
            const auto response = client.Get(path);
            FMT_CHECK(response != nullptr);
            if (response != nullptr) {
                FMT_CHECK_EQ(response->status, 404);
                const nlohmann::json body = nlohmann::json::parse(response->body);
                FMT_CHECK_EQ(body["error"]["code"].get<std::string>(), std::string("FMT-017"));
            }
        }
    }

    server.stop();
}

FMT_TEST(Server, File路由与上传) {
    fmt_test::TempDir temp("server-file");
    const auto root = temp / "FMT";

    auto context = fmt::initialize_service_context(root);
    FMT_CHECK(fmt::ok(context));
    fmt::AppContext& app = *std::get<std::unique_ptr<fmt::AppContext>>(context);

    fmt::BucketService buckets(*app.paths, app.config, app.logger.get());
    FMT_CHECK(fmt::ok(buckets.create("工作")));

    const auto source = temp / "upload.bin";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(source, "0123456789")));

    // upload 在真实服务里由运行体做两段式；这里用同一对函数（prepare + commit）代跑。
    auto handler = [&app](const std::string& operation,
                          const nlohmann::json& args) -> fmt::Result<nlohmann::json> {
        if (operation == "file.upload") {
            std::string from;
            std::string name;
            if (args.contains("argv") && args["argv"].is_array()) {
                if (!args["argv"].empty()) {
                    from = args["argv"][0].get<std::string>();
                }
                if (args["argv"].size() > 1) {
                    name = args["argv"][1].get<std::string>();
                }
            }
            auto prepared = fmt::prepare_upload(*app.paths, from, name, app.config.max_upload_size,
                                                app.logger.get());
            if (!fmt::ok(prepared)) {
                return *fmt::error_of(prepared);
            }
            fmt::PreparedUpload upload = std::get<fmt::PreparedUpload>(prepared);
            fmt::FileService files(*app.paths, app.config, app.logger.get());
            auto record = files.commit_upload(upload);
            if (!fmt::ok(record)) {
                return *fmt::error_of(record);
            }
            const fmt::FileRecord& stored = std::get<fmt::FileRecord>(record);
            nlohmann::json data = nlohmann::json::object();
            data["file_id"] = stored.file_id;
            data["file_name"] = stored.file_name;
            data["size"] = stored.size;
            data["message"] = "文件已入库：" + stored.file_name;
            return data;
        }
        return fmt::service::execute_business(app, operation, args);
    };

    fmt::server::HttpServer server;
    FMT_CHECK(fmt::ok(server.start("127.0.0.1", 0, fmt::path_to_utf8(root), app.logger.get(),
                                   handler, test_verifier())));
    httplib::Client client("127.0.0.1", server.port());
    client.set_default_headers({{"X-FMT-Token", kTestToken}});

    // 空请求体 -> 400 + FMT-001
    const auto empty_body = client.Post("/api/file", "", "application/json");
    FMT_CHECK(empty_body != nullptr);
    if (empty_body != nullptr) {
        FMT_CHECK_EQ(empty_body->status, 400);
        FMT_CHECK_EQ(nlohmann::json::parse(empty_body->body)["error"]["code"].get<std::string>(),
                     std::string("FMT-001"));
    }

    // 与 file_id 同形的文件名 -> 400 + FMT-106（不能落到 default 的 500）
    const nlohmann::json bad_name{{"path", fmt::path_to_utf8(source)},
                                  {"file_name", "fmt-20261008-0"}};
    const auto rejected_name = client.Post("/api/file", bad_name.dump(), "application/json");
    FMT_CHECK(rejected_name != nullptr);
    if (rejected_name != nullptr) {
        FMT_CHECK_EQ(rejected_name->status, 400);
        FMT_CHECK_EQ(
            nlohmann::json::parse(rejected_name->body)["error"]["code"].get<std::string>(),
            std::string("FMT-106"));
    }

    // 上传：CLI 传来源，不传内容
    std::string file_id;
    const nlohmann::json body{{"path", fmt::path_to_utf8(source)}, {"file_name", "doc.bin"}};
    const auto uploaded = client.Post("/api/file", body.dump(), "application/json");
    FMT_CHECK(uploaded != nullptr);
    if (uploaded != nullptr) {
        FMT_CHECK_EQ(uploaded->status, 200);
        const nlohmann::json parsed = nlohmann::json::parse(uploaded->body);
        FMT_CHECK(parsed["ok"].get<bool>());
        file_id = parsed["data"]["file_id"].get<std::string>();
        FMT_CHECK_EQ(parsed["data"]["file_name"].get<std::string>(), std::string("doc.bin"));
    }
    FMT_CHECK(!file_id.empty());

    // 列表
    const auto listed = client.Get("/api/file");
    FMT_CHECK(listed != nullptr);
    if (listed != nullptr) {
        FMT_CHECK_EQ(listed->status, 200);
        FMT_CHECK_EQ(nlohmann::json::parse(listed->body)["data"]["files"].size(), std::size_t{1});
    }

    // 按 file_id 与按文件名都能查
    for (const std::string& key : {file_id, std::string("doc.bin")}) {
        const auto detail = client.Get("/api/file/" + fmt::url_encode(key));
        FMT_CHECK(detail != nullptr);
        if (detail != nullptr) {
            FMT_CHECK_EQ(detail->status, 200);
            FMT_CHECK_EQ(nlohmann::json::parse(detail->body)["data"]["file_id"].get<std::string>(),
                         file_id);
        }
    }

    // 不存在 -> 404 + FMT-002
    const auto missing = client.Get("/api/file/nope.bin");
    FMT_CHECK(missing != nullptr);
    if (missing != nullptr) {
        FMT_CHECK_EQ(missing->status, 404);
        FMT_CHECK_EQ(nlohmann::json::parse(missing->body)["error"]["code"].get<std::string>(),
                     std::string("FMT-002"));
    }

    // 预检：?dry_run=1 只读、零副作用，同桶删除不需要确认
    const auto precheck = client.Delete("/api/file/" + fmt::url_encode(file_id) + "?dry_run=1");
    FMT_CHECK(precheck != nullptr);
    if (precheck != nullptr) {
        FMT_CHECK_EQ(precheck->status, 200);
        const nlohmann::json plan = nlohmann::json::parse(precheck->body);
        FMT_CHECK(!plan["data"]["needs_confirm"].get<bool>());
        FMT_CHECK(!plan["data"]["blocked"].get<bool>());
        FMT_CHECK_EQ(plan["data"]["bucket"].get<std::string>(), std::string("工作"));
    }
    // 预检没动数据：文件还在
    const auto still_there = client.Get("/api/file/" + fmt::url_encode(file_id));
    FMT_CHECK(still_there != nullptr && still_there->status == 200);

    // 软删除
    const auto removed = client.Delete("/api/file/" + fmt::url_encode(file_id));
    FMT_CHECK(removed != nullptr);
    if (removed != nullptr) {
        FMT_CHECK_EQ(removed->status, 200);
        FMT_CHECK_EQ(nlohmann::json::parse(removed->body)["data"]["moved_to"]
                         .get<std::string>()
                         .rfind("trash/user/.files/", 0),
                     std::size_t{0});
    }

    const auto after = client.Get("/api/file");
    FMT_CHECK(after != nullptr);
    if (after != nullptr) {
        FMT_CHECK_EQ(nlohmann::json::parse(after->body)["data"]["files"].size(), std::size_t{0});
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
