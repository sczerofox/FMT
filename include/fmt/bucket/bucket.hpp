// Bucket 业务规则
//
// 关键约定（开发文档第 27～30、54～57、61 节）：
//   * Bucket **没有独立 ID**：它就是 `repository/<user>/<bucket>/` 这个目录，
//     存在性等于目录存在；
//   * `current_bucket` 存在 `config.json` 里，`use` 只改它，不动 Bucket 本身；
//   * 删除 = 把整个目录移到 `trash/<user>/<名字>_<时间戳>/`，并在
//     `trash/<user>/.original` 里记一条桶级记录，**不创建 file_id**；
//   * 回收站里的名字**一律带时间戳**，所以同一个桶删多少次都不会互相覆盖；
//   * 回退是**整单判定**：原位置已有同名 Bucket 就拒绝（FMT-401），
//     不覆盖、不改名、不做「部分恢复」；
//   * 删除的是当前 Bucket 时置空，**不自动切换到别的 Bucket**；
//   * `current_bucket` 失效（不存在 / 不属于当前用户）时置空（第 61 节）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "fmt/common/error.hpp"
#include "fmt/common/logger.hpp"
#include "fmt/config/config.hpp"
#include "fmt/core/path_manager.hpp"

namespace fmt {

// 回收站里桶级记录的权威文件名（位于 `trash/<user>/` 下）。
inline constexpr const char* kOriginalIndexName = ".original";

struct BucketInfo {
    std::string name;
    bool is_current = false;
};

// 删除 Bucket 的结果：删到哪儿去了、影响了几个文件、删的是不是当前 Bucket。
// 调用方（管道/HTTP 响应）要如实回报这些，而不是只说一句「成功」。
struct BucketRemoval {
    std::filesystem::path moved_to;
    std::string trashed_name;  // trash/<user>/<trashed_name>
    std::size_t files_affected = 0;
    bool was_current = false;
};

// 回收站里的一个 Bucket 条目。
//
// **桶级记录的唯一权威是 `trash/<user>/.original`**：它跟着数据走，`data/*.json`
// 丢了也还在。名字带时间戳是**不能反推**原名的（`x_2026...` 也可能是本来就叫这个），
// 所以一律查这张表，不靠猜。
struct TrashBucket {
    std::string trashed_name;       // trash/<user>/<trashed_name>
    std::string original_name;      // 原来的桶名；索引里没有则为空（回退时拒绝）
    std::string deleted_at;
    bool directory_present = true;  // false = 索引里有、目录没了（异常，只报告）
};

// 单个回收站条目的详情（trash get 用）。
struct TrashBucketDetail {
    TrashBucket bucket;
    std::filesystem::path directory;
    std::size_t file_count = 0;
    std::uintmax_t byte_count = 0;
};

// 永久删除之后的结果。
struct TrashPurge {
    std::string trashed_name;
    std::string original_name;
    std::size_t removed_files = 0;    // 真正删掉的磁盘文件数
    std::size_t removed_records = 0;  // file.json 里清掉的记录数
};

class BucketService {
public:
    BucketService(const PathManager& paths, Config& config, Logger* logger);

    Status create(std::string_view name);
    Result<std::vector<BucketInfo>> list();
    Result<BucketInfo> get(std::string_view name);
    Status use(std::string_view name);
    Result<BucketRemoval> remove(std::string_view name);

    // 回收站里的 Bucket 列表（索引 + 目录扫描；异常状态如实报告，不擅自修）。
    Result<std::vector<TrashBucket>> list_trashed();

    // 回退一个被删除的 Bucket：整目录搬回 `repository/<user>/<原名>`。
    // identifier 可以是回收站里的名字，也可以是原桶名（同名多条时必须用前者）。
    Result<TrashBucket> restore(std::string_view identifier);

    // 单个条目的详情：目录、文件数、占用字节数。索引里有、目录没了的如实报告。
    Result<TrashBucketDetail> get_trashed(std::string_view identifier);

    // **永久删除，不可恢复**：删掉回收站目录本身、清掉该桶的 file.json 记录、
    // 摘掉索引条目。顺序刻意是「数据 → metadata → 索引」，中途失败都能重来。
    // 调用方必须先取得用户确认（服务端另外要求请求里带 force）。
    Result<TrashPurge> purge(std::string_view identifier);

    // Bucket 目录：repository/<user>/<bucket>。
    std::filesystem::path directory_of(std::string_view name) const;

    // 当前 Bucket 不存在时置空并落盘；不做任何自动切换。
    Status refresh_current_bucket();

private:
    // 在索引里定位条目：先用回收站里的名字精确匹配，再用原桶名（必须唯一）。
    // **找不到不算错**（found=false）——孤儿目录要能走到「按目录名处理」那条路。
    struct TrashLookup {
        bool found = false;
        std::size_t index = 0;
    };

    std::filesystem::path user_root() const;
    std::filesystem::path bucket_path(std::string_view name) const;
    std::filesystem::path trash_user_root() const;
    std::filesystem::path original_index_path() const;

    Status require_current_user() const;
    Status persist_config();
    Status move_bucket_to_trash(std::string_view name, const std::string& trashed_name,
                                std::filesystem::path* moved_to);
    // 该桶的文件记录批量改 is_trash；回退时只翻回「因桶被删除」而置位的那些，
    // 单个文件自己删过的不受影响（靠 trash_reason 区分）。
    Status set_bucket_files_trash_flag(std::string_view name, bool trashed,
                                       std::size_t* affected);

    Result<std::vector<TrashBucket>> load_original_index() const;
    Status save_original_index(const std::vector<TrashBucket>& entries) const;
    std::string unique_trashed_name(std::string_view name) const;
    Result<TrashLookup> find_trashed(const std::vector<TrashBucket>& entries,
                                     std::string_view identifier) const;
    // 永久删除时清掉 file.json 里「因桶被删」的记录（只清那一种，不动文件级删除的）。
    Status remove_bucket_file_records(std::string_view bucket_name, std::size_t* removed);

    const PathManager& paths_;
    Config& config_;
    Logger* logger_ = nullptr;
};

}  // namespace fmt
