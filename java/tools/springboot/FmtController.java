package com.hncgy.mybatis.controller;

import org.lazy.fmt.client.FmtResult;
import org.lazy.fmt.client.model.*;
import com.hncgy.mybatis.service.FmtService;
import org.springframework.core.io.Resource;
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

    /** 下载：文件基本信息 + 附件响应都在 FmtFileResponse 里。 */
    @GetMapping("/files/{id}/download")
    public ResponseEntity<Resource> download(@PathVariable String id) {
        return fmtService.downloadResponse(id).download();
    }

    /** 预览：图片/文本类内联返回，浏览器可直接作为 img src / iframe。 */
    @GetMapping("/files/{id}/preview")
    public ResponseEntity<Resource> preview(@PathVariable String id) {
        return fmtService.previewResponse(id).preview();
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
