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

    // 再声明同一个根：不重复切换，仍然成功
    const fmt::ipc::Response again = runtime.handle(hello);
    FMT_CHECK(again.ok);
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

FMT_TEST(Service, 未实现的操作与未知操作被明确拒绝) {
    fmt_test::TempDir temp("service-ops");
    fmt::service::ServerRuntime runtime(temp / "root", temp / "state");
    FMT_CHECK(fmt::ok(runtime.start()));

    fmt::ipc::Request business;
    business.id = 2;
    business.op = "file.list";
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
