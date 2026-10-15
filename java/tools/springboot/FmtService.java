package com.hncgy.mybatis.service;

import org.lazy.fmt.client.FmtChannel;
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
import org.lazy.fmt.client.model.FmtTrashEmpty;
import org.lazy.fmt.client.model.FmtTrashListing;
import org.lazy.fmt.client.model.FmtUpload;
import org.springframework.stereotype.Service;

import java.io.InputStream;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;

/**
 * FMT 服务门面：通道由 starter 自动配置（fmt.* 里的地址与凭证），这里只做转发。
 *
 * <p>业务代码从头到尾看不到 token —— 它绑在通道上（设计文档 §1、§4）。
 * 通道是单例、由 Spring 管生命周期（destroyMethod = "close"），所以这里<strong>不要</strong>
 * try-with-resources，也不要 close()。
 */
@Service
public class FmtService {

    private final FmtChannel fmt;

    public FmtService(FmtChannel fmt) {
        this.fmt = fmt;
    }

    // ------------------------------------------------------------------
    // 健康检测与只读信息
    // ------------------------------------------------------------------

    /** 健康检测：GET /api/ping，唯一不需要 token 的接口；token 配错时它照样通。 */
    public boolean ping() {
        return fmt.ping().isOk();
    }

    /** 通道描述：地址 + 掩码后的凭证，可以放心打日志。 */
    public String describe() {
        return fmt.describe();
    }

    // 状态
    public FmtResult<FmtServerStatus> status() {
        return fmt.status();
    }

    // 配置（只读，响应里永远没有 token）
    public FmtResult<FmtConfig> getConfig() {
        return fmt.getConfig();
    }

    // 桶列表（只读；创建/切换/删除桶是 CLI 的活）
    public FmtResult<FmtBucketListing> listBuckets() {
        return fmt.listBuckets();
    }

    // ------------------------------------------------------------------
    // 文件
    // ------------------------------------------------------------------

    // 列文件：pageSize 给 0 表示不分页（要全量就传 0）
    public FmtResult<FmtPage> listFiles(String search, FileQuery.Sort sort, int page, int pageSize) {
        return fmt.listFiles(FileQuery.builder()
                .search(search)
                .sort(sort)
                .page(page)
                .pageSize(pageSize)
                .build());
    }

    // 单条信息（含相对数据根的 path）
    public FmtResult<FmtFile> getFile(String idOrName) {
        return fmt.getFile(idOrName);
    }

    // 上传字节
    public FmtResult<FmtUpload> upload(byte[] data, String fileName) {
        return fmt.upload(data, fileName);
    }

    // 上传流（chunked，不参与 401 重试；由调用方自己开流，SDK 不收 Path）
    public FmtResult<FmtUpload> upload(InputStream in, String fileName) {
        return fmt.upload(in, fileName);
    }

    // 下载（流式落盘，先写 .part 再改名；目标已存在会覆盖）
    public FmtResult<FmtPayload> download(String idOrName, Path target) {
        return fmt.download(idOrName, target);
    }

    // 下载：overwrite=false 时本地已存在就在发请求前拦下（FMT-003）
    public FmtResult<FmtPayload> download(String idOrName, Path target, boolean overwrite) {
        return fmt.download(idOrName, target, overwrite);
    }

    // 预览：图片与文本类内联，其余 FMT-701；下载不受这个限制
    public FmtResult<FmtPayload> preview(String idOrName, Path target, boolean overwrite) {
        return fmt.preview(idOrName, target, overwrite);
    }

    // ------------------------------------------------------------------
    // 删除与回收站
    // ------------------------------------------------------------------

    // 删除预检：?dry_run=1，零副作用
    public FmtResult<FmtDeleteCheck> checkDelete(String idOrName) {
        return fmt.checkDelete(idOrName);
    }

    // 软删除进回收站：跨桶删除必须 force=true，否则 FMT-016
    public FmtResult<FmtFile> delete(String idOrName, boolean force) {
        return fmt.delete(idOrName, force);
    }

    // 回收站列表
    public FmtResult<FmtTrashListing> listTrash() {
        return fmt.listTrash();
    }

    // 清空回收站：先 dryRun=true 预检，确认后 force=true（有内容缺 force → FMT-016）
    public FmtResult<FmtTrashEmpty> emptyTrash(boolean dryRun, boolean force) {
        return fmt.emptyTrash(dryRun, force);
    }

    // ------------------------------------------------------------------
    // 分享
    // ------------------------------------------------------------------

    // 建分享（POST /api/share，默认 20 次 / 7 天）
    public FmtResult<FmtShare> createShare(String fileId) {
        return fmt.createShare(fileId);
    }

    public FmtResult<FmtShare> getShare(String shareId) {
        return fmt.getShare(shareId);
    }

    public FmtResult<List<FmtShare>> listShares(String fileId) {
        return fmt.listShares(fileId);
    }

    public FmtResult<FmtShare> revokeShare(String shareId) {
        return fmt.revokeShare(shareId);
    }

    // 公开下载分享内容：故意不带 token（分享链接本身即凭证）
    public FmtResult<FmtPayload> downloadShare(String shareId, Path target, boolean overwrite) {
        return fmt.downloadShare(shareId, target, overwrite);
    }

    // ------------------------------------------------------------------
    // 逃生舱
    // ------------------------------------------------------------------

    // 调 SDK 还没包装 / FMT 以后新增的 JSON 接口；不要用它做上传下载
    public FmtResult<Object> call(String method, String path, Map<String, String> query, Object jsonBody) {
        return fmt.call(method, path, query, jsonBody);
    }
}