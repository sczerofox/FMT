#include "fmt/common/http_client.hpp"

#include <windows.h>

#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include "fmt/common/string.hpp"

namespace fmt {
namespace {

constexpr std::size_t kReadChunk = 64 * 1024;

// WinHTTP 的句柄必须成对关闭；用 RAII 免得每条错误路径都手写清理。
class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET value) : handle_(value) {}
    ~WinHttpHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) {
                WinHttpCloseHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    HINTERNET get() const { return handle_; }
    bool valid() const { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

struct ParsedUrl {
    bool secure = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path;  // 含查询串，必须以 '/' 开头
};

bool has_scheme(std::string_view text, std::string_view scheme) {
    if (text.size() < scheme.size()) {
        return false;
    }
    for (std::size_t i = 0; i < scheme.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(scheme[i]))) {
            return false;
        }
    }
    return true;
}

// 把 WinHTTP / Win32 错误码翻成能看懂的话。
std::string describe_win32_error(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT:
            return "超时";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return "域名解析失败";
        case ERROR_WINHTTP_CANNOT_CONNECT:
            return "无法连接（端口不通或被拒绝）";
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return "连接被中断";
        case ERROR_WINHTTP_SECURE_FAILURE:
            return "TLS 握手失败（证书或加密套件问题）";
        case ERROR_WINHTTP_INVALID_URL:
        case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
            return "URL 非法或协议不支持";
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
            return "服务器响应异常";
        case ERROR_WINHTTP_RESEND_REQUEST:
            return "请求被要求重发";
        default:
            break;
    }

    LPWSTR text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_HMODULE,
        LoadLibraryW(L"winhttp.dll"), code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::string message = "Win32 " + std::to_string(code);
    if (length > 0 && text != nullptr) {
        std::wstring wide(text, length);
        while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
            wide.pop_back();
        }
        message += " " + to_utf8(wide);
    }
    if (text != nullptr) {
        LocalFree(text);
    }
    return message;
}

// 证书类失败时，Schannel 会把「哪一项没通过」写进安全标志位，用它给出更准的提示。
std::string describe_security_flags(HINTERNET request) {
    DWORD flags = 0;
    DWORD size = sizeof(flags);
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_SECURITY_FLAGS, &flags, &size)) {
        return {};
    }

    std::string reasons;
    const auto add = [&reasons](const char* text) {
        reasons += (reasons.empty() ? "" : "、");
        reasons += text;
    };
    if ((flags & SECURITY_FLAG_IGNORE_UNKNOWN_CA) != 0) {
        add("根证书不受信任");
    }
    if ((flags & SECURITY_FLAG_IGNORE_CERT_CN_INVALID) != 0) {
        add("证书主机名不符");
    }
    if ((flags & SECURITY_FLAG_IGNORE_CERT_DATE_INVALID) != 0) {
        add("证书已过期或尚未生效");
    }
    return reasons;
}

DWORD last_error() { return GetLastError(); }

Result<ParsedUrl> parse_url(const std::string& url) {
    ParsedUrl parsed;
    std::string rest;

    if (has_scheme(url, "https://")) {
        parsed.secure = true;
        rest = url.substr(8);
    } else if (has_scheme(url, "http://")) {
        rest = url.substr(7);
    } else {
        return make_error(ErrorCode::UrlInvalid, "只支持 http:// 与 https:// 的 URL：" + url);
    }

    const std::size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (authority.empty()) {
        return make_error(ErrorCode::UrlInvalid, "URL 缺少主机名：" + url);
    }

    // 主机里的用户名/密码（user:pass@host）不接受：下载不需要，也免得把凭据写进日志。
    if (authority.find('@') != std::string::npos) {
        return make_error(ErrorCode::UrlInvalid, "URL 不支持带用户名密码的形式：" + url);
    }

    std::string host = authority;
    INTERNET_PORT port = parsed.secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (const std::size_t colon = authority.rfind(':'); colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string port_text = authority.substr(colon + 1);
        if (port_text.empty() ||
            !std::all_of(port_text.begin(), port_text.end(),
                         [](char ch) { return ch >= '0' && ch <= '9'; })) {
            return make_error(ErrorCode::UrlInvalid, "URL 的端口不是数字：" + url);
        }
        try {
            const int value = std::stoi(port_text);
            if (value <= 0 || value > 65535) {
                return make_error(ErrorCode::UrlInvalid, "URL 的端口超出范围：" + url);
            }
            port = static_cast<INTERNET_PORT>(value);
        } catch (const std::exception&) {
            return make_error(ErrorCode::UrlInvalid, "URL 的端口不合法：" + url);
        }
    }
    if (host.empty()) {
        return make_error(ErrorCode::UrlInvalid, "URL 缺少主机名：" + url);
    }

    parsed.host = to_wide(host);
    parsed.port = port;
    parsed.path = to_wide(path);
    return parsed;
}

}  // namespace

bool is_remote_url(std::string_view source) {
    return has_scheme(source, "http://") || has_scheme(source, "https://");
}

Result<HttpDownloadResult> http_download(
    const HttpDownloadRequest& request,
    const std::function<bool(const char* data, std::size_t size)>& sink) {
    Result<ParsedUrl> parsed = parse_url(request.url);
    if (!ok(parsed)) {
        return *error_of(parsed);
    }
    const ParsedUrl target = std::get<ParsedUrl>(parsed);

    // 优先用「自动检测系统代理」（Win8.1+）；老系统退回默认代理配置。
    WinHttpHandle session(WinHttpOpen(to_wide(request.user_agent).c_str(),
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.valid()) {
        session = WinHttpHandle(WinHttpOpen(to_wide(request.user_agent).c_str(),
                                            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    }
    if (!session.valid()) {
        return make_error(ErrorCode::DownloadFailed,
                          "初始化 WinHTTP 失败：" + describe_win32_error(last_error()));
    }

    WinHttpSetTimeouts(session.get(), 0, request.connect_timeout_ms, request.send_timeout_ms,
                       request.receive_timeout_ms);

    WinHttpHandle connection(WinHttpConnect(session.get(), target.host.c_str(), target.port, 0));
    if (!connection.valid()) {
        return make_error(ErrorCode::DownloadFailed,
                          "连接主机失败：" + describe_win32_error(last_error()));
    }

    WinHttpHandle http_request(WinHttpOpenRequest(
        connection.get(), L"GET", target.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, target.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!http_request.valid()) {
        return make_error(ErrorCode::DownloadFailed,
                          "创建请求失败：" + describe_win32_error(last_error()));
    }

    if (request.follow_redirects) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(http_request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy,
                         sizeof(policy));  // 失败就按系统默认，不致命
    }

    // 明确要「不压缩」的原始字节：压缩会让 Content-Length 与实际落盘字节对不上，
    // 也可能把压缩内容当文件存下来。
    const wchar_t kEncodingHeader[] = L"Accept-Encoding: identity\r\n";
    WinHttpAddRequestHeaders(http_request.get(), kEncodingHeader, static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!WinHttpSendRequest(http_request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        const DWORD code = last_error();
        if (code == ERROR_WINHTTP_TIMEOUT) {
            return make_error(ErrorCode::DownloadTimeout,
                              "发送请求超时：" + request.url + "（" + describe_win32_error(code) +
                                  "）");
        }
        return make_error(ErrorCode::DownloadFailed,
                          "发送请求失败：" + request.url + "（" + describe_win32_error(code) + "）");
    }

    if (!WinHttpReceiveResponse(http_request.get(), nullptr)) {
        const DWORD code = last_error();
        if (code == ERROR_WINHTTP_TIMEOUT) {
            return make_error(ErrorCode::DownloadTimeout,
                              "等待响应超时：" + request.url + "（" + describe_win32_error(code) +
                                  "）");
        }
        if (code == ERROR_WINHTTP_SECURE_FAILURE) {
            const std::string reasons = describe_security_flags(http_request.get());
            return make_error(ErrorCode::DownloadFailed,
                              "TLS 校验失败：" + request.url +
                                  (reasons.empty() ? std::string{} : "（" + reasons + "）"));
        }
        return make_error(ErrorCode::DownloadFailed,
                          "接收响应失败：" + request.url + "（" + describe_win32_error(code) + "）");
    }

    HttpDownloadResult result;

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (WinHttpQueryHeaders(http_request.get(),
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                            WINHTTP_NO_HEADER_INDEX)) {
        result.status = static_cast<int>(status);
    }

    // Content-Length / Content-Type / Content-Encoding
    const auto query_header = [&http_request](DWORD index) -> std::string {
        DWORD size = 0;
        WinHttpQueryHeaders(http_request.get(), index, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (size == 0) {
            return {};
        }
        std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
        if (!WinHttpQueryHeaders(http_request.get(), index, WINHTTP_HEADER_NAME_BY_INDEX,
                                 buffer.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
            return {};
        }
        buffer.resize(wcslen(buffer.c_str()));
        return to_utf8(buffer);
    };

    const std::string length = query_header(WINHTTP_QUERY_CONTENT_LENGTH);
    if (!length.empty()) {
        try {
            result.content_length = static_cast<std::uintmax_t>(std::stoull(length));
            result.has_content_length = true;
        } catch (const std::exception&) {
            result.has_content_length = false;
        }
    }
    result.content_type = query_header(WINHTTP_QUERY_CONTENT_TYPE);

    const std::string encoding = query_header(WINHTTP_QUERY_CONTENT_ENCODING);
    if (!encoding.empty() && !iequals(encoding, "identity")) {
        return make_error(ErrorCode::DownloadFailed,
                          "服务器返回了 " + encoding + " 压缩内容，暂不支持：" + request.url);
    }

    // 非 2xx：把响应体丢掉，只回状态码（错误页不该落进用户的文件里）。
    const bool accept_body = result.status >= 200 && result.status < 300;

    std::vector<char> buffer(std::min<std::size_t>(kReadChunk, 4096));
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(http_request.get(), &available)) {
            const DWORD code = last_error();
            if (code == ERROR_WINHTTP_TIMEOUT) {
                return make_error(ErrorCode::DownloadTimeout,
                                  "读取数据超时：" + request.url + "（已收 " +
                                      std::to_string(result.bytes) + " 字节）");
            }
            return make_error(ErrorCode::DownloadFailed,
                              "读取数据失败：" + request.url + "（" + describe_win32_error(code) +
                                  "）");
        }
        if (available == 0) {
            break;  // 读完了
        }

        buffer.resize(std::min<std::size_t>(available, kReadChunk));
        DWORD read = 0;
        if (!WinHttpReadData(http_request.get(), buffer.data(),
                             static_cast<DWORD>(buffer.size()), &read)) {
            const DWORD code = last_error();
            if (code == ERROR_WINHTTP_TIMEOUT) {
                return make_error(ErrorCode::DownloadTimeout,
                                  "读取数据超时：" + request.url + "（已收 " +
                                      std::to_string(result.bytes) + " 字节）");
            }
            return make_error(ErrorCode::DownloadFailed,
                              "读取数据失败：" + request.url + "（" + describe_win32_error(code) +
                                  "）");
        }
        if (read == 0) {
            break;
        }

        if (!accept_body) {
            continue;
        }
        if (sink != nullptr && !sink(buffer.data(), static_cast<std::size_t>(read))) {
            result.aborted = true;
            break;
        }
        result.bytes += read;
    }

    return result;
}

}  // namespace fmt
