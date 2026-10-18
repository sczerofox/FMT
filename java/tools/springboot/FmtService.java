package com.hncgy.mybatis.service;

import org.lazy.fmt.client.FmtApiException;
import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtError;
import org.lazy.fmt.client.FmtErrorCodes;
import org.lazy.fmt.client.FmtResult;
import org.lazy.fmt.client.model.FileQuery;
import org.lazy.fmt.client.model.FmtBucketListing;
import org.lazy.fmt.client.model.FmtConfig;
import org.lazy.fmt.client.model.FmtDeleteCheck;
import org.lazy.fmt.client.model.FmtFile;
import org.lazy.fmt.client.model.FmtPage;
import org.lazy.fmt.client.model.FmtPayload;
import org.lazy.fmt.client.model.FmtServerStatus;
import org.lazy.fmt.client.model.FmtShare;
import org.lazy.fmt.client.model.FmtTrashListing;
import org.lazy.fmt.client.model.FmtUpload;
import org.springframework.stereotype.Service;
import org.springframework.web.multipart.MultipartFile;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Path;
import java.util.List;

/**
 * FMT 业务层：Controller 只做 HTTP 参数/响应，真正的业务编排在这里。
 *
 * <p>以后 {@code UserService} 可以直接注入本类：
 * <ul>
 *   <li>{@link #upload(MultipartFile)} 返回的 {@link FmtUpload} 里带
 *       {@code file_id/file_name/path/size/md5}，可以落库；</li>
 *   <li>{@link #getFile(String)} 可拿单条记录；预览/下载由 Controller 或后续业务决定；</li>
 *   <li>{@link #download(String, Path)} 可把预览所需内容落到调用方指定的文件。</li>
 * </ul>
 *
 * <p><b>凭证不在这层出现</b>：无论是 {@code fmt.token} 还是账号密码，starter 都把它
 * 绑在 {@link FmtChannel} 上；Controller / UserService 调业务方法时不需要任何 token 参数。
 */
@Service
public class FmtService {

    private final FmtChannel fmt;

    public FmtService(FmtChannel fmt) {
        this.fmt = fmt;
    }

    // ------------------------------------------------------------------
    // 探活与只读信息
    // ------------------------------------------------------------------

    public boolean ping() {
        return fmt.ping().isOk();
    }

    /** 保留信封形状（{@code {ok,data}}），Controller 直接返回。 */
    public FmtResult<FmtServerStatus> status() {
        return fmt.status();
    }

    public FmtConfig config() {
        return fmt.getConfig().orThrow();
    }

    public FmtBucketListing buckets() {
        return fmt.listBuckets().orThrow();
    }

    // ------------------------------------------------------------------
    // 文件
    // ------------------------------------------------------------------

    public FmtPage listFiles(String search, FileQuery.Sort sort, int page, int pageSize) {
        return fmt.listFiles(FileQuery.builder()
                .search(search)
                .sort(sort)
                .page(page)
                .pageSize(pageSize)
                .build()).orThrow();
    }

    public FmtFile getFile(String idOrName) {
        return fmt.getFile(idOrName).orThrow();
    }

    /**
     * 上传：空文件/缺文件名的兜底在这里做；返回的 {@link FmtUpload} 可直接交给
     * {@code UserService} 落库（含 file_id、file_name、path 等）。
     */
    public FmtUpload upload(MultipartFile file) throws IOException {
        if (file == null || file.isEmpty()) {
            throw new FmtApiException(
                    FmtError.local(FmtErrorCodes.INVALID_ARGUMENT, "上传的文件是空的"));
        }
        String name = file.getOriginalFilename();
        if (name == null || name.isBlank()) {
            name = "upload.bin";
        }
        try (InputStream in = file.getInputStream()) {
            return fmt.upload(in, name).orThrow();
        }
    }

    /** 下载/预览落盘；{@code target} 由调用方决定，SDK 不接受服务端路径。 */
    public FmtPayload download(String idOrName, Path target) {
        return fmt.download(idOrName, target).orThrow();
    }

    public FmtDeleteCheck checkDelete(String idOrName) {
        return fmt.checkDelete(idOrName).orThrow();
    }

    public FmtFile delete(String idOrName, boolean force) {
        return fmt.delete(idOrName, force).orThrow();
    }

    public FmtTrashListing trash() {
        return fmt.listTrash().orThrow();
    }

    // ------------------------------------------------------------------
    // 分享
    // ------------------------------------------------------------------

    public FmtShare createShare(String fileId) {
        return fmt.createShare(fileId).orThrow();
    }

    public List<FmtShare> listShares(String fileId) {
        return fmt.listShares(fileId).orThrow();
    }

    public FmtShare revokeShare(String shareId) {
        return fmt.revokeShare(shareId).orThrow();
    }
}
