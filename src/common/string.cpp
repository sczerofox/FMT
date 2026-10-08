#include "fmt/common/string.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

namespace fmt {

std::string to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), length, result.data(), size);
    return result;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto a = static_cast<unsigned char>(left[i]);
        const auto b = static_cast<unsigned char>(right[i]);
        // 只折叠 ASCII：>= 0x80 的字节是 UTF-8 多字节序列的一部分，
        // 交给 std::tolower 会随 locale 变化，中文名字可能被改坏。
        const auto fold = [](unsigned char value) {
            if (value < 0x80) {
                return static_cast<unsigned char>(std::tolower(value));
            }
            return value;
        };
        if (fold(a) != fold(b)) {
            return false;
        }
    }
    return true;
}

std::string to_lower(std::string_view text) {
    std::string result(text);
    for (char& ch : result) {
        const auto value = static_cast<unsigned char>(ch);
        if (value < 0x80) {
            ch = static_cast<char>(std::tolower(value));
        }
    }
    return result;
}

std::string trim(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\r\n";
    const std::size_t begin = text.find_first_not_of(kWhitespace);
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(kWhitespace);
    return std::string(text.substr(begin, end - begin + 1));
}

namespace {

// 粘贴路径会带进来的不可见字符。它们都在 BMP 内、是单个码位，
// 所以先转 UTF-16 再按码位过滤最省事（代理对不在这个集合里，会原样保留）。
bool is_invisible_codepoint(wchar_t code) {
    switch (code) {
        case 0x00A0:  // 不换行空格（网页复制常见）
        case 0x00AD:  // 软连字符
        case 0x200B:  // 零宽空格
        case 0x200C:  // 零宽非连字
        case 0x200D:  // 零宽连字
        case 0x200E:  // 从左到右标记
        case 0x200F:  // 从右到左标记
        case 0x202A:  // 从左到右嵌入  ← 用户这次遇到的
        case 0x202B:  // 从右到左嵌入
        case 0x202C:  // 方向格式弹出  ← 与 202A 成对出现
        case 0x202D:  // 从左到右覆盖
        case 0x202E:  // 从右到左覆盖
        case 0x2060:  // 词连接符
        case 0x2061:
        case 0x2062:
        case 0x2063:
        case 0x2064:
        case 0x2066:  // 方向隔离
        case 0x2067:
        case 0x2068:
        case 0x2069:
        case 0xFEFF:  // BOM / 零宽不换行空格
            return true;
        default:
            return false;
    }
}

std::string codepoint_name(wchar_t code) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "U+%04X", static_cast<unsigned>(code));
    return std::string(buffer);
}

}  // namespace

std::string clean_user_path(std::string_view text) {
    std::string cleaned;
    const std::wstring wide = to_wide(text);
    if (wide.empty() && !text.empty()) {
        // 不是合法 UTF-8：不做码位过滤，只做首尾清理，别把内容弄丢
        cleaned = trim(text);
    } else {
        std::wstring kept;
        kept.reserve(wide.size());
        for (const wchar_t code : wide) {
            if (!is_invisible_codepoint(code)) {
                kept.push_back(code);
            }
        }
        cleaned = trim(to_utf8(kept));
    }

    // **成对**引号才去掉：既覆盖 Explorer 的「复制路径」，又不动名字里真的带引号的情况
    const auto strip_pair = [&cleaned](std::string_view open, std::string_view close) {
        if (cleaned.size() < open.size() + close.size()) {
            return false;
        }
        if (cleaned.compare(0, open.size(), open) != 0) {
            return false;
        }
        if (cleaned.compare(cleaned.size() - close.size(), close.size(), close) != 0) {
            return false;
        }
        cleaned = std::string(std::string_view(cleaned).substr(
            open.size(), cleaned.size() - open.size() - close.size()));
        return true;
    };
    if (strip_pair("\"", "\"") || strip_pair("'", "'") ||
        strip_pair("\xE2\x80\x9C", "\xE2\x80\x9D")) {
        cleaned = trim(cleaned);
    }
    return cleaned;
}

std::vector<std::string> invisible_characters(std::string_view text) {
    std::vector<std::string> found;
    for (const wchar_t code : to_wide(text)) {
        if (!is_invisible_codepoint(code)) {
            continue;
        }
        const std::string name = codepoint_name(code);
        if (std::find(found.begin(), found.end(), name) == found.end()) {
            found.push_back(name);
        }
    }
    return found;
}

std::vector<std::string> split(std::string_view text, char delimiter) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (true) {
        const std::size_t position = text.find(delimiter, begin);
        if (position == std::string_view::npos) {
            parts.emplace_back(text.substr(begin));
            break;
        }
        parts.emplace_back(text.substr(begin, position - begin));
        begin = position + 1;
    }
    return parts;
}

std::string format_size(std::uint64_t bytes) {
    constexpr std::array<std::string_view, 5> kUnits{"B", "KB", "MB", "GB", "TB"};

    std::size_t unit = 0;
    double value = static_cast<double>(bytes);
    while (value >= 1024.0 && unit + 1 < kUnits.size()) {
        value /= 1024.0;
        ++unit;
    }

    char buffer[32] = {};
    if (unit == 0) {
        std::snprintf(buffer, sizeof(buffer), "%llu%s", static_cast<unsigned long long>(bytes),
                      kUnits[unit].data());
        return std::string(buffer);
    }

    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    std::string text(buffer);
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text + std::string(kUnits[unit]);
}

std::string to_forward_slashes(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

namespace {

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool is_unreserved(char ch) {
    const auto value = static_cast<unsigned char>(ch);
    if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
        (value >= '0' && value <= '9')) {
        return true;
    }
    return ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

}  // namespace

std::string url_decode(std::string_view text) {
    std::string result;
    result.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%' || i + 2 >= text.size()) {
            result.push_back(text[i]);
            continue;
        }
        const int high = hex_value(text[i + 1]);
        const int low = hex_value(text[i + 2]);
        if (high < 0 || low < 0) {
            result.push_back(text[i]);  // 非法转义原样保留，不猜
            continue;
        }
        result.push_back(static_cast<char>((high << 4) | low));
        i += 2;
    }
    return result;
}

std::string url_encode(std::string_view text) {
    constexpr char kHex[] = "0123456789ABCDEF";

    std::string result;
    result.reserve(text.size());
    for (const char ch : text) {
        if (is_unreserved(ch)) {
            result.push_back(ch);
            continue;
        }
        const auto value = static_cast<unsigned char>(ch);
        result.push_back('%');
        result.push_back(kHex[value >> 4]);
        result.push_back(kHex[value & 0x0F]);
    }
    return result;
}

}  // namespace fmt
