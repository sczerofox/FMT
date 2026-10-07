#include "fmt/common/hash.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <new>

namespace fmt {
namespace {

constexpr std::size_t kDigestBytes = 16;  // MD5 固定 16 字节

std::string to_hex(const unsigned char* digest, std::size_t size) {
    constexpr char kDigits[] = "0123456789abcdef";

    std::string text;
    text.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        text.push_back(kDigits[digest[i] >> 4]);
        text.push_back(kDigits[digest[i] & 0x0F]);
    }
    return text;
}

}  // namespace

Md5::Md5() {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0) {
        failed_ = true;
        return;
    }
    algorithm_ = algorithm;

    // BCrypt 的哈希对象要调用方自己提供缓冲区，长度得先从算法属性里问。
    DWORD object_size = 0;
    DWORD written = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &written,
                          0) < 0 ||
        object_size == 0) {
        failed_ = true;
        return;
    }

    object_ = new (std::nothrow) unsigned char[object_size];
    if (object_ == nullptr) {
        failed_ = true;
        return;
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, static_cast<PUCHAR>(object_), object_size, nullptr, 0,
                         0) < 0) {
        failed_ = true;
        return;
    }
    hash_ = hash;
}

Md5::~Md5() {
    if (hash_ != nullptr) {
        BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    }
    if (algorithm_ != nullptr) {
        BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
    }
    delete[] static_cast<unsigned char*>(object_);
}

bool Md5::update(const void* data, std::size_t size) {
    if (failed_ || hash_ == nullptr) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_),
                       static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size),
                       0) < 0) {
        failed_ = true;
        return false;
    }
    return true;
}

bool Md5::update(std::string_view text) { return update(text.data(), text.size()); }

std::string Md5::finish() {
    if (failed_ || hash_ == nullptr) {
        return {};
    }

    unsigned char digest[kDigestBytes] = {};
    if (BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), digest, kDigestBytes, 0) < 0) {
        failed_ = true;
        return {};
    }
    return to_hex(digest, kDigestBytes);
}

std::string md5_hex(std::string_view text) {
    Md5 hash;
    if (!hash.update(text)) {
        return {};
    }
    return hash.finish();
}

}  // namespace fmt
