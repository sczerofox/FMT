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
        if (std::tolower(a) != std::tolower(b)) {
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
