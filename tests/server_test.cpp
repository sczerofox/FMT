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
    std::string trashed;
    const auto removed = client.Delete("/api/bucket/" + fmt::url_encode("工作"));
    FMT_CHECK(removed != nullptr);
    if (removed != nullptr) {
        FMT_CHECK_EQ(removed->status, 200);
        const nlohmann::json body = nlohmann::json::parse(removed->body);
        FMT_CHECK(body["ok"].get<bool>());
        FMT_CHECK_EQ(body["data"]["moved_to"].get<std::string>().rfind("trash/", 0), std::size_t{0});
        // 回收站里的名字一律带时间戳
        trashed = body["data"]["trashed_name"].get<std::string>();
        FMT_CHECK_EQ(trashed.rfind("工作_", 0), std::size_t{0});
    }

    // 回收站：GET /api/trash
    const auto trash = client.Get("/api/trash");
    FMT_CHECK(trash != nullptr);
    if (trash != nullptr) {
        FMT_CHECK_EQ(trash->status, 200);
        const nlohmann::json body = nlohmann::json::parse(trash->body);
        FMT_CHECK_EQ(body["data"]["deleted_buckets"].size(), std::size_t{1});
        FMT_CHECK_EQ(body["data"]["deleted_buckets"][0].value("original", std::string{}),
                     std::string("工作"));
    }

    // POST /api/trash/<名字>/restore：回退后桶回到 repository
    if (!trashed.empty()) {
        const auto restored =
            client.Post("/api/trash/" + fmt::url_encode(trashed) + "/restore", "", "application/json");
        FMT_CHECK(restored != nullptr);
        if (restored != nullptr) {
            FMT_CHECK_EQ(restored->status, 200);
            const nlohmann::json body = nlohmann::json::parse(restored->body);
            FMT_CHECK_EQ(body["data"]["original"].get<std::string>(), std::string("工作"));
        }
    }
    const auto back = client.Get("/api/bucket/" + fmt::url_encode("工作"));
    FMT_CHECK(back != nullptr);
    if (back != nullptr) {
        FMT_CHECK_EQ(back->status, 200);
    }

    // 再删一次，测 trash get 与永久删除（DELETE 需要显式 force）
    const auto removed_again = client.Delete("/api/bucket/" + fmt::url_encode("工作"));
    FMT_CHECK(removed_again != nullptr);
    std::string second;
    if (removed_again != nullptr && removed_again->status == 200) {
        second = nlohmann::json::parse(removed_again->body)["data"]["trashed_name"]
                     .get<std::string>();
    }
    FMT_CHECK(!second.empty());

    if (!second.empty()) {
        const auto detail = client.Get("/api/trash/" + fmt::url_encode(second));
        FMT_CHECK(detail != nullptr);
        if (detail != nullptr) {
            FMT_CHECK_EQ(detail->status, 200);
            const nlohmann::json body = nlohmann::json::parse(detail->body);
            FMT_CHECK_EQ(body["data"]["original"].get<std::string>(), std::string("工作"));
            FMT_CHECK(body["data"]["present"].get<bool>());
        }

        // 预检：?dry_run=1 要把要永久删掉的东西说清楚，且不改数据
        const auto plan = client.Delete("/api/trash/" + fmt::url_encode(second) + "?dry_run=1");
        FMT_CHECK(plan != nullptr);
        if (plan != nullptr) {
            FMT_CHECK_EQ(plan->status, 200);
            const nlohmann::json body = nlohmann::json::parse(plan->body);
            FMT_CHECK(body["data"]["needs_confirm"].get<bool>());
            FMT_CHECK_EQ(body["data"]["original"].get<std::string>(), std::string("工作"));
            FMT_CHECK(body["data"].contains("files"));
        }

        // 没确认 -> 400 + FMT-016（需要显式确认）
        const auto refused = client.Delete("/api/trash/" + fmt::url_encode(second));
        FMT_CHECK(refused != nullptr);
        if (refused != nullptr) {
            FMT_CHECK_EQ(refused->status, 400);
            FMT_CHECK_EQ(
                nlohmann::json::parse(refused->body)["error"]["code"].get<std::string>(),
                std::string("FMT-016"));
        }

        // ?force=1 -> 真的删掉
        const auto purged = client.Delete("/api/trash/" + fmt::url_encode(second) + "?force=1");
        FMT_CHECK(purged != nullptr);
        if (purged != nullptr) {
            FMT_CHECK_EQ(purged->status, 200);
        }

        const auto empty_trash = client.Get("/api/trash");
        FMT_CHECK(empty_trash != nullptr);
        if (empty_trash != nullptr) {
            FMT_CHECK_EQ(
                nlohmann::json::parse(empty_trash->body)["data"]["deleted_buckets"].size(),
                std::size_t{0});
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
                                   handler)));
    httplib::Client client("127.0.0.1", server.port());

    // 空请求体 -> 400 + FMT-001
    const auto empty_body = client.Post("/api/file", "", "application/json");
    FMT_CHECK(empty_body != nullptr);
    if (empty_body != nullptr) {
        FMT_CHECK_EQ(empty_body->status, 400);
        FMT_CHECK_EQ(nlohmann::json::parse(empty_body->body)["error"]["code"].get<std::string>(),
                     std::string("FMT-001"));
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
