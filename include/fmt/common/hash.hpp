// MD5（Windows CNG / bcrypt）
//
// 上传要**边下载边算**，所以这里是增量接口，不是「一次给一整块」。
// 输出统一 32 位小写十六进制（开发文档第 36 节）。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fmt {

class Md5 {
public:
    Md5();
    ~Md5();

    Md5(const Md5&) = delete;
    Md5& operator=(const Md5&) = delete;

    // 追加数据。失败（算法不可用等极端情况）返回 false，之后 finish() 返回空串。
    bool update(const void* data, std::size_t size);
    bool update(std::string_view text);

    // 收尾并返回 32 位小写十六进制。只能调用一次。
    std::string finish();

private:
    void* algorithm_ = nullptr;  // BCRYPT_ALG_HANDLE
    void* hash_ = nullptr;       // BCRYPT_HASH_HANDLE
    void* object_ = nullptr;     // 哈希对象缓冲（BCrypt 要求调用方提供）
    bool failed_ = false;
};

// 一次性计算（小数据与测试用）。
std::string md5_hex(std::string_view text);

}  // namespace fmt
