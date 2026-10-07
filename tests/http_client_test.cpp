// common/http_client 的单元测试
//
// 本地起一个 http 服务打自己，覆盖成功/404/中止/连接失败与协议校验；
// https 需要真实证书，这里只验证「确实去做了 TLS 握手」这条错误路径，
// 真正下载 https 由环境变量 FMT_TEST_HTTPS_URL 的用例手工跑（见文末）。
#include "fmt/common/http_client.hpp"

#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <cpp-httplib/httplib.h>

#include "fmt_test.hpp"

namespace {

// 起一个只在本次用例里活着的本地 http 服务。
class LocalServer {
public:
    explicit LocalServer(std::string body) : body_(std::move(body)) {
        server_.Get("/payload.bin", [this](const httplib::Request&, httplib::Response& response) {
            response.set_content(body_, "application/octet-stream");
        });
        port_ = server_.bind_to_any_port("127.0.0.1");
        worker_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~LocalServer() {
        server_.stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    LocalServer(const LocalServer&) = delete;
    LocalServer& operator=(const LocalServer&) = delete;

    int port() const { return port_; }
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

private:
    std::string body_;
    httplib::Server server_;
    int port_ = 0;
    std::thread worker_;
};

std::string download_all(const std::string& url) {
    std::string collected;
    const auto result = fmt::http_download(
        fmt::HttpDownloadRequest{url},
        [&collected](const char* data, std::size_t size) {
            collected.append(data, size);
            return true;
        });
    if (!fmt::ok(result)) {
        return {};
    }
    return collected;
}

}  // namespace

FMT_TEST(HttpClient, 本地HTTP下载) {
    // 256 KB：比读取块大，能真正跑多轮读取循环
    std::string payload;
    for (int i = 0; i < 256; ++i) {
        payload += std::string(1024, static_cast<char>('a' + (i % 26)));
    }

    LocalServer server(payload);
    const auto result = fmt::http_download(
        fmt::HttpDownloadRequest{server.url("/payload.bin")},
        [&payload](const char* data, std::size_t size) {
            return std::string(data, size).size() == size;  // 总是接受
        });

    FMT_CHECK(fmt::ok(result));
    if (!fmt::ok(result)) {
        return;
    }
    const fmt::HttpDownloadResult& response = std::get<fmt::HttpDownloadResult>(result);
    FMT_CHECK_EQ(response.status, 200);
    FMT_CHECK(!response.aborted);
    FMT_CHECK_EQ(response.bytes, static_cast<std::uintmax_t>(payload.size()));
    FMT_CHECK(response.has_content_length);
    FMT_CHECK_EQ(response.content_length, static_cast<std::uintmax_t>(payload.size()));

    // 逐字节核对内容
    FMT_CHECK_EQ(download_all(server.url("/payload.bin")), payload);
}

FMT_TEST(HttpClient, 非2xx不交给调用方) {
    LocalServer server("payload");

    bool sink_called = false;
    const auto result = fmt::http_download(
        fmt::HttpDownloadRequest{server.url("/missing.bin")},
        [&sink_called](const char*, std::size_t) {
            sink_called = true;
            return true;
        });

    FMT_CHECK(fmt::ok(result));
    if (!fmt::ok(result)) {
        return;
    }
    FMT_CHECK_EQ(std::get<fmt::HttpDownloadResult>(result).status, 404);
    FMT_CHECK(!sink_called);  // 错误页不该落进调用方的文件里
}

FMT_TEST(HttpClient, 中止下载) {
    LocalServer server(std::string(256 * 1024, 'x'));

    const auto result = fmt::http_download(
        fmt::HttpDownloadRequest{server.url("/payload.bin")},
        [](const char*, std::size_t) { return false; });  // 第一块就要求停下

    FMT_CHECK(fmt::ok(result));
    if (!fmt::ok(result)) {
        return;
    }
    const fmt::HttpDownloadResult& response = std::get<fmt::HttpDownloadResult>(result);
    FMT_CHECK(response.aborted);
    FMT_CHECK_EQ(response.bytes, std::uintmax_t{0});  // 被拒绝的那块不算收到
}

FMT_TEST(HttpClient, 连接失败与协议校验) {
    // 连接失败：用不可路由地址 + 短超时。
    // （不要用「绑一个端口再连它」来造失败：httplib 的 bind_to_any_port 已经
    //   listen 了，只是没有 accept 循环，连上去会挂等到接收超时。）
    fmt::HttpDownloadRequest unreachable;
    unreachable.url = "http://10.255.255.1:8080/x";
    unreachable.connect_timeout_ms = 2000;
    unreachable.send_timeout_ms = 2000;
    unreachable.receive_timeout_ms = 2000;

    const auto refused = fmt::http_download(unreachable, nullptr);
    FMT_CHECK(!fmt::ok(refused));
    if (!fmt::ok(refused)) {
        const fmt::ErrorCode code = fmt::error_of(refused)->code;
        FMT_CHECK(code == fmt::ErrorCode::DownloadFailed || code == fmt::ErrorCode::DownloadTimeout);
    }

    // 不支持的协议
    const auto scheme = fmt::http_download(fmt::HttpDownloadRequest{"ftp://example.com/a"}, nullptr);
    FMT_CHECK(!fmt::ok(scheme));
    FMT_CHECK(fmt::error_of(scheme)->code == fmt::ErrorCode::UrlInvalid);

    // 非法端口
    const auto port = fmt::http_download(fmt::HttpDownloadRequest{"http://127.0.0.1:99999/a"},
                                         nullptr);
    FMT_CHECK(!fmt::ok(port));
    FMT_CHECK(fmt::error_of(port)->code == fmt::ErrorCode::UrlInvalid);

    FMT_CHECK(fmt::is_remote_url("https://example.com/a"));
    FMT_CHECK(fmt::is_remote_url("http://example.com/a"));
    FMT_CHECK(!fmt::is_remote_url(R"(D:\a\b.txt)"));
}

FMT_TEST(HttpClient, https会真的做TLS握手) {
    // 对着一台**明文** HTTP 服务发 https 请求：握手必然失败。
    // 这条用例的意义是证明 https 走的是真 TLS 通道，而不是被当成 http 处理。
    LocalServer server("payload");

    // 超时必须调短：服务器不会回 TLS 应答，默认的 5 分钟接收超时会把用例挂住。
    fmt::HttpDownloadRequest request;
    request.url = "https://127.0.0.1:" + std::to_string(server.port()) + "/payload.bin";
    request.connect_timeout_ms = 2000;
    request.send_timeout_ms = 2000;
    request.receive_timeout_ms = 2000;

    const auto result = fmt::http_download(request, nullptr);
    FMT_CHECK(!fmt::ok(result));
    if (!fmt::ok(result)) {
        const fmt::ErrorCode code = fmt::error_of(result)->code;
        FMT_CHECK(code == fmt::ErrorCode::DownloadFailed || code == fmt::ErrorCode::DownloadTimeout);
    }
}

// 真实 https 下载：需要网络与证书，默认不跑。
//   PowerShell: $env:FMT_TEST_HTTPS_URL='https://example.com/'; .\fmt_tests.exe HttpClient
FMT_TEST(HttpClient, 真实https下载可选) {
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, "FMT_TEST_HTTPS_URL") != 0 || raw == nullptr) {
        return;  // 没设置就跳过（不算失败）
    }
    const std::string url(raw);
    free(raw);
    if (url.empty()) {
        return;
    }

    std::uintmax_t received = 0;
    const auto result = fmt::http_download(fmt::HttpDownloadRequest{url},
                                           [&received](const char*, std::size_t size) {
                                               received += size;
                                               return true;
                                           });
    FMT_CHECK(fmt::ok(result));
    if (fmt::ok(result)) {
        const fmt::HttpDownloadResult& response = std::get<fmt::HttpDownloadResult>(result);
        // 打印出来当证据：手工跑这条时能直接看到状态码、字节数与内容类型。
        std::printf("    真实 https：%s -> HTTP %d，%s 字节，Content-Type: %s\n", url.c_str(),
                    response.status, std::to_string(received).c_str(),
                    response.content_type.c_str());
        FMT_CHECK_EQ(response.status, 200);
        FMT_CHECK(received > 0);
    }
}
