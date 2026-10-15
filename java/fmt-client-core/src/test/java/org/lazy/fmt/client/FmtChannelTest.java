package org.lazy.fmt.client;

import org.lazy.fmt.client.model.FileQuery;
import org.lazy.fmt.client.model.FmtBucketListing;
import org.lazy.fmt.client.model.FmtConfig;
import org.lazy.fmt.client.model.FmtDeleteCheck;
import org.lazy.fmt.client.model.FmtFile;
import org.lazy.fmt.client.model.FmtPage;
import org.lazy.fmt.client.model.FmtPayload;
import org.lazy.fmt.client.model.FmtServerStatus;
import org.lazy.fmt.client.model.FmtShare;
import org.lazy.fmt.client.model.FmtTrashEmpty;
import org.lazy.fmt.client.model.FmtTrashEntry;
import org.lazy.fmt.client.model.FmtTrashListing;
import org.lazy.fmt.client.model.FmtUpload;

import org.junit.jupiter.api.Assumptions;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * 通道行为测试。跑的是**真 HTTP**（JDK HttpServer 起一个照抄 FMT 认证/信封行为的 stub），
 * 所以「token 有没有真的进请求头」「401 有没有真的自愈」「上传的字节有没有真的到」都是实测。
 */
class FmtChannelTest {

    private static final String TOKEN = "1a2867378b112b8e7864218fd2fd693f";
    private static final String STATUS_OK =
            "{\"root\":\"D:/Tools/fmt\",\"pid\":4321,\"version\":\"1.0.0\",\"channel\":\"http\"}";

    // ------------------------------------------------------------------
    // 凭证：这就是「token 放进通道」的可验证形式
    // ------------------------------------------------------------------

    @Test
    void pingIsPublicAndCarriesNoCredential() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), "wrong-token")) {
            stub.on("GET", "/api/ping", request -> StubFmtServer.Reply.ok("{}"));
            FmtResult<Map<String, Object>> result = fmt.ping();
            assertTrue(result.isOk(), () -> "ping 是公开接口，token 配错也该通：" + result);
            assertNull(stub.lastRequest().header("X-FMT-Token"), "ping 不该带凭证");
        }
    }

    @Test
    void channelInjectsTokenOnEveryAuthenticatedCall() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/status", request -> StubFmtServer.Reply.ok(STATUS_OK));
            stub.on("GET", "/api/file", request -> StubFmtServer.Reply.ok(
                    "{\"files\":[],\"count\":0,\"total\":0,\"page\":1,\"page_size\":0,\"total_pages\":0,\"sort\":\"name\"}"));

            // 业务代码里一个 token 参数都没有
            assertTrue(fmt.status().isOk());
            assertTrue(fmt.listFiles(FileQuery.all()).isOk());

            assertEquals(2, stub.requestCount());
            for (StubFmtServer.Recorded request : stub.requests()) {
                assertEquals(TOKEN, request.header("X-FMT-Token"),
                        "每个请求都应由通道注入凭证：" + request.path());
            }
        }
    }

    @Test
    void bearerSchemePutsTokenInAuthorizationHeader() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(Fmt.builder(stub.baseUrl(), TOKEN)
                     .authScheme(FmtAuthScheme.BEARER).build())) {
            stub.on("GET", "/api/status", request -> StubFmtServer.Reply.ok(STATUS_OK));
            assertTrue(fmt.status().isOk());
            assertNull(stub.lastRequest().header("X-FMT-Token"));
            assertEquals("Bearer " + TOKEN, stub.lastRequest().header("Authorization"));
        }
    }

    @Test
    void bothSchemeSendsBothHeaders() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(Fmt.builder(stub.baseUrl(), TOKEN)
                     .authScheme(FmtAuthScheme.BOTH).build())) {
            stub.on("GET", "/api/status", request -> StubFmtServer.Reply.ok(STATUS_OK));
            assertTrue(fmt.status().isOk());
            assertEquals(TOKEN, stub.lastRequest().header("X-FMT-Token"));
            assertEquals("Bearer " + TOKEN, stub.lastRequest().header("Authorization"));
        }
    }

    @Test
    void unauthorizedTriggersCredentialRefreshAndExactlyOneRetry() {
        RotatingTokenProvider provider = new RotatingTokenProvider("stale-token");
        try (StubFmtServer stub = StubFmtServer.start("fresh-token");
             FmtChannel fmt = Fmt.channel(Fmt.builder(stub.baseUrl(), "placeholder")
                     .tokenProvider(provider).build())) {
            stub.on("GET", "/api/status", request -> StubFmtServer.Reply.ok(STATUS_OK));

            FmtResult<FmtServerStatus> result = fmt.status();

            assertTrue(result.isOk(), () -> "收到 401 应作废凭证、重取、重试一次：" + result);
            assertEquals(2, stub.requestCount());
            assertEquals("stale-token", stub.requests().get(0).header("X-FMT-Token"));
            assertEquals("fresh-token", stub.requests().get(1).header("X-FMT-Token"));
            assertEquals(1, provider.invalidations.get(), "只该作废一次");
            assertEquals("1.0.0", result.data().version());
        }
    }

    @Test
    void unauthorizedIsReportedAsFmt018WhenRetryIsDisabled() {
        try (StubFmtServer stub = StubFmtServer.start("server-token");
             FmtChannel fmt = Fmt.channel(Fmt.builder(stub.baseUrl(), "client-token")
                     .retryOnUnauthorized(false).build())) {
            stub.on("GET", "/api/status", request -> StubFmtServer.Reply.ok(STATUS_OK));

            FmtResult<FmtServerStatus> result = fmt.status();

            assertTrue(result.isFailure());
            assertEquals(FmtErrorCodes.UNAUTHORIZED, result.error().code());
            assertEquals(401, result.error().httpStatus());
            assertEquals(5, result.error().exitCode(), "FMT-018 的退出码是 5（权限/访问）");
            assertEquals(1, stub.requestCount(), "关掉重试就只发一次");
        }
    }

    @Test
    void describeMasksTheToken() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            String description = fmt.describe();
            assertFalse(description.contains(TOKEN), "描述里不能出现 token 明文：" + description);
            assertTrue(description.contains("1a28…693f"), description);
        }
    }

    @Test
    void buildingAChannelWithoutCredentialFails() {
        assertThrows(FmtConfigurationException.class, () -> FmtChannelConfig.builder().build());
        assertThrows(FmtConfigurationException.class,
                () -> FmtChannelConfig.builder().baseUrl("ftp://127.0.0.1").token(TOKEN).build());
        assertThrows(FmtConfigurationException.class,
                () -> FmtChannelConfig.builder().baseUrl("http://127.0.0.1:4122").build());
    }

    // ------------------------------------------------------------------
    // 错误模型
    // ------------------------------------------------------------------

    @Test
    void businessErrorBecomesResultFailureAndOrThrowThrows() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/file/", request -> StubFmtServer.Reply.error(
                    404, FmtErrorCodes.FILE_NOT_FOUND, "文件不存在：nope.txt"));

            FmtResult<FmtFile> result = fmt.getFile("nope.txt");

            assertTrue(result.isFailure());
            assertEquals(FmtErrorCodes.FILE_NOT_FOUND, result.error().code());
            assertEquals(404, result.error().httpStatus());
            assertEquals(3, result.error().exitCode());
            assertTrue(result.error().message().contains("nope.txt"));
            assertThrows(FmtApiException.class, result::orThrow);
            assertEquals(1, stub.requestCount(), "业务错误不是传输错误，不该重试");
        }
    }

    @Test
    void responseWithoutEnvelopeBecomesTransportException() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/status", request -> StubFmtServer.Reply.raw(200, "<html>别的服务</html>"));
            FmtTransportException failure = assertThrows(FmtTransportException.class, fmt::status);
            assertTrue(failure.getMessage().contains("不是 JSON 信封"), failure.getMessage());
        }
    }

    @Test
    void unknownRouteComesBackAsFmt017() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            // stub 没登记这个路由，行为与真服务端的兜底路由一致
            FmtResult<Object> result = fmt.call("GET", "/api/bucket", null, null);
            assertTrue(result.isFailure());
            assertEquals(FmtErrorCodes.ROUTE_NOT_FOUND, result.error().code());
        }
    }

    @Test
    void connectionRefusedGivesActionableTransportError() {
        String deadUrl;
        try (StubFmtServer stub = StubFmtServer.start(TOKEN)) {
            deadUrl = stub.baseUrl();
        }
        try (FmtChannel fmt = Fmt.channel(deadUrl, TOKEN)) {
            FmtTransportException failure = assertThrows(FmtTransportException.class, fmt::ping);
            assertTrue(failure.getMessage().contains("fmt.exe"), failure.getMessage());
            assertTrue(failure.getMessage().contains("config/server.json"), failure.getMessage());
        }
    }

    @Test
    void fallsBackToTheOtherLoopbackFamilyWhenLocalhostResolvesToTheDeadOne() {
        StubFmtServer stub;
        try {
            // 只绑 IPv6 回环（真机上 FMT 的 host: localhost 就是这样绑的）
            stub = StubFmtServer.startOn("::1", TOKEN);
        } catch (RuntimeException noIpv6) {
            Assumptions.abort("本机没有 IPv6 回环，跳过：" + noIpv6.getMessage());
            return;
        }
        try (stub;
             FmtChannel fmt = Fmt.channel("http://localhost:" + stub.port(), TOKEN)) {
            stub.on("GET", "/api/ping", request -> StubFmtServer.Reply.ok("{}"));

            FmtResult<Map<String, Object>> ping = fmt.ping();

            assertTrue(ping.isOk(),
                    () -> "localhost 的第一个解析地址连不上时，通道应回落到 ::1：" + ping);
            assertEquals(1, stub.requestCount(),
                    "被拒的那几次连接根本没到服务端，所以服务端只该看到 1 个请求");
        }
    }

    @Test
    void closedChannelRejectsFurtherCalls() {
        StubFmtServer stub = StubFmtServer.start(TOKEN);
        try {
            FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN);
            stub.on("GET", "/api/ping", request -> StubFmtServer.Reply.ok("{}"));
            fmt.close();
            assertThrows(IllegalStateException.class, fmt::ping);
        } finally {
            stub.close();
        }
    }

    // ------------------------------------------------------------------
    // 文件列表 / 单条 / 删除
    // ------------------------------------------------------------------

    @Test
    void listFilesEncodesQueryAndParsesPage() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/file", request -> StubFmtServer.Reply.ok("""
                    {"files":[{"file_id":"fmt-1","file_name":"报表.xlsx","extension":"xlsx",
                    "file_type":"spreadsheet","size":2048,"md5":"d41d8"}],
                    "count":1,"total":45,"page":1,"page_size":20,"total_pages":3,"sort":"size"}"""));

            FmtPage page = fmt.listFiles(FileQuery.builder()
                    .search("中文").sort(FileQuery.Sort.SIZE).page(1).pageSize(20).build()).data();

            StubFmtServer.Recorded request = stub.lastRequest();
            assertEquals("中文", request.queryParam("search"));
            assertEquals("size", request.queryParam("sort"));
            assertEquals("1", request.queryParam("page"));
            assertEquals("20", request.queryParam("page_size"));
            assertTrue(request.query().contains("search=%E4%B8%AD%E6%96%87"),
                    "查询串必须百分号编码：" + request.query());

            assertEquals(1, page.files().size());
            assertEquals("报表.xlsx", page.files().get(0).fileName());
            assertEquals(2048L, page.files().get(0).size());
            assertEquals(45L, page.total());
            assertTrue(page.hasMore());
            assertEquals("size", page.sort());
        }
    }

    @Test
    void listFilesSendsNoQueryWhenNothingAsked() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/file", request -> StubFmtServer.Reply.ok(
                    "{\"files\":[],\"count\":0,\"total\":0,\"page\":1,\"page_size\":0,\"total_pages\":0,\"sort\":\"name\"}"));
            assertTrue(fmt.listFiles(null).isOk());
            assertEquals("", stub.lastRequest().query(), "不该塞默认参数");
        }
    }

    @Test
    void chineseFileNameIsPercentEncodedInThePath() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/file/", request -> StubFmtServer.Reply.ok(
                    "{\"file_id\":\"fmt-1\",\"file_name\":\"报表.xlsx\",\"bucket\":\"工作\",\"size\":12,"
                            + "\"is_trash\":false,\"path\":\"user/工作/2026/10/09/报表.xlsx\"}"));

            FmtFile file = fmt.getFile("报表.xlsx").data();

            assertEquals("/api/file/%E6%8A%A5%E8%A1%A8.xlsx", stub.lastRequest().path(),
                    "路径段必须百分号编码（真服务端看到的是编码后的原始路径）");
            assertEquals("报表.xlsx", file.fileName());
            assertEquals("工作", file.bucket());
            assertFalse(file.inTrash());
            assertTrue(file.path().endsWith("报表.xlsx"));
        }
    }

    @Test
    void checkDeleteUsesDryRunAndReportsAmbiguity() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("DELETE", "/api/file/", request -> StubFmtServer.Reply.ok("""
                    {"ambiguous":true,"other_bucket":false,"blocked":true,"needs_confirm":false,
                    "current_bucket":"工作","message":"同名文件有 2 个",
                    "candidates":[{"file_id":"fmt-1","file_name":"a.txt","bucket":"工作"},
                                  {"file_id":"fmt-2","file_name":"a.txt","bucket":"私人"}]}"""));

            FmtDeleteCheck check = fmt.checkDelete("a.txt").data();

            assertEquals("1", stub.lastRequest().queryParam("dry_run"));
            assertTrue(check.blocked());
            assertFalse(check.needsConfirm());
            assertEquals(2, check.candidates().size());
            assertEquals("私人", check.candidates().get(1).bucket());
        }
    }

    @Test
    void deleteSendsForceAndParsesMovedFile() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("DELETE", "/api/file/", request -> StubFmtServer.Reply.ok(
                    "{\"file_id\":\"fmt-1\",\"file_name\":\"a.txt\",\"bucket\":\"工作\","
                            + "\"moved_to\":\"user/.files/fmt-1\",\"message\":\"文件已移入回收站：a.txt\"}"));

            FmtFile file = fmt.delete("fmt-1", true).data();

            assertEquals("1", stub.lastRequest().queryParam("force"));
            assertEquals("fmt-1", file.fileId());
            assertEquals("a.txt", file.fileName());
        }
    }

    @Test
    void deleteWithoutForceSendsNoForceParam() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("DELETE", "/api/file/", request -> StubFmtServer.Reply.error(
                    400, FmtErrorCodes.CONFIRM_REQUIRED, "跨 Bucket 删除需要确认"));
            FmtResult<FmtFile> result = fmt.delete("fmt-1", false);
            assertTrue(stub.lastRequest().query().isEmpty(), "没确认就不该带 force");
            assertTrue(result.error().isConfirmRequired());
        }
    }

    // ------------------------------------------------------------------
    // 上传 / 下载
    // ------------------------------------------------------------------

    @Test
    void uploadBytesCarriesNameAndBody() {
        byte[] payload = "报表内容".getBytes(StandardCharsets.UTF_8);
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("POST", "/api/file/upload", request -> StubFmtServer.Reply.ok(
                    "{\"file_id\":\"fmt-2\",\"file_name\":\"报告 v2.xlsx\",\"size\":12,\"md5\":\"abc\","
                            + "\"extension\":\"xlsx\",\"file_type\":\"spreadsheet\",\"bucket\":\"工作\","
                            + "\"message\":\"流式上传完成：报告 v2.xlsx（fmt-2）\"}"));

            FmtUpload upload = fmt.upload(payload, "报告 v2.xlsx").data();

            StubFmtServer.Recorded request = stub.lastRequest();
            assertEquals("报告 v2.xlsx", request.queryParam("name"));
            assertArrayEquals(payload, request.body());
            assertEquals("application/octet-stream", request.header("Content-Type"));
            assertNotNull(request.header("Content-Disposition"));
            assertEquals(TOKEN, request.header("X-FMT-Token"));
            assertEquals("fmt-2", upload.file().fileId());
            assertTrue(upload.message().contains("流式上传完成"));
        }
    }

    @Test
    void uploadLocalFileViaStreamUsesItsName(@TempDir Path dir) throws IOException {
        // SDK **不收路径**：本机文件由调用方自己开成流（安全口径——收 Path 就会有人把
        // 服务端路径递进来）。这条钉住「本机文件走流上传」这件事仍然好用。
        Path source = dir.resolve("报表.txt");
        Files.writeString(source, "第一行\n第二行\n", StandardCharsets.UTF_8);
        try (InputStream in = Files.newInputStream(source);
             StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("POST", "/api/file/upload", request -> StubFmtServer.Reply.ok(
                    "{\"file_id\":\"fmt-3\",\"file_name\":\"报表.txt\",\"size\":12}"));

            FmtUpload upload = fmt.upload(in, source.getFileName().toString()).data();

            assertEquals("报表.txt", stub.lastRequest().queryParam("name"));
            assertEquals(Files.readString(source, StandardCharsets.UTF_8),
                    stub.lastRequest().bodyText());
            assertEquals("报表.txt", upload.file().fileName());
        }
    }

    @Test
    void uploadWithoutAStreamIsRejectedBeforeCallingFmt() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            FmtResult<FmtUpload> result = fmt.upload((InputStream) null, "x.bin");
            assertTrue(result.isFailure());
            assertEquals(FmtErrorCodes.INVALID_ARGUMENT, result.error().code());
            assertEquals(0, stub.requestCount());
        }
    }

    @Test
    void uploadStreamIsChunkedAndArrivesIntact() {
        byte[] payload = new byte[128 * 1024];
        for (int i = 0; i < payload.length; i++) {
            payload[i] = (byte) (i % 251);
        }
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("POST", "/api/file/upload", request -> StubFmtServer.Reply.ok(
                    "{\"file_id\":\"fmt-4\",\"file_name\":\"big.bin\",\"size\":" + payload.length + "}"));

            FmtUpload upload = fmt.upload(new ByteArrayInputStream(payload), "big.bin").data();

            assertArrayEquals(payload, stub.lastRequest().body(), "chunked 上传的字节必须一字不差");
            assertNull(stub.lastRequest().header("Content-Length"),
                    "未知长度的流走 Transfer-Encoding: chunked");
            assertEquals(payload.length, upload.file().size());
        }
    }

    @Test
    void downloadStreamsToTargetAndReadsUtf8FileName(@TempDir Path dir) throws IOException {
        byte[] content = "报表内容，含中文".getBytes(StandardCharsets.UTF_8);
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/file/", request -> StubFmtServer.Reply.bytes(200, content, Map.of(
                    "Content-Type", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
                    "Content-Disposition", "attachment; filename*=UTF-8''%E6%8A%A5%E8%A1%A8.xlsx")));

            Path target = dir.resolve("downloaded.bin");
            FmtPayload payload = fmt.download("fmt-1", target).data();

            assertArrayEquals(content, Files.readAllBytes(target));
            assertEquals("报表.xlsx", payload.fileName());
            assertEquals(content.length, payload.bytes());
            assertFalse(payload.inline());
            assertFalse(Files.exists(dir.resolve("downloaded.bin.part")), "不该留下半成品");
        }
    }

    @Test
    void previewMarksPayloadInline(@TempDir Path dir) throws IOException {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/file/", request -> StubFmtServer.Reply.bytes(200,
                    "hello".getBytes(StandardCharsets.UTF_8),
                    Map.of("Content-Type", "text/plain", "Content-Disposition", "inline; filename=\"a.txt\"")));

            FmtPayload payload = fmt.preview("a.txt", dir.resolve("a.txt")).data();

            assertTrue(payload.inline());
            assertTrue(stub.lastRequest().path().endsWith("/preview"));
            assertEquals("a.txt", payload.fileName());
            assertEquals("hello", Files.readString(dir.resolve("a.txt")));
        }
    }

    @Test
    void downloadFailureLeavesNoPartialFile(@TempDir Path dir) {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/file/", request -> StubFmtServer.Reply.error(
                    404, FmtErrorCodes.FILE_NOT_FOUND, "文件数据不存在"));

            Path target = dir.resolve("x.bin");
            FmtResult<FmtPayload> result = fmt.download("fmt-9", target);

            assertTrue(result.isFailure());
            assertEquals(FmtErrorCodes.FILE_NOT_FOUND, result.error().code());
            assertFalse(Files.exists(target));
            assertFalse(Files.exists(dir.resolve("x.bin.part")));
        }
    }

    @Test
    void downloadRefusesToOverwriteUnlessToldTo(@TempDir Path dir) throws IOException {
        Path target = dir.resolve("x.bin");
        Files.writeString(target, "旧的", StandardCharsets.UTF_8);
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/file/", request -> StubFmtServer.Reply.bytes(200,
                    "新的".getBytes(StandardCharsets.UTF_8), Map.of()));

            FmtResult<FmtPayload> refused = fmt.download("fmt-1", target, false);
            assertTrue(refused.isFailure());
            assertEquals(FmtErrorCodes.FILE_ALREADY_EXISTS, refused.error().code());
            assertEquals("旧的", Files.readString(target));
            assertEquals(0, stub.requestCount(), "本地能拦住的就别麻烦服务端");

            FmtPayload payload = fmt.download("fmt-1", target, true).data();
            assertEquals("新的", Files.readString(target));
            assertEquals(1, stub.requestCount());
            assertEquals(target, payload.target());
        }
    }

    @Test
    void truncatedDownloadIsRejectedAndCleanedUp(@TempDir Path dir) {
        byte[] declared = "0123456789".getBytes(StandardCharsets.UTF_8);
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            // 声明 10 字节、只给 4 字节：模拟连接中途断掉
            stub.on("GET", "/api/file/", request -> new StubFmtServer.ReplyBuilder()
                    .status(200)
                    .header("Content-Type", "application/octet-stream")
                    .declaredLength(declared.length)
                    .body("0123".getBytes(StandardCharsets.UTF_8))
                    .build());

            Path target = dir.resolve("broken.bin");
            assertThrows(FmtTransportException.class, () -> fmt.download("fmt-1", target));
            assertFalse(Files.exists(target));
            assertFalse(Files.exists(dir.resolve("broken.bin.part")), "半截文件必须清掉");
        }
    }

    // ------------------------------------------------------------------
    // 回收站 / 分享 / 逃生舱
    // ------------------------------------------------------------------

    @Test
    void trashListParsesBothEntryKinds() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/trash", request -> StubFmtServer.Reply.ok("""
                    {"entries":[
                      {"type":"file","id":"fmt-1","name":"a.txt","bucket":"工作","deleted_at":"2026-10-09 10:00:00",
                       "bytes":12,"files":0,"present":true,"restorable":true,"trash_path":"user/.files/fmt-1"},
                      {"type":"bucket","id":"工作_20261009","name":"工作","bucket":"工作",
                       "deleted_at":"2026-10-09 11:00:00","bytes":99,"files":3,"present":true,
                       "restorable":true,"reason":"随桶删除"}],
                     "count":2,"files":1,"buckets":1}"""));

            FmtTrashListing listing = fmt.listTrash().data();

            assertEquals(2, listing.entries().size());
            assertTrue(listing.entries().get(0).fileEntry());
            assertTrue(listing.entries().get(1).bucketEntry());
            assertEquals("随桶删除", listing.entries().get(1).reason());
            assertEquals(1, listing.files());
            assertEquals(1, listing.buckets());
        }
    }

    @Test
    void restoreTrashPostsAndReadsEntry() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("POST", "/api/trash/", request -> StubFmtServer.Reply.ok("""
                    {"entry":{"type":"file","id":"fmt-1","name":"a.txt","bucket":"工作",
                     "deleted_at":"2026-10-09 10:00:00","bytes":12,"files":0,"present":true,"restorable":true},
                     "message":"文件已回退：a.txt"}"""));

            FmtTrashEntry entry = fmt.restoreTrash("fmt-1").data();

            assertEquals("POST", stub.lastRequest().method());
            assertEquals("/api/trash/fmt-1/restore", stub.lastRequest().path());
            assertEquals("a.txt", entry.name());
        }
    }

    @Test
    void deleteTrashNeedsForceAndParsesEntry() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("DELETE", "/api/trash/", request -> StubFmtServer.Reply.ok("""
                    {"entry":{"type":"file","id":"fmt-1","name":"a.txt","restorable":false},
                     "message":"已永久删除：a.txt"}"""));

            FmtTrashEntry entry = fmt.deleteTrash("fmt-1", true).data();

            assertEquals("1", stub.lastRequest().queryParam("force"));
            assertEquals("fmt-1", entry.id());
        }
    }

    @Test
    void createSharePostsJsonBodyAndParsesShare() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("POST", "/api/share", request -> StubFmtServer.Reply.ok("""
                    {"share_id":"0d7b3cf4bc2e","file_id":"fmt-1","max_download_count":20,
                     "download_count":0,"expire_time":"2026-10-16 10:00:00","is_valid":true,
                     "message":"分享已创建"}"""));

            FmtShare share = fmt.createShare("fmt-1").data();

            assertEquals("application/json; charset=utf-8", stub.lastRequest().header("Content-Type"));
            assertEquals("{\"file_id\":\"fmt-1\"}", stub.lastRequest().bodyText());
            assertEquals("0d7b3cf4bc2e", share.shareId());
            assertEquals(20, share.maxDownloadCount());
            assertEquals("2026-10-16 10:00:00", share.expireTime());
        }
    }

    @Test
    void getShareReportsAvailabilityFaithfully() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/share/", request -> StubFmtServer.Reply.ok("""
                    {"share_id":"0d7b3cf4bc2e","file_id":"fmt-1","max_download_count":20,
                     "download_count":20,"expire_time":null,"is_valid":true,"state":"limit_reached",
                     "available":false,"message":"下载次数已用尽","file_name":"a.txt","size":12,"bucket":"工作"}"""));

            FmtShare share = fmt.getShare("0d7b3cf4bc2e").data();

            assertFalse(share.available());
            assertEquals("limit_reached", share.state());
            assertEquals(20, share.downloadCount());
            assertNull(share.expireTime());
            assertEquals(12L, share.size());
        }
    }

    @Test
    void listSharesReadsArray() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/share", request -> StubFmtServer.Reply.ok("""
                    {"shares":[{"share_id":"a1","file_id":"fmt-1","is_valid":true},
                               {"share_id":"b2","file_id":"fmt-1","is_valid":false}],
                     "count":2,"file_id":"fmt-1"}"""));

            List<FmtShare> shares = fmt.listShares("fmt-1").data();

            assertEquals(2, shares.size());
            assertEquals("a1", shares.get(0).shareId());
            assertFalse(shares.get(1).valid());
            assertEquals("fmt-1", stub.lastRequest().queryParam("file_id"));
        }
    }

    @Test
    void downloadShareNeedsNoToken(@TempDir Path dir) throws IOException {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), "totally-wrong-token")) {
            stub.on("GET", "/api/share/", request -> StubFmtServer.Reply.bytes(200,
                    "shared-bytes".getBytes(StandardCharsets.UTF_8),
                    Map.of("Content-Type", "application/octet-stream")));

            Path target = dir.resolve("shared.bin");
            FmtPayload payload = fmt.downloadShare("0d7b3cf4bc2e", target, true).data();

            assertEquals("shared-bytes", Files.readString(target));
            assertEquals(payload.bytes(), Files.size(target));
            assertNull(stub.lastRequest().header("X-FMT-Token"),
                    "公开分享下载刻意不带 token（带错 token 反而说明设计不对）");
        }
    }

    @Test
    void emptyTrashSendsConfirmFlagsAndParsesCounts() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("DELETE", "/api/trash/", request -> StubFmtServer.Reply.ok(
                    "{\"files\":2,\"buckets\":1,\"bytes\":4096,\"message\":\"回收站已清空\"}"));

            FmtTrashEmpty emptied = fmt.emptyTrash(false, true).data();

            assertEquals("DELETE", stub.lastRequest().method());
            assertEquals("/api/trash/empty", stub.lastRequest().path(),
                    "必须是 /api/trash/empty 这条独立路由，不能落到 /api/trash/<名字>");
            assertEquals("1", stub.lastRequest().queryParam("force"));
            assertNull(stub.lastRequest().queryParam("dry_run"), "确认执行时不该带 dry_run");
            assertEquals(2, emptied.files());
            assertEquals(1, emptied.buckets());
            assertEquals(3, emptied.total());
            assertEquals(4096L, emptied.bytes());
            assertNull(emptied.needsConfirm(), "执行分支服务端不给 needs_confirm");
        }
    }

    @Test
    void emptyTrashDryRunReportsWhatWouldGo() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("DELETE", "/api/trash/", request -> StubFmtServer.Reply.ok(
                    "{\"files\":3,\"buckets\":0,\"bytes\":99,\"needs_confirm\":true,"
                            + "\"blocked\":false,\"message\":\"将永久删除 3 项\"}"));

            FmtTrashEmpty plan = fmt.emptyTrash(true, false).data();

            assertEquals("1", stub.lastRequest().queryParam("dry_run"));
            assertNull(stub.lastRequest().queryParam("force"));
            assertTrue(plan.needsConfirm());
            assertFalse(plan.blocked());
            assertEquals(3, plan.total());
        }
    }

    @Test
    void emptyTrashWithoutForceSurfacesFmt016() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("DELETE", "/api/trash/", request -> StubFmtServer.Reply.error(
                    400, FmtErrorCodes.CONFIRM_REQUIRED, "回收站里有 2 项；确认清空请加 force"));

            FmtResult<FmtTrashEmpty> refused = fmt.emptyTrash(false, false);

            assertTrue(refused.isFailure());
            assertTrue(refused.error().isConfirmRequired(), "缺确认就是 FMT-016，不是别的错");
            assertEquals(400, refused.error().httpStatus());
            assertEquals(2, refused.error().exitCode());
        }
    }

    @Test
    void getConfigNeverAsksForOrCarriesTheToken() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            // 故意让 stub 把 token 塞回来：模型里没有这个字段，不该被带进业务
            stub.on("GET", "/api/config", request -> StubFmtServer.Reply.ok("""
                    {"current_user":"user","current_bucket":"工作","max_upload_size":10485760,
                     "size_unit":"MB","language":"zh-CN","path":"config/config.json",
                     "user_id":"u-1","user_created_at":"2026-10-05 23:40:01",
                     "last_login_at":"2026-10-09 08:00:00","token":"leaked-should-be-ignored"}"""));

            FmtConfig config = fmt.getConfig().data();

            assertEquals("/api/config", stub.lastRequest().path());
            assertNull(stub.lastRequest().queryParam("include_token"),
                    "服务端不给 token，客户端也不该去要");
            assertTrue(stub.lastRequest().query().isEmpty(), "这个接口不需要任何查询参数");
            assertEquals("工作", config.currentBucket());
            assertEquals(10485760L, config.maxUploadSize());
            assertEquals("u-1", config.userId());

            // 结构上就没有 token 这一项（服务端不给，模型也不装）
            assertTrue(java.util.Arrays.stream(FmtConfig.class.getRecordComponents())
                            .noneMatch(component -> component.getName().equals("token")),
                    "FmtConfig 不该有 token 组件");
        }
    }

    @Test
    void listBucketsParsesCurrentBucket() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/bucket", request -> StubFmtServer.Reply.ok("""
                    {"buckets":[{"name":"工作","is_current":true},{"name":"私人","is_current":false}],
                     "count":2,"current_bucket":"工作"}"""));

            FmtBucketListing listing = fmt.listBuckets().data();

            assertEquals("/api/bucket", stub.lastRequest().path());
            assertEquals(2, listing.count());
            assertEquals("工作", listing.currentBucket());
            assertEquals("工作", listing.buckets().get(0).name());
            assertTrue(listing.buckets().get(0).current());
            assertFalse(listing.buckets().get(1).current());
            assertEquals(TOKEN, stub.lastRequest().header("X-FMT-Token"));
        }
    }

    @Test
    void callEscapeHatchReturnsRawData() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            stub.on("GET", "/api/file", request -> StubFmtServer.Reply.ok(
                    "{\"files\":[],\"count\":0,\"total\":7,\"page\":1,\"page_size\":0,\"total_pages\":0,\"sort\":\"name\"}"));

            Object data = fmt.call("GET", "/api/file", Map.of("search", "报表"), null).data();

            assertEquals("报表", stub.lastRequest().queryParam("search"));
            assertEquals(7L, ((Number) ((Map<?, ?>) data).get("total")).longValue());
        }
    }

    @Test
    void callValidatesItsArguments() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN);
             FmtChannel fmt = Fmt.channel(stub.baseUrl(), TOKEN)) {
            assertTrue(fmt.call("", "/api/file", null, null).isFailure());
            assertTrue(fmt.call("GET", "api/file", null, null).isFailure());
            assertEquals(0, stub.requestCount());
        }
    }

    /** 可轮换凭证：第一次给旧 token，401 之后换成新的。 */
    private static final class RotatingTokenProvider implements TokenProvider {

        private final AtomicReference<String> current;
        private final AtomicInteger invalidations = new AtomicInteger();

        RotatingTokenProvider(String initial) {
            this.current = new AtomicReference<>(initial);
        }

        @Override
        public String token() {
            return current.get();
        }

        @Override
        public void invalidate() {
            invalidations.incrementAndGet();
            current.set("fresh-token");
        }
    }
}
