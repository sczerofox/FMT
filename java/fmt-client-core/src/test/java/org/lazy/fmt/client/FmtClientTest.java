package org.lazy.fmt.client;

import org.lazy.fmt.client.model.FmtFilePackage;
import org.lazy.fmt.client.model.FmtMedia;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

/** FmtClient 的简化入口、自动改名、FMT-304 复用与预览包。 */
class FmtClientTest {

    private static final String TOKEN = "0123456789abcdef0123456789abcdef";

    @Test
    void usernamePasswordConstructorLogsInAutomatically() {
        String salt = "00112233445566778899aabbccddeeff";
        String session = "session-token-0123456789abcdef";
        try (StubFmtServer stub = StubFmtServer.start(session)) {
            stub.on("GET", "/api/auth/salt", request -> StubFmtServer.Reply.ok(
                    "{\"username\":\"user\",\"salt\":\"" + salt + "\"}"));
            stub.on("POST", "/api/auth/login", request -> StubFmtServer.Reply.ok(
                    "{\"token\":\"" + session + "\",\"token_type\":\"session\","
                            + "\"expires_at\":\"2099-01-01T00:00:00\",\"username\":\"user\","
                            + "\"bucket\":\"work\"}"));
            stub.on("GET", "/api/status", request -> StubFmtServer.Reply.ok(
                    "{\"root\":\"D:/fmt\",\"version\":\"2.3.0\",\"pid\":1,\"channel\":\"http\"}"));

            try (FmtClient client = new FmtClient("user", "password", "work")
                    .setBaseUrl(stub.baseUrl())) {
                assertEquals("2.3.0", client.status().version());
            }
        }
    }

    @Test
    void tokenConstructorAndSettersAreEnough() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN)) {
            stub.on("GET", "/api/status", request -> StubFmtServer.Reply.ok(
                    "{\"root\":\"D:/fmt\",\"version\":\"2.2.1\",\"pid\":1,\"channel\":\"http\"}"));

            FmtClient fromToken = FmtClient.token(TOKEN, "work")
                    .setBaseUrl(stub.baseUrl());
            assertEquals("2.2.1", fromToken.status().version());
            assertEquals("work", fromToken.bucket());

            FmtClient fromSetters = new FmtClient()
                    .setBaseUrl(stub.baseUrl())
                    .setToken(TOKEN)
                    .setBucket("work");
            assertEquals("2.2.1", fromSetters.status().version());
        }
    }

    @Test
    void sameNameDifferentContentIsRenamedAndRetried() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN)) {
            AtomicInteger uploads = new AtomicInteger();
            stub.on("POST", "/api/file/upload", request -> {
                if (uploads.incrementAndGet() == 1) {
                    return StubFmtServer.Reply.error(409, FmtErrorCodes.FILE_NAME_CONFLICT,
                            "文件名重复");
                }
                String actual = request.queryParam("name");
                assertEquals("123456701.png", actual);
                return StubFmtServer.Reply.ok("""
                        {"file_id":"fmt-2","file_name":"123456701.png","bucket":"work",
                         "extension":".png","file_type":"image","size":3,
                         "md5":"900150983cd24fb0d6963f7d28e17f72","message":"ok"}""");
            });

            try (FmtClient client = FmtClient.token(TOKEN, "work").setBaseUrl(stub.baseUrl())) {
                FmtFilePackage pkg = client.upload("abc".getBytes(StandardCharsets.UTF_8), "1234567.png");
                assertEquals("fmt-2", pkg.fileId());
                assertEquals("123456701.png", pkg.fileName());
                assertEquals("123456701.png", pkg.uploadedName());
            }
        }
    }

    @Test
    void sameContentAndNameReturnsExistingRecord() {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN)) {
            stub.on("POST", "/api/file/upload", request -> StubFmtServer.Reply.error(
                    409, FmtErrorCodes.MD5_DUPLICATE, "文件已存在"));
            stub.on("GET", "/api/file/avatar.png", request -> StubFmtServer.Reply.ok("""
                    {"file_id":"fmt-old","file_name":"avatar.png","bucket":"work",
                     "extension":".png","file_type":"image","size":3,
                     "md5":"900150983cd24fb0d6963f7d28e17f72","path":"repository/work/avatar.png"}"""));

            try (FmtClient client = FmtClient.token(TOKEN, "work").setBaseUrl(stub.baseUrl())) {
                FmtFilePackage pkg = client.upload("abc".getBytes(StandardCharsets.UTF_8), "avatar.png");
                assertEquals("fmt-old", pkg.fileId());
                assertEquals("avatar.png", pkg.fileName());
            }
        }
    }

    @Test
    void packageLazilyFetchesPreviewAndDownload() throws IOException {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN)) {
            stub.on("POST", "/api/file/upload", request -> StubFmtServer.Reply.ok("""
                    {"file_id":"fmt-1","file_name":"a.png","bucket":"work",
                     "extension":".png","file_type":"image","size":3,
                     "md5":"900150983cd24fb0d6963f7d28e17f72","message":"ok"}"""));
            Map<String, String> previewHeaders = new LinkedHashMap<>();
            previewHeaders.put("Content-Type", "image/png");
            previewHeaders.put("Content-Disposition", "inline; filename=a.png");
            stub.on("GET", "/api/file/fmt-1/preview", request ->
                    StubFmtServer.Reply.bytes(200, new byte[]{1, 2, 3}, previewHeaders));
            Map<String, String> downloadHeaders = new LinkedHashMap<>();
            downloadHeaders.put("Content-Type", "image/png");
            downloadHeaders.put("Content-Disposition", "attachment; filename=a.png");
            stub.on("GET", "/api/file/fmt-1/download", request ->
                    StubFmtServer.Reply.bytes(200, new byte[]{4, 5, 6}, downloadHeaders));

            try (FmtClient client = FmtClient.token(TOKEN, "work").setBaseUrl(stub.baseUrl());
                 FmtFilePackage pkg = client.upload(new byte[]{1, 2, 3}, "a.png")) {
                FmtMedia preview = pkg.preview();
                assertEquals("image/png", preview.contentType());
                assertTrue(preview.inline());
                assertArrayEquals(new byte[]{1, 2, 3}, preview.readBytes());

                FmtMedia download = pkg.download();
                assertFalse(download.inline());
                assertArrayEquals(new byte[]{4, 5, 6}, download.readBytes());
            }
        }
    }

    @Test
    void previewUnsupportedReturnsNullWithoutBreakingPackage() throws IOException {
        try (StubFmtServer stub = StubFmtServer.start(TOKEN)) {
            stub.on("POST", "/api/file/upload", request -> StubFmtServer.Reply.ok("""
                    {"file_id":"fmt-v","file_name":"a.mp4","bucket":"work",
                     "extension":".mp4","file_type":"video","size":3,
                     "md5":"900150983cd24fb0d6963f7d28e17f72","message":"ok"}"""));
            stub.on("GET", "/api/file/fmt-v/preview", request -> StubFmtServer.Reply.error(
                    415, FmtErrorCodes.PREVIEW_UNSUPPORTED, "不支持预览"));
            Map<String, String> headers = new LinkedHashMap<>();
            headers.put("Content-Type", "video/mp4");
            stub.on("GET", "/api/file/fmt-v/download", request ->
                    StubFmtServer.Reply.bytes(200, new byte[]{7}, headers));

            try (FmtClient client = FmtClient.token(TOKEN, "work").setBaseUrl(stub.baseUrl());
                 FmtFilePackage pkg = client.upload(new byte[]{7}, "a.mp4")) {
                assertNull(pkg.previewOrNull());
                assertArrayEquals(new byte[]{7}, pkg.download().readBytes());
            }
        }
    }
}
