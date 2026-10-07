#include "fmt/core/path.hpp"

#include <windows.h>

#include <vector>

#include "fmt/common/string.hpp"

namespace fmt {

std::filesystem::path executable_path() {
    // GetModuleFileNameW 的缓冲区按需倍增，路径很长时也能拿到完整结果。
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path executable_directory() {
    const std::filesystem::path path = executable_path();
    return path.has_parent_path() ? path.parent_path() : std::filesystem::current_path();
}

std::filesystem::path service_state_directory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    const DWORD length = GetEnvironmentVariableW(L"ProgramData", buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
    std::filesystem::path base;
    if (length > 0 && length < buffer.size()) {
        base = std::filesystem::path(std::wstring(buffer.data(), length));
    } else {
        base = std::filesystem::path(L"C:\\ProgramData");
    }
    return base / L"FMT";
}

std::string path_to_utf8(const std::filesystem::path& path) {
    return to_utf8(path.wstring());
}

std::filesystem::path path_from_utf8(const std::string& text) {
    return std::filesystem::path(to_wide(text));
}

std::string relative_path_text(const std::filesystem::path& root,
                               const std::filesystem::path& path) {
    std::error_code code;
    const std::filesystem::path relative = std::filesystem::relative(path, root, code);
    return to_forward_slashes(path_to_utf8(code ? path : relative));
}

}  // namespace fmt
