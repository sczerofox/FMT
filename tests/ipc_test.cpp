// ipc 的单元测试：帧协议 + 真实命名管道往返
#include "fmt/ipc/pipe.hpp"
#include "fmt/ipc/protocol.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "fmt_test.hpp"

FMT_TEST(Ipc, 管道名可被FMT_PIPE覆盖) {
    // 默认名（线上就是这个）
    SetEnvironmentVariableW(L"FMT_PIPE", nullptr);
    FMT_CHECK(fmt::ipc::pipe_name() == std::wstring(fmt::ipc::kPipeName));

    // 覆盖名：端到端测试靠它让自己起的服务与真服务互不干扰
    SetEnvironmentVariableW(L"FMT_PIPE", L"\\\\.\\pipe\\fmt-unit-test");
    FMT_CHECK(fmt::ipc::pipe_name() == std::wstring(L"\\\\.\\pipe\\fmt-unit-test"));

    // 空值等于没设
    SetEnvironmentVariableW(L"FMT_PIPE", L"");
    FMT_CHECK(fmt::ipc::pipe_name() == std::wstring(fmt::ipc::kPipeName));

    SetEnvironmentVariableW(L"FMT_PIPE", nullptr);  // 别影响后面的用例
}

namespace {

void sleep_ms(int milliseconds) {    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}  // namespace

FMT_TEST(Ipc, 帧编码与解码) {
    nlohmann::json value = nlohmann::json::object();
    value["id"] = 7;
    value["op"] = "hello";
    value["root"] = R"(D:\FMT2)";

    std::string buffer = fmt::ipc::encode_frame(value);

    // 4 字节小端长度 + payload
    FMT_CHECK(buffer.size() > 4);
    const std::string payload = value.dump();
    const auto length = static_cast<unsigned char>(buffer[0]) |
                        (static_cast<unsigned>(static_cast<unsigned char>(buffer[1])) << 8) |
                        (static_cast<unsigned>(static_cast<unsigned char>(buffer[2])) << 16) |
                        (static_cast<unsigned>(static_cast<unsigned char>(buffer[3])) << 24);
    FMT_CHECK_EQ(static_cast<std::size_t>(length), payload.size());
    FMT_CHECK_EQ(buffer.substr(4), payload);

    nlohmann::json decoded;
    const auto taken = fmt::ipc::take_frame(buffer, &decoded);
    FMT_CHECK(fmt::ok(taken));
    FMT_CHECK(std::get<bool>(taken));
    FMT_CHECK_EQ(decoded["op"].get<std::string>(), std::string("hello"));
    FMT_CHECK(buffer.empty());
}

FMT_TEST(Ipc, 半帧不消费缓冲区) {
    std::string buffer = fmt::ipc::encode_frame(nlohmann::json{{"id", 1}});
    const std::string full = buffer;

    // 只给前 3 字节：不足一个长度字段
    std::string partial = full.substr(0, 3);
    nlohmann::json value;
    const auto taken = fmt::ipc::take_frame(partial, &value);
    FMT_CHECK(fmt::ok(taken));
    FMT_CHECK(!std::get<bool>(taken));
    FMT_CHECK_EQ(partial, full.substr(0, 3));

    // 给到长度字段但 payload 不完整
    std::string half = full.substr(0, full.size() - 1);
    const auto second = fmt::ipc::take_frame(half, &value);
    FMT_CHECK(fmt::ok(second));
    FMT_CHECK(!std::get<bool>(second));
    FMT_CHECK_EQ(half, full.substr(0, full.size() - 1));
}

FMT_TEST(Ipc, 两帧连在一起逐帧取出) {
    std::string buffer = fmt::ipc::encode_frame(nlohmann::json{{"id", 1}, {"op", "a"}});
    buffer += fmt::ipc::encode_frame(nlohmann::json{{"id", 2}, {"op", "b"}});

    nlohmann::json first;
    nlohmann::json second;
    FMT_CHECK(std::get<bool>(fmt::ipc::take_frame(buffer, &first)));
    FMT_CHECK(std::get<bool>(fmt::ipc::take_frame(buffer, &second)));
    FMT_CHECK_EQ(first["op"].get<std::string>(), std::string("a"));
    FMT_CHECK_EQ(second["op"].get<std::string>(), std::string("b"));
    FMT_CHECK(buffer.empty());
}

FMT_TEST(Ipc, 非法帧被拒绝) {
    nlohmann::json value;

    std::string zero_length("\0\0\0\0", 4);
    FMT_CHECK(!fmt::ok(fmt::ipc::take_frame(zero_length, &value)));

    std::string huge(4, '\0');
    huge[3] = static_cast<char>(0x7F);  // 天文数字长度
    FMT_CHECK(!fmt::ok(fmt::ipc::take_frame(huge, &value)));

    std::string bad_json(4, '\0');
    bad_json[0] = 3;
    bad_json += "{,}";
    const auto result = fmt::ipc::take_frame(bad_json, &value);
    FMT_CHECK(!fmt::ok(result));
    FMT_CHECK(fmt::error_of(result)->code == fmt::ErrorCode::JsonParseError);
}

FMT_TEST(Ipc, 请求与响应信封) {
    fmt::ipc::Request request;
    request.id = 42;
    request.op = "file.list";
    request.root = R"(D:\FMT)";
    request.pid = 1234;
    request.args["bucket"] = "工作";

    const nlohmann::json json = fmt::ipc::to_json(request);
    FMT_CHECK_EQ(json["id"].get<int>(), 42);
    FMT_CHECK_EQ(json["pid"].get<unsigned long>(), 1234UL);
    FMT_CHECK_EQ(json["args"]["bucket"].get<std::string>(), std::string("工作"));

    const auto parsed = fmt::ipc::request_from_json(json);
    FMT_CHECK(fmt::ok(parsed));
    FMT_CHECK_EQ(std::get<fmt::ipc::Request>(parsed).op, std::string("file.list"));
    FMT_CHECK_EQ(std::get<fmt::ipc::Request>(parsed).root, std::string(R"(D:\FMT)"));

    // 缺 op 必须被拒绝
    FMT_CHECK(!fmt::ok(fmt::ipc::request_from_json(nlohmann::json{{"id", 1}})));

    // 成功响应
    fmt::ipc::Response good;
    good.id = 42;
    good.data["count"] = 3;
    const auto good_parsed = fmt::ipc::response_from_json(fmt::ipc::to_json(good));
    FMT_CHECK(fmt::ok(good_parsed));
    FMT_CHECK(std::get<fmt::ipc::Response>(good_parsed).ok);
    FMT_CHECK_EQ(std::get<fmt::ipc::Response>(good_parsed).data["count"].get<int>(), 3);

    // 失败响应：错误码必须能跨进程还原
    fmt::ipc::Response bad;
    bad.id = 42;
    bad.ok = false;
    bad.error = fmt::make_error(fmt::ErrorCode::ServiceNotInstalled);
    const auto bad_parsed = fmt::ipc::response_from_json(fmt::ipc::to_json(bad));
    FMT_CHECK(fmt::ok(bad_parsed));
    FMT_CHECK(!std::get<fmt::ipc::Response>(bad_parsed).ok);
    FMT_CHECK(std::get<fmt::ipc::Response>(bad_parsed).error.code ==
              fmt::ErrorCode::ServiceNotInstalled);
    FMT_CHECK_EQ(fmt::exit_code(std::get<fmt::ipc::Response>(bad_parsed).error.code), 8);

    // 未知错误码不能变成「成功」
    nlohmann::json unknown = {{"id", 1},
                              {"ok", false},
                              {"error", {{"code", "FMT-999"}, {"message", "?"}}}};
    const auto unknown_parsed = fmt::ipc::response_from_json(unknown);
    FMT_CHECK(fmt::ok(unknown_parsed));
    FMT_CHECK(std::get<fmt::ipc::Response>(unknown_parsed).error.code ==
              fmt::ErrorCode::InvalidArgument);
}

FMT_TEST(Ipc, 管道真实往返) {
    // 用独立管道名：真实服务可能正在监听 \\.\pipe\fmt.control，
    // 抢同一个名字会让这条测试时好时坏。
    const std::wstring pipe_name =
        L"\\\\.\\pipe\\fmt.test." + std::to_wstring(GetCurrentProcessId());

    // 服务端线程故意晚 300 ms 才开始监听，用来验证客户端的等待重试。
    std::atomic<bool> server_ok{false};
    std::thread server([&server_ok, pipe_name] {
        sleep_ms(300);

        auto connection = fmt::ipc::PipeConnection::accept(5000, pipe_name.c_str());
        if (!fmt::ok(connection)) {
            return;
        }
        fmt::ipc::PipeConnection pipe =
            std::move(std::get<fmt::ipc::PipeConnection>(connection));

        const fmt::ipc::RequestOutcome outcome = pipe.read_request(5000);
        if (outcome.status != fmt::ipc::ReadStatus::Ok) {
            return;
        }
        const fmt::ipc::Request& received = outcome.request;

        fmt::ipc::Response response;
        response.id = received.id;
        response.ok = true;
        response.data["echo"] = received.op;
        response.data["root"] = received.root;
        if (!fmt::ok(pipe.write_response(response, 5000))) {
            return;
        }
        server_ok = true;
    });

    // connect_waiting 会一直重试到对端出现为止。
    auto connected = fmt::ipc::PipeClient::connect_waiting(5000, pipe_name.c_str());
    FMT_CHECK(fmt::ok(connected));

    fmt::ipc::Request request;
    request.id = 1;
    request.op = "hello";
    request.root = R"(D:\FMT)";
    request.pid = 4242;

    if (fmt::ok(connected)) {
        fmt::ipc::PipeClient client = std::move(std::get<fmt::ipc::PipeClient>(connected));
        const auto response = client.call(request, 5000);
        FMT_CHECK(fmt::ok(response));
        if (fmt::ok(response)) {
            const fmt::ipc::Response& value = std::get<fmt::ipc::Response>(response);
            FMT_CHECK(value.ok);
            FMT_CHECK_EQ(value.id, 1);
            FMT_CHECK_EQ(value.data["echo"].get<std::string>(), std::string("hello"));
            FMT_CHECK_EQ(value.data["root"].get<std::string>(), std::string(R"(D:\FMT)"));
        }
    }

    server.join();
    FMT_CHECK(server_ok.load());
}

FMT_TEST(Ipc, 没有服务时连接失败给出FMT601) {
    // 同样用独立名字，避免连到机器上真正在跑的服务。
    const std::wstring pipe_name =
        L"\\\\.\\pipe\\fmt.nothing." + std::to_wstring(GetCurrentProcessId());

    const auto client = fmt::ipc::PipeClient::connect(200, pipe_name.c_str());
    FMT_CHECK(!fmt::ok(client));
    FMT_CHECK(fmt::error_of(client)->code == fmt::ErrorCode::ServiceNotInstalled);
    FMT_CHECK_EQ(fmt::exit_code(fmt::error_of(client)->code), 8);

    // 等待版本也要在超时后如实报 FMT-601，而不是一直挂着。
    const auto waited = fmt::ipc::PipeClient::connect_waiting(300, pipe_name.c_str());
    FMT_CHECK(!fmt::ok(waited));
    FMT_CHECK(fmt::error_of(waited)->code == fmt::ErrorCode::ServiceNotInstalled);
}
