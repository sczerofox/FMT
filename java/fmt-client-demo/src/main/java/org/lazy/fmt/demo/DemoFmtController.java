package org.lazy.fmt.demo;

import org.lazy.fmt.client.FmtChannel;
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

import jakarta.servlet.http.HttpServletResponse;
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
import java.io.OutputStream;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 桥接 REST：别的系统调这些接口，它们在内部用**同一个通道**调 FMT。
 *
 * <p>注意这里没有一个 token 参数——凭证在 {@link FmtChannel} 里，控制器只写业务。
 */
@RestController
@RequestMapping("/demo")
public class DemoFmtController {

    private final FmtChannel fmt;

    public DemoFmtController(FmtChannel fmt) {
        this.fmt = fmt;
    }

    /** FMT 活着吗、我的凭证对不对（凭证错时 status 会失败，ping 仍然 ok）。 */
    @GetMapping("/status")
    public Map<String, Object> status() {
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("baseUrl", fmt.baseUri().toString());
        out.put("ping", fmt.ping().isOk());
        FmtServerStatus status = fmt.status().orThrow();
        out.put("root", status.root());
        out.put("version", status.version());
        out.put("pid", status.pid());
        return out;
    }

    @GetMapping("/files")
    public FmtPage files(@RequestParam(required = false) String search,
                         @RequestParam(defaultValue = "name") String sort,
                         @RequestParam(defaultValue = "1") int page,
                         @RequestParam(name = "page_size", defaultValue = "0") int pageSize) {
        FileQuery.Sort order = switch (sort.toLowerCase()) {
            case "size" -> FileQuery.Sort.SIZE;
            case "id" -> FileQuery.Sort.ID;
            default -> FileQuery.Sort.NAME;
        };
        return fmt.listFiles(FileQuery.builder()
                .search(search).sort(order).page(page).pageSize(pageSize).build()).orThrow();
    }

    @GetMapping("/files/{id}")
    public FmtFile file(@PathVariable String id) {
        return fmt.getFile(id).orThrow();
    }

    /** 删除前预检：`blocked` 是要用户选具体对象，`needs_confirm` 只是补一个 force。 */
    @GetMapping("/files/{id}/delete-check")
    public FmtDeleteCheck deleteCheck(@PathVariable String id) {
        return fmt.checkDelete(id).orThrow();
    }

    @DeleteMapping("/files/{id}")
    public FmtFile delete(@PathVariable String id,
                          @RequestParam(defaultValue = "false") boolean force) {
        return fmt.delete(id, force).orThrow();
    }

    /** 把上传的文件流式转给 FMT（浏览器 → 桥接 jar → FMT，中途不落两块盘）。 */
    @PostMapping("/files")
    public FmtUpload upload(@RequestParam("file") MultipartFile file) throws IOException {
        if (file.isEmpty()) {
            throw new IllegalArgumentException("上传的文件是空的");
        }
        String name = file.getOriginalFilename();
        if (name == null || name.isBlank()) {
            name = "upload.bin";
        }
        try (InputStream in = file.getInputStream()) {
            return fmt.upload(in, name).orThrow();
        }
    }

    @GetMapping("/files/{id}/download")
    public void download(@PathVariable String id, HttpServletResponse response) throws IOException {
        Path temp = Files.createTempFile("fmt-demo-", ".bin");
        try {
            FmtPayload payload = fmt.download(id, temp, true).orThrow();
            String contentType = payload.contentType() == null || payload.contentType().isBlank()
                    ? "application/octet-stream"
                    : payload.contentType();
            response.setContentType(contentType);
            response.setHeader("Content-Disposition", "attachment; filename*=UTF-8''"
                    + URLEncoder.encode(payload.fileName(), StandardCharsets.UTF_8).replace("+", "%20"));
            response.setContentLengthLong(payload.bytes());
            try (OutputStream out = response.getOutputStream()) {
                Files.copy(temp, out);
            }
        } finally {
            Files.deleteIfExists(temp);
        }
    }

    @PostMapping("/shares")
    public FmtShare createShare(@RequestParam("file_id") String fileId) {
        return fmt.createShare(fileId).orThrow();
    }

    @GetMapping("/shares")
    public List<FmtShare> shares(@RequestParam("file_id") String fileId) {
        return fmt.listShares(fileId).orThrow();
    }

    @GetMapping("/shares/{shareId}")
    public FmtShare share(@PathVariable String shareId) {
        return fmt.getShare(shareId).orThrow();
    }

    @DeleteMapping("/shares/{shareId}")
    public FmtShare revokeShare(@PathVariable String shareId) {
        return fmt.revokeShare(shareId).orThrow();
    }

    /** 读配置（**只读，且响应里永远没有 token**）：上传前先看 `maxUploadSize`，少踩一次 FMT-303。 */
    @GetMapping("/config")
    public FmtConfig config() {
        return fmt.getConfig().orThrow();
    }

    /** 桶列表（只读）：HTTP 的文件类接口都作用在 `currentBucket` 上。 */
    @GetMapping("/buckets")
    public FmtBucketListing buckets() {
        return fmt.listBuckets().orThrow();
    }

    /** 清空回收站：`dry_run=true` 只预检；`force=true` 才真删（缺 force → 409 + FMT-016）。 */
    @DeleteMapping("/trash/empty")
    public FmtTrashEmpty emptyTrash(
            @RequestParam(name = "dry_run", defaultValue = "false") boolean dryRun,
            @RequestParam(defaultValue = "false") boolean force) {
        return fmt.emptyTrash(dryRun, force).orThrow();
    }

    @GetMapping("/trash")
    public FmtTrashListing trash() {
        return fmt.listTrash().orThrow();
    }

    @PostMapping("/trash/{key}/restore")
    public FmtTrashEntry restore(@PathVariable String key) {
        return fmt.restoreTrash(key).orThrow();
    }
}
