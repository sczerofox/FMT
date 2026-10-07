// Bucket 业务规则
//
// 关键约定（开发文档第 27～30、61 节）：
//   * Bucket **没有独立 ID**：它就是 `repository/<user>/<bucket>/` 这个目录，
//     存在性等于目录存在；
//   * `current_bucket` 存在 `config.json` 里，`use` 只改它，不动 Bucket 本身；
//   * 删除 = 把整个目录移到 `trash/<user>/<bucket>/` 并写一条 trash.json 记录，
//     **不创建 file_id**；
//   * 删除的是当前 Bucket 时置空，**不自动切换到别的 Bucket**；
//   * `current_bucket` 失效（不存在 / 不属于当前用户）时置空（第 61 节）。
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "fmt/common/error.hpp"
#include "fmt/common/logger.hpp"
#include "fmt/config/config.hpp"
#include "fmt/core/path_manager.hpp"

namespace fmt {

struct BucketInfo {
    std::string name;
    bool is_current = false;
};

// 删除 Bucket 的结果：删到哪儿去了、影响了几个文件、删的是不是当前 Bucket。
// 调用方（管道/HTTP 响应）要如实回报这些，而不是只说一句「成功」。
struct BucketRemoval {
    std::filesystem::path moved_to;
    std::size_t files_affected = 0;
    bool was_current = false;
};

class BucketService {
public:
    BucketService(const PathManager& paths, Config& config, Logger* logger);

    Status create(std::string_view name);
    Result<std::vector<BucketInfo>> list();
    Result<BucketInfo> get(std::string_view name);
    Status use(std::string_view name);
    Result<BucketRemoval> remove(std::string_view name);

    // Bucket 目录：repository/<user>/<bucket>。
    std::filesystem::path directory_of(std::string_view name) const;

    // 当前 Bucket 不存在时置空并落盘；不做任何自动切换。
    Status refresh_current_bucket();

private:
    std::filesystem::path user_root() const;
    std::filesystem::path bucket_path(std::string_view name) const;
    std::filesystem::path trash_bucket_path(std::string_view name) const;

    Status require_current_user() const;
    Status persist_config();
    Status move_bucket_to_trash(std::string_view name, std::filesystem::path* moved_to);
    Status mark_bucket_files_trashed(std::string_view name, std::size_t* affected);
    Status append_trash_record(std::string_view name, const std::filesystem::path& from,
                               const std::filesystem::path& to);

    const PathManager& paths_;
    Config& config_;
    Logger* logger_ = nullptr;
};

}  // namespace fmt
