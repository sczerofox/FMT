package org.lazy.fmt.client;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;
import org.lazy.fmt.client.model.FmtFilePackage;
import org.lazy.fmt.client.model.FmtUploadProgress;

import java.io.ByteArrayInputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

class FmtLargeUploadTest {

    private static final String TOKEN = "large-token";

    @Test
    void 大文件自动分片并完成(@TempDir Path temp) throws Exception {
        byte[] content = new byte[3 * 1024 * 1024];
        for (int i = 0; i < content.length; i++) {
            content[i] = (byte) ('a' + (i % 26));
        }
        Path source = temp.resolve("large.bin");
        Files.write(source, content);

        try (StubFmtServer stub = StubFmtServer.start(TOKEN)) {
            stub.on("POST", "/api/upload/init", request -> {
                String body = request.bodyText();
                assertTrue(body.contains("\"file_name\":\"large.bin\""), body);
                assertTrue(body.contains("\"size\":3145728"), body);
                assertTrue(body.contains("\"chunk_size\":1048576"), body);
                assertTrue(body.contains("\"md5\""), body);
                return StubFmtServer.Reply.ok("""
                        {"upload_id":"up-large","file_name":"large.bin","size":3145728,
                         "chunk_size":1048576,"chunk_count":3,"state":"uploading",
                         "uploaded_bytes":0,"received":[],"received_count":0}
                        """);
            });
            stub.on("PUT", "/api/upload/up-large/chunk", request -> {
                assertTrue(List.of("0", "1", "2").contains(request.queryParam("index")),
                        "index=" + request.queryParam("index"));
                assertNotNull(request.header("X-Chunk-MD5"));
                assertNotNull(request.body());
                assertEquals(1024 * 1024, request.body().length);
                return StubFmtServer.Reply.ok("""
                        {"upload_id":"up-large","file_name":"large.bin","size":3145728,
                         "chunk_size":1048576,"chunk_count":3,"state":"uploading",
                         "uploaded_bytes":1048576,"received":[0],"received_count":1}
                        """);
            });
            stub.on("POST", "/api/upload/up-large/complete", request ->
                    StubFmtServer.Reply.ok("""
                            {"upload_id":"up-large","file_id":"fmt-20261022-1",
                             "file_name":"large.bin","size":3145728,
                             "md5":"6f1ed002ab5595859014ebf0951522d9",
                             "state":"completed"}
                            """));
            stub.on("GET", "/api/upload/up-large", request -> StubFmtServer.Reply.ok("""
                    {"upload_id":"up-large","file_name":"large.bin","size":3145728,
                     "chunk_size":1048576,"chunk_count":3,"state":"uploading",
                     "uploaded_bytes":0,"received":[],"received_count":0}
                    """));

            List<FmtUploadProgress> progress = new ArrayList<>();
            FmtClient client = FmtClient.token(TOKEN, "pet").setBaseUrl(stub.baseUrl());
            FmtFilePackage pkg = client.uploadLarge(source, "large.bin",
                    FmtLargeUploadOptions.builder()
                            .chunkSize(1024 * 1024)
                            .progress(progress::add)
                            .build());

            assertEquals("fmt-20261022-1", pkg.fileId());
            assertEquals("large.bin", pkg.fileName());
            assertEquals(3, stub.requests().stream()
                    .filter(r -> "PUT".equals(r.method())).count());
            assertTrue(progress.size() >= 4, "应至少收到 init + 3 片 + completed");
            assertEquals("completed", progress.get(progress.size() - 1).state());
            assertEquals(100, progress.get(progress.size() - 1).percent());
        }
    }

    @Test
    void 流式分片直接上传且不需要本地临时文件() throws Exception {
        int size = 2 * 1024 * 1024 + 512 * 1024;   // 2.5 MB -> 3 片
        byte[] content = new byte[size];
        for (int i = 0; i < content.length; i++) {
            content[i] = (byte) (i % 251);
        }
        try (StubFmtServer stub = StubFmtServer.start(TOKEN)) {
            stub.on("POST", "/api/upload/init", request -> {
                String body = request.bodyText();
                assertTrue(body.contains("\"file_name\":\"stream.bin\""), body);
                assertTrue(body.contains("\"size\":" + size), body);
                assertTrue(body.contains("\"chunk_size\":1048576"), body);
                // 调用方没给 md5：不要伪造一个，complete 时由服务端计算。
                assertFalse(body.contains("\"md5\""), body);
                return StubFmtServer.Reply.ok("""
                        {"upload_id":"up-stream","file_name":"stream.bin","size":2621440,
                         "chunk_size":1048576,"chunk_count":3,"state":"uploading",
                         "uploaded_bytes":0,"received":[],"received_count":0}
                        """);
            });
            stub.on("PUT", "/api/upload/up-stream/chunk", request -> {
                int index = Integer.parseInt(request.queryParam("index"));
                assertEquals(index == 2 ? 512 * 1024 : 1024 * 1024, request.body().length);
                return StubFmtServer.Reply.ok("""
                        {"upload_id":"up-stream","file_name":"stream.bin","size":2621440,
                         "chunk_size":1048576,"chunk_count":3,"state":"uploading",
                         "uploaded_bytes":1048576,"received":[0],"received_count":1}
                        """);
            });
            stub.on("POST", "/api/upload/up-stream/complete", request ->
                    StubFmtServer.Reply.ok("""
                            {"upload_id":"up-stream","file_id":"fmt-20261023-1",
                             "file_name":"stream.bin","size":2621440,
                             "md5":"ignored","state":"completed"}
                            """));
            stub.on("GET", "/api/upload/up-stream", request -> StubFmtServer.Reply.ok("""
                    {"upload_id":"up-stream","file_name":"stream.bin","size":2621440,
                     "chunk_size":1048576,"chunk_count":3,"state":"uploading",
                     "uploaded_bytes":0,"received":[],"received_count":0}
                    """));

            FmtClient client = FmtClient.token(TOKEN, "pet").setBaseUrl(stub.baseUrl());
            FmtFilePackage pkg = client.uploadLarge(
                    new ByteArrayInputStream(content), size, "stream.bin",
                    FmtLargeUploadOptions.builder().chunkSize(1024 * 1024).build());

            assertEquals("fmt-20261023-1", pkg.fileId());
            assertEquals(3, stub.requests().stream()
                    .filter(r -> "PUT".equals(r.method())).count());
        }
    }

    @Test
    void 续传只传缺失分片(@TempDir Path temp) throws Exception {
        Path source = temp.resolve("resume.bin");
        Files.write(source, new byte[2 * 1024 * 1024]);
        try (StubFmtServer stub = StubFmtServer.start(TOKEN)) {
            stub.on("GET", "/api/upload/up-resume", request -> StubFmtServer.Reply.ok("""
                    {"upload_id":"up-resume","file_name":"resume.bin","size":2097152,
                     "chunk_size":1048576,"chunk_count":2,"state":"paused",
                     "uploaded_bytes":1048576,"received":[0],"received_count":1}
                    """));
            stub.on("POST", "/api/upload/up-resume/resume", request ->
                    StubFmtServer.Reply.ok("""
                            {"upload_id":"up-resume","file_name":"resume.bin","size":2097152,
                             "chunk_size":1048576,"chunk_count":2,"state":"uploading",
                             "uploaded_bytes":1048576,"received":[0],"received_count":1}
                            """));
            stub.on("PUT", "/api/upload/up-resume/chunk", request -> {
                assertEquals("1", request.queryParam("index"));
                assertEquals(1024 * 1024, request.body().length);
                return StubFmtServer.Reply.ok("""
                        {"upload_id":"up-resume","file_name":"resume.bin","size":2097152,
                         "chunk_size":1048576,"chunk_count":2,"state":"uploading",
                         "uploaded_bytes":2097152,"received":[0,1],"received_count":2}
                        """);
            });
            stub.on("POST", "/api/upload/up-resume/complete", request ->
                    StubFmtServer.Reply.ok("""
                            {"upload_id":"up-resume","file_id":"fmt-20261022-2",
                             "file_name":"resume.bin","size":2097152,"state":"completed"}
                            """));

            FmtClient client = FmtClient.token(TOKEN, "pet").setBaseUrl(stub.baseUrl());
            FmtFilePackage pkg = client.resumeLargeUpload(source, "up-resume",
                    FmtLargeUploadOptions.builder().chunkSize(1024 * 1024).build());

            assertEquals("fmt-20261022-2", pkg.fileId());
            assertEquals(1, stub.requests().stream()
                    .filter(r -> "PUT".equals(r.method())).count(),
                    "已经有 index=0，只应传 index=1");
        }
    }
}
