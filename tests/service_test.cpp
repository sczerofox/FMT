// service 的单元测试：状态文件、SCM 查询、服务端请求处理与数据根切换
#include "fmt/service/runtime.hpp"
#include "fmt/service/service.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fmt/common/string.hpp"
#include "fmt/core/path.hpp"
#include "fmt/ipc/protocol.hpp"
#include "fmt/share/share.hpp"
#include "fmt/storage/storage.hpp"
#include "fmt/user/user.hpp"
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
    pending.op = "server.nosuch";  // server.* 还是空壳：已登记、没实现（share 提交后已实现）
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
        FMT_CHECK_EQ(listed.data["entries"].size(), std::size_t{1});
        // 回收站条目现在统一形状：type / id / name，**文件与桶都标出来**
        FMT_CHECK_EQ(listed.data["entries"][0].value("type", std::string{}), std::string("bucket"));
        FMT_CHECK_EQ(listed.data["entries"][0].value("name", std::string{}), std::string("工作"));
        FMT_CHECK(listed.data["entries"][0].value("present", false));
    }

    // 回退：桶回到 repository/<user>/工作
    fmt::ipc::Request restore;
    restore.id = 23;
    restore.op = "trash.restore";
    restore.args["argv"] = nlohmann::json::array({trashed});
    const fmt::ipc::Response restored = runtime.handle(restore);
    FMT_CHECK(restored.ok);
    if (restored.ok) {
        FMT_CHECK_EQ(restored.data["entry"].value("name", std::string{}), std::string("工作"));
        FMT_CHECK_EQ(restored.data["entry"].value("type", std::string{}), std::string("bucket"));
        // present 的语义是「数据还在不在」——回退只是把它搬回仓库，数据当然还在。
        // 若这里翻转成 false，CLI 就会打印「状态：数据已不存在」这种误导信息。
        FMT_CHECK(restored.data["entry"].value("present", false));
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
        FMT_CHECK_EQ(detail.data["entry"].value("name", std::string{}), std::string("工作"));
        FMT_CHECK(detail.data["entry"].value("present", false));
        FMT_CHECK_EQ(detail.data["entry"].value("files", std::size_t{9}), std::size_t{0});
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
        FMT_CHECK_EQ(purged.data["entry"].value("id", std::string{}), second);
        FMT_CHECK_EQ(purged.data["entry"].value("type", std::string{}), std::string("bucket"));
        // 同 restore：结果条目描述的是**删除前**那一份，present 不能被翻转
        FMT_CHECK(purged.data["entry"].value("present", false));
    }

    fmt::ipc::Request list_after;
    list_after.id = 29;
    list_after.op = "trash.list";
    const fmt::ipc::Response after = runtime.handle(list_after);
    FMT_CHECK(after.ok);
    FMT_CHECK_EQ(after.data["entries"].size(), std::size_t{0});
}

FMT_TEST(Service, 目标桶已存在时回退要先确认再合并) {
    fmt_test::TempDir temp("service-merge");
    const auto root = temp / "root";

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    // 造一个桶，里面按日期树放一个文件
    fmt::ipc::Request create;
    create.id = 60;
    create.op = "bucket.create";
    create.args["argv"] = nlohmann::json::array({"test"});
    FMT_CHECK(runtime.handle(create).ok);

    const std::filesystem::path bucket = root / "repository" / "user" / "test";
    FMT_CHECK(fmt::ok(fmt::ensure_directory(bucket / "2026" / "10" / "08")));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(bucket / "2026" / "10" / "08" / "a5.png", "png")));

    fmt::ipc::Request remove;
    remove.id = 61;
    remove.op = "bucket.delete";
    remove.args["argv"] = nlohmann::json::array({"test"});
    remove.args["force"] = true;  // 有内容要确认，测试里直接给
    const fmt::ipc::Response removed = runtime.handle(remove);
    FMT_CHECK(removed.ok);
    const std::string trashed = removed.data.value("trashed_name", std::string{});
    FMT_CHECK(!trashed.empty());

    // 原位置又建了同名桶（用户先单独回退过桶里某个文件时就是这样）
    fmt::ipc::Request recreate;
    recreate.id = 62;
    recreate.op = "bucket.create";
    recreate.args["argv"] = nlohmann::json::array({"test"});
    FMT_CHECK(runtime.handle(recreate).ok);

    // 预检：这次回退是**合并**，要一次确认（不是「Bucket 已存在」的硬拒绝）
    fmt::ipc::Request check;
    check.id = 63;
    check.op = "trash.restore";
    check.args["argv"] = nlohmann::json::array({trashed});
    check.args["dry_run"] = true;
    const fmt::ipc::Response checked = runtime.handle(check);
    FMT_CHECK(checked.ok);
    if (checked.ok) {
        FMT_CHECK(checked.data.value("merge", false));
        FMT_CHECK(checked.data.value("needs_confirm", false));
        FMT_CHECK(!checked.data.value("blocked", true));
        FMT_CHECK_EQ(checked.data.value("merge_files", std::size_t{0}), std::size_t{1});
        FMT_CHECK_EQ(checked.data.value("target_files", std::size_t{0}), std::size_t{0});
    }

    // 没带 force：拒绝，而且**一个文件都没搬**
    fmt::ipc::Request unconfirmed;
    unconfirmed.id = 64;
    unconfirmed.op = "trash.restore";
    unconfirmed.args["argv"] = nlohmann::json::array({trashed});
    const fmt::ipc::Response refused = runtime.handle(unconfirmed);
    FMT_CHECK(!refused.ok);
    FMT_CHECK(refused.error.code == fmt::ErrorCode::ConfirmRequired);
    FMT_CHECK_EQ(fmt::exit_code(refused.error.code), 2);
    FMT_CHECK(fmt::file_exists(root / "trash" / "user" / fmt::path_from_utf8(trashed) / "2026" /
                               "10" / "08" / "a5.png"));

    // 带 force：合并进去
    fmt::ipc::Request forced;
    forced.id = 65;
    forced.op = "trash.restore";
    forced.args["argv"] = nlohmann::json::array({trashed});
    forced.args["force"] = true;
    const fmt::ipc::Response merged = runtime.handle(forced);
    if (!merged.ok) {
        std::printf("    合并失败：%s %s\n", fmt::code_string(merged.error.code).c_str(),
                    merged.error.message.c_str());
    }
    FMT_CHECK(merged.ok);
    if (merged.ok) {
        FMT_CHECK_EQ(merged.data["entry"].value("type", std::string{}), std::string("bucket"));
        FMT_CHECK(merged.data.value("message", std::string{}).find("已合并") != std::string::npos);
    }
    FMT_CHECK(fmt::file_exists(bucket / "2026" / "10" / "08" / "a5.png"));

    // 桶还在（是合并，不是搬回），回收站里那条已经摘掉
    FMT_CHECK(fmt::directory_exists(bucket));
    fmt::ipc::Request list_after;
    list_after.id = 66;
    list_after.op = "trash.list";
    const fmt::ipc::Response after = runtime.handle(list_after);
    FMT_CHECK(after.ok);
    FMT_CHECK_EQ(after.data["entries"].size(), std::size_t{0});

    // 已经合并过的条目再回退一次：条目没了
    fmt::ipc::Request again;
    again.id = 67;
    again.op = "trash.restore";
    again.args["argv"] = nlohmann::json::array({trashed});
    const fmt::ipc::Response missing = runtime.handle(again);
    FMT_CHECK(!missing.ok);
    FMT_CHECK(missing.error.code == fmt::ErrorCode::TrashEntryNotFound);
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

    // ① **刚写下的** `fmt-` 文件必须留着：提权副本正在回传的结果文件也叫这个名字，
    //    而 install 会在同一次操作里启动服务、服务启动就来清 temp/。
    //    以前不分青红皂白地删，父进程只好报 FMT-602「提权副本没有返回结果」。
    {
        fmt::service::ServerRuntime runtime(root, temp / "state");
        FMT_CHECK(fmt::ok(runtime.start()));
        FMT_CHECK(fmt::file_exists(root / "temp" / "fmt-elev-123.json"));
        FMT_CHECK(fmt::file_exists(mine));
    }

    // ② 陈旧的要清掉：把时间拨回一小时再启动一次
    std::error_code code;
    const std::filesystem::path stale = root / "temp" / "fmt-elev-123.json";
    std::filesystem::last_write_time(
        stale, std::filesystem::file_time_type::clock::now() - std::chrono::hours(1), code);
    FMT_CHECK(!code);

    fmt::service::ServerRuntime runtime(root, temp / "state-2");
    FMT_CHECK(fmt::ok(runtime.start()));
    FMT_CHECK(!fmt::file_exists(stale));
    FMT_CHECK(fmt::file_exists(mine));  // 用户手放的其它文件始终不动
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
            // 预览 / 下载链接（本次改动（未提交））：完整地址，按 file_id 拼、与按什么查无关
            FMT_CHECK_EQ(detail.data.value("preview_url", std::string{}),
                         std::string("http://localhost:4122/api/file/") + file_id + "/preview");
            FMT_CHECK_EQ(detail.data.value("download_url", std::string{}),
                         std::string("http://localhost:4122/api/file/") + file_id + "/download");
        }
    }

    // 同一条命令重放（同一个文件：内容与名字都相同）→ FMT-304「文件已存在」
    fmt::ipc::Request duplicate = upload;
    duplicate.id = 44;
    const fmt::ipc::Response rejected = runtime.handle(duplicate);
    FMT_CHECK(!rejected.ok);
    FMT_CHECK(rejected.error.code == fmt::ErrorCode::Md5Duplicate);
    FMT_CHECK(rejected.error.message.find("文件已存在") != std::string::npos);

    // 同一份内容换个名字 → 同样是 FMT-304（内容判重不看名字），且不要求改文件名
    fmt::ipc::Request same_content = upload;
    same_content.id = 47;
    same_content.args["argv"] = nlohmann::json::array(
        {fmt::path_to_utf8(source), std::string("payload-copy.txt")});
    const fmt::ipc::Response content_hit = runtime.handle(same_content);
    FMT_CHECK(!content_hit.ok);
    FMT_CHECK(content_hit.error.code == fmt::ErrorCode::Md5Duplicate);
    FMT_CHECK(content_hit.error.message.find("文件已存在") != std::string::npos);
    FMT_CHECK(content_hit.error.message.find("修改文件名") == std::string::npos);

    // 内容不同 + 名字相同 → FMT-105「文件名重复」，这次要求改文件名
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(source, "another content")));
    fmt::ipc::Request same_name = upload;
    same_name.id = 48;
    const fmt::ipc::Response name_hit = runtime.handle(same_name);
    FMT_CHECK(!name_hit.ok);
    FMT_CHECK(name_hit.error.code == fmt::ErrorCode::FileNameConflict);
    FMT_CHECK(name_hit.error.message.find("文件名重复") != std::string::npos);
    FMT_CHECK(name_hit.error.message.find("修改文件名") != std::string::npos);

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
    const fmt::ipc::Response uploaded = runtime.handle(upload);
    FMT_CHECK(uploaded.ok);
    if (!uploaded.ok) {
        return;
    }
    const std::string payload_id = uploaded.data.value("file_id", std::string{});
    FMT_CHECK(!payload_id.empty());

    fmt::ipc::Request use_other;
    use_other.id = 53;
    use_other.op = "bucket.use";
    use_other.args["argv"] = nlohmann::json::array({"生活"});
    FMT_CHECK(runtime.handle(use_other).ok);

    // ── 软删除：目标在别的桶里，预检要说清楚，执行要先确认 ──
    // 按名字在别的桶里已经够不到（本次改动（未提交）：名字的命名空间是 Bucket），跨桶只能用 file_id。
    fmt::ipc::Request by_name;
    by_name.id = 57;
    by_name.op = "file.delete";
    by_name.args["argv"] = nlohmann::json::array({"payload.txt"});
    by_name.args["dry_run"] = true;
    const fmt::ipc::Response name_missed = runtime.handle(by_name);
    FMT_CHECK(!name_missed.ok);
    FMT_CHECK(name_missed.error.code == fmt::ErrorCode::FileNotFound);

    fmt::ipc::Request check;
    check.id = 54;
    check.op = "file.delete";
    check.args["argv"] = nlohmann::json::array({payload_id});
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

    // 桶里有文件：删除前必须确认（提醒「之后只能整体恢复这个桶」）
    fmt::ipc::Request bucket_check;
    bucket_check.id = 70;
    bucket_check.op = "bucket.delete";
    bucket_check.args["argv"] = nlohmann::json::array({"工作"});
    bucket_check.args["dry_run"] = true;
    const fmt::ipc::Response bucket_plan = runtime.handle(bucket_check);
    FMT_CHECK(bucket_plan.ok);
    if (bucket_plan.ok) {
        FMT_CHECK(bucket_plan.data.value("has_content", false));
        FMT_CHECK(bucket_plan.data.value("needs_confirm", false));
        FMT_CHECK(bucket_plan.data.value("message", std::string{}).find("只能整体恢复") !=
                  std::string::npos);
    }

    fmt::ipc::Request delete_bucket;
    delete_bucket.id = 57;
    delete_bucket.op = "bucket.delete";
    delete_bucket.args["argv"] = nlohmann::json::array({"工作"});
    const fmt::ipc::Response unconfirmed_bucket = runtime.handle(delete_bucket);
    FMT_CHECK(!unconfirmed_bucket.ok);
    FMT_CHECK(unconfirmed_bucket.error.code == fmt::ErrorCode::ConfirmRequired);

    delete_bucket.id = 71;
    delete_bucket.args["force"] = true;
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
        // 统一形状：条目信息在 entry 里，文件与桶都标出来
        FMT_CHECK_EQ(plan.data["entry"].value("type", std::string{}), std::string("bucket"));
        FMT_CHECK_EQ(plan.data["entry"].value("name", std::string{}), std::string("工作"));
        FMT_CHECK_EQ(plan.data["entry"].value("files", std::size_t{9}), std::size_t{1});
        FMT_CHECK(plan.data["entry"].value("bytes", std::uintmax_t{0}) > 0);
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

FMT_TEST(Service, 列表排序配置与清空回收站) {
    fmt_test::TempDir temp("service-misc");
    const auto root = temp / "root";

    // 三个大小不同的素材，用来看排序
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(temp / "small.txt", "s")));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(temp / "medium.txt", std::string(500, 'm'))));
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(temp / "large.txt", std::string(2000, 'l'))));

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request create;
    create.id = 1;
    create.op = "bucket.create";
    create.args["argv"] = nlohmann::json::array({"工作"});
    FMT_CHECK(runtime.handle(create).ok);

    for (const char* name : {"medium.txt", "large.txt", "small.txt"}) {
        fmt::ipc::Request upload;
        upload.id = 2;
        upload.op = "file.upload";
        upload.args["argv"] = nlohmann::json::array({fmt::path_to_utf8(temp / name)});
        FMT_CHECK(runtime.handle(upload).ok);
    }

    // ---- file.list 排序 ----
    const auto list_names = [&runtime](const std::string& sort) {
        fmt::ipc::Request request;
        request.id = 3;
        request.op = "file.list";
        if (!sort.empty()) {
            request.args["sort"] = sort;
        }
        const fmt::ipc::Response response = runtime.handle(request);
        std::vector<std::string> names;
        if (response.ok) {
            for (const nlohmann::json& item : response.data["files"]) {
                names.push_back(item.value("file_name", std::string{}));
            }
        }
        return names;
    };

    FMT_CHECK_EQ(list_names("").size(), std::size_t{3});
    FMT_CHECK_EQ(list_names("").front(), std::string("large.txt"));   // 默认按名字
    FMT_CHECK_EQ(list_names("size").front(), std::string("large.txt"));  // 大的在前
    FMT_CHECK_EQ(list_names("size").back(), std::string("small.txt"));
    FMT_CHECK_EQ(list_names("id").front(), std::string("medium.txt"));   // 入库顺序

    // 乱写的排序方式要报参数错误，而不是静默按默认排
    fmt::ipc::Request bad_sort;
    bad_sort.id = 4;
    bad_sort.op = "file.list";
    bad_sort.args["sort"] = "乱写";
    const fmt::ipc::Response bad = runtime.handle(bad_sort);
    FMT_CHECK(!bad.ok);
    FMT_CHECK(bad.error.code == fmt::ErrorCode::InvalidArgument);

    // ---- config ----
    fmt::ipc::Request config_list;
    config_list.id = 5;
    config_list.op = "config.list";
    const fmt::ipc::Response config = runtime.handle(config_list);
    FMT_CHECK(config.ok);
    FMT_CHECK_EQ(config.data.value("max_upload_size", std::uintmax_t{0}),
                 std::uintmax_t{52428800});
    // 本次改动（未提交）：config.list 也输出 config/server.json 的公共字段（带 server_ 前缀），
    // 且**不含任何凭证**（token 单独给，口令哈希从不出现）
    FMT_CHECK_EQ(config.data.value("server_enabled", false), true);  // 默认已改成 true
    FMT_CHECK_EQ(config.data.value("server_host", std::string{}), std::string("localhost"));
    FMT_CHECK_EQ(config.data.value("server_port", 0), 4122);
    FMT_CHECK(config.data.value("server_path", std::string{}).rfind("config/", 0) == 0);
    FMT_CHECK_EQ(config.data.value("server_token", std::string{}), std::string{});
    FMT_CHECK(!config.data.contains("password_hash"));
    FMT_CHECK(!config.data.contains("password_salt"));

    fmt::ipc::Request set;
    set.id = 6;
    set.op = "config.set";
    set.args["argv"] = nlohmann::json::array({"max_upload_size", "2MB"});
    const fmt::ipc::Response changed = runtime.handle(set);
    FMT_CHECK(changed.ok);
    FMT_CHECK_EQ(changed.data.value("max_upload_size", std::uintmax_t{0}),
                 std::uintmax_t{2097152});

    // 落盘了：直接用同一套路径规则重新读配置，也是新值
    {
        const fmt::PathManager paths{root};
        const auto reloaded = fmt::load_config(paths);
        FMT_CHECK(fmt::ok(reloaded));
        if (fmt::ok(reloaded)) {
            FMT_CHECK_EQ(std::get<fmt::Config>(reloaded).max_upload_size,
                         std::uintmax_t{2097152});
        }
    }

    // config set token_ttl：访问 token 的有效期（用户要求 token 有时效）。
    // 接受 `7d` / `12h` / 纯秒数；**拒绝 0**（允许 0 等于留了「永不过期」的后门）。
    {
        fmt::ipc::Request ttl;
        ttl.id = 20;
        ttl.op = "config.set";
        ttl.args["argv"] = nlohmann::json::array({"token_ttl", "12h"});
        const fmt::ipc::Response applied = runtime.handle(ttl);
        FMT_CHECK(applied.ok);
        FMT_CHECK_EQ(applied.data.value("token_ttl_seconds", 0ULL), 12ULL * 60 * 60);

        fmt::ipc::Request days;
        days.id = 21;
        days.op = "config.set";
        days.args["argv"] = nlohmann::json::array({"token_ttl", "3d"});
        const fmt::ipc::Response applied_days = runtime.handle(days);
        FMT_CHECK(applied_days.ok);
        FMT_CHECK_EQ(applied_days.data.value("token_ttl_seconds", 0ULL), 3ULL * 24 * 60 * 60);

        fmt::ipc::Request zero;
        zero.id = 22;
        zero.op = "config.set";
        zero.args["argv"] = nlohmann::json::array({"token_ttl", "0"});
        const fmt::ipc::Response rejected = runtime.handle(zero);
        FMT_CHECK(!rejected.ok);
        FMT_CHECK_EQ(rejected.error.code, fmt::ErrorCode::InvalidArgument);

        fmt::ipc::Request garbage;
        garbage.id = 23;
        garbage.op = "config.set";
        garbage.args["argv"] = nlohmann::json::array({"token_ttl", "soon"});
        const fmt::ipc::Response garbage_rejected = runtime.handle(garbage);
        FMT_CHECK(!garbage_rejected.ok);

        // 落盘：重读配置拿到的是 3 天（最后一次成功的那次）。
        const fmt::PathManager paths{root};
        const auto reloaded = fmt::load_config(paths);
        FMT_CHECK(fmt::ok(reloaded));
        if (fmt::ok(reloaded)) {
            FMT_CHECK_EQ(std::get<fmt::Config>(reloaded).token_ttl_seconds,
                         3ULL * 24 * 60 * 60);
        }
    }

    // config set server_enabled：改 server.json（本次改动（未提交）），并回报要重启服务；
    // **服务侧不做 reinstall**（那由 CLI 做），所以这里只断言文件与响应字段
    {
        fmt::ipc::Request disable;
        disable.id = 11;
        disable.op = "config.set";
        disable.args["argv"] = nlohmann::json::array({"server_enabled", "false"});
        const fmt::ipc::Response off = runtime.handle(disable);
        FMT_CHECK(off.ok);
        FMT_CHECK_EQ(off.data.value("server_enabled", true), false);
        FMT_CHECK(off.data.value("restart_required", false));
        FMT_CHECK(off.data.value("message", std::string{}).find("重启") != std::string::npos);

        const fmt::PathManager paths{root};
        const auto reloaded = fmt::load_server_config(paths);
        FMT_CHECK(fmt::ok(reloaded));
        if (fmt::ok(reloaded)) {
            FMT_CHECK_EQ(std::get<fmt::ServerConfig>(reloaded).enabled, false);
        }

        // 打开回来，并顺手校验 host / port 也能改
        fmt::ipc::Request enable;
        enable.id = 12;
        enable.op = "config.set";
        enable.args["argv"] = nlohmann::json::array({"server_enabled", "true"});
        FMT_CHECK(runtime.handle(enable).ok);

        fmt::ipc::Request host;
        host.id = 13;
        host.op = "config.set";
        host.args["argv"] = nlohmann::json::array({"server_host", "127.0.0.1"});
        FMT_CHECK(runtime.handle(host).ok);

        fmt::ipc::Request port;
        port.id = 14;
        port.op = "config.set";
        port.args["argv"] = nlohmann::json::array({"server_port", "4321"});
        const fmt::ipc::Response moved = runtime.handle(port);
        FMT_CHECK(moved.ok);
        FMT_CHECK_EQ(moved.data.value("server_port", 0), 4321);

        const auto final_config = fmt::load_server_config(paths);
        FMT_CHECK(fmt::ok(final_config));
        if (fmt::ok(final_config)) {
            const fmt::ServerConfig& value = std::get<fmt::ServerConfig>(final_config);
            FMT_CHECK_EQ(value.enabled, true);
            FMT_CHECK_EQ(value.host, std::string("127.0.0.1"));
            FMT_CHECK_EQ(value.port, 4321);
        }

        // 非法值：布尔写错、端口越界、host 空 → FMT-001，且**不改文件**
        for (const auto& invalid : {std::pair<const char*, const char*>{"server_enabled", "maybe"},
                                    {"server_port", "70000"},
                                    {"server_port", "abc"},
                                    {"server_host", "  "}}) {
            fmt::ipc::Request request;
            request.id = 15;
            request.op = "config.set";
            request.args["argv"] = nlohmann::json::array({invalid.first, invalid.second});
            const fmt::ipc::Response refused = runtime.handle(request);
            FMT_CHECK(!refused.ok);
            FMT_CHECK(refused.error.code == fmt::ErrorCode::InvalidArgument);
        }
        const auto unchanged = fmt::load_server_config(paths);
        FMT_CHECK(fmt::ok(unchanged));
        if (fmt::ok(unchanged)) {
            FMT_CHECK_EQ(std::get<fmt::ServerConfig>(unchanged).port, 4321);
            FMT_CHECK_EQ(std::get<fmt::ServerConfig>(unchanged).host, std::string("127.0.0.1"));
        }
    }

    // 只读项与非法值都要拒绝
    fmt::ipc::Request readonly;
    readonly.id = 7;
    readonly.op = "config.set";
    readonly.args["argv"] = nlohmann::json::array({"current_user", "someone"});
    FMT_CHECK(!runtime.handle(readonly).ok);

    fmt::ipc::Request tiny;
    tiny.id = 8;
    tiny.op = "config.set";
    tiny.args["argv"] = nlohmann::json::array({"max_upload_size", "0"});
    FMT_CHECK(!runtime.handle(tiny).ok);

    // ---- trash.empty ----
    for (const char* name : {"small.txt", "medium.txt"}) {
        fmt::ipc::Request remove;
        remove.id = 9;
        remove.op = "file.delete";
        remove.args["argv"] = nlohmann::json::array({name});
        remove.args["force"] = true;
        FMT_CHECK(runtime.handle(remove).ok);
    }

    fmt::ipc::Request empty_check;
    empty_check.id = 10;
    empty_check.op = "trash.empty";
    empty_check.args["dry_run"] = true;
    const fmt::ipc::Response plan = runtime.handle(empty_check);
    FMT_CHECK(plan.ok);
    FMT_CHECK_EQ(plan.data.value("files", std::size_t{0}), std::size_t{2});
    FMT_CHECK(plan.data.value("needs_confirm", false));
    FMT_CHECK(plan.data.value("message", std::string{}).find("不可恢复") != std::string::npos);

    // 没确认 -> FMT-016
    fmt::ipc::Request unconfirmed;
    unconfirmed.id = 11;
    unconfirmed.op = "trash.empty";
    const fmt::ipc::Response refused = runtime.handle(unconfirmed);
    FMT_CHECK(!refused.ok);
    FMT_CHECK(refused.error.code == fmt::ErrorCode::ConfirmRequired);

    fmt::ipc::Request forced;
    forced.id = 12;
    forced.op = "trash.empty";
    forced.args["force"] = true;
    const fmt::ipc::Response emptied = runtime.handle(forced);
    FMT_CHECK(emptied.ok);
    FMT_CHECK_EQ(emptied.data.value("files", std::size_t{0}), std::size_t{2});

    fmt::ipc::Request after;
    after.id = 13;
    after.op = "trash.list";
    const fmt::ipc::Response remaining = runtime.handle(after);
    FMT_CHECK(remaining.ok);
    FMT_CHECK_EQ(remaining.data["entries"].size(), std::size_t{0});

    // 空回收站再清一次：成功且什么都不做（不该弹确认）
    fmt::ipc::Request again;
    again.id = 14;
    again.op = "trash.empty";
    const fmt::ipc::Response idle = runtime.handle(again);
    FMT_CHECK(idle.ok);
    FMT_CHECK_EQ(idle.data.value("files", std::size_t{9}), std::size_t{0});
}

FMT_TEST(Service, 文件列表的搜索与分页) {
    fmt_test::TempDir temp("service-page");
    const auto root = temp / "root";
    const std::vector<std::string> names = {"Report-A.txt", "report-b.txt", "notes.md",
                                            "data.bin", "report-c.txt"};
    for (const std::string& name : names) {
        // 内容=名字：避免不同文件 MD5 相同被拒
        FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(temp / name, name)));
    }

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request create_bucket;
    create_bucket.id = 1;
    create_bucket.op = "bucket.create";
    create_bucket.args["argv"] = nlohmann::json::array({"工作"});
    FMT_CHECK(runtime.handle(create_bucket).ok);

    for (const std::string& name : names) {
        fmt::ipc::Request upload;
        upload.id = 2;
        upload.op = "file.upload";
        upload.args["argv"] = nlohmann::json::array({fmt::path_to_utf8(temp / name)});
        FMT_CHECK(runtime.handle(upload).ok);
    }

    // ---- 搜索：不区分大小写、子串 ----
    fmt::ipc::Request search_request;
    search_request.id = 3;
    search_request.op = "file.list";
    search_request.args["search"] = "REPORT";
    const fmt::ipc::Response searched = runtime.handle(search_request);
    FMT_CHECK(searched.ok);
    FMT_CHECK_EQ(searched.data["total"].get<std::size_t>(), std::size_t{3});
    FMT_CHECK_EQ(searched.data["count"].get<std::size_t>(), std::size_t{3});
    FMT_CHECK_EQ(searched.data["files"].size(), std::size_t{3});

    // ---- 分页：total 是命中总数，不是本页条数 ----
    fmt::ipc::Request page_request;
    page_request.id = 4;
    page_request.op = "file.list";
    page_request.args["page"] = 1;
    page_request.args["page_size"] = 2;
    const fmt::ipc::Response page1 = runtime.handle(page_request);
    FMT_CHECK(page1.ok);
    FMT_CHECK_EQ(page1.data["total"].get<std::size_t>(), std::size_t{5});
    FMT_CHECK_EQ(page1.data["total_pages"].get<std::size_t>(), std::size_t{3});
    FMT_CHECK_EQ(page1.data["files"].size(), std::size_t{2});

    // ---- 翻页不漏不重：三页拼起来正好 5 条，且无重复 ----
    std::vector<std::string> collected;
    for (int page = 1; page <= 3; ++page) {
        fmt::ipc::Request request;
        request.id = 5;
        request.op = "file.list";
        request.args["page"] = page;
        request.args["page_size"] = 2;
        const fmt::ipc::Response response = runtime.handle(request);
        FMT_CHECK(response.ok);
        for (const nlohmann::json& item : response.data["files"]) {
            collected.push_back(item["file_id"].get<std::string>());
        }
    }
    FMT_CHECK_EQ(collected.size(), std::size_t{5});
    std::vector<std::string> unique_check = collected;
    std::sort(unique_check.begin(), unique_check.end());
    FMT_CHECK(std::unique(unique_check.begin(), unique_check.end()) == unique_check.end());

    // ---- 非法参数一律 FMT-001 ----
    fmt::ipc::Request bad;
    bad.id = 6;
    bad.op = "file.list";
    bad.args["page"] = 0;
    const fmt::ipc::Response rejected = runtime.handle(bad);
    FMT_CHECK(!rejected.ok);
    FMT_CHECK(rejected.error.code == fmt::ErrorCode::InvalidArgument);

    // ---- 默认不分页（保持老行为），但 total 一定有 ----
    fmt::ipc::Request all_request;
    all_request.id = 7;
    all_request.op = "file.list";
    const fmt::ipc::Response all = runtime.handle(all_request);
    FMT_CHECK(all.ok);
    FMT_CHECK_EQ(all.data["files"].size(), std::size_t{5});
    FMT_CHECK_EQ(all.data["page_size"].get<int>(), 0);
    FMT_CHECK_EQ(all.data["total"].get<std::size_t>(), std::size_t{5});
}

FMT_TEST(Service, 分享的创建查看列出撤销与计数) {
    fmt_test::TempDir temp("service-share");
    const auto root = temp / "root";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(temp / "payload.txt", "share me")));

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request create_bucket;
    create_bucket.id = 1;
    create_bucket.op = "bucket.create";
    create_bucket.args["argv"] = nlohmann::json::array({"工作"});
    FMT_CHECK(runtime.handle(create_bucket).ok);

    fmt::ipc::Request upload;
    upload.id = 2;
    upload.op = "file.upload";
    upload.args["argv"] = nlohmann::json::array({fmt::path_to_utf8(temp / "payload.txt")});
    const fmt::ipc::Response uploaded = runtime.handle(upload);
    FMT_CHECK(uploaded.ok);
    const std::string file_id = uploaded.data.value("file_id", std::string{});
    FMT_CHECK(!file_id.empty());

    // ---- 创建 ----
    fmt::ipc::Request create;
    create.id = 3;
    create.op = "share.create";
    create.args["argv"] = nlohmann::json::array({file_id});
    const fmt::ipc::Response created = runtime.handle(create);
    FMT_CHECK(created.ok);
    const std::string share_id = created.data.value("share_id", std::string{});
    FMT_CHECK_EQ(share_id.size(), std::size_t{12});
    FMT_CHECK(share_id.find_first_not_of("0123456789abcdef") == std::string::npos);
    FMT_CHECK_EQ(created.data.value("max_download_count", 0), fmt::kDefaultShareDownloads);
    FMT_CHECK_EQ(created.data.value("download_count", -1), 0);
    FMT_CHECK(created.data["expire_time"].is_string());  // 默认 7 天，不是 null
    FMT_CHECK(created.data.value("is_valid", false));

    // 完整分享链接（本次改动（未提交））：地址来自 config/server.json 的 host / port，
    // **不看 HTTP 服务有没有开**（这里根本没起 HTTP），share_id 就是路径里的凭证。
    const std::string share_url = created.data.value("share_url", std::string{});
    FMT_CHECK_EQ(share_url, std::string("http://localhost:4122/api/share/") + share_id + "/download");
    FMT_CHECK_EQ(created.data.value("url", std::string{}), share_url);
    // share.get 也带同一条链接
    {
        fmt::ipc::Request get_url;
        get_url.id = 30;
        get_url.op = "share.get";
        get_url.args["argv"] = nlohmann::json::array({share_id});
        const fmt::ipc::Response with_url = runtime.handle(get_url);
        FMT_CHECK(with_url.ok);
        FMT_CHECK_EQ(with_url.data.value("share_url", std::string{}), share_url);
    }

    const fmt::ipc::Response second = runtime.handle(create);
    FMT_CHECK(second.ok);
    FMT_CHECK(second.data.value("share_id", std::string{}) != share_id);

    // ---- 查看 ----
    fmt::ipc::Request get;
    get.id = 4;
    get.op = "share.get";
    get.args["argv"] = nlohmann::json::array({share_id});
    const fmt::ipc::Response viewed = runtime.handle(get);
    FMT_CHECK(viewed.ok);
    FMT_CHECK(viewed.data.value("available", false));
    FMT_CHECK_EQ(viewed.data.value("state", std::string{}), std::string("可用"));
    FMT_CHECK_EQ(viewed.data.value("file_name", std::string{}), std::string("payload.txt"));

    fmt::ipc::Request missing;
    missing.id = 5;
    missing.op = "share.get";
    missing.args["argv"] = nlohmann::json::array({"000000000000"});
    const fmt::ipc::Response not_found = runtime.handle(missing);
    FMT_CHECK(!not_found.ok);
    FMT_CHECK(not_found.error.code == fmt::ErrorCode::ShareNotFound);
    FMT_CHECK_EQ(fmt::exit_code(not_found.error.code), 3);

    // ---- 列出（带参数：只看这个文件）----
    fmt::ipc::Request list;
    list.id = 6;
    list.op = "share.list";
    list.args["argv"] = nlohmann::json::array({file_id});
    const fmt::ipc::Response listed = runtime.handle(list);
    FMT_CHECK(listed.ok);
    // 这个快照排在下面「按文件名再建一条」之前：那时会变成 3 条
    FMT_CHECK_EQ(listed.data.value("count", std::size_t{0}), std::size_t{2});
    FMT_CHECK_EQ(listed.data.value("scope", std::string{}), std::string("file"));
    FMT_CHECK_EQ(listed.data["shares"][0].value("share_id", std::string{}),
                 second.data.value("share_id", std::string{}));
    // 每条都带「为什么不能下载」的独立标记（本次改动（未提交））
    FMT_CHECK_EQ(listed.data["shares"][0].value("expired", true), false);
    FMT_CHECK_EQ(listed.data["shares"][0].value("limit_reached", true), false);
    FMT_CHECK_EQ(listed.data["shares"][0].value("downloads_left", 0), fmt::kDefaultShareDownloads);
    FMT_CHECK_EQ(listed.data["shares"][0].value("file_name", std::string{}),
                 std::string("payload.txt"));
    FMT_CHECK_EQ(listed.data["shares"][0].value("bucket", std::string{}), std::string("工作"));
    FMT_CHECK_EQ(listed.data.value("current_bucket", std::string{}), std::string("工作"));

    // ---- 列出（**不带参数**：列全部，本次改动（未提交））----
    {
        fmt::ipc::Request list_all;
        list_all.id = 35;
        list_all.op = "share.list";
        const fmt::ipc::Response all = runtime.handle(list_all);
        FMT_CHECK(all.ok);
        FMT_CHECK_EQ(all.data.value("scope", std::string{}), std::string("all"));
        FMT_CHECK_EQ(all.data.value("count", std::size_t{0}), std::size_t{2});
        FMT_CHECK(!all.data.contains("file_id"));  // 不带参数就没有「过滤的那个文件」
        FMT_CHECK_EQ(all.data["shares"][0].value("file_name", std::string{}),
                     std::string("payload.txt"));
        FMT_CHECK(all.data["shares"][0].value("share_url", std::string{})
                      .rfind("http://localhost:4122/api/share/", 0) == 0);
    }

    // ---- 按**文件名**创建与列出（本次改动（未提交）：与 file get / file delete 同一套规则）----
    {
        fmt::ipc::Request by_name;
        by_name.id = 31;
        by_name.op = "share.create";
        by_name.args["argv"] = nlohmann::json::array({"payload.txt"});
        const fmt::ipc::Response named = runtime.handle(by_name);
        FMT_CHECK(named.ok);
        FMT_CHECK_EQ(named.data.value("share_id", std::string{}).size(), std::size_t{12});
        FMT_CHECK_EQ(named.data.value("file_id", std::string{}), file_id);
        const std::string named_share_id = named.data.value("share_id", std::string{});

        fmt::ipc::Request list_by_name;
        list_by_name.id = 32;
        list_by_name.op = "share.list";
        list_by_name.args["argv"] = nlohmann::json::array({"payload.txt"});
        const fmt::ipc::Response named_list = runtime.handle(list_by_name);
        FMT_CHECK(named_list.ok);
        FMT_CHECK_EQ(named_list.data.value("file_id", std::string{}), file_id);
        FMT_CHECK_EQ(named_list.data.value("count", std::size_t{0}), std::size_t{3});
        FMT_CHECK(named_list.data["shares"][0].value("share_url", std::string{})
                       .rfind("http://localhost:4122/api/share/", 0) == 0);

        // 名字不存在 → FMT-002（与 file get 的说法一致）
        fmt::ipc::Request absent;
        absent.id = 33;
        absent.op = "share.create";
        absent.args["argv"] = nlohmann::json::array({"nope.txt"});
        const fmt::ipc::Response missing_file = runtime.handle(absent);
        FMT_CHECK(!missing_file.ok);
        FMT_CHECK(missing_file.error.code == fmt::ErrorCode::FileNotFound);

        // 立刻撤销掉它：后面的「过期」断言按 share.json 的条数写，不想被这条影响
        fmt::ipc::Request revoke_named;
        revoke_named.id = 34;
        revoke_named.op = "share.delete";
        revoke_named.args["argv"] = nlohmann::json::array({named_share_id});
        FMT_CHECK(runtime.handle(revoke_named).ok);
    }

    // ---- 下载记账：20 次之后第 21 次必须被拒（§51）----
    const auto download = [&runtime, &share_id](int id) {
        fmt::ipc::Request request;
        request.id = id;
        request.op = "share.download";
        request.args["argv"] = nlohmann::json::array({share_id});
        return runtime.handle(request);
    };
    for (int i = 0; i < fmt::kDefaultShareDownloads; ++i) {
        FMT_CHECK(download(100 + i).ok);
    }
    const fmt::ipc::Response over = download(200);
    FMT_CHECK(!over.ok);
    FMT_CHECK(over.error.code == fmt::ErrorCode::ShareDownloadLimitReached);
    FMT_CHECK_EQ(fmt::exit_code(over.error.code), 5);

    const fmt::ipc::Response exhausted = runtime.handle(get);
    FMT_CHECK(exhausted.ok);
    FMT_CHECK_EQ(exhausted.data.value("download_count", -1), fmt::kDefaultShareDownloads);
    FMT_CHECK_EQ(exhausted.data.value("state", std::string{}), std::string("下载次数已用尽"));
    FMT_CHECK(!exhausted.data.value("available", true));

    // ---- 分享不得绕过文件状态（§50）----
    fmt::ipc::Request remove_file;
    remove_file.id = 7;
    remove_file.op = "file.delete";
    remove_file.args["argv"] = nlohmann::json::array({file_id});
    remove_file.args["force"] = true;
    FMT_CHECK(runtime.handle(remove_file).ok);

    const fmt::ipc::Response blocked = runtime.handle(get);
    FMT_CHECK(blocked.ok);  // 查得到（不是「不存在」），状态说明它在回收站
    FMT_CHECK_EQ(blocked.data.value("state", std::string{}), std::string("关联文件不可用"));
    FMT_CHECK(!blocked.data.value("available", true));

    const fmt::ipc::Response refused = download(300);
    FMT_CHECK(!refused.ok);
    FMT_CHECK(refused.error.code == fmt::ErrorCode::ShareFileUnavailable);

    fmt::ipc::Request late = create;
    late.id = 8;
    const fmt::ipc::Response late_created = runtime.handle(late);
    FMT_CHECK(!late_created.ok);
    FMT_CHECK(late_created.error.code == fmt::ErrorCode::ShareFileUnavailable);

    // ---- 撤销 ----
    fmt::ipc::Request revoke;
    revoke.id = 9;
    revoke.op = "share.delete";
    revoke.args["argv"] = nlohmann::json::array({share_id});
    FMT_CHECK(runtime.handle(revoke).ok);

    fmt::ipc::Request after;
    after.id = 10;
    after.op = "share.get";
    after.args["argv"] = nlohmann::json::array({share_id});
    const fmt::ipc::Response gone = runtime.handle(after);
    FMT_CHECK(!gone.ok);
    FMT_CHECK(gone.error.code == fmt::ErrorCode::ShareNotFound);

    revoke.id = 11;
    const fmt::ipc::Response again = runtime.handle(revoke);
    FMT_CHECK(!again.ok);
    FMT_CHECK(again.error.code == fmt::ErrorCode::ShareNotFound);

    // ---- 过期：直接写一条过去的 expire_time，看是否如实报「已过期」----
    // 先把文件从回收站恢复：§49 的检查顺序是「文件状态 -> 过期」，
    // 文件还在回收站时得到的是「关联文件不可用」，那就测不到过期这条。
    {
        fmt::ipc::Request restore_file;
        restore_file.id = 20;
        restore_file.op = "trash.restore";
        restore_file.args["argv"] = nlohmann::json::array({file_id});
        FMT_CHECK(runtime.handle(restore_file).ok);
    }

    const fmt::PathManager paths{root};
    const auto document = fmt::read_json_file(paths.share_data());
    FMT_CHECK(fmt::ok(document));
    nlohmann::json value = std::get<nlohmann::json>(document);
    FMT_CHECK_EQ(value["shares"].size(), std::size_t{1});  // 只剩第二条（第一条被撤销）
    value["shares"][0]["expire_time"] = "2000-01-01T00:00:00";
    FMT_CHECK(fmt::ok(fmt::write_json_file(paths.share_data(), value)));

    fmt::ipc::Request expired;
    expired.id = 12;
    expired.op = "share.get";
    expired.args["argv"] =
        nlohmann::json::array({value["shares"][0].value("share_id", std::string{})});
    const fmt::ipc::Response expired_view = runtime.handle(expired);
    FMT_CHECK(expired_view.ok);
    FMT_CHECK_EQ(expired_view.data.value("state", std::string{}), std::string("已过期"));
    FMT_CHECK(expired_view.data.value("message", std::string{}).find("过期") != std::string::npos);
}

FMT_TEST(Service, 永久删除后分享如实报文件不存在) {
    fmt_test::TempDir temp("service-share-purged");
    const auto root = temp / "root";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(temp / "gone.txt", "gone soon")));

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request create_bucket;
    create_bucket.id = 1;
    create_bucket.op = "bucket.create";
    create_bucket.args["argv"] = nlohmann::json::array({"工作"});
    FMT_CHECK(runtime.handle(create_bucket).ok);

    fmt::ipc::Request upload;
    upload.id = 2;
    upload.op = "file.upload";
    upload.args["argv"] = nlohmann::json::array({fmt::path_to_utf8(temp / "gone.txt")});
    const fmt::ipc::Response uploaded = runtime.handle(upload);
    FMT_CHECK(uploaded.ok);
    const std::string file_id = uploaded.data.value("file_id", std::string{});
    FMT_CHECK(!file_id.empty());

    fmt::ipc::Request create;
    create.id = 3;
    create.op = "share.create";
    create.args["argv"] = nlohmann::json::array({file_id});
    const fmt::ipc::Response created = runtime.handle(create);
    FMT_CHECK(created.ok);
    const std::string share_id = created.data.value("share_id", std::string{});
    FMT_CHECK_EQ(share_id.size(), std::size_t{12});

    const auto view_share = [&runtime, &share_id](int id) {
        fmt::ipc::Request request;
        request.id = id;
        request.op = "share.get";
        request.args["argv"] = nlohmann::json::array({share_id});
        return runtime.handle(request);
    };

    // ---- ① 软删除：文件还在回收站里 → 「在回收站里」----
    fmt::ipc::Request soft_delete;
    soft_delete.id = 4;
    soft_delete.op = "file.delete";
    soft_delete.args["argv"] = nlohmann::json::array({file_id});
    FMT_CHECK(runtime.handle(soft_delete).ok);

    const fmt::ipc::Response in_trash = view_share(5);
    FMT_CHECK(in_trash.ok);  // 查得到（不是「不存在」），如实报告状态
    FMT_CHECK(!in_trash.data.value("available", true));
    FMT_CHECK_EQ(in_trash.data.value("state", std::string{}), std::string("关联文件不可用"));
    FMT_CHECK(in_trash.data.value("message", std::string{}).find("回收站") != std::string::npos);

    // ---- ② 恢复：文件回到正常状态 → 分享**重新可用**（不是"恢复了但分享仍废"）----
    fmt::ipc::Request restore;
    restore.id = 6;
    restore.op = "trash.restore";
    restore.args["argv"] = nlohmann::json::array({file_id});
    FMT_CHECK(runtime.handle(restore).ok);

    const fmt::ipc::Response restored = view_share(7);
    FMT_CHECK(restored.ok);
    FMT_CHECK(restored.data.value("available", false));
    FMT_CHECK_EQ(restored.data.value("state", std::string{}), std::string("可用"));

    // ---- ③ 再软删 + 永久删除：file.json 里那条记录被清掉 → 「已不存在」----
    // 与 ① 的**原因**必须分开：同样是「关联文件不可用」，一个说"在回收站里"、
    // 一个说"已不存在"，排障时这两句话指向完全不同的处理方式。
    fmt::ipc::Request soft_delete_again;
    soft_delete_again.id = 8;
    soft_delete_again.op = "file.delete";
    soft_delete_again.args["argv"] = nlohmann::json::array({file_id});
    FMT_CHECK(runtime.handle(soft_delete_again).ok);

    fmt::ipc::Request purge;
    purge.id = 9;
    purge.op = "trash.delete";
    purge.args["argv"] = nlohmann::json::array({file_id});
    purge.args["force"] = true;
    FMT_CHECK(runtime.handle(purge).ok);

    const fmt::ipc::Response gone = view_share(10);
    FMT_CHECK(gone.ok);
    FMT_CHECK(!gone.data.value("available", true));
    FMT_CHECK_EQ(gone.data.value("state", std::string{}), std::string("关联文件不可用"));
    FMT_CHECK(gone.data.value("message", std::string{}).find("已不存在") != std::string::npos);

    // 下载被同一道闸拦住：是 FMT-503（分享的关联文件不可用），不是 FMT-002 / 404
    fmt::ipc::Request download;
    download.id = 11;
    download.op = "share.download";
    download.args["argv"] = nlohmann::json::array({share_id});
    const fmt::ipc::Response refused = runtime.handle(download);
    FMT_CHECK(!refused.ok);
    FMT_CHECK(refused.error.code == fmt::ErrorCode::ShareFileUnavailable);
    FMT_CHECK_EQ(fmt::exit_code(refused.error.code), 5);
}

FMT_TEST(Service, 并发最后一次下载不会超发) {
    fmt_test::TempDir temp("service-share-race");
    const auto root = temp / "root";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(temp / "race.txt", "race me")));

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request create_bucket;
    create_bucket.id = 1;
    create_bucket.op = "bucket.create";
    create_bucket.args["argv"] = nlohmann::json::array({"工作"});
    FMT_CHECK(runtime.handle(create_bucket).ok);

    fmt::ipc::Request upload;
    upload.id = 2;
    upload.op = "file.upload";
    upload.args["argv"] = nlohmann::json::array({fmt::path_to_utf8(temp / "race.txt")});
    const fmt::ipc::Response uploaded = runtime.handle(upload);
    FMT_CHECK(uploaded.ok);
    const std::string file_id = uploaded.data.value("file_id", std::string{});
    FMT_CHECK(!file_id.empty());

    fmt::ipc::Request create;
    create.id = 3;
    create.op = "share.create";
    create.args["argv"] = nlohmann::json::array({file_id});
    const fmt::ipc::Response created = runtime.handle(create);
    FMT_CHECK(created.ok);
    const std::string share_id = created.data.value("share_id", std::string{});
    FMT_CHECK_EQ(share_id.size(), std::size_t{12});
    FMT_CHECK_EQ(created.data.value("max_download_count", 0), fmt::kDefaultShareDownloads);

    // 先下满 19 次，只剩最后一次
    const int nearly_done = fmt::kDefaultShareDownloads - 1;
    for (int i = 0; i < nearly_done; ++i) {
        fmt::ipc::Request request;
        request.id = 100 + i;
        request.op = "share.download";
        request.args["argv"] = nlohmann::json::array({share_id});
        FMT_CHECK(runtime.handle(request).ok);
    }

    // 两个线程同时抢最后一次（§51 的「19 + 两次 = 21」）：
    // 业务锁（ServerRuntime::handle 全程持 mutex_）保证「检查 + 计数 + 落盘」是一次完成的，
    // 所以只能有一个成功——没有锁的话这里会变成 21。
    std::atomic<int> succeeded{0};
    std::atomic<int> limited{0};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 2; ++worker) {
        threads.emplace_back([&runtime, &share_id, &succeeded, &limited, worker] {
            fmt::ipc::Request request;
            request.id = 200 + worker;
            request.op = "share.download";
            request.args["argv"] = nlohmann::json::array({share_id});
            const fmt::ipc::Response response = runtime.handle(request);
            if (response.ok) {
                ++succeeded;
            } else if (response.error.code == fmt::ErrorCode::ShareDownloadLimitReached) {
                ++limited;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    FMT_CHECK_EQ(succeeded.load(), 1);  // 最后一次只能给一个人
    FMT_CHECK_EQ(limited.load(), 1);    // 另一个必须是 FMT-502，而不是也成功

    fmt::ipc::Request final_view;
    final_view.id = 300;
    final_view.op = "share.get";
    final_view.args["argv"] = nlohmann::json::array({share_id});
    const fmt::ipc::Response view = runtime.handle(final_view);
    FMT_CHECK(view.ok);
    FMT_CHECK_EQ(view.data.value("download_count", -1), fmt::kDefaultShareDownloads);  // 20，不是 21
    FMT_CHECK_EQ(view.data.value("state", std::string{}), std::string("下载次数已用尽"));
    FMT_CHECK(!view.data.value("available", true));
}

FMT_TEST(Service, 未实现的操作与未知操作被明确拒绝) {
    fmt_test::TempDir temp("service-ops");
    fmt::service::ServerRuntime runtime(temp / "root", temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request business;
    business.id = 2;
    business.op = "server.nosuch";  // server.* 还没做（share 提交后已实现）
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

FMT_TEST(Service, 大文件分片会话可后台完成) {
    fmt_test::TempDir temp("service-upload-session");
    const auto root = temp / "root";
    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    const std::string salt = fmt::directory_salt("user");
    const std::string digest = fmt::password_digest("password", salt);
    fmt::ipc::Request create_user;
    create_user.id = 1;
    create_user.op = "user.create";
    create_user.args["username"] = "user";
    create_user.args["password_digest"] = digest;
    FMT_CHECK(runtime.handle(create_user).ok);

    fmt::ipc::Request create_bucket;
    create_bucket.id = 2;
    create_bucket.op = "bucket.create";
    create_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(create_bucket).ok);

    fmt::ipc::Request use_bucket;
    use_bucket.id = 20;
    use_bucket.op = "bucket.use";
    use_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(use_bucket).ok);

    const std::filesystem::path source = temp / "large.bin";
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        std::vector<char> block(256 * 1024, 'x');
        for (int i = 0; i < 8; ++i) {
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }

    fmt::ipc::Request start;
    start.id = 3;
    start.op = "upload.start";
    start.args["argv"] = nlohmann::json::array({source.string(), "large.bin"});
    start.args["chunk_size"] = 256 * 1024;
    const fmt::ipc::Response started = runtime.handle(start);
    if (!started.ok) {
        std::fprintf(stderr, "upload.start failed: %s\n", started.error.message.c_str());
        FMT_CHECK(started.ok);
        return;
    }
    const std::string upload_id = started.data.value("upload_id", std::string{});
    FMT_CHECK(!upload_id.empty());
    FMT_CHECK_EQ(started.data.value("chunk_count", std::uint64_t{0}), std::uint64_t{8});

    fmt::ipc::Response status;
    for (int i = 0; i < 300; ++i) {
        fmt::ipc::Request query;
        query.id = 100 + i;
        query.op = "upload.status";
        query.args["argv"] = nlohmann::json::array({upload_id});
        status = runtime.handle(query);
        if (!status.ok || status.data.value("state", std::string{}) == "completed") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    FMT_CHECK(status.ok);
    FMT_CHECK_EQ(status.data.value("state", std::string{}), std::string("completed"));
    FMT_CHECK(!status.data.value("final_file_id", std::string{}).empty());

    const std::filesystem::path session_dir =
        root / "temp" / "upload-sessions" / upload_id;
    FMT_CHECK(!std::filesystem::exists(session_dir / "chunks"));
}

FMT_TEST(Service, 大文件分片会话可取消) {
    fmt_test::TempDir temp("service-upload-cancel");
    const auto root = temp / "root";
    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    const std::string salt = fmt::directory_salt("user");
    const std::string digest = fmt::password_digest("password", salt);
    fmt::ipc::Request create_user;
    create_user.id = 1;
    create_user.op = "user.create";
    create_user.args["username"] = "user";
    create_user.args["password_digest"] = digest;
    FMT_CHECK(runtime.handle(create_user).ok);
    fmt::ipc::Request create_bucket;
    create_bucket.id = 2;
    create_bucket.op = "bucket.create";
    create_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(create_bucket).ok);

    fmt::ipc::Request use_bucket;
    use_bucket.id = 20;
    use_bucket.op = "bucket.use";
    use_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(use_bucket).ok);

    const std::filesystem::path source = temp / "cancel.bin";
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        out << "cancel-me";
    }
    fmt::ipc::Request start;
    start.id = 3;
    start.op = "upload.start";
    start.args["argv"] = nlohmann::json::array({source.string(), "cancel.bin"});
    start.args["chunk_size"] = 64 * 1024;
    const fmt::ipc::Response started = runtime.handle(start);
    if (!started.ok) {
        std::fprintf(stderr, "upload.start failed: %s\n", started.error.message.c_str());
        FMT_CHECK(started.ok);
        return;
    }
    const std::string upload_id = started.data.value("upload_id", std::string{});

    fmt::ipc::Request cancel;
    cancel.id = 4;
    cancel.op = "upload.cancel";
    cancel.args["argv"] = nlohmann::json::array({upload_id});
    const fmt::ipc::Response canceled = runtime.handle(cancel);
    FMT_CHECK(canceled.ok);
    FMT_CHECK(canceled.data.value("state", std::string{}) == "canceled" ||
              canceled.data.value("state", std::string{}) == "completed");

    // 给后台线程一个分片边界；最终状态只能是 canceled 或已抢先完成。
    for (int i = 0; i < 100; ++i) {
        fmt::ipc::Request query;
        query.id = 200 + i;
        query.op = "upload.status";
        query.args["argv"] = nlohmann::json::array({upload_id});
        const fmt::ipc::Response current = runtime.handle(query);
        if (!current.ok || current.data.value("state", std::string{}) != "uploading") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}


FMT_TEST(Service, 大文件内容与文件名验证) {
    fmt_test::TempDir temp("service-upload-validate");
    const auto root = temp / "root";
    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    const std::string salt = fmt::directory_salt("user");
    const std::string digest = fmt::password_digest("password", salt);
    fmt::ipc::Request create_user;
    create_user.id = 1;
    create_user.op = "user.create";
    create_user.args["username"] = "user";
    create_user.args["password_digest"] = digest;
    FMT_CHECK(runtime.handle(create_user).ok);
    fmt::ipc::Request create_bucket;
    create_bucket.id = 2;
    create_bucket.op = "bucket.create";
    create_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(create_bucket).ok);
    fmt::ipc::Request use_bucket;
    use_bucket.id = 20;
    use_bucket.op = "bucket.use";
    use_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(use_bucket).ok);

    const auto write_file = [&](const char* name, const std::string& text) {
        const std::filesystem::path path = temp / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
        return path;
    };
    const auto start = [&](const std::filesystem::path& path, const std::string& name) {
        fmt::ipc::Request request;
        request.id = 30;
        request.op = "upload.start";
        request.args["argv"] = nlohmann::json::array({path.string(), name});
        request.args["chunk_size"] = 64 * 1024;
        return runtime.handle(request);
    };
    const auto wait_completed = [&](const std::string& id) {
        for (int i = 0; i < 200; ++i) {
            fmt::ipc::Request query;
            query.id = 40 + i;
            query.op = "upload.status";
            query.args["argv"] = nlohmann::json::array({id});
            const fmt::ipc::Response current = runtime.handle(query);
            if (current.ok && current.data.value("state", std::string{}) == "completed") {
                return current;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return fmt::ipc::Response{};
    };

    const auto first = write_file("first.bin", "same-content");
    const fmt::ipc::Response first_start = start(first, "same.bin");
    FMT_CHECK(first_start.ok);
    const std::string first_id = first_start.data.value("upload_id", std::string{});
    const fmt::ipc::Response first_done = wait_completed(first_id);
    FMT_CHECK(first_done.ok);
    const std::string first_file = first_done.data.value("final_file_id", std::string{});
    FMT_CHECK(!first_file.empty());

    // 同一个本地文件再传：直接秒传复用同一个 upload_id，不新增会话。
    const fmt::ipc::Response reused = start(first, "same.bin");
    FMT_CHECK(reused.ok);
    FMT_CHECK(reused.data.value("reused", false));
    FMT_CHECK_EQ(reused.data.value("upload_id", std::string{}), first_id);

    // 同内容不同文件名：应直接复用 first_file，不重新传。
    const fmt::ipc::Response duplicate = start(first, "other-name.bin");
    FMT_CHECK(duplicate.ok);
    const fmt::ipc::Response duplicate_done =
        wait_completed(duplicate.data.value("upload_id", std::string{}));
    FMT_CHECK(duplicate_done.ok);
    FMT_CHECK_EQ(duplicate_done.data.value("final_file_id", std::string{}), first_file);

    // 同名不同内容：开始前就改成 same01.bin。
    const auto second = write_file("second.bin", "different-content");
    const fmt::ipc::Response conflict = start(second, "same.bin");
    FMT_CHECK(conflict.ok);
    const fmt::ipc::Response conflict_done =
        wait_completed(conflict.data.value("upload_id", std::string{}));
    FMT_CHECK(conflict_done.ok);
    FMT_CHECK_EQ(conflict_done.data.value("file_name", std::string{}),
                 std::string("same01.bin"));

    // 两个 completed 会话指向同一个 file_id 时，list 合并成一行。
    fmt::ipc::Request list;
    list.id = 50;
    list.op = "upload.list";
    const fmt::ipc::Response listed = runtime.handle(list);
    FMT_CHECK(listed.ok);
    FMT_CHECK_EQ(listed.data.value("count", std::size_t{0}), std::size_t{2});
    bool saw_duplicate = false;
    for (const auto& item : listed.data["uploads"]) {
        if (item.value("final_file_id", std::string{}) == first_file &&
            item.value("duplicate_count", 1) >= 2) {
            saw_duplicate = true;
        }
    }
    FMT_CHECK(saw_duplicate);

    // file delete <upload_id> 走软删除，file_id 不应再作为正常文件存在。
    fmt::ipc::Request remove;
    remove.id = 60;
    remove.op = "file.delete";
    remove.args["argv"] = nlohmann::json::array({first_id});
    remove.args["force"] = true;
    const fmt::ipc::Response removed = runtime.handle(remove);
    FMT_CHECK(removed.ok);

    fmt::ipc::Request get_file;
    get_file.id = 61;
    get_file.op = "file.get";
    get_file.args["argv"] = nlohmann::json::array({"same.bin"});
    FMT_CHECK(!runtime.handle(get_file).ok);

    fmt::ipc::Request after_delete;
    after_delete.id = 62;
    after_delete.op = "upload.list";
    const fmt::ipc::Response remaining = runtime.handle(after_delete);
    FMT_CHECK(remaining.ok);
    for (const auto& item : remaining.data["uploads"]) {
        FMT_CHECK(item.value("final_file_id", std::string{}) != first_file);
    }

    // 物理清空回收站：大文件会话记录要跟着删；同一文件的重复历史也一起删。
    fmt::ipc::Request purge;
    purge.id = 63;
    purge.op = "trash.delete";
    purge.args["argv"] = nlohmann::json::array({first_file});
    purge.args["force"] = true;
    const fmt::ipc::Response purged = runtime.handle(purge);
    FMT_CHECK(purged.ok);

    fmt::ipc::Request gone_status;
    gone_status.id = 64;
    gone_status.op = "upload.status";
    gone_status.args["argv"] = nlohmann::json::array({first_id});
    FMT_CHECK(!runtime.handle(gone_status).ok);

    fmt::ipc::Request after_purge;
    after_purge.id = 65;
    after_purge.op = "upload.list";
    const fmt::ipc::Response after_purge_list = runtime.handle(after_purge);
    FMT_CHECK(after_purge_list.ok);
    FMT_CHECK_EQ(after_purge_list.data.value("count", std::size_t{0}), std::size_t{1});

    // 非法文件名在 upload.start 就拒绝，不建会话、不传分片。
    const fmt::ipc::Response invalid = start(first, "fmt-20261021-1");
    FMT_CHECK(!invalid.ok);
    FMT_CHECK(invalid.error.code == fmt::ErrorCode::FileNameLikeFileId);
}

FMT_TEST(Service, 切Bucket后按upload_id删除仍用当前身份) {
    fmt_test::TempDir temp("service-upload-stale-bucket");
    const auto root = temp / "root";
    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    const std::string salt = fmt::directory_salt("user");
    const std::string digest = fmt::password_digest("password", salt);
    fmt::ipc::Request create_user;
    create_user.id = 1;
    create_user.op = "user.create";
    create_user.args["username"] = "user";
    create_user.args["password_digest"] = digest;
    FMT_CHECK(runtime.handle(create_user).ok);
    for (const char* name : {"work", "other"}) {
        fmt::ipc::Request create_bucket;
        create_bucket.id = 2;
        create_bucket.op = "bucket.create";
        create_bucket.args["argv"] = nlohmann::json::array({name});
        FMT_CHECK(runtime.handle(create_bucket).ok);
    }
    const auto use_bucket = [&](const char* name, int id) {
        fmt::ipc::Request request;
        request.id = id;
        request.op = "bucket.use";
        request.args["argv"] = nlohmann::json::array({name});
        return runtime.handle(request).ok;
    };
    FMT_CHECK(use_bucket("work", 3));

    const std::filesystem::path source = temp / "stale.bin";
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        out << std::string(200 * 1024, 's');
    }
    fmt::ipc::Request start;
    start.id = 4;
    start.op = "upload.start";
    start.args["argv"] = nlohmann::json::array({source.string(), "stale.bin"});
    start.args["chunk_size"] = 64 * 1024;
    const fmt::ipc::Response started = runtime.handle(start);
    FMT_CHECK(started.ok);
    const std::string upload_id = started.data.value("upload_id", std::string{});
    FMT_CHECK(!upload_id.empty());

    std::string state;
    for (int i = 0; i < 200; ++i) {
        fmt::ipc::Request query;
        query.id = 20 + i;
        query.op = "upload.status";
        query.args["argv"] = nlohmann::json::array({upload_id});
        const fmt::ipc::Response current = runtime.handle(query);
        if (current.ok) {
            state = current.data.value("state", std::string{});
            if (state == "completed") {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FMT_CHECK_EQ(state, std::string("completed"));

    // 切到 other，做一次 upload.*（把 UploadSessionManager 内部的 config_ 刷成 other），
    // 再切回 work；此时 file delete <upload_id> 必须按当前用户/桶重新解析，
    // 不能用旧缓存里那个 other 桶。
    FMT_CHECK(use_bucket("other", 300));
    fmt::ipc::Request list;
    list.id = 301;
    list.op = "upload.list";
    FMT_CHECK(runtime.handle(list).ok);
    FMT_CHECK(use_bucket("work", 302));

    fmt::ipc::Request remove;
    remove.id = 303;
    remove.op = "file.delete";
    remove.args["argv"] = nlohmann::json::array({upload_id});
    remove.args["force"] = true;
    FMT_CHECK(runtime.handle(remove).ok);

    fmt::ipc::Request get_file;
    get_file.id = 304;
    get_file.op = "file.get";
    get_file.args["argv"] = nlohmann::json::array({"stale.bin"});
    FMT_CHECK(!runtime.handle(get_file).ok);
}

FMT_TEST(Service, 大文件上传要求当前桶目录存在) {
    fmt_test::TempDir temp("service-upload-bucket-check");
    const auto root = temp / "root";
    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    const std::string salt = fmt::directory_salt("user");
    const std::string digest = fmt::password_digest("password", salt);
    fmt::ipc::Request create_user;
    create_user.id = 1;
    create_user.op = "user.create";
    create_user.args["username"] = "user";
    create_user.args["password_digest"] = digest;
    FMT_CHECK(runtime.handle(create_user).ok);
    fmt::ipc::Request create_bucket;
    create_bucket.id = 2;
    create_bucket.op = "bucket.create";
    create_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(create_bucket).ok);
    fmt::ipc::Request use_bucket;
    use_bucket.id = 3;
    use_bucket.op = "bucket.use";
    use_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(use_bucket).ok);

    const std::filesystem::path source = temp / "small.bin";
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        out << "bucket-check";
    }

    // 模拟 user delete / 手工删目录后 config 仍指向旧桶。
    std::error_code ignored;
    std::filesystem::remove_all(root / "repository" / "user" / "work", ignored);

    fmt::ipc::Request start;
    start.id = 4;
    start.op = "upload.start";
    start.args["argv"] = nlohmann::json::array({source.string(), "small.bin"});
    const fmt::ipc::Response rejected = runtime.handle(start);
    FMT_CHECK(!rejected.ok);
    FMT_CHECK(rejected.error.code == fmt::ErrorCode::NoCurrentBucket);

    fmt::ipc::Request list;
    list.id = 5;
    list.op = "upload.list";
    const fmt::ipc::Response listed = runtime.handle(list);
    FMT_CHECK(listed.ok);
    FMT_CHECK_EQ(listed.data.value("count", std::size_t{1}), std::size_t{0});
}

FMT_TEST(Service, 重载后清空当前账号与桶) {
    fmt_test::TempDir temp("service-reload");
    const auto root = temp / "root";
    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    const std::string salt = fmt::directory_salt("user");
    const std::string digest = fmt::password_digest("password", salt);
    fmt::ipc::Request create_user;
    create_user.id = 1;
    create_user.op = "user.create";
    create_user.args["username"] = "user";
    create_user.args["password_digest"] = digest;
    FMT_CHECK(runtime.handle(create_user).ok);
    fmt::ipc::Request create_bucket;
    create_bucket.id = 2;
    create_bucket.op = "bucket.create";
    create_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(create_bucket).ok);
    fmt::ipc::Request use_bucket;
    use_bucket.id = 3;
    use_bucket.op = "bucket.use";
    use_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(use_bucket).ok);

    std::error_code ignored;
    std::filesystem::remove_all(root / "repository", ignored);
    std::filesystem::remove_all(root / "data", ignored);
    std::filesystem::remove_all(root / "config", ignored);
    const fmt::PathManager paths(root);
    FMT_CHECK(fmt::ok(fmt::ensure_root(paths)));
    FMT_CHECK(fmt::ok(runtime.reload()));

    fmt::ipc::Request config;
    config.id = 4;
    config.op = "config.list";
    const fmt::ipc::Response listed = runtime.handle(config);
    FMT_CHECK(listed.ok);
    FMT_CHECK_EQ(listed.data.value("current_bucket", std::string{}), std::string{});
}

FMT_TEST(Service, 大文件暂停重启后从缺口继续) {
    fmt_test::TempDir temp("service-upload-resume");
    const auto root = temp / "root";
    const auto state = temp / "state";
    const std::string salt = fmt::directory_salt("user");
    const std::string digest = fmt::password_digest("password", salt);

    const std::filesystem::path source = temp / "resume.bin";
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        std::vector<char> block(1024 * 1024, 'r');
        for (int i = 0; i < 128; ++i) {
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }

    std::string upload_id;
    {
        fmt::service::ServerRuntime runtime(root, state);
        FMT_CHECK(fmt::ok(runtime.start()));

        fmt::ipc::Request create_user;
        create_user.id = 1;
        create_user.op = "user.create";
        create_user.args["username"] = "user";
        create_user.args["password_digest"] = digest;
        FMT_CHECK(runtime.handle(create_user).ok);
        fmt::ipc::Request create_bucket;
        create_bucket.id = 2;
        create_bucket.op = "bucket.create";
        create_bucket.args["argv"] = nlohmann::json::array({"work"});
        FMT_CHECK(runtime.handle(create_bucket).ok);
        fmt::ipc::Request use_bucket;
        use_bucket.id = 3;
        use_bucket.op = "bucket.use";
        use_bucket.args["argv"] = nlohmann::json::array({"work"});
        FMT_CHECK(runtime.handle(use_bucket).ok);

        fmt::ipc::Request start;
        start.id = 4;
        start.op = "upload.start";
        start.args["argv"] = nlohmann::json::array({source.string(), "resume.bin"});
        start.args["chunk_size"] = 1024 * 1024;
        const fmt::ipc::Response started = runtime.handle(start);
        FMT_CHECK(started.ok);
        upload_id = started.data.value("upload_id", std::string{});
        FMT_CHECK(!upload_id.empty());

        fmt::ipc::Request pause;
        pause.id = 5;
        pause.op = "upload.pause";
        pause.args["argv"] = nlohmann::json::array({upload_id});
        const fmt::ipc::Response paused = runtime.handle(pause);
        FMT_CHECK(paused.ok);
        FMT_CHECK_EQ(paused.data.value("state", std::string{}), std::string("paused"));
    }

    {
        fmt::service::ServerRuntime runtime(root, state);
        FMT_CHECK(fmt::ok(runtime.start()));

        fmt::ipc::Request query;
        query.id = 10;
        query.op = "upload.status";
        query.args["argv"] = nlohmann::json::array({upload_id});
        const fmt::ipc::Response before = runtime.handle(query);
        FMT_CHECK(before.ok);
        FMT_CHECK_EQ(before.data.value("state", std::string{}), std::string("paused"));

        fmt::ipc::Request resume;
        resume.id = 11;
        resume.op = "upload.resume";
        resume.args["argv"] = nlohmann::json::array({upload_id});
        FMT_CHECK(runtime.handle(resume).ok);

        fmt::ipc::Response done;
        for (int i = 0; i < 500; ++i) {
            fmt::ipc::Request poll;
            poll.id = 20 + i;
            poll.op = "upload.status";
            poll.args["argv"] = nlohmann::json::array({upload_id});
            done = runtime.handle(poll);
            if (!done.ok || done.data.value("state", std::string{}) == "completed") {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        FMT_CHECK(done.ok);
        FMT_CHECK_EQ(done.data.value("state", std::string{}), std::string("completed"));
        FMT_CHECK(!done.data.value("final_file_id", std::string{}).empty());
    }
}

FMT_TEST(Service, 大文件分片损坏会被发现并可重传) {
    fmt_test::TempDir temp("service-upload-corrupt");
    const auto root = temp / "root";
    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    const std::string salt = fmt::directory_salt("user");
    const std::string digest = fmt::password_digest("password", salt);
    fmt::ipc::Request create_user;
    create_user.id = 1;
    create_user.op = "user.create";
    create_user.args["username"] = "user";
    create_user.args["password_digest"] = digest;
    FMT_CHECK(runtime.handle(create_user).ok);
    fmt::ipc::Request create_bucket;
    create_bucket.id = 2;
    create_bucket.op = "bucket.create";
    create_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(create_bucket).ok);
    fmt::ipc::Request use_bucket;
    use_bucket.id = 3;
    use_bucket.op = "bucket.use";
    use_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(use_bucket).ok);

    const std::filesystem::path source = temp / "corrupt.bin";
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        std::vector<char> block(1024 * 1024, 'c');
        for (int i = 0; i < 256; ++i) {
            out.write(block.data(), static_cast<std::streamsize>(block.size()));
        }
    }

    fmt::ipc::Request start;
    start.id = 4;
    start.op = "upload.start";
    start.args["argv"] = nlohmann::json::array({source.string(), "corrupt.bin"});
    start.args["chunk_size"] = 1024 * 1024;
    const fmt::ipc::Response started = runtime.handle(start);
    FMT_CHECK(started.ok);
    const std::string upload_id = started.data.value("upload_id", std::string{});

    std::uint64_t corrupt_index = 0;
    for (int i = 0; i < 1000; ++i) {
        fmt::ipc::Request query;
        query.id = 10 + i;
        query.op = "upload.status";
        query.args["argv"] = nlohmann::json::array({upload_id});
        const fmt::ipc::Response current = runtime.handle(query);
        if (!current.ok) {
            break;
        }
        if (current.data.value("received_count", std::uint64_t{0}) > 0) {
            corrupt_index = current.data["received"][0].get<std::uint64_t>();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    FMT_CHECK(corrupt_index < 256);

    fmt::ipc::Request pause;
    pause.id = 5;
    pause.op = "upload.pause";
    pause.args["argv"] = nlohmann::json::array({upload_id});
    FMT_CHECK(runtime.handle(pause).ok);

    std::ostringstream chunk_name;
    chunk_name << std::setw(6) << std::setfill('0') << corrupt_index << ".part";
    const std::filesystem::path chunk =
        root / "temp" / "upload-sessions" / upload_id / "chunks" / chunk_name.str();
    {
        std::fstream file(chunk, std::ios::binary | std::ios::in | std::ios::out);
        FMT_CHECK(file.good());
        char byte = 0;
        file.read(&byte, 1);
        byte = static_cast<char>(byte ^ 0x5a);
        file.seekp(0);
        file.write(&byte, 1);
    }

    fmt::ipc::Request resume;
    resume.id = 6;
    resume.op = "upload.resume";
    resume.args["argv"] = nlohmann::json::array({upload_id});
    FMT_CHECK(runtime.handle(resume).ok);

    fmt::ipc::Response failed;
    for (int i = 0; i < 1000; ++i) {
        fmt::ipc::Request query;
        query.id = 100 + i;
        query.op = "upload.status";
        query.args["argv"] = nlohmann::json::array({upload_id});
        failed = runtime.handle(query);
        if (!failed.ok || failed.data.value("state", std::string{}) == "error" ||
            failed.data.value("state", std::string{}) == "completed") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FMT_CHECK(failed.ok);
    FMT_CHECK_EQ(failed.data.value("state", std::string{}), std::string("error"));
    FMT_CHECK(failed.data.value("error", std::string{}).find("校验失败") != std::string::npos);
    // 失败不是暂停：残片应全部清掉，只留 session.json 供 resume。
    FMT_CHECK(!std::filesystem::exists(root / "temp" / "upload-sessions" / upload_id / "chunks"));

    // 坏分片已被删除，再 resume 会重传并完成。
    bool resumed = false;
    for (int attempt = 0; attempt < 20 && !resumed; ++attempt) {
        fmt::ipc::Request again;
        again.id = 300 + attempt;
        again.op = "upload.resume";
        again.args["argv"] = nlohmann::json::array({upload_id});
        resumed = runtime.handle(again).ok;
        if (!resumed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    FMT_CHECK(resumed);

    fmt::ipc::Response done;
    for (int i = 0; i < 500; ++i) {
        fmt::ipc::Request query;
        query.id = 400 + i;
        query.op = "upload.status";
        query.args["argv"] = nlohmann::json::array({upload_id});
        done = runtime.handle(query);
        if (!done.ok || done.data.value("state", std::string{}) == "completed") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    FMT_CHECK(done.ok);
    FMT_CHECK_EQ(done.data.value("state", std::string{}), std::string("completed"));
}

FMT_TEST(Service, 账号密码登录签发临时会话) {
    fmt_test::TempDir temp("service-login");
    const auto root = temp / "root";

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    const std::string salt = fmt::directory_salt("user");
    const std::string digest = fmt::password_digest("password", salt);
    FMT_CHECK(!digest.empty());

    fmt::ipc::Request create_user;
    create_user.id = 1;
    create_user.op = "user.create";
    create_user.args["username"] = "user";
    create_user.args["password_digest"] = digest;
    const fmt::ipc::Response created = runtime.handle(create_user);
    FMT_CHECK(created.ok);
    if (!created.ok) {
        return;
    }

    fmt::ipc::Request create_bucket;
    create_bucket.id = 2;
    create_bucket.op = "bucket.create";
    create_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(create_bucket).ok);

    fmt::ipc::Request use_bucket;
    use_bucket.id = 20;
    use_bucket.op = "bucket.use";
    use_bucket.args["argv"] = nlohmann::json::array({"work"});
    FMT_CHECK(runtime.handle(use_bucket).ok);

    // 盐接口：客户端拿它算摘要
    const auto salt_result = runtime.salt_http("user");
    FMT_CHECK(fmt::ok(salt_result));
    if (fmt::ok(salt_result)) {
        FMT_CHECK_EQ(std::get<std::string>(salt_result), salt);
    }

    // 登录：摘要 + 明确的 Bucket
    const auto login = runtime.login_http("user", digest, "work");
    FMT_CHECK(fmt::ok(login));
    if (!fmt::ok(login)) {
        return;
    }
    const fmt::server::LoginResult& session = std::get<fmt::server::LoginResult>(login);
    FMT_CHECK(!session.token.empty());
    FMT_CHECK_EQ(session.username, std::string("user"));
    FMT_CHECK_EQ(session.bucket, std::string("work"));
    FMT_CHECK_EQ(session.token_type, std::string("session"));

    // 不填 Bucket：默认用服务端 current_bucket（这里已由第一个 bucket.create 设为 work）
    const auto login_current = runtime.login_http("user", digest, "");
    FMT_CHECK(fmt::ok(login_current));
    if (fmt::ok(login_current)) {
        FMT_CHECK_EQ(std::get<fmt::server::LoginResult>(login_current).bucket,
                     std::string("work"));
    }

    // 会话 token 能验出来，并且绑定 work
    const auto identity = runtime.verify_session_http(session.token);
    FMT_CHECK(fmt::ok(identity));
    if (fmt::ok(identity)) {
        const fmt::server::AuthIdentity& who = std::get<fmt::server::AuthIdentity>(identity);
        FMT_CHECK(who.session);
        FMT_CHECK_EQ(who.username, std::string("user"));
        FMT_CHECK_EQ(who.bucket, std::string("work"));
    }

    // 错误密码 / 不存在的 Bucket 都不能登录
    FMT_CHECK(!fmt::ok(runtime.login_http("user", std::string(64, '0'), "work")));
    FMT_CHECK(!fmt::ok(runtime.login_http("user", digest, "missing")));

    // 注销后会话立刻失效
    FMT_CHECK(fmt::ok(runtime.logout_http(session.token)));
    const auto after = runtime.verify_session_http(session.token);
    FMT_CHECK(!fmt::ok(after));
    if (!fmt::ok(after)) {
        FMT_CHECK(fmt::error_of(after)->code == fmt::ErrorCode::Unauthorized);
    }
}

FMT_TEST(Service, 请求级Bucket隔离) {
    fmt_test::TempDir temp("service-bucket-scope");
    const auto root = temp / "root";
    const auto source = temp / "payload.txt";
    FMT_CHECK(fmt::ok(fmt::write_text_file_atomic(source, "scoped")));

    fmt::service::ServerRuntime runtime(root, temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request create;
    create.id = 1;
    create.op = "bucket.create";
    create.args["argv"] = nlohmann::json::array({"工作"});
    FMT_CHECK(runtime.handle(create).ok);

    fmt::ipc::Request upload;
    upload.id = 2;
    upload.op = "file.upload";
    upload.args["argv"] = nlohmann::json::array({fmt::path_to_utf8(source)});
    const fmt::ipc::Response uploaded = runtime.handle(upload);
    FMT_CHECK(uploaded.ok);
    if (!uploaded.ok) {
        return;
    }
    const std::string file_id = uploaded.data.value("file_id", std::string{});

    fmt::ipc::Request other;
    other.id = 3;
    other.op = "bucket.create";
    other.args["argv"] = nlohmann::json::array({"other"});
    FMT_CHECK(runtime.handle(other).ok);

    // 不带 _bucket：仍用 current_bucket（工作），旧行为不变
    fmt::ipc::Request plain;
    plain.id = 4;
    plain.op = "file.list";
    const fmt::ipc::Response plain_list = runtime.handle(plain);
    FMT_CHECK(plain_list.ok);
    if (plain_list.ok) {
        FMT_CHECK_EQ(plain_list.data["files"].size(), std::size_t{1});
    }

    // 带 _bucket=other：同一个数据根里看不到「工作」的文件
    fmt::ipc::Request scoped;
    scoped.id = 5;
    scoped.op = "file.list";
    scoped.args["_bucket"] = "other";
    const fmt::ipc::Response scoped_list = runtime.handle(scoped);
    FMT_CHECK(scoped_list.ok);
    if (scoped_list.ok) {
        FMT_CHECK_EQ(scoped_list.data["files"].size(), std::size_t{0});
    }

    // 按 file_id 查也一样：other 里没有这个文件
    fmt::ipc::Request wrong_get;
    wrong_get.id = 6;
    wrong_get.op = "file.get";
    wrong_get.args["argv"] = nlohmann::json::array({file_id});
    wrong_get.args["_bucket"] = "other";
    const fmt::ipc::Response missing = runtime.handle(wrong_get);
    FMT_CHECK(!missing.ok);
    if (!missing.ok) {
        FMT_CHECK(missing.error.code == fmt::ErrorCode::FileNotFound);
    }

    // 显式指定「工作」则能查到
    fmt::ipc::Request right_get;
    right_get.id = 7;
    right_get.op = "file.get";
    right_get.args["argv"] = nlohmann::json::array({file_id});
    right_get.args["_bucket"] = "工作";
    const fmt::ipc::Response found = runtime.handle(right_get);
    FMT_CHECK(found.ok);
    if (found.ok) {
        FMT_CHECK_EQ(found.data.value("file_id", std::string{}), file_id);
    }
}
