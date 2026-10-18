package com.hncgy.mybatis.controller;

import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtApiException;
import org.lazy.fmt.client.FmtError;
import org.lazy.fmt.client.FmtErrorCodes;
import org.lazy.fmt.client.FmtResult;
import org.lazy.fmt.client.model.FileQuery;
import org.lazy.fmt.client.model.FmtDeleteCheck;
import org.lazy.fmt.client.model.FmtFile;
import org.lazy.fmt.client.model.FmtPage;
import org.lazy.fmt.client.model.FmtPayload;
import org.lazy.fmt.client.model.FmtServerStatus;
import org.lazy.fmt.client.model.FmtShare;
import org.lazy.fmt.client.model.FmtUpload;
import org.springframework.core.io.InputStreamResource;
import org.springframework.core.io.Resource;
import org.springframework.http.HttpHeaders;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.multipart.MultipartFile;

import java.io.IOException;
import java.io.InputStream;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;

/**
 * FMT 的 REST 桥接：HTTP 进来，通道出去。这里没有 token 参数。
 *
 * <p>失败时 {@code orThrow()} 会抛 FmtApiException（业务错误）/
 * FmtTransportException / FmtConfigurationException（连不上、配置不对），
 * 由 starter 自动注册的 {@code FmtExceptionAdvice} 统一翻成 JSON；宿主不需要再复制那个类。
 */
@RestController
@RequestMapping("/api/fmt")
public class FmtController {

    private final FmtChannel fmt;

    public FmtController(FmtChannel fmt) {
        this.fmt = fmt;
    }

    /** 探活：ping 不需要 token。 */
    @GetMapping("/ping")
    public boolean ping() {
        return fmt.ping().isOk();
    }

    @GetMapping("/status")
    public FmtResult<FmtServerStatus> status() {
        return fmt.status();                 // 直接返回 record，Jackson 自动序列化
    }

    @GetMapping("/config")
    public Object config() {
        return fmt.getConfig().orThrow();
    }

    @GetMapping("/buckets")
    public Object buckets() {
        return fmt.listBuckets().orThrow();
    }

    @GetMapping("/files")
    public FmtPage files(@RequestParam(required = false) String search,
                         @RequestParam(defaultValue = "NAME") FileQuery.Sort sort,
                         @RequestParam(defaultValue = "1") int page,
                         @RequestParam(name = "page_size", defaultValue = "20") int pageSize) {
        return fmt.listFiles(FileQuery.builder()
                .search(search)
                .sort(sort)
                .page(page)
                .pageSize(pageSize)
                .build()).orThrow();
    }

    @GetMapping("/files/{id}")
    public FmtFile file(@PathVariable String id) {
        return fmt.getFile(id).orThrow();
    }

    /** 上传：浏览器/前端 → 桥接 → FMT，流式转发不落两块盘。 */
    @PostMapping("/upload")
    public FmtUpload upload(@RequestParam("file") MultipartFile file) throws IOException {
        if (file.isEmpty()) {
            throw new FmtApiException(
                    FmtError.local(FmtErrorCodes.INVALID_ARGUMENT, "上传的文件是空的"));
        }
        String name = file.getOriginalFilename();
        if (name == null || name.isBlank()) {
            name = "upload.bin";
        }
        try (InputStream in = file.getInputStream()) {      // SDK 不收 Path，只收流/字节
            return fmt.upload(in, name).orThrow();
        }
    }

    /** 下载：先落到临时文件（流式，SDK 内部 .part → 改名），再原样吐给调用方，最后删临时文件。 */
    @GetMapping("/files/{id}/download")
    public ResponseEntity<Resource> download(@PathVariable String id) throws IOException {
        Path tmp = Files.createTempFile("fmt-", ".bin");
        boolean ok = false;
        try {
            FmtPayload payload = fmt.download(id, tmp).orThrow();
            String fileName = payload.fileName() == null || payload.fileName().isBlank()
                    ? "download.bin"
                    : payload.fileName();
            InputStream in = Files.newInputStream(tmp);
            ok = true;                                  // 交给内存流之后由它负责关闭
            return ResponseEntity.ok()
                    .header(HttpHeaders.CONTENT_DISPOSITION,
                            "attachment; filename*=UTF-8''"
                                    + URLEncoder.encode(fileName, StandardCharsets.UTF_8)
                                    .replace("+", "%20"))
                    .contentLength(payload.bytes())
                    .body(new InputStreamResource(in));
        } finally {
            if (!ok) {
                Files.deleteIfExists(tmp);               // 失败路径不留临时文件
            }
        }
    }

    /** 删除预检：?dry_run=1，零副作用。 */
    @GetMapping("/files/{id}/delete-check")
    public FmtDeleteCheck deleteCheck(@PathVariable String id) {
        return fmt.checkDelete(id).orThrow();
    }

    /** 软删除进回收站；跨桶删除要 force=true，否则 FMT-016。 */
    @DeleteMapping("/files/{id}")
    public FmtFile delete(@PathVariable String id,
                          @RequestParam(defaultValue = "false") boolean force) {
        return fmt.delete(id, force).orThrow();
    }

    @GetMapping("/trash")
    public Object trash() {
        return fmt.listTrash().orThrow();
    }

    @PostMapping("/shares")
    public FmtShare createShare(@RequestParam("file_id") String fileId) {
        return fmt.createShare(fileId).orThrow();
    }

    @GetMapping("/shares")
    public List<FmtShare> shares(@RequestParam("file_id") String fileId) {
        return fmt.listShares(fileId).orThrow();
    }

    @DeleteMapping("/shares/{shareId}")
    public FmtShare revokeShare(@PathVariable String shareId) {
        return fmt.revokeShare(shareId).orThrow();
    }
}