package com.hncgy.mybatis.controller;

import com.hncgy.mybatis.service.FmtService;
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
 * FMT 的 REST 桥接：HTTP 进来，业务层出去。
 *
 * <p>这里**没有 token 参数**：token/账号密码由 starter 绑在 {@code FmtChannel} 上，
 * 业务方法只管调 {@link FmtService}；调用本 Controller 的外部系统也不需要带 token
 * （除非你自己在 Spring Security 里另加一层）。
 */
@RestController
@RequestMapping("/api/fmt")
public class FmtController {

    private final FmtService fmtService;

    public FmtController(FmtService fmtService) {
        this.fmtService = fmtService;
    }

    /** 探活：ping 不需要凭证。 */
    @GetMapping("/ping")
    public boolean ping() {
        return fmtService.ping();
    }

    @GetMapping("/status")
    public FmtResult<FmtServerStatus> status() {
        return fmtService.status();                 // 保留 {ok,data} 信封
    }

    @GetMapping("/config")
    public Object config() {
        return fmtService.config();
    }

    @GetMapping("/buckets")
    public Object buckets() {
        return fmtService.buckets();
    }

    @GetMapping("/files")
    public FmtPage files(@RequestParam(required = false) String search,
                         @RequestParam(defaultValue = "NAME") FileQuery.Sort sort,
                         @RequestParam(defaultValue = "1") int page,
                         @RequestParam(name = "page_size", defaultValue = "20") int pageSize) {
        return fmtService.listFiles(search, sort, page, pageSize);
    }

    @GetMapping("/files/{id}")
    public FmtFile file(@PathVariable String id) {
        return fmtService.getFile(id);
    }

    /** 上传：空文件/文件名兜底由业务层处理，返回的 FmtUpload 可继续落库。 */
    @PostMapping("/upload")
    public FmtUpload upload(@RequestParam("file") MultipartFile file) throws IOException {
        return fmtService.upload(file);
    }

    /** 下载：先落到临时文件（流式，SDK 内部 .part → 改名），再原样吐给调用方，最后删临时文件。 */
    @GetMapping("/files/{id}/download")
    public ResponseEntity<Resource> download(@PathVariable String id) throws IOException {
        Path tmp = Files.createTempFile("fmt-", ".bin");
        boolean ok = false;
        try {
            FmtPayload payload = fmtService.download(id, tmp);
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
        return fmtService.checkDelete(id);
    }

    /** 软删除进回收站；跨桶删除要 force=true，否则 FMT-016。 */
    @DeleteMapping("/files/{id}")
    public FmtFile delete(@PathVariable String id,
                          @RequestParam(defaultValue = "false") boolean force) {
        return fmtService.delete(id, force);
    }

    @GetMapping("/trash")
    public Object trash() {
        return fmtService.trash();
    }

    @PostMapping("/shares")
    public FmtShare createShare(@RequestParam("file_id") String fileId) {
        return fmtService.createShare(fileId);
    }

    @GetMapping("/shares")
    public List<FmtShare> shares(@RequestParam("file_id") String fileId) {
        return fmtService.listShares(fileId);
    }

    @DeleteMapping("/shares/{shareId}")
    public FmtShare revokeShare(@PathVariable String shareId) {
        return fmtService.revokeShare(shareId);
    }
}
