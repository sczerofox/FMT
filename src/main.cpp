// FMT 入口点
//
// 阶段 3 会在这里接入 StartServiceCtrlDispatcherW，完成三种形态的分发
// （CLI / Service / 提权副本）；当前只有 CLI 形态的最小实现。
#include <windows.h>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fmt/common/error.hpp"
#include "fmt/version.hpp"

namespace {

// argv 在中文 Windows 上是 UTF-16，直接塞进窄字符串会丢字节。
std::string to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}

void print_version() {
    std::printf("%.*s\n", static_cast<int>(fmt::version::STRING.size()),
                fmt::version::STRING.data());
}

void print_usage() {
    std::printf(
        "FMT %.*s - Windows 文件管理系统\n"
        "\n"
        "用法：\n"
        "  fmt.exe --help              显示本帮助\n"
        "  fmt.exe --version           显示版本\n"
        "  fmt.exe service install     安装并启动服务\n"
        "  fmt.exe service uninstall   停止并删除服务\n"
        "  fmt.exe service start       启动服务\n"
        "  fmt.exe service stop        停止服务\n"
        "\n"
        "直接双击进入交互式命令行。\n"
        "\n"
        "退出码：0 成功  1 通用错误  2 参数错误  3 对象不存在  4 冲突\n"
        "        5 权限/访问  6 数据一致性  7 配置错误  8 Service 错误\n",
        static_cast<int>(fmt::version::STRING.size()), fmt::version::STRING.data());
}

// 处理一次性的命令行开关：返回退出码表示已处理完，返回空表示要继续进入
// 交互循环。（阶段 3 的 cli 模块会接管完整的命令表。）
std::optional<int> handle_one_shot(const std::vector<std::string>& args) {
    if (args.empty()) {
        return std::nullopt;
    }

    const std::string& arg = args.front();
    if (arg == "--help" || arg == "-h") {
        print_usage();
        return fmt::exit_code(fmt::ErrorCode::Ok);
    }
    if (arg == "--version" || arg == "-v") {
        print_version();
        return fmt::exit_code(fmt::ErrorCode::Ok);
    }

    const fmt::Error error = fmt::make_error(fmt::ErrorCode::InvalidArgument, "未知参数：" + arg);
    std::fprintf(stderr, "%s %s\n\n", fmt::code_string(error.code).c_str(),
                 error.message.c_str());
    print_usage();
    return fmt::exit_code(error.code);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // 控制台统一 UTF-8，避免中文输出乱码。
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) {
        args.push_back(to_utf8(argv[i]));
    }

    if (const std::optional<int> code = handle_one_shot(args)) {
        return *code;
    }

    // 阶段 2 结束时还没有交互循环：先打印版本与用法占位，
    // 阶段 3 的 cli 模块会接管这里（横幅 + 提示符 + 单实例 + 提权）。
    print_version();
    print_usage();
    return fmt::exit_code(fmt::ErrorCode::Ok);
}
