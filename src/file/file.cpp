#include "fmt/file/file.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <random>
#include <system_error>
#include <utility>

#include "fmt/common/hash.hpp"
#include "fmt/common/http_client.hpp"
#include "fmt/common/string.hpp"
#include "fmt/common/time.hpp"
#include "fmt/common/validation.hpp"
#include "fmt/core/path.hpp"
#include "fmt/storage/storage.hpp"

namespace fmt {
namespace {

constexpr std::size_t kChunkBytes = 64 * 1024;
constexpr const char* kTrashReasonFile = "file";

// <数据根>/temp/fmt-upload-<随机>-<序号>.tmp
// `fmt-` 前缀会被服务启动时的 temp 清理顺手收走，不会越积越多。
std::filesystem::path unique_temp_path(const PathManager& paths) {
    static std::atomic<unsigned> sequence{0};
    static const unsigned token = std::random_device{}();
    return paths.temp() / ("fmt-upload-" + std::to_string(token) + "-" +
                           std::to_string(sequence.fetch_add(1)) + ".tmp");
}

// 失败路径上一定要把临时文件删掉（第 35 节）。
struct TempGuard {
    std::filesystem::path path;
    bool keep = false;

    ~TempGuard() {
        if (!keep && !path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};

// 同卷用 rename（原子）；跨卷（临时文件退到 %TEMP% 时可能）才复制 -> 校验大小 -> 删源。
Status move_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code code;
    std::filesystem::rename(from, to, code);
    if (!code) {
        return std::monostate{};
    }

    std::error_code copy_code;
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing,
                               copy_code);
    if (copy_code) {
        return make_error(ErrorCode::StorageError, "移动文件失败：" + path_to_utf8(from) + " -> " +
                                                       path_to_utf8(to) + "（" +
                                                       copy_code.message() + "）");
    }

    const std::uintmax_t from_size = std::filesystem::file_size(from, copy_code);
    const std::uintmax_t to_size = std::filesystem::file_size(to, copy_code);
    if (copy_code || from_size != to_size) {
        std::error_code ignored;
        std::filesystem::remove(to, ignored);
        return make_error(ErrorCode::StorageError, "跨卷复制后大小不一致，已放弃：" +
                                                       path_to_utf8(to));
    }

    std::filesystem::remove(from, copy_code);
    return std::monostate{};
}

std::string file_type_of(const std::string& extension) {
    static const std::pair<const char*, const char*> kTable[] = {
        {".jpg", "image"},  {".jpeg", "image"}, {".png", "image"},  {".gif", "image"},
        {".bmp", "image"},  {".webp", "image"}, {".svg", "image"},  {".ico", "image"},
        {".tif", "image"},  {".tiff", "image"},
        {".txt", "text"},   {".md", "text"},    {".log", "text"},   {".json", "text"},
        {".xml", "text"},   {".csv", "text"},   {".ini", "text"},   {".yml", "text"},
        {".yaml", "text"},  {".html", "text"},  {".htm", "text"},   {".css", "text"},
        {".js", "text"},    {".ts", "text"},    {".c", "text"},     {".h", "text"},
        {".hpp", "text"},   {".cpp", "text"},   {".py", "text"},    {".java", "text"},
        {".sh", "text"},    {".ps1", "text"},
        {".mp4", "video"},  {".avi", "video"},  {".mkv", "video"},  {".mov", "video"},
        {".wmv", "video"},  {".flv", "video"},  {".webm", "video"},
        {".zip", "archive"}, {".rar", "archive"}, {".7z", "archive"}, {".tar", "archive"},
        {".gz", "archive"}, {".bz2", "archive"}, {".xz", "archive"}, {".iso", "archive"},
    };
    for (const auto& [candidate, type] : kTable) {
        if (extension == candidate) {
            return type;
        }
    }
    return "other";  // 技术文档第 1000 行的取值集合是 image/text/video/archive/other
}

Result<DateParts> date_from_file_id(std::string_view file_id) {
    // fmt-YYYYMMDD-N（第 22 节）
    if (file_id.size() < 13 || file_id.substr(0, 4) != "fmt-" || file_id[12] != '-') {
        return make_error(ErrorCode::ConsistencyError, "file_id 形状不对：" + std::string(file_id));
    }
    const std::string stamp(file_id.substr(4, 8));
    for (const char ch : stamp) {
        if (ch < '0' || ch > '9') {
            return make_error(ErrorCode::ConsistencyError,
                              "file_id 里的日期不是数字：" + std::string(file_id));
        }
    }

    DateParts date;
    date.year = stamp.substr(0, 4);
    date.month = stamp.substr(4, 2);
    date.day = stamp.substr(6, 2);
    return date;
}

// 在 repository/<user>/<bucket>/YYYY/MM/DD/ 里找同名文件（桶下三层）。
void find_in_date_tree(const std::filesystem::path& base, const std::filesystem::path& file_name,
                       int depth, std::vector<std::filesystem::path>* found) {
    if (depth > 3 || found->size() > 1) {
        return;
    }

    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(base, code)) {
        if (code) {
            return;
        }
        std::error_code type_code;
        if (!entry.is_directory(type_code)) {
            continue;
        }
        if (depth == 3) {
            const std::filesystem::path candidate = entry.path() / file_name;
            if (file_exists(candidate)) {
                found->push_back(candidate);
            }
            continue;
        }
        find_in_date_tree(entry.path(), file_name, depth + 1, found);
    }
}

// ---- data/trash.json 的文件级条目（第 17.1 节）----

Status append_trash_record(const PathManager& paths, const FileRecord& record,
                           const std::string& original_path, const std::string& trash_path) {
    nlohmann::json document = make_collection(1, "trash");
    if (file_exists(paths.trash_data())) {
        Result<nlohmann::json> parsed = read_json_file(paths.trash_data());
        if (!ok(parsed)) {
            return *error_of(parsed);
        }
        document = std::get<nlohmann::json>(parsed);
        if (const Status version = check_version(document, 1); !ok(version)) {
            return version;
        }
    }

    nlohmann::json item = nlohmann::json::object();
    item["file_id"] = record.file_id;
    item["file_name"] = record.file_name;
    item["original_path"] = original_path;
    item["trash_path"] = trash_path;
    item["deleted_at"] = local_datetime_iso();
    item["type"] = kTrashReasonFile;
    document["trash"].push_back(std::move(item));
    return write_json_file(paths.trash_data(), document);
}

Status remove_trash_record(const PathManager& paths, std::string_view file_id) {
    if (!file_exists(paths.trash_data())) {
        return std::monostate{};
    }

    Result<nlohmann::json> parsed = read_json_file(paths.trash_data());
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    nlohmann::json document = std::get<nlohmann::json>(parsed);
    if (const Status version = check_version(document, 1); !ok(version)) {
        return version;
    }
    if (!document.contains("trash") || !document["trash"].is_array()) {
        return make_error(ErrorCode::JsonParseError, "trash.json 缺少 trash 数组");
    }

    nlohmann::json kept = nlohmann::json::array();
    for (const nlohmann::json& item : document["trash"]) {
        if (item.is_object() && item.value("file_id", std::string{}) == file_id) {
            continue;
        }
        kept.push_back(item);
    }
    document["trash"] = std::move(kept);
    return write_json_file(paths.trash_data(), document);
}

}  // namespace

// ---------------------------------------------------------------------------
// 文件名推断
// ---------------------------------------------------------------------------

std::string file_name_from_source(std::string_view source) {
    std::string text(source);

    // URL：先砍掉 scheme://host，只看路径部分。
    // **绝不能拿主机名当文件名**——`http://example.com/` 推断不出名字，
    // 这时应该让用户显式给名字，而不是存一个叫 example.com 的文件。
    if (const std::size_t scheme = text.find("://"); scheme != std::string::npos) {
        const std::size_t slash = text.find('/', scheme + 3);
        text = slash == std::string::npos ? std::string{} : text.substr(slash);
    }

    // 查询串与锚点不属于文件名
    if (const std::size_t cut = text.find_first_of("?#"); cut != std::string::npos) {
        text.erase(cut);
    }
    while (!text.empty() && (text.back() == '/' || text.back() == '\\')) {
        text.pop_back();
    }

    const std::size_t slash = text.find_last_of("/\\");
    std::string name = slash == std::string::npos ? text : text.substr(slash + 1);
    return url_decode(name);  // URL 里的中文是百分号编码的
}

std::string extension_of(const std::string& file_name) {
    const std::size_t dot = file_name.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= file_name.size()) {
        return {};  // ".bashrc" / "a." 都不算有扩展名
    }

    std::string extension = file_name.substr(dot);
    for (char& ch : extension) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return extension;
}

// ---------------------------------------------------------------------------
// 上传第一阶段（锁外）
// ---------------------------------------------------------------------------

Result<PreparedUpload> prepare_upload(const PathManager& paths, const std::string& source,
                                      const std::string& name, std::uintmax_t size_limit,
                                      Logger* logger) {
    if (source.empty()) {
        return make_error(ErrorCode::UrlInvalid, "缺少上传来源（http:// URL 或本地路径）");
    }

    PreparedUpload prepared;
    prepared.file_name = name.empty() ? file_name_from_source(source) : name;
    if (prepared.file_name.empty()) {
        return make_error(ErrorCode::FileNameEmpty, "无法从来源推断文件名，请显式给出文件名");
    }
    if (const Status status = validate_file_name(prepared.file_name); !ok(status)) {
        return *error_of(status);
    }

    const bool remote = is_remote_url(source);
    if (!remote && source.find("://") != std::string::npos) {
        // 长得像 URL 但不是 http/https：别当成「本地文件不存在」，那会误导排查。
        return make_error(ErrorCode::UrlInvalid,
                          "只支持 http:// 与 https:// 的来源：" + source);
    }
    if (!remote && !file_exists(path_from_utf8(source))) {
        return make_error(ErrorCode::FileNotFound, "本地文件不存在：" + source);
    }
    if (const Status status = ensure_directory(paths.temp()); !ok(status)) {
        return *error_of(status);
    }

    prepared.temp_path = unique_temp_path(paths);
    TempGuard guard{prepared.temp_path};

    std::ofstream out(prepared.temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return make_error(ErrorCode::StorageError,
                          "无法创建临时文件：" + path_to_utf8(prepared.temp_path));
    }

    enum class Stage { Ok, TooLarge, WriteFailed };
    Stage stage = Stage::Ok;
    Md5 hash;
    std::uintmax_t written = 0;

    const auto sink = [&](const char* data, std::size_t length) {
        if (written + length > size_limit) {
            stage = Stage::TooLarge;  // 边写边判，不把整个大文件拉完才发现
            return false;
        }
        out.write(data, static_cast<std::streamsize>(length));
        if (!out) {
            stage = Stage::WriteFailed;
            return false;
        }
        if (!hash.update(data, length)) {
            stage = Stage::WriteFailed;
            return false;
        }
        written += length;
        return true;
    };

    if (remote) {
        // 下载走 WinHTTP + Schannel：http 与 https 都支持，不需要 OpenSSL。
        HttpDownloadRequest request;
        request.url = source;

        const Result<HttpDownloadResult> downloaded = http_download(
            request, [&](const char* data, std::size_t length) { return sink(data, length); });

        if (stage == Stage::TooLarge) {
            return make_error(ErrorCode::SizeLimitExceeded,
                              "文件超过大小上限（" + std::to_string(size_limit) + " 字节）");
        }
        if (stage == Stage::WriteFailed) {
            return make_error(ErrorCode::IoError,
                              "写临时文件失败：" + path_to_utf8(prepared.temp_path));
        }
        if (!ok(downloaded)) {
            return *error_of(downloaded);
        }

        const HttpDownloadResult& response = std::get<HttpDownloadResult>(downloaded);
        if (response.status != 200) {
            return make_error(ErrorCode::DownloadFailed,
                              "下载失败：HTTP " + std::to_string(response.status) + " " + source);
        }
        // 第 7 步：完整性检查。服务器给了 Content-Length 就必须对上。
        if (response.has_content_length && response.content_length != written) {
            return make_error(ErrorCode::DownloadFailed,
                              "下载不完整：声明 " + std::to_string(response.content_length) +
                                  " 字节，实收 " + std::to_string(written) + " 字节");
        }
    } else {
        std::ifstream in(path_from_utf8(source), std::ios::binary);
        if (!in) {
            return make_error(ErrorCode::FileNotFound, "无法读取本地文件：" + source);
        }

        std::vector<char> buffer(kChunkBytes);
        while (in.good()) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize got = in.gcount();
            if (got <= 0) {
                break;
            }
            if (!sink(buffer.data(), static_cast<std::size_t>(got))) {
                break;
            }
        }
        if (stage == Stage::TooLarge) {
            return make_error(ErrorCode::SizeLimitExceeded,
                              "文件超过大小上限（" + std::to_string(size_limit) + " 字节）");
        }
        if (stage == Stage::WriteFailed || in.bad()) {
            return make_error(ErrorCode::IoError, "读取或写入失败：" + source);
        }
    }

    out.flush();
    if (!out) {
        return make_error(ErrorCode::IoError, "写临时文件失败：" + path_to_utf8(prepared.temp_path));
    }
    out.close();

    prepared.size = written;
    prepared.md5 = hash.finish();
    if (prepared.md5.empty()) {
        return make_error(ErrorCode::StorageError, "MD5 计算失败：" + path_to_utf8(prepared.temp_path));
    }

    guard.keep = true;  // 交给第二阶段消费
    if (logger != nullptr) {
        logger->info("File", "上传暂存完成：" + prepared.file_name + "（" +
                                 std::to_string(prepared.size) + " 字节，" + prepared.md5 + "）");
    }
    return prepared;
}

// ---------------------------------------------------------------------------
// FileService
// ---------------------------------------------------------------------------

FileService::FileService(const PathManager& paths, Config& config, Logger* logger)
    : paths_(paths), config_(config), logger_(logger) {}

std::filesystem::path FileService::bucket_path() const {
    return paths_.repository() / path_from_utf8(config_.current_user) /
           path_from_utf8(config_.current_bucket);
}

Result<std::vector<FileRecord>> FileService::load_records() const {
    std::vector<FileRecord> records;
    if (!file_exists(paths_.file_data())) {
        return records;
    }

    Result<nlohmann::json> parsed = read_json_file(paths_.file_data());
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    const nlohmann::json& document = std::get<nlohmann::json>(parsed);

    if (const Status version = check_version(document, 1); !ok(version)) {
        return *error_of(version);
    }
    const auto list = document.find("files");
    if (list == document.end() || !list->is_array()) {
        return make_error(ErrorCode::JsonParseError, "file.json 缺少 files 数组");
    }

    for (const nlohmann::json& item : *list) {
        if (!item.is_object()) {
            continue;
        }
        FileRecord record;
        record.file_id = item.value("file_id", std::string{});
        if (record.file_id.empty()) {
            continue;
        }
        record.user = item.value("user", std::string{});
        record.bucket = item.value("bucket", std::string{});
        record.file_name = item.value("file_name", std::string{});
        record.extension = item.value("extension", std::string{});
        record.file_type = item.value("file_type", std::string{});
        const long long size = item.value("size", 0LL);
        record.size = size > 0 ? static_cast<std::uintmax_t>(size) : 0;
        record.md5 = item.value("md5", std::string{});
        record.is_trash = item.value("is_trash", false);
        record.trash_reason = item.value("trash_reason", std::string{});
        records.push_back(std::move(record));
    }
    return records;
}

Status FileService::save_records(const std::vector<FileRecord>& records) const {
    nlohmann::json document = nlohmann::json::object();
    document["version"] = 1;
    document["files"] = nlohmann::json::array();

    for (const FileRecord& record : records) {
        nlohmann::json item = nlohmann::json::object();
        item["file_id"] = record.file_id;
        item["user"] = record.user;
        item["bucket"] = record.bucket;
        item["file_name"] = record.file_name;
        item["extension"] = record.extension;
        item["file_type"] = record.file_type;
        item["size"] = record.size;
        item["md5"] = record.md5;
        item["is_trash"] = record.is_trash;
        item["trash_reason"] = record.trash_reason;
        document["files"].push_back(std::move(item));
    }
    return write_json_file(paths_.file_data(), document);
}

std::string FileService::next_file_id(const std::vector<FileRecord>& records) const {
    const std::string prefix = "fmt-" + local_date_compact() + "-";

    // 取当天最大序号 + 1：不用「条数」是因为删除过的 id 不能被复用（第 23 节）。
    unsigned long long next = 0;
    for (const FileRecord& record : records) {
        if (record.file_id.rfind(prefix, 0) != 0) {
            continue;
        }
        const std::string number = record.file_id.substr(prefix.size());
        if (number.empty() || !std::all_of(number.begin(), number.end(), [](char ch) {
                return ch >= '0' && ch <= '9';
            })) {
            continue;
        }
        try {
            next = std::max(next, std::stoull(number) + 1);
        } catch (const std::exception&) {
            continue;  // 数字大到溢出的 id：跳过，不影响别的
        }
    }
    return prefix + std::to_string(next);
}

Result<FileRecord> FileService::commit_upload(PreparedUpload& prepared) {
    // 从这个函数接手开始，临时文件的生命周期就归它管：
    // 任何一条失败路径都必须把 temp/ 里的暂存数据清掉（第 35 节）。
    TempGuard guard{prepared.temp_path};

    if (config_.current_user.empty()) {
        return make_error(ErrorCode::NoCurrentUser, "未设置当前用户");
    }
    if (config_.current_bucket.empty()) {
        return make_error(ErrorCode::NoCurrentBucket, "未设置当前 Bucket，请先 bucket create / bucket use");
    }
    if (!directory_exists(bucket_path())) {
        return make_error(ErrorCode::BucketNotFound, "当前 Bucket 的目录不存在：" + config_.current_bucket);
    }
    if (!file_exists(prepared.temp_path)) {
        return make_error(ErrorCode::StorageError,
                          "临时文件已不存在：" + path_to_utf8(prepared.temp_path));
    }
    if (const Status status = validate_file_name(prepared.file_name); !ok(status)) {
        return *error_of(status);
    }

    Result<std::vector<FileRecord>> loaded = load_records();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    std::vector<FileRecord> records = std::get<std::vector<FileRecord>>(loaded);

    // 第 37 节：同用户已有相同 MD5 就终止，不新建 id、不复制文件、不改旧名字。
    for (const FileRecord& record : records) {
        if (record.user == config_.current_user && !record.is_trash &&
            !record.md5.empty() && record.md5 == prepared.md5) {
            return make_error(ErrorCode::Md5Duplicate,
                              "该文件已经存在：" + record.file_name + "（" + record.file_id + "）");
        }
    }
    // 第 38 节：同用户 + 正常文件 + 同名 -> 拒绝，**不自动改名**。
    //
    // 名字比较**不区分大小写**：Windows 的文件名不区分大小写，`DOC.TXT` 与 `doc.txt`
    // 会落到同一个磁盘路径上。按精确匹配放过的话，第二次上传会**直接覆盖**第一个文件的
    // 字节，而 file.json 里两条记录的 size/md5 各说各话——这是数据损坏，不是显示问题。
    for (const FileRecord& record : records) {
        if (record.user == config_.current_user && !record.is_trash &&
            iequals(record.file_name, prepared.file_name)) {
            return make_error(ErrorCode::FileNameConflict,
                              "同名文件已存在：" + record.file_name +
                                  "（换一个文件名再上传）");
        }
    }

    FileRecord record;
    record.file_id = next_file_id(records);
    record.user = config_.current_user;
    record.bucket = config_.current_bucket;
    record.file_name = prepared.file_name;
    record.extension = extension_of(prepared.file_name);
    record.file_type = file_type_of(record.extension);
    record.size = prepared.size;
    record.md5 = prepared.md5;
    record.is_trash = false;

    Result<DateParts> date = date_from_file_id(record.file_id);
    if (!ok(date)) {
        return *error_of(date);
    }
    Result<std::filesystem::path> target = paths_.repository_file(
        record.user, record.bucket, std::get<DateParts>(date), record.file_name);
    if (!ok(target)) {
        return *error_of(target);
    }
    const std::filesystem::path destination = std::get<std::filesystem::path>(target);
    if (const Status status = ensure_directory(destination.parent_path()); !ok(status)) {
        return *error_of(status);
    }
    // 第 39 节：临时文件 -> repository -> file.json。
    if (const Status status = move_file(prepared.temp_path, destination); !ok(status)) {
        return *error_of(status);
    }
    guard.keep = true;  // 已经搬进仓库，别再按临时文件删

    records.push_back(record);
    if (const Status status = save_records(records); !ok(status)) {
        // 第 40 节：file.json 写不进去就把刚提交的仓库文件删掉，绝不报「上传成功」。
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        if (logger_ != nullptr) {
            logger_->error("File", "写 file.json 失败，已回滚仓库文件：" +
                                       path_to_utf8(destination));
        }
        return *error_of(status);
    }

    if (logger_ != nullptr) {
        logger_->info("File", "入库：" + record.file_id + " " + record.file_name + " -> " +
                                  path_to_utf8(destination));
    }
    return record;
}

Result<std::vector<FileRecord>> FileService::list() {
    if (config_.current_user.empty()) {
        return make_error(ErrorCode::NoCurrentUser, "未设置当前用户");
    }
    if (config_.current_bucket.empty()) {
        return make_error(ErrorCode::NoCurrentBucket, "未设置当前 Bucket");
    }

    Result<std::vector<FileRecord>> loaded = load_records();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }

    std::vector<FileRecord> files;
    for (FileRecord& record : std::get<std::vector<FileRecord>>(loaded)) {
        // 第 41 节：当前用户 + 当前 Bucket + 正常文件
        if (record.user == config_.current_user && record.bucket == config_.current_bucket &&
            !record.is_trash) {
            files.push_back(std::move(record));
        }
    }
    std::sort(files.begin(), files.end(), [](const FileRecord& left, const FileRecord& right) {
        return left.file_id < right.file_id;  // 按入库顺序
    });
    return files;
}

Result<FileRecord> FileService::get_by_id(std::string_view file_id) {
    if (file_id.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少 file_id");
    }

    Result<std::vector<FileRecord>> loaded = load_records();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    // 第 42 节：按 file_id 查是全局唯一，不限当前用户/Bucket
    for (const FileRecord& record : std::get<std::vector<FileRecord>>(loaded)) {
        if (record.file_id == file_id) {
            return record;
        }
    }
    return make_error(ErrorCode::FileNotFound, "文件不存在：" + std::string(file_id));
}

Result<FileRecord> FileService::get_by_name(std::string_view file_name) {
    if (file_name.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少文件名");
    }

    Result<std::vector<FileRecord>> loaded = load_records();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    for (const FileRecord& record : std::get<std::vector<FileRecord>>(loaded)) {
        if (record.user == config_.current_user && !record.is_trash &&
            iequals(record.file_name, file_name)) {
            return record;
        }
    }
    return make_error(ErrorCode::FileNotFound, "文件不存在：" + std::string(file_name));
}

Result<std::filesystem::path> FileService::resolve_path(const FileRecord& record) const {
    Result<DateParts> date = date_from_file_id(record.file_id);
    if (!ok(date)) {
        return *error_of(date);
    }

    Result<std::filesystem::path> derived = paths_.repository_file(
        record.user, record.bucket, std::get<DateParts>(date), record.file_name);
    if (!ok(derived)) {
        return *error_of(derived);
    }
    const std::filesystem::path path = std::get<std::filesystem::path>(derived);
    if (file_exists(path)) {
        return path;
    }

    // 兜底：日期对不上（记录被手工改过、或文件被挪过）时在桶的日期树里找同名文件。
    // 唯一命中才认，多个命中说明有歧义，交给人处理。
    std::vector<std::filesystem::path> found;
    const std::filesystem::path base = paths_.repository() / path_from_utf8(record.user) /
                                       path_from_utf8(record.bucket);
    find_in_date_tree(base, path_from_utf8(record.file_name), 1, &found);
    if (found.size() == 1) {
        return found.front();
    }
    if (found.size() > 1) {
        return make_error(ErrorCode::ConsistencyError,
                          "同名文件在日期树里有多份，无法确定是哪一份：" + record.file_name);
    }
    return make_error(ErrorCode::FileNotFound,
                      "磁盘上找不到文件：" + path_to_utf8(path));
}

Result<std::filesystem::path> FileService::trash_path_of(const FileRecord& record) const {
    Result<DateParts> date = date_from_file_id(record.file_id);
    if (!ok(date)) {
        return *error_of(date);
    }
    return paths_.trash_file(record.user, record.bucket, std::get<DateParts>(date),
                             record.file_name);
}

Result<std::size_t> FileService::locate_record(const std::vector<FileRecord>& records,
                                               std::string_view key) const {
    // 标识比较**一律不区分大小写**（Windows 习惯：文件名与路径都不区分大小写，
    // 用户敲 `DOC.TXT` 或大写 file_id 时不该得到「文件不存在」）。

    // ① 先当 file_id：全局唯一，形状固定（fmt-YYYYMMDD-N），先查不会误伤名字。
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (iequals(records[i].file_id, key)) {
            return i;
        }
    }

    // ② 再当文件名：当前用户 + 正常文件。
    //    名字在同用户范围内唯一（重名上传会被 FMT-105 拒绝，且比较不区分大小写），
    //    所以不会撞出歧义。
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i].user == config_.current_user && !records[i].is_trash &&
            iequals(records[i].file_name, key)) {
            return i;
        }
    }

    // ③ 名字存在、但已经在回收站里：说清楚是哪一条，别让用户以为文件没了。
    for (const FileRecord& record : records) {
        if (record.user == config_.current_user && record.is_trash &&
            iequals(record.file_name, key)) {
            return make_error(ErrorCode::InvalidArgument,
                              "该文件已经在回收站里：" + std::string(key) + "（file_id " +
                                  record.file_id + "）");
        }
    }

    return make_error(ErrorCode::FileNotFound, "文件不存在：" + std::string(key));
}

Result<FileRecord> FileService::remove(std::string_view file_id_or_name) {
    if (config_.current_user.empty()) {
        return make_error(ErrorCode::NoCurrentUser, "未设置当前用户");
    }
    if (file_id_or_name.empty()) {
        return make_error(ErrorCode::InvalidArgument, "缺少 file_id 或文件名");
    }

    Result<std::vector<FileRecord>> loaded = load_records();
    if (!ok(loaded)) {
        return *error_of(loaded);
    }
    std::vector<FileRecord> records = std::get<std::vector<FileRecord>>(loaded);

    Result<std::size_t> located = locate_record(records, file_id_or_name);
    if (!ok(located)) {
        return *error_of(located);
    }
    const std::size_t index = std::get<std::size_t>(located);

    if (records[index].user != config_.current_user) {
        return make_error(ErrorCode::PermissionDenied, "这个文件不属于当前用户");
    }
    if (records[index].is_trash) {
        return make_error(ErrorCode::InvalidArgument,
                          "该文件已经在回收站里：" + records[index].file_id);
    }

    Result<std::filesystem::path> source = resolve_path(records[index]);
    if (!ok(source)) {
        return *error_of(source);
    }
    Result<std::filesystem::path> target = trash_path_of(records[index]);
    if (!ok(target)) {
        return *error_of(target);
    }
    const std::filesystem::path from = std::get<std::filesystem::path>(source);
    const std::filesystem::path to = std::get<std::filesystem::path>(target);

    if (const Status status = ensure_directory(to.parent_path()); !ok(status)) {
        return *error_of(status);
    }
    // 第 43 节：移动到 Trash -> is_trash = true -> 更新 trash.json。
    if (const Status status = move_file(from, to); !ok(status)) {
        return *error_of(status);
    }

    if (const Status status =
            append_trash_record(paths_, records[index],
                                relative_path_text(paths_.root(), from),
                                relative_path_text(paths_.root(), to));
        !ok(status)) {
        std::error_code ignored;
        std::filesystem::rename(to, from, ignored);  // 回滚
        return *error_of(status);
    }

    records[index].is_trash = true;
    records[index].trash_reason = kTrashReasonFile;
    if (const Status status = save_records(records); !ok(status)) {
        // 回滚：先撤掉 trash 记录，再把文件搬回仓库
        const Status undone = remove_trash_record(paths_, records[index].file_id);
        std::error_code ignored;
        std::filesystem::rename(to, from, ignored);
        if (logger_ != nullptr) {
            logger_->error("File", "写 file.json 失败，已回滚软删除" +
                                       (ok(undone) ? std::string{} : "（trash.json 回滚也失败）"));
        }
        return *error_of(status);
    }

    if (logger_ != nullptr) {
        logger_->info("File", "软删除：" + records[index].file_id + " " + records[index].file_name +
                                  " -> " + path_to_utf8(to));
    }
    return records[index];
}

}  // namespace fmt
