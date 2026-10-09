// 测试用的临时目录：构造时创建，析构时整棵删除。
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace fmt_test {

class TempDir {
public:
    explicit TempDir(const std::string& tag) {
        ++counter_;
        const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("fmt-test-" + tag + "-" + std::to_string(ticks) + "-" + std::to_string(counter_));
        std::error_code code;
        std::filesystem::create_directories(path_, code);
    }

    ~TempDir() {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    std::filesystem::path operator/(const std::string& name) const { return path_ / name; }

private:
    static std::atomic<unsigned> counter_;

    std::filesystem::path path_;
};

inline std::atomic<unsigned> TempDir::counter_{0};

}  // namespace fmt_test
