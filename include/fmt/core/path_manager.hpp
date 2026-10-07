// 数据根的唯一定义处
//
// 所有实际文件路径都由 PathManager 生成，禁止用用户输入直接拼接路径
// （docx/FMT 开发文档.md §19）。
//
// 数据根由 CLI 在连接服务时声明，服务维护「当前数据根」；PathManager 本身
// 只负责一个根内的路径计算，不关心根是怎么来的。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "fmt/common/error.hpp"
#include "fmt/common/time.hpp"

namespace fmt {

class PathManager {
public:
    explicit PathManager(std::filesystem::path root);

    const std::filesystem::path& root() const { return root_; }

    // ---- 目录 ----
    std::filesystem::path repository() const { return root_ / L"repository"; }
    std::filesystem::path trash() const { return root_ / L"trash"; }
    std::filesystem::path config() const { return root_ / L"config"; }
    std::filesystem::path data() const { return root_ / L"data"; }
    std::filesystem::path log() const { return root_ / L"log"; }

    // ---- 文件 ----
    std::filesystem::path config_file() const { return config() / L"config.json"; }
    std::filesystem::path server_file() const { return config() / L"server.json"; }
    std::filesystem::path user_data() const { return data() / L"user.json"; }
    std::filesystem::path file_data() const { return data() / L"file.json"; }
    std::filesystem::path share_data() const { return data() / L"share.json"; }
    std::filesystem::path trash_data() const { return data() / L"trash.json"; }

    // 初始化要创建的目录，顺序固定（repository、trash、config、data、log）。
    static const std::vector<std::string>& required_directories();

    // repository/<user>/<bucket>/YYYY/MM/DD/<file_name>
    Result<std::filesystem::path> repository_file(std::string_view user, std::string_view bucket,
                                                  const DateParts& date,
                                                  std::string_view file_name) const;

    // trash/<user>/<bucket>/YYYY/MM/DD/<file_name>（保持原层级，便于恢复）
    Result<std::filesystem::path> trash_file(std::string_view user, std::string_view bucket,
                                             const DateParts& date,
                                             std::string_view file_name) const;

private:
    Result<std::filesystem::path> build(std::filesystem::path base, std::string_view user,
                                        std::string_view bucket, const DateParts& date,
                                        std::string_view file_name) const;

    std::filesystem::path root_;
};

}  // namespace fmt
