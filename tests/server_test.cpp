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
#include "fmt/share/share.hpp"
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

// 上传上限的取值器：生产里由运行体注入的那个会现读 config，
// 测试里给个固定值就够（0 = 不限）。
fmt::server::UploadLimitProvider fixed_upload_limit(std::uintmax_t bytes) {
    return [bytes]() -> std::uintmax_t { return bytes; };
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

    // 已知模块但没这个接口（走兜底路由）→ **501 Not Implemented**
    //（原来是 500，等于说「服务器坏了」；而这只是「这个接口没做」）
    // 例子要用**真正落兜底**的路径：`/api/server/*`、`/api/preview/*` 没登记过路由。
    // 别拿 `/api/file/list` 当例子——它会被参数路由 `([^/]+)` 接住（`file.get("list")`），
    // 真实 handler 下得到 404 + FMT-002，不是 501。
    const auto pending = client.Get("/api/server/nosuch");
    FMT_CHECK(pending != nullptr);
    if (pending != nullptr) {
        FMT_CHECK_EQ(pending->status, 501);
        const nlohmann::json body = nlohmann::json::parse(pending->body);
        FMT_CHECK(!body["ok"].get<bool>());
        FMT_CHECK_EQ(body["error"]["code"].get<std::string>(), std::string("FMT-602"));
    }

    // 命中**已登记**的路由、但这条路由要业务处理器而这里没有注入 → 同样 501 + FMT-602
    //（「HTTP 未接入业务处理」）。注意这与上面那条走的是**不同分支**：
    // `share` 模块本身早已实现，真实 handler 下 `/api/share/x` 走 `share.get` → 404 + FMT-500。
    const auto share = client.Get("/api/share/x");
    FMT_CHECK(share != nullptr);
    if (share != nullptr) {
        FMT_CHECK_EQ(share->status, 501);
        FMT_CHECK_EQ(nlohmann::json::parse(share->body)["error"]["code"].get<std::string>(),
                     std::string("FMT-602"));
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

FMT_TEST(Server, 管理接口要token且桶写接口未提供) {
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

    // ⑤ 桶的**写**接口在 HTTP 侧故意没有（桶由 CLI 管）：未登记的桶路径回
    //    404「没有这个接口」，而不是 501「还没实现」。
    //    （GET /api/bucket 是只读列表，见「配置桶列表与清空回收站路由」用例）
    {
        httplib::Client client("127.0.0.1", server.port());
        client.set_default_headers({{"X-FMT-Token", kTestToken}});
        const auto response = client.Get("/api/bucket/x");
        FMT_CHECK(response != nullptr);
        if (response != nullptr) {
            FMT_CHECK_EQ(response->status, 404);
            const nlohmann::json body = nlohmann::json::parse(response->body);
            FMT_CHECK_EQ(body["error"]["code"].get<std::string>(), std::string("FMT-017"));
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
                                   handler, test_verifier(), fixed_upload_limit(4096))));
    httplib::Client client("127.0.0.1", server.port());
    client.set_default_headers({{"X-FMT-Token", kTestToken}});

    // 流式上传：请求体就是文件内容（不再要求文件在服务端本地有路径）。
    // 这里上限传 4096，用来验「边写边判」——HTTP 层收满就中止，不等写完。

    // 没给文件名 -> 400 + FMT-100（FileNameEmpty）
    const auto no_name = client.Post("/api/file/upload", "hello", "application/octet-stream");
    FMT_CHECK(no_name != nullptr);
    if (no_name != nullptr) {
        FMT_CHECK_EQ(no_name->status, 400);
        FMT_CHECK_EQ(nlohmann::json::parse(no_name->body)["error"]["code"].get<std::string>(),
                     std::string("FMT-100"));
    }

    // 与 file_id 同形的文件名 -> 400 + FMT-106（不能落到 default 的 500）
    const auto rejected_name =
        client.Post("/api/file/upload?name=fmt-20261008-0", "hello", "application/octet-stream");
    FMT_CHECK(rejected_name != nullptr);
    if (rejected_name != nullptr) {
        FMT_CHECK_EQ(rejected_name->status, 400);
        FMT_CHECK_EQ(
            nlohmann::json::parse(rejected_name->body)["error"]["code"].get<std::string>(),
            std::string("FMT-106"));
    }

    // 超过上限 -> 400 + FMT-303，而且暂存文件必须被清掉（不留碎片）
    const std::string too_big(10 * 1024, 'y');
    const auto over = client.Post("/api/file/upload?name=big.bin", too_big,
                                  "application/octet-stream");
    FMT_CHECK(over != nullptr);
    if (over != nullptr) {
        FMT_CHECK_EQ(over->status, 400);
        FMT_CHECK_EQ(nlohmann::json::parse(over->body)["error"]["code"].get<std::string>(),
                     std::string("FMT-303"));
    }
    {
        const fmt::PathManager paths(root);
        std::size_t leftovers = 0;
        std::error_code code;
        for (const auto& entry : std::filesystem::directory_iterator(paths.temp(), code)) {
            if (fmt::path_to_utf8(entry.path().filename()).rfind("fmt-upload-", 0) == 0) {
                ++leftovers;
            }
        }
        FMT_CHECK_EQ(leftovers, std::size_t{0});
    }

    // 正常上传（100 字节），文件名走 ?name=
    const std::string payload(100, 'x');
    std::string file_id;
    const auto uploaded =
        client.Post("/api/file/upload?name=doc.bin", payload, "application/octet-stream");
    FMT_CHECK(uploaded != nullptr);
    if (uploaded != nullptr) {
        FMT_CHECK_EQ(uploaded->status, 200);
        const nlohmann::json parsed = nlohmann::json::parse(uploaded->body);
        FMT_CHECK(parsed["ok"].get<bool>());
        file_id = parsed["data"]["file_id"].get<std::string>();
        FMT_CHECK_EQ(parsed["data"]["file_name"].get<std::string>(), std::string("doc.bin"));
        FMT_CHECK_EQ(parsed["data"]["size"].get<std::size_t>(), payload.size());
    }
    FMT_CHECK(!file_id.empty());

    // 下载：字节必须与原文件一模一样，并且是 attachment
    const auto downloaded = client.Get("/api/file/" + file_id + "/download");
    FMT_CHECK(downloaded != nullptr);
    if (downloaded != nullptr) {
        FMT_CHECK_EQ(downloaded->status, 200);
        FMT_CHECK(downloaded->body == payload);
        FMT_CHECK(downloaded->get_header_value("Content-Disposition").find("attachment") !=
                  std::string::npos);
    }

    // 预览：.bin 不在可预览类型里 -> 400 + FMT-701
    const auto preview = client.Get("/api/file/" + file_id + "/preview");
    FMT_CHECK(preview != nullptr);
    if (preview != nullptr) {
        FMT_CHECK_EQ(preview->status, 400);
        FMT_CHECK_EQ(nlohmann::json::parse(preview->body)["error"]["code"].get<std::string>(),
                     std::string("FMT-701"));
    }

    // 列表
    const auto listed = client.Get("/api/file");
    FMT_CHECK(listed != nullptr);
    if (listed != nullptr) {
        FMT_CHECK_EQ(listed->status, 200);
        FMT_CHECK_EQ(nlohmann::json::parse(listed->body)["data"]["files"].size(), std::size_t{1});
    }

    // 查询串必须**透到业务层**：?search= / ?sort= / ?page= / ?page_size=
    //（这里曾经只声明不生效：路由忘了把 request 的查询参数放进 args）
    const auto queried = client.Get("/api/file?search=doc&page=1&page_size=1");
    FMT_CHECK(queried != nullptr);
    if (queried != nullptr) {
        FMT_CHECK_EQ(queried->status, 200);
        const nlohmann::json parsed = nlohmann::json::parse(queried->body);
        FMT_CHECK_EQ(parsed["data"].value("search", std::string{}), std::string("doc"));
        FMT_CHECK_EQ(parsed["data"].value("page_size", 0), 1);
        FMT_CHECK_EQ(parsed["data"].value("total", std::size_t{0}), std::size_t{1});
    }

    // 非法的 page_size 要报 FMT-001，不能悄悄当成 0（那就成了"参数随便写都行"）
    const auto bad_page_size = client.Get("/api/file?page_size=abc");
    FMT_CHECK(bad_page_size != nullptr);
    if (bad_page_size != nullptr) {
        FMT_CHECK_EQ(bad_page_size->status, 400);
        FMT_CHECK_EQ(
            nlohmann::json::parse(bad_page_size->body)["error"]["code"].get<std::string>(),
            std::string("FMT-001"));
    }

    // 按 file_id 与按文件名都能查
    for (const std::string& key : {file_id, std::string("doc.bin")}) {
        const auto detail = client.Get("/api/file/" + fmt::url_encode(key));
        FMT_CHECK(detail != nullptr);
        if (detail != nullptr) {
            FMT_CHECK_EQ(detail->status, 200);
            const nlohmann::json data = nlohmann::json::parse(detail->body)["data"];
            FMT_CHECK_EQ(data["file_id"].get<std::string>(), file_id);
            // 预览 / 下载链接（本次改动（未提交））：完整 http:// 地址，路径里是 file_id
            FMT_CHECK_EQ(data.value("preview_url", std::string{}),
                         std::string("http://localhost:4122/api/file/") + file_id + "/preview");
            FMT_CHECK_EQ(data.value("download_url", std::string{}),
                         std::string("http://localhost:4122/api/file/") + file_id + "/download");
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

// multipart/form-data 上传：Postman 的 form-data、浏览器表单、Spring 的 MultipartFile
// 都是这个形状。它与裸字节形态**共用同一条链路**（边收边写 temp/ → 补算 MD5 → 入库），
// 差别只在「字节从哪个 part 里取」。裸字节那条路有自己的用例（上面那个），不在这里重复。
FMT_TEST(Server, 流式上传支持multipart) {
    fmt_test::TempDir temp("server-multipart");
    const auto root = temp / "FMT";

    auto context = fmt::initialize_service_context(root);
    FMT_CHECK(fmt::ok(context));
    fmt::AppContext& app = *std::get<std::unique_ptr<fmt::AppContext>>(context);

    fmt::BucketService buckets(*app.paths, app.config, app.logger.get());
    FMT_CHECK(fmt::ok(buckets.create("工作")));

    auto handler = [&app](const std::string& operation,
                          const nlohmann::json& args) -> fmt::Result<nlohmann::json> {
        return fmt::service::execute_business(app, operation, args);
    };

    // 失败路径一条都不能在 temp/ 里留碎片（fmt- 前缀的那些暂存文件）
    const auto leftovers = [&app]() -> std::size_t {
        std::size_t count = 0;
        std::error_code code;
        for (const auto& entry : std::filesystem::directory_iterator(app.paths->temp(), code)) {
            if (fmt::path_to_utf8(entry.path().filename()).rfind("fmt-upload-", 0) == 0) {
                ++count;
            }
        }
        return count;
    };
    const auto code_of = [](const httplib::Result& response) {
        return nlohmann::json::parse(response->body)["error"]["code"].get<std::string>();
    };

    // 上限 4096：既测正常路径，也测 multipart 里的「边收边判」。
    fmt::server::HttpServer server;
    FMT_CHECK(fmt::ok(server.start("127.0.0.1", 0, fmt::path_to_utf8(root), app.logger.get(),
                                   handler, test_verifier(), fixed_upload_limit(4096))));
    httplib::Client client("127.0.0.1", server.port());
    client.set_default_headers({{"X-FMT-Token", kTestToken}});

    // 正文里故意放 CRLF 和「像 boundary」的内容：part 之间的 framing 一个字节都不该混进来
    const std::string payload_a = "line1\r\nline2\r\n--not-a-boundary\r\n0123456789";
    const std::string payload_b = "B: 普通字段前后的字节都不该进来\r\n";
    const std::string payload_c = "C: ?name= 覆盖 part 的 filename\r\n";
    const std::string payload_d = "D: filename* 的百分号编码名字\r\n";

    // ---- ① Postman 形态：一行 File 字段（part 的 filename 就是落库的名字，中文要能落地）----
    std::string first_id;
    const auto one_file = client.Post("/api/file/upload",
                                      httplib::MultipartFormDataItems{
                                          {"File", payload_a, "照片 v2.jpg", "image/jpeg"}});
    FMT_CHECK(one_file != nullptr);
    if (one_file != nullptr) {
        FMT_CHECK_EQ(one_file->status, 200);
        const nlohmann::json data = nlohmann::json::parse(one_file->body)["data"];
        first_id = data["file_id"].get<std::string>();
        FMT_CHECK_EQ(data["file_name"].get<std::string>(), std::string("照片 v2.jpg"));
        FMT_CHECK_EQ(data["size"].get<std::size_t>(), payload_a.size());
    }
    FMT_CHECK(!first_id.empty());

    // 下载回来逐字节一致：落盘的是**part 正文**，不是整段 multipart 报文
    const auto downloaded = client.Get("/api/file/" + first_id + "/download");
    FMT_CHECK(downloaded != nullptr);
    if (downloaded != nullptr) {
        FMT_CHECK_EQ(downloaded->status, 200);
        FMT_CHECK_EQ(downloaded->body, payload_a);
    }

    // ---- ② 字段混排：文件前后都有普通文本字段——它们读掉，但不落盘 ----
    std::string mixed_id;
    const auto mixed = client.Post("/api/file/upload",
                                   httplib::MultipartFormDataItems{
                                       {"bucket", "工作", "", ""},
                                       {"file", payload_b, "mixed.bin", "application/octet-stream"},
                                       {"note", "这个字段不该进文件", "", ""}});
    FMT_CHECK(mixed != nullptr);
    if (mixed != nullptr) {
        FMT_CHECK_EQ(mixed->status, 200);
        const nlohmann::json data = nlohmann::json::parse(mixed->body)["data"];
        mixed_id = data["file_id"].get<std::string>();
        FMT_CHECK_EQ(data["file_name"].get<std::string>(), std::string("mixed.bin"));
        FMT_CHECK_EQ(data["size"].get<std::size_t>(), payload_b.size());  // 普通字段的字节没算进来
    }
    if (!mixed_id.empty()) {
        const auto body = client.Get("/api/file/" + mixed_id + "/download");
        FMT_CHECK(body != nullptr);
        if (body != nullptr) {
            FMT_CHECK_EQ(body->body, payload_b);  // 一个字节不多、一个字节不少
        }
    }

    // ---- ③ 只有普通字段、没有文件 part -> 400 + FMT-100 ----
    const auto no_file = client.Post("/api/file/upload",
                                     httplib::MultipartFormDataItems{
                                         {"bucket", "工作", "", ""},
                                         {"note", "只有文本", "", ""}});
    FMT_CHECK(no_file != nullptr);
    if (no_file != nullptr) {
        FMT_CHECK_EQ(no_file->status, 400);
        FMT_CHECK_EQ(code_of(no_file), std::string("FMT-100"));
    }
    FMT_CHECK_EQ(leftovers(), std::size_t{0});

    // ---- ④ 文件 part 超过上限 -> 400 + FMT-303（边收边判），暂存文件清掉 ----
    const std::string too_big(10 * 1024, 'y');
    const auto over = client.Post("/api/file/upload",
                                  httplib::MultipartFormDataItems{
                                      {"file", too_big, "big.bin", "application/octet-stream"}});
    FMT_CHECK(over != nullptr);
    if (over != nullptr) {
        FMT_CHECK_EQ(over->status, 400);
        const nlohmann::json error = nlohmann::json::parse(over->body)["error"];
        FMT_CHECK_EQ(error["code"].get<std::string>(), std::string("FMT-303"));
        // 消息来自 **HTTP 层**（"请求体超过上传上限（已收…"）：说明是收的过程中就中止，
        // 不是收完再被业务层拒——那句是"文件超过上传上限（… > … 字节）"
        FMT_CHECK(error["message"].get<std::string>().rfind("请求体超过上传上限", 0) == 0);
    }
    FMT_CHECK_EQ(leftovers(), std::size_t{0});

    // ---- ⑤ 普通字段也算进上限：判的是**收到的字节**，不是只算落盘的 ----
    // 文件 part 只有 10 字节，普通字段 5000 字节，合计 5010 > 4096 → 一样中止
    const auto fields_over = client.Post(
        "/api/file/upload",
        httplib::MultipartFormDataItems{
            {"file", std::string(10, 'z'), "small.bin", "application/octet-stream"},
            {"note", std::string(5000, 'n'), "", ""}});
    FMT_CHECK(fields_over != nullptr);
    if (fields_over != nullptr) {
        FMT_CHECK_EQ(fields_over->status, 400);
        FMT_CHECK_EQ(code_of(fields_over), std::string("FMT-303"));
    }
    FMT_CHECK_EQ(leftovers(), std::size_t{0});

    // ---- ⑥ 两个文件 part -> 400 + FMT-001（一次只收一个，不能让「传了两个只存一个」发生）----
    const auto two_files = client.Post("/api/file/upload",
                                       httplib::MultipartFormDataItems{
                                           {"a", payload_a, "a.bin", "application/octet-stream"},
                                           {"b", payload_b, "b.bin", "application/octet-stream"}});
    FMT_CHECK(two_files != nullptr);
    if (two_files != nullptr) {
        FMT_CHECK_EQ(two_files->status, 400);
        FMT_CHECK_EQ(code_of(two_files), std::string("FMT-001"));
    }
    FMT_CHECK_EQ(leftovers(), std::size_t{0});

    // ---- ⑦ ?name= 优先于 part 的 filename ----
    const auto named = client.Post("/api/file/upload?name=override.bin",
                                   httplib::MultipartFormDataItems{
                                       {"file", payload_c, "part-name.bin", "application/octet-stream"}});
    FMT_CHECK(named != nullptr);
    if (named != nullptr) {
        FMT_CHECK_EQ(named->status, 200);
        FMT_CHECK_EQ(nlohmann::json::parse(named->body)["data"]["file_name"].get<std::string>(),
                     std::string("override.bin"));
    }

    // ---- ⑧ 手工拼的 multipart：filename*=UTF-8''… （Postman / 浏览器给中文名的写法）----
    //     顺便证明这条路不依赖 httplib 客户端的序列化，裸字符串也能走通。
    const std::string boundary = "----FMTBoundary42";
    std::string manual = "--" + boundary + "\r\n";
    manual += "Content-Disposition: form-data; name=\"file\"; "
              "filename*=UTF-8''%E6%8A%A5%E8%A1%A8.txt\r\n";
    manual += "Content-Type: text/plain\r\n\r\n";
    manual += payload_d;
    manual += "\r\n--" + boundary + "--\r\n";
    const auto encoded = client.Post("/api/file/upload", manual,
                                     "multipart/form-data; boundary=" + boundary);
    FMT_CHECK(encoded != nullptr);
    if (encoded != nullptr) {
        FMT_CHECK_EQ(encoded->status, 200);
        FMT_CHECK_EQ(nlohmann::json::parse(encoded->body)["data"]["file_name"].get<std::string>(),
                     std::string("报表.txt"));
    }

    // ---- 成功的四次入库（① ② ⑦ ⑧），失败的四次（③ ④ ⑤ ⑥）一个都没进 ----
    const auto listed = client.Get("/api/file");
    FMT_CHECK(listed != nullptr);
    if (listed != nullptr) {
        FMT_CHECK_EQ(listed->status, 200);
        FMT_CHECK_EQ(nlohmann::json::parse(listed->body)["data"]["files"].size(), std::size_t{4});
    }
    FMT_CHECK_EQ(leftovers(), std::size_t{0});

    server.stop();
}

// 上限是**每次请求现取**的，不是启动时的快照：`config set max_upload_size` 之后
// 不用重启 HTTP 监听就生效（取快照会让「调大上限」当场失效——HTTP 层还按旧的小值
// 中止接收，业务层根本没机会看到新值）。
FMT_TEST(Server, 上传上限每次请求现取) {
    fmt_test::TempDir temp("server-live-limit");
    const auto root = temp / "FMT";

    auto context = fmt::initialize_service_context(root);
    FMT_CHECK(fmt::ok(context));
    fmt::AppContext& app = *std::get<std::unique_ptr<fmt::AppContext>>(context);

    fmt::BucketService buckets(*app.paths, app.config, app.logger.get());
    FMT_CHECK(fmt::ok(buckets.create("工作")));

    auto handler = [&app](const std::string& operation,
                          const nlohmann::json& args) -> fmt::Result<nlohmann::json> {
        return fmt::service::execute_business(app, operation, args);
    };

    // 这个变量扮演「服务里那份 config.max_upload_size」：测试直接改它，等价于 config set
    std::uintmax_t limit = 64;
    fmt::server::HttpServer server;
    FMT_CHECK(fmt::ok(server.start("127.0.0.1", 0, fmt::path_to_utf8(root), app.logger.get(),
                                   handler, test_verifier(),
                                   [&limit]() -> std::uintmax_t { return limit; })));
    httplib::Client client("127.0.0.1", server.port());
    client.set_default_headers({{"X-FMT-Token", kTestToken}});

    const std::string small_body = "0123456789";  // 10 字节
    const std::string big_body(100, 'x');         // 100 字节

    // ① 100 > 64：HTTP 层在收的过程中就中止（消息是 HTTP 层那句）
    const auto refused =
        client.Post("/api/file/upload?name=a.bin", big_body, "application/octet-stream");
    FMT_CHECK(refused != nullptr);
    if (refused != nullptr) {
        FMT_CHECK_EQ(refused->status, 400);
        const nlohmann::json error = nlohmann::json::parse(refused->body)["error"];
        FMT_CHECK_EQ(error["code"].get<std::string>(), std::string("FMT-303"));
        FMT_CHECK(error["message"].get<std::string>().rfind("请求体超过上传上限", 0) == 0);
    }

    // ② 调大上限：**同一个服务实例、不重启**，下一次请求立刻能过
    limit = 4096;
    const auto allowed =
        client.Post("/api/file/upload?name=b.bin", big_body, "application/octet-stream");
    FMT_CHECK(allowed != nullptr);
    if (allowed != nullptr) {
        FMT_CHECK_EQ(allowed->status, 200);
    }

    // ③ 再调小：下一次请求立刻又是 400（而不是沿用上一次取到的 4096）
    limit = 8;
    const auto tiny =
        client.Post("/api/file/upload?name=c.bin", small_body, "application/octet-stream");
    FMT_CHECK(tiny != nullptr);
    if (tiny != nullptr) {
        FMT_CHECK_EQ(tiny->status, 400);
        FMT_CHECK_EQ(nlohmann::json::parse(tiny->body)["error"]["code"].get<std::string>(),
                     std::string("FMT-303"));
    }

    server.stop();
}

FMT_TEST(Server, 配置桶列表与清空回收站路由) {
    fmt_test::TempDir temp("server-new-routes");
    const auto root = temp / "FMT";

    auto context = fmt::initialize_service_context(root);
    FMT_CHECK(fmt::ok(context));
    fmt::AppContext& app = *std::get<std::unique_ptr<fmt::AppContext>>(context);

    fmt::BucketService buckets(*app.paths, app.config, app.logger.get());
    FMT_CHECK(fmt::ok(buckets.create("工作")));
    FMT_CHECK(fmt::ok(buckets.create("私人")));

    // 造一条回收站条目：先入库、再软删。HTTP 这一步只测「清空」，
    // 上传与软删各自有自己的用例，不在这里重复。
    const auto source = temp / "old.txt";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(source, "0123456789")));
    auto prepared = fmt::prepare_staged_upload(*app.paths, source, "old.txt",
                                               app.config.max_upload_size, app.logger.get());
    FMT_CHECK(fmt::ok(prepared));
    fmt::PreparedUpload upload = std::get<fmt::PreparedUpload>(prepared);
    fmt::FileService files(*app.paths, app.config, app.logger.get());
    auto stored = files.commit_upload(upload);
    FMT_CHECK(fmt::ok(stored));
    FMT_CHECK(fmt::ok(files.remove(std::get<fmt::FileRecord>(stored).file_id)));

    auto handler = [&app](const std::string& operation,
                          const nlohmann::json& args) -> fmt::Result<nlohmann::json> {
        return fmt::service::execute_business(app, operation, args);
    };

    fmt::server::HttpServer server;
    FMT_CHECK(fmt::ok(server.start("127.0.0.1", 0, fmt::path_to_utf8(root), app.logger.get(),
                                   handler, test_verifier())));
    httplib::Client client("127.0.0.1", server.port());
    client.set_default_headers({{"X-FMT-Token", kTestToken}});

    // ---- GET /api/config：只读，而且**任何情况下都不返回 token** ----
    {
        const auto response = client.Get("/api/config");
        FMT_CHECK(response != nullptr);
        if (response != nullptr) {
            FMT_CHECK_EQ(response->status, 200);
            const nlohmann::json body = nlohmann::json::parse(response->body);
            FMT_CHECK(body["ok"].get<bool>());
            FMT_CHECK(!body["data"].contains("token"));
            FMT_CHECK_EQ(body["data"]["current_bucket"].get<std::string>(), std::string("工作"));
            FMT_CHECK(body["data"].contains("max_upload_size"));
        }

        // **连开关都不提供**：就算调用方把老参数塞进来，token 也一个字节都不给
        // （HTTP 响应体会被日志/代理/工单带走，凭证不该多一份副本）
        const auto with_token = client.Get("/api/config?include_token=1");
        FMT_CHECK(with_token != nullptr);
        if (with_token != nullptr) {
            FMT_CHECK_EQ(with_token->status, 200);
            const nlohmann::json body = nlohmann::json::parse(with_token->body);
            FMT_CHECK(!body["data"].contains("token"));
        }

        // 它照样是要认证的接口
        httplib::Client anonymous("127.0.0.1", server.port());
        const auto denied = anonymous.Get("/api/config");
        FMT_CHECK(denied != nullptr && denied->status == 401);
    }

    // ---- GET /api/bucket：只读列表（当前 Bucket 也在里面） ----
    {
        const auto response = client.Get("/api/bucket");
        FMT_CHECK(response != nullptr);
        if (response != nullptr) {
            FMT_CHECK_EQ(response->status, 200);
            const nlohmann::json body = nlohmann::json::parse(response->body);
            FMT_CHECK_EQ(body["data"]["count"].get<std::size_t>(), std::size_t{2});
            FMT_CHECK_EQ(body["data"]["current_bucket"].get<std::string>(), std::string("工作"));
            bool found_current = false;
            for (const auto& entry : body["data"]["buckets"]) {
                if (entry["name"] == "工作" && entry["is_current"].get<bool>()) {
                    found_current = true;
                }
            }
            FMT_CHECK(found_current);
        }

        // 写接口故意不提供：GET 走兜底路由 → 404 + FMT-017
        const auto unknown = client.Get("/api/bucket/x");
        FMT_CHECK(unknown != nullptr);
        if (unknown != nullptr) {
            FMT_CHECK_EQ(unknown->status, 404);
            FMT_CHECK_EQ(nlohmann::json::parse(unknown->body)["error"]["code"].get<std::string>(),
                         std::string("FMT-017"));
        }
        // 非 GET 没有兜底路由，落到通用错误处理器 → 404 + FMT-002（同样是"没这个接口"）
        const auto create = client.Post("/api/bucket", "{}", "application/json");
        FMT_CHECK(create != nullptr);
        if (create != nullptr) {
            FMT_CHECK_EQ(create->status, 404);
            FMT_CHECK_EQ(nlohmann::json::parse(create->body)["error"]["code"].get<std::string>(),
                         std::string("FMT-002"));
        }
    }

    // ---- DELETE /api/trash/empty ----
    {
        // ① 有内容又没确认 → 400 + FMT-016。
        //    这一条同时钉住路由顺序：如果 /api/trash/empty 注册在 /api/trash/([^/]+) 之后，
        //    "empty" 会被当成条目名走 trash.delete，这里就会变成 FMT-400。
        const auto refused = client.Delete("/api/trash/empty");
        FMT_CHECK(refused != nullptr);
        if (refused != nullptr) {
            FMT_CHECK_EQ(refused->status, 400);
            FMT_CHECK_EQ(nlohmann::json::parse(refused->body)["error"]["code"].get<std::string>(),
                         std::string("FMT-016"));
        }

        // ② 预检：说清楚有几项、多少字节，零副作用
        const auto plan = client.Delete("/api/trash/empty?dry_run=1");
        FMT_CHECK(plan != nullptr);
        if (plan != nullptr) {
            FMT_CHECK_EQ(plan->status, 200);
            const nlohmann::json data = nlohmann::json::parse(plan->body)["data"];
            FMT_CHECK(data["needs_confirm"].get<bool>());
            FMT_CHECK_EQ(data["files"].get<std::size_t>(), std::size_t{1});
        }
        {
            const auto still_there = client.Get("/api/trash");
            FMT_CHECK(still_there != nullptr);
            if (still_there != nullptr) {
                FMT_CHECK_EQ(nlohmann::json::parse(still_there->body)["data"]["entries"].size(),
                             std::size_t{1});
            }
        }

        // ③ 确认后真删（不可恢复）
        const auto emptied = client.Delete("/api/trash/empty?force=1");
        FMT_CHECK(emptied != nullptr);
        if (emptied != nullptr) {
            FMT_CHECK_EQ(emptied->status, 200);
            const nlohmann::json data = nlohmann::json::parse(emptied->body)["data"];
            FMT_CHECK_EQ(data["files"].get<std::size_t>(), std::size_t{1});
            FMT_CHECK(!data["message"].get<std::string>().empty());
        }
        {
            const auto after = client.Get("/api/trash");
            FMT_CHECK(after != nullptr);
            if (after != nullptr) {
                FMT_CHECK_EQ(nlohmann::json::parse(after->body)["data"]["entries"].size(),
                             std::size_t{0});
            }
        }

        // ④ 空回收站再清一次：不为一个空操作要确认，直接成功
        const auto again = client.Delete("/api/trash/empty");
        FMT_CHECK(again != nullptr);
        if (again != nullptr) {
            FMT_CHECK_EQ(again->status, 200);
            FMT_CHECK_EQ(nlohmann::json::parse(again->body)["data"]["files"].get<std::size_t>(),
                         std::size_t{0});
        }
    }

    server.stop();
}

FMT_TEST(Server, 分享路由与公开下载) {
    fmt_test::TempDir temp("server-share");
    const auto root = temp / "FMT";

    auto context = fmt::initialize_service_context(root);
    FMT_CHECK(fmt::ok(context));
    fmt::AppContext& app = *std::get<std::unique_ptr<fmt::AppContext>>(context);

    fmt::BucketService buckets(*app.paths, app.config, app.logger.get());
    FMT_CHECK(fmt::ok(buckets.create("工作")));

    // 文件走业务层入库：这条用例只测**分享**路由，上传有自己的用例。
    const auto source = temp / "share.txt";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(source, "shared bytes 0123456789")));
    auto prepared = fmt::prepare_staged_upload(*app.paths, source, "share.txt",
                                               app.config.max_upload_size, app.logger.get());
    FMT_CHECK(fmt::ok(prepared));
    fmt::PreparedUpload upload = std::get<fmt::PreparedUpload>(prepared);
    fmt::FileService files(*app.paths, app.config, app.logger.get());
    auto stored = files.commit_upload(upload);
    FMT_CHECK(fmt::ok(stored));
    const std::string file_id = std::get<fmt::FileRecord>(stored).file_id;
    FMT_CHECK(!file_id.empty());

    auto handler = [&app](const std::string& operation,
                          const nlohmann::json& args) -> fmt::Result<nlohmann::json> {
        return fmt::service::execute_business(app, operation, args);
    };

    fmt::server::HttpServer server;
    FMT_CHECK(fmt::ok(server.start("127.0.0.1", 0, fmt::path_to_utf8(root), app.logger.get(),
                                   handler, test_verifier())));
    httplib::Client client("127.0.0.1", server.port());
    client.set_default_headers({{"X-FMT-Token", kTestToken}});

    // ① POST /api/share：请求体 {"file_id":…} → 12 位 share_id
    std::string share_id;
    {
        const nlohmann::json body{{"file_id", file_id}};
        const auto response = client.Post("/api/share", body.dump(), "application/json");
        FMT_CHECK(response != nullptr);
        if (response != nullptr) {
            FMT_CHECK_EQ(response->status, 200);
            const nlohmann::json parsed = nlohmann::json::parse(response->body);
            FMT_CHECK(parsed["ok"].get<bool>());
            share_id = parsed["data"].value("share_id", std::string{});
            FMT_CHECK_EQ(parsed["data"].value("max_download_count", 0),
                         static_cast<int>(fmt::kDefaultShareDownloads));
        }
        FMT_CHECK_EQ(share_id.size(), std::size_t{12});
    }

    // ② GET /api/share?file_id=…：列出这个文件的分享
    {
        const auto response = client.Get("/api/share?file_id=" + file_id);
        FMT_CHECK(response != nullptr);
        if (response != nullptr) {
            FMT_CHECK_EQ(response->status, 200);
            const nlohmann::json parsed = nlohmann::json::parse(response->body);
            FMT_CHECK_EQ(parsed["data"]["count"].get<std::size_t>(), std::size_t{1});
            FMT_CHECK_EQ(parsed["data"]["shares"][0].value("share_id", std::string{}), share_id);
        }
    }

    // ③ GET /api/share/<id>：如实报「可用」
    {
        const auto response = client.Get("/api/share/" + share_id);
        FMT_CHECK(response != nullptr);
        if (response != nullptr) {
            FMT_CHECK_EQ(response->status, 200);
            const nlohmann::json data = nlohmann::json::parse(response->body)["data"];
            FMT_CHECK(data.value("available", false));
            FMT_CHECK_EQ(data.value("state", std::string{}), std::string("可用"));
            FMT_CHECK_EQ(data.value("file_name", std::string{}), std::string("share.txt"));
            FMT_CHECK_EQ(data.value("download_count", -1), 0);
        }
    }

    // ④ GET /api/share/<id>/download：**公开**（不带 token）→ 200 + 逐字节一致
    {
        httplib::Client anonymous("127.0.0.1", server.port());
        const auto response = anonymous.Get("/api/share/" + share_id + "/download");
        FMT_CHECK(response != nullptr);
        if (response != nullptr) {
            FMT_CHECK_EQ(response->status, 200);
            FMT_CHECK_EQ(response->body, std::string("shared bytes 0123456789"));
            FMT_CHECK(response->get_header_value("Content-Disposition").find("attachment") !=
                      std::string::npos);
        }
    }

    // ⑤ 先记账再放行：下过一次，计数就是 1
    {
        const auto response = client.Get("/api/share/" + share_id);
        FMT_CHECK(response != nullptr);
        if (response != nullptr) {
            FMT_CHECK_EQ(response->status, 200);
            FMT_CHECK_EQ(nlohmann::json::parse(response->body)["data"].value("download_count", -1), 1);
        }
    }

    // ⑥ DELETE /api/share/<id> 撤销 → 之后 get 是 FMT-500（404）
    {
        const auto revoked = client.Delete("/api/share/" + share_id);
        FMT_CHECK(revoked != nullptr);
        if (revoked != nullptr) {
            FMT_CHECK_EQ(revoked->status, 200);
        }
        const auto after = client.Get("/api/share/" + share_id);
        FMT_CHECK(after != nullptr);
        if (after != nullptr) {
            FMT_CHECK_EQ(after->status, 404);
            FMT_CHECK_EQ(nlohmann::json::parse(after->body)["error"]["code"].get<std::string>(),
                         std::string("FMT-500"));
        }
    }

    // ⑦ 未知 share_id 的公开下载：不要 token，但仍是 FMT-500（404）——不是 401
    {
        httplib::Client anonymous("127.0.0.1", server.port());
        const auto response = anonymous.Get("/api/share/0123456789ab/download");
        FMT_CHECK(response != nullptr);
        if (response != nullptr) {
            FMT_CHECK_EQ(response->status, 404);
            FMT_CHECK_EQ(nlohmann::json::parse(response->body)["error"]["code"].get<std::string>(),
                         std::string("FMT-500"));
        }
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
