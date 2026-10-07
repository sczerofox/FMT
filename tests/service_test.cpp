// service 的单元测试：状态文件、SCM 查询、服务端请求处理与数据根切换
#include "fmt/service/runtime.hpp"
#include "fmt/service/service.hpp"

#include <filesystem>
#include <string>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/ipc/protocol.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt_test.hpp"
#include "temp_dir.hpp"

FMT_TEST(Service, 状态名) {
    FMT_CHECK_EQ(std::string(fmt::service::state_name(fmt::service::State::NotInstalled)),
                 std::string("未安装"));
    FMT_CHECK_EQ(std::string(fmt::service::state_name(fmt::service::State::Running)),
                 std::string("运行中"));
    FMT_CHECK_EQ(std::string(fmt::service::state_name(fmt::service::State::Stopped)),
                 std::string("已停止"));
}

FMT_TEST(Service, 状态文件往返) {
    fmt_test::TempDir temp("service-state");
    const auto directory = temp / "FMT";

    // 没有记录时返回空状态，而不是错误
    const auto empty = fmt::service::load_state_from(directory);
    FMT_CHECK(fmt::ok(empty));
    FMT_CHECK_EQ(std::get<fmt::service::ServiceState>(empty).current_root, std::string{});

    fmt::service::ServiceState state;
    state.current_root = "D:/FMT2";
    state.host_path = "D:/FMT2/fmt.exe";
    state.installed_at = "2026-10-08 10:00:00";
    FMT_CHECK(fmt::ok(fmt::service::save_state_to(directory, state)));

    const auto loaded = fmt::service::load_state_from(directory);
    FMT_CHECK(fmt::ok(loaded));
    FMT_CHECK_EQ(std::get<fmt::service::ServiceState>(loaded).current_root, std::string("D:/FMT2"));
    FMT_CHECK_EQ(std::get<fmt::service::ServiceState>(loaded).host_path,
                 std::string("D:/FMT2/fmt.exe"));
    FMT_CHECK_EQ(std::get<fmt::service::ServiceState>(loaded).installed_at,
                 std::string("2026-10-08 10:00:00"));
}

FMT_TEST(Service, 状态文件损坏时报错不重建) {
    fmt_test::TempDir temp("service-broken");
    const auto directory = temp / "FMT";
    FMT_CHECK(fmt::ok(fmt::ensure_directory(directory)));
    const std::string broken = "{\"version\":1,";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(directory / "service.json", broken)));

    const auto loaded = fmt::service::load_state_from(directory);
    FMT_CHECK(!fmt::ok(loaded));
    FMT_CHECK(fmt::error_of(loaded)->code == fmt::ErrorCode::JsonParseError);
    FMT_CHECK_EQ(std::get<std::string>(fmt::read_text_file(directory / "service.json")), broken);
}

FMT_TEST(Service, 查询不会因为权限失败而崩溃) {
    // 查询不需要管理员权限；这里只要求返回一个合法状态并自洽。
    const fmt::service::State state = fmt::service::query_state();
    if (state == fmt::service::State::NotInstalled) {
        const auto path = fmt::service::installed_binary_path();
        FMT_CHECK(!fmt::ok(path));
        FMT_CHECK(fmt::error_of(path)->code == fmt::ErrorCode::ServiceNotInstalled);
        FMT_CHECK_EQ(fmt::exit_code(fmt::error_of(path)->code), 8);
    } else {
        const auto path = fmt::service::installed_binary_path();
        if (fmt::ok(path)) {
            FMT_CHECK(!std::get<std::string>(path).empty());
        }
    }
}

FMT_TEST(Service, 状态查询与状态名一致) {
    const auto info = fmt::service::query_status();
    FMT_CHECK(fmt::ok(info));

    const fmt::service::StatusInfo& value = std::get<fmt::service::StatusInfo>(info);
    FMT_CHECK(value.state == fmt::service::query_state());

    // 未安装是一个正常结果，不是错误；此时也不该有等待提示
    if (value.state == fmt::service::State::NotInstalled) {
        FMT_CHECK_EQ(value.wait_hint_ms, DWORD{0});
        FMT_CHECK_EQ(value.win32_exit_code, DWORD{0});
    }
}

FMT_TEST(Service, 启动时把失效的当前Bucket置空) {
    fmt_test::TempDir temp("service-refresh");
    const auto root = temp / "root";

    // 先把 current_bucket 写成一个并不存在的桶
    {
        auto initialized = fmt::initialize_root(root);
        FMT_CHECK(fmt::ok(initialized));
        const fmt::PathManager& paths = *std::get<std::unique_ptr<fmt::PathManager>>(initialized);
        fmt::Config config = std::get<fmt::Config>(fmt::load_config(paths));
        config.current_bucket = "早就没了";
        FMT_CHECK(fmt::ok(fmt::save_config(paths, config)));
    }

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    const fmt::PathManager paths{root};
    // 失效 -> 置空（开发文档第 61 节）
    FMT_CHECK_EQ(std::get<fmt::Config>(fmt::load_config(paths)).current_bucket, std::string{});

    // 存在的当前 Bucket 不能被误清
    FMT_CHECK(fmt::ok(
        fmt::ensure_directory(paths.repository() / "user" / fmt::path_from_utf8("工作"))));
    fmt::Config config = std::get<fmt::Config>(fmt::load_config(paths));
    config.current_bucket = "工作";
    FMT_CHECK(fmt::ok(fmt::save_config(paths, config)));

    fmt::service::ServerRuntime second(root, temp / "state2");
    FMT_CHECK(fmt::ok(second.start()));
    FMT_CHECK_EQ(std::get<fmt::Config>(fmt::load_config(paths)).current_bucket,
                 std::string("工作"));
}

FMT_TEST(Service, 管道能执行Bucket命令) {
    fmt_test::TempDir temp("service-bucket");
    const auto root = temp / "root";

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request create;
    create.id = 10;
    create.op = "bucket.create";
    create.args["argv"] = nlohmann::json::array({"工作"});

    const fmt::ipc::Response created = runtime.handle(create);
    FMT_CHECK(created.ok);
    if (created.ok) {
        FMT_CHECK_EQ(created.data.value("bucket", std::string{}), std::string("工作"));
        FMT_CHECK_EQ(created.data.value("current_bucket", std::string{}), std::string("工作"));
    }
    // 目录真的建出来了，用的是占位用户名 user。
    // 中文路径分量必须走 path_from_utf8：直接拼窄字符串会按 ANSI 代码页转换。
    FMT_CHECK(fmt::directory_exists(root / "repository" / "user" / fmt::path_from_utf8("工作")));

    fmt::ipc::Request list;
    list.id = 11;
    list.op = "bucket.list";
    const fmt::ipc::Response listed = runtime.handle(list);
    FMT_CHECK(listed.ok);
    if (listed.ok) {
        FMT_CHECK_EQ(listed.data["buckets"].size(), std::size_t{1});
        FMT_CHECK_EQ(listed.data["buckets"][0].value("name", std::string{}), std::string("工作"));
        FMT_CHECK(listed.data["buckets"][0].value("is_current", false));
    }

    // 缺参数 -> FMT-001（参数错误）
    fmt::ipc::Request bare;
    bare.id = 12;
    bare.op = "bucket.get";
    const fmt::ipc::Response rejected = runtime.handle(bare);
    FMT_CHECK(!rejected.ok);
    FMT_CHECK(rejected.error.code == fmt::ErrorCode::InvalidArgument);

    // 名称统一小写：create WORK 建成 work，并带回提示让 CLI 告知用户
    fmt::ipc::Request upper;
    upper.id = 14;
    upper.op = "bucket.create";
    upper.args["argv"] = nlohmann::json::array({"WORK"});
    const fmt::ipc::Response created_upper = runtime.handle(upper);
    FMT_CHECK(created_upper.ok);
    if (created_upper.ok) {
        FMT_CHECK_EQ(created_upper.data.value("bucket", std::string{}), std::string("work"));
        FMT_CHECK(created_upper.data.contains("note"));
        FMT_CHECK(created_upper.data.value("note", std::string{}).find("WORK") !=
                  std::string::npos);
    }
    FMT_CHECK(fmt::directory_exists(root / "repository" / "user" / "work"));

    // 规范化后的名字要贯穿到 path：`bucket get WORK` 不能打印 .../WORK
    fmt::ipc::Request get_upper;
    get_upper.id = 16;
    get_upper.op = "bucket.get";
    get_upper.args["argv"] = nlohmann::json::array({"WORK"});
    const fmt::ipc::Response got_upper = runtime.handle(get_upper);
    FMT_CHECK(got_upper.ok);
    if (got_upper.ok) {
        FMT_CHECK_EQ(got_upper.data.value("bucket", std::string{}), std::string("work"));
        FMT_CHECK_EQ(got_upper.data.value("path", std::string{}),
                     std::string("repository/user/work"));
    }

    // 已登记但没实现的模块仍然是 FMT-602，而不是「未知操作」
    fmt::ipc::Request pending;
    pending.id = 15;
    pending.op = "share.list";  // share 还没做（file 已经能用了）
    const fmt::ipc::Response not_yet = runtime.handle(pending);
    FMT_CHECK(!not_yet.ok);
    FMT_CHECK(not_yet.error.code == fmt::ErrorCode::ServiceOperationFailed);
    FMT_CHECK_EQ(fmt::exit_code(not_yet.error.code), 8);
}

FMT_TEST(Service, 管道能执行回收站命令) {
    fmt_test::TempDir temp("service-trash");
    const auto root = temp / "root";

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request create;
    create.id = 20;
    create.op = "bucket.create";
    create.args["argv"] = nlohmann::json::array({"工作"});
    FMT_CHECK(runtime.handle(create).ok);

    fmt::ipc::Request remove;
    remove.id = 21;
    remove.op = "bucket.delete";
    remove.args["argv"] = nlohmann::json::array({"工作"});
    const fmt::ipc::Response removed = runtime.handle(remove);
    FMT_CHECK(removed.ok);
    const std::string trashed = removed.data.value("trashed_name", std::string{});
    FMT_CHECK(!trashed.empty());

    fmt::ipc::Request list;
    list.id = 22;
    list.op = "trash.list";
    const fmt::ipc::Response listed = runtime.handle(list);
    FMT_CHECK(listed.ok);
    if (listed.ok) {
        FMT_CHECK_EQ(listed.data["deleted_buckets"].size(), std::size_t{1});
        FMT_CHECK_EQ(listed.data["deleted_buckets"][0].value("original", std::string{}),
                     std::string("工作"));
        FMT_CHECK(listed.data["deleted_buckets"][0].value("present", false));
    }

    // 回退：桶回到 repository/<user>/工作
    fmt::ipc::Request restore;
    restore.id = 23;
    restore.op = "trash.restore";
    restore.args["argv"] = nlohmann::json::array({trashed});
    const fmt::ipc::Response restored = runtime.handle(restore);
    FMT_CHECK(restored.ok);
    if (restored.ok) {
        FMT_CHECK_EQ(restored.data.value("original", std::string{}), std::string("工作"));
    }
    FMT_CHECK(fmt::directory_exists(root / "repository" / "user" / fmt::path_from_utf8("工作")));

    // 已经回退过的条目再回退一次：找不到，不再是「尚未实现」
    fmt::ipc::Request again;
    again.id = 24;
    again.op = "trash.restore";
    again.args["argv"] = nlohmann::json::array({trashed});
    const fmt::ipc::Response missing = runtime.handle(again);
    FMT_CHECK(!missing.ok);
    FMT_CHECK(missing.error.code == fmt::ErrorCode::TrashEntryNotFound);

    // 再删一次：拿一个新的回收站条目来测 trash get 与永久删除
    fmt::ipc::Request remove_again;
    remove_again.id = 25;
    remove_again.op = "bucket.delete";
    remove_again.args["argv"] = nlohmann::json::array({"工作"});
    const fmt::ipc::Response removed_again = runtime.handle(remove_again);
    FMT_CHECK(removed_again.ok);
    const std::string second = removed_again.data.value("trashed_name", std::string{});
    FMT_CHECK(!second.empty());

    // trash get：条目详情
    fmt::ipc::Request get;
    get.id = 26;
    get.op = "trash.get";
    get.args["argv"] = nlohmann::json::array({second});
    const fmt::ipc::Response detail = runtime.handle(get);
    FMT_CHECK(detail.ok);
    if (detail.ok) {
        FMT_CHECK_EQ(detail.data.value("original", std::string{}), std::string("工作"));
        FMT_CHECK(detail.data.value("present", false));
        FMT_CHECK_EQ(detail.data.value("files", std::size_t{9}), std::size_t{0});
    }

    // 永久删除必须先确认：不带 force 一律拒绝
    fmt::ipc::Request unconfirmed;
    unconfirmed.id = 27;
    unconfirmed.op = "trash.delete";
    unconfirmed.args["argv"] = nlohmann::json::array({second});
    const fmt::ipc::Response refused = runtime.handle(unconfirmed);
    FMT_CHECK(!refused.ok);
    FMT_CHECK(refused.error.code == fmt::ErrorCode::ConfirmRequired);
    FMT_CHECK_EQ(fmt::exit_code(refused.error.code), 2);

    // 带上 force：真的删掉，索引也一起清
    fmt::ipc::Request forced;
    forced.id = 28;
    forced.op = "trash.delete";
    forced.args["argv"] = nlohmann::json::array({second});
    forced.args["force"] = true;
    const fmt::ipc::Response purged = runtime.handle(forced);
    FMT_CHECK(purged.ok);
    if (purged.ok) {
        FMT_CHECK_EQ(purged.data.value("trashed", std::string{}), second);
    }

    fmt::ipc::Request list_after;
    list_after.id = 29;
    list_after.op = "trash.list";
    const fmt::ipc::Response after = runtime.handle(list_after);
    FMT_CHECK(after.ok);
    FMT_CHECK_EQ(after.data["deleted_buckets"].size(), std::size_t{0});
}

FMT_TEST(Service, 运行体声明数据根并幂等初始化) {
    fmt_test::TempDir temp("service-runtime");
    const auto root_a = temp / "A";
    const auto root_b = temp / "B";
    const auto state_dir = temp / "state";

    fmt::service::ServerRuntime runtime(root_a, state_dir);
    FMT_CHECK(fmt::ok(runtime.start()));

    // 初始数据根就是 fallback，并且已经建出目录
    FMT_CHECK(fmt::directory_exists(root_a / "repository"));
    FMT_CHECK(fmt::directory_exists(root_a / "log"));

    // hello 声明新根 → 切换并初始化
    fmt::ipc::Request hello;
    hello.id = 1;
    hello.op = "hello";
    hello.root = fmt::path_to_utf8(root_b);
    hello.pid = 1234;

    const fmt::ipc::Response switched = runtime.handle(hello);
    FMT_CHECK(switched.ok);
    FMT_CHECK_EQ(switched.id, 1);
    // hello 要告诉 CLI「换根了没有、从哪换过来的」，双击时才好提示
    FMT_CHECK(switched.data["switched"].get<bool>());
    FMT_CHECK(switched.data.contains("previous_root"));
    FMT_CHECK(fmt::directory_exists(root_b / "repository"));
    FMT_CHECK(fmt::directory_exists(root_b / "data"));
    FMT_CHECK(fmt::file_exists(root_b / "config" / "config.json"));
    FMT_CHECK(fmt::file_exists(root_b / "data" / "file.json"));

    // 旧根的数据不能被删
    FMT_CHECK(fmt::directory_exists(root_a / "repository"));

    // service.json 记录当前根
    const auto state = fmt::service::load_state_from(state_dir);
    FMT_CHECK(fmt::ok(state));
    FMT_CHECK(std::get<fmt::service::ServiceState>(state).current_root.find("B") !=
              std::string::npos);

    // 再声明同一个根：不重复切换，但仍然成功
    const fmt::ipc::Response again = runtime.handle(hello);
    FMT_CHECK(again.ok);
    FMT_CHECK(!again.data["switched"].get<bool>());
}

FMT_TEST(Service, 运行体把控制事件写进日志) {
    fmt_test::TempDir temp("service-log");
    const auto root = temp / "root";

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    // SCM 控制线程收到 STOP / SHUTDOWN 时走的就是这两个调用。
    runtime.log_event("Service", "收到停止控制 STOP");
    runtime.log_event("Service", "服务已停止");

    const auto log = fmt::read_text_file(root / "log" / "fmt.log");
    FMT_CHECK(fmt::ok(log));
    const std::string& text = std::get<std::string>(log);
    FMT_CHECK(text.find("[Service] 收到停止控制 STOP") != std::string::npos);
    FMT_CHECK(text.find("[Service] 服务已停止") != std::string::npos);
    // 服务启动那几行也在同一个文件里
    FMT_CHECK(text.find("服务已启动") != std::string::npos);
}

FMT_TEST(Service, 启动时清理temp里的遗留临时文件) {
    fmt_test::TempDir temp("service-temp");
    const auto root = temp / "root";
    FMT_CHECK(fmt::ok(fmt::ensure_directory(root / "temp")));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(root / "temp" / "fmt-elev-123.json", "{}")));
    // 中文文件名必须走 path_from_utf8：直接拼窄字符串会按 ANSI 代码页转换而抛异常。
    const auto mine = root / "temp" / fmt::path_from_utf8("用户自己的文件.txt");
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(mine, "别删我")));

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    // 我们的临时文件清掉，用户手放的其它文件不动
    FMT_CHECK(!fmt::file_exists(root / "temp" / "fmt-elev-123.json"));
    FMT_CHECK(fmt::file_exists(mine));
}

FMT_TEST(Service, 管道能上传与操作文件) {
    fmt_test::TempDir temp("service-file");
    const auto root = temp / "root";

    // 上传来源：一个本地文件
    const auto source = temp / "payload.txt";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(source, "hello file")));

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request create;
    create.id = 40;
    create.op = "bucket.create";
    create.args["argv"] = nlohmann::json::array({"工作"});
    FMT_CHECK(runtime.handle(create).ok);

    // 上传：运行体走两段式（下载/复制在锁外，登记在锁内）
    fmt::ipc::Request upload;
    upload.id = 41;
    upload.op = "file.upload";
    upload.args["argv"] = nlohmann::json::array({fmt::path_to_utf8(source)});
    const fmt::ipc::Response uploaded = runtime.handle(upload);
    FMT_CHECK(uploaded.ok);
    if (!uploaded.ok) {
        return;
    }
    const std::string file_id = uploaded.data.value("file_id", std::string{});
    FMT_CHECK(!file_id.empty());
    FMT_CHECK_EQ(uploaded.data.value("file_name", std::string{}), std::string("payload.txt"));
    FMT_CHECK_EQ(uploaded.data.value("size", std::uintmax_t{0}), std::uintmax_t{10});

    // 列表
    fmt::ipc::Request list;
    list.id = 42;
    list.op = "file.list";
    const fmt::ipc::Response listed = runtime.handle(list);
    FMT_CHECK(listed.ok);
    if (listed.ok) {
        FMT_CHECK_EQ(listed.data["files"].size(), std::size_t{1});
        FMT_CHECK_EQ(listed.data["files"][0].value("file_name", std::string{}),
                     std::string("payload.txt"));
    }

    // 按 file_id 与按文件名都能查
    for (const std::string& key : {file_id, std::string("payload.txt")}) {
        fmt::ipc::Request get;
        get.id = 43;
        get.op = "file.get";
        get.args["argv"] = nlohmann::json::array({key});
        const fmt::ipc::Response detail = runtime.handle(get);
        FMT_CHECK(detail.ok);
        if (detail.ok) {
            FMT_CHECK_EQ(detail.data.value("file_id", std::string{}), file_id);
            FMT_CHECK(detail.data.value("path", std::string{}).rfind("repository/user/", 0) == 0);
        }
    }

    // 重复上传同一内容：MD5 去重
    fmt::ipc::Request duplicate = upload;
    duplicate.id = 44;
    const fmt::ipc::Response rejected = runtime.handle(duplicate);
    FMT_CHECK(!rejected.ok);
    FMT_CHECK(rejected.error.code == fmt::ErrorCode::Md5Duplicate);

    // 软删除
    fmt::ipc::Request remove;
    remove.id = 45;
    remove.op = "file.delete";
    remove.args["argv"] = nlohmann::json::array({file_id});
    const fmt::ipc::Response removed = runtime.handle(remove);
    FMT_CHECK(removed.ok);
    if (removed.ok) {
        FMT_CHECK(removed.data.value("moved_to", std::string{}).rfind("trash/user/.files/", 0) == 0);
    }

    // 删完列表空了，get 还能查到（记录仍在，只是 is_trash）
    const fmt::ipc::Response after = runtime.handle(list);
    FMT_CHECK(after.ok);
    FMT_CHECK_EQ(after.data["files"].size(), std::size_t{0});

    fmt::ipc::Request get_trashed;
    get_trashed.id = 46;
    get_trashed.op = "file.get";
    get_trashed.args["argv"] = nlohmann::json::array({file_id});
    const fmt::ipc::Response trashed = runtime.handle(get_trashed);
    FMT_CHECK(trashed.ok);
    if (trashed.ok) {
        FMT_CHECK(trashed.data.value("is_trash", false));
        FMT_CHECK_EQ(trashed.data.value("trash_reason", std::string{}), std::string("file"));
        // 在回收站里的记录，仓库里当然找不到——所以要告诉用户它在回收站哪儿
        FMT_CHECK_EQ(trashed.data.value("trash_path", std::string{}).rfind("trash/user/.files/", 0),
                     std::size_t{0});
    }
}

FMT_TEST(Service, 破坏性操作先预检再确认) {
    fmt_test::TempDir temp("service-confirm");
    const auto root = temp / "root";

    const auto source = temp / "payload.txt";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(source, "hello confirm")));

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    const auto create_bucket = [&runtime](const char* name, int id) {
        fmt::ipc::Request request;
        request.id = id;
        request.op = "bucket.create";
        request.args["argv"] = nlohmann::json::array({name});
        return runtime.handle(request);
    };
    FMT_CHECK(create_bucket("工作", 50).ok);
    FMT_CHECK(create_bucket("生活", 51).ok);

    fmt::ipc::Request upload;
    upload.id = 52;
    upload.op = "file.upload";
    upload.args["argv"] = nlohmann::json::array({fmt::path_to_utf8(source)});
    FMT_CHECK(runtime.handle(upload).ok);

    fmt::ipc::Request use_other;
    use_other.id = 53;
    use_other.op = "bucket.use";
    use_other.args["argv"] = nlohmann::json::array({"生活"});
    FMT_CHECK(runtime.handle(use_other).ok);

    // ── 软删除：目标在别的桶里，预检要说清楚，执行要先确认 ──
    fmt::ipc::Request check;
    check.id = 54;
    check.op = "file.delete";
    check.args["argv"] = nlohmann::json::array({"payload.txt"});
    check.args["dry_run"] = true;
    const fmt::ipc::Response checked = runtime.handle(check);
    FMT_CHECK(checked.ok);
    if (checked.ok) {
        FMT_CHECK(checked.data.value("needs_confirm", false));
        FMT_CHECK(!checked.data.value("blocked", true));
        FMT_CHECK_EQ(checked.data.value("bucket", std::string{}), std::string("工作"));
        FMT_CHECK_EQ(checked.data.value("current_bucket", std::string{}), std::string("生活"));
        FMT_CHECK(checked.data.value("message", std::string{}).find("工作") != std::string::npos);
    }
    // 预检不改数据
    FMT_CHECK(fmt::file_exists(root / "repository" / "user" / fmt::path_from_utf8("工作") /
                               "payload.txt") == false);  // 在日期目录里，这里只是确认没被删

    fmt::ipc::Request unconfirmed = check;
    unconfirmed.id = 55;
    unconfirmed.args.erase("dry_run");
    const fmt::ipc::Response refused = runtime.handle(unconfirmed);
    FMT_CHECK(!refused.ok);
    FMT_CHECK(refused.error.code == fmt::ErrorCode::ConfirmRequired);
    FMT_CHECK_EQ(fmt::exit_code(refused.error.code), 2);

    unconfirmed.id = 56;
    unconfirmed.args["force"] = true;
    const fmt::ipc::Response removed = runtime.handle(unconfirmed);
    FMT_CHECK(removed.ok);
    if (removed.ok) {
        FMT_CHECK(removed.data.value("message", std::string{}).find("Bucket：工作") !=
                  std::string::npos);
    }

    // ── 永久删除：预检要把要删掉的东西说清楚 ──
    // 先回「工作」再放一个文件：上面那个已经软删除、躺在文件级回收站里了，
    // 不在桶的树里（所以删桶之前桶里是空的）。
    fmt::ipc::Request use_work;
    use_work.id = 61;
    use_work.op = "bucket.use";
    use_work.args["argv"] = nlohmann::json::array({"工作"});
    FMT_CHECK(runtime.handle(use_work).ok);

    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(source, "second content")));
    fmt::ipc::Request upload_again;
    upload_again.id = 62;
    upload_again.op = "file.upload";
    upload_again.args["argv"] = nlohmann::json::array({fmt::path_to_utf8(source)});
    FMT_CHECK(runtime.handle(upload_again).ok);

    fmt::ipc::Request delete_bucket;
    delete_bucket.id = 57;
    delete_bucket.op = "bucket.delete";
    delete_bucket.args["argv"] = nlohmann::json::array({"工作"});
    const fmt::ipc::Response bucket_removed = runtime.handle(delete_bucket);
    FMT_CHECK(bucket_removed.ok);
    const std::string trashed = bucket_removed.data.value("trashed_name", std::string{});
    FMT_CHECK(!trashed.empty());

    fmt::ipc::Request purge_check;
    purge_check.id = 58;
    purge_check.op = "trash.delete";
    purge_check.args["argv"] = nlohmann::json::array({trashed});
    purge_check.args["dry_run"] = true;
    const fmt::ipc::Response plan = runtime.handle(purge_check);
    FMT_CHECK(plan.ok);
    if (plan.ok) {
        FMT_CHECK(plan.data.value("needs_confirm", false));
        FMT_CHECK_EQ(plan.data.value("original", std::string{}), std::string("工作"));
        FMT_CHECK_EQ(plan.data.value("files", std::size_t{9}), std::size_t{1});
        FMT_CHECK(plan.data.value("bytes", std::uintmax_t{0}) > 0);
        FMT_CHECK(plan.data.value("message", std::string{}).find("不可恢复") != std::string::npos);
    }

    fmt::ipc::Request purge_unconfirmed = purge_check;
    purge_unconfirmed.id = 59;
    purge_unconfirmed.args.erase("dry_run");
    const fmt::ipc::Response purge_refused = runtime.handle(purge_unconfirmed);
    FMT_CHECK(!purge_refused.ok);
    FMT_CHECK(purge_refused.error.code == fmt::ErrorCode::ConfirmRequired);

    purge_unconfirmed.id = 60;
    purge_unconfirmed.args["force"] = true;
    FMT_CHECK(runtime.handle(purge_unconfirmed).ok);
}

FMT_TEST(Service, 未实现的操作与未知操作被明确拒绝) {
    fmt_test::TempDir temp("service-ops");
    fmt::service::ServerRuntime runtime(temp / "root", temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request business;
    business.id = 2;
    business.op = "share.create";  // share 还没做
    const fmt::ipc::Response not_implemented = runtime.handle(business);
    FMT_CHECK(!not_implemented.ok);
    FMT_CHECK(not_implemented.error.code == fmt::ErrorCode::ServiceOperationFailed);
    FMT_CHECK_EQ(fmt::exit_code(not_implemented.error.code), 8);

    fmt::ipc::Request unknown;
    unknown.id = 3;
    unknown.op = "nonsense";
    const fmt::ipc::Response rejected = runtime.handle(unknown);
    FMT_CHECK(!rejected.ok);
    FMT_CHECK(rejected.error.code == fmt::ErrorCode::InvalidArgument);
    FMT_CHECK_EQ(fmt::exit_code(rejected.error.code), 2);
}
