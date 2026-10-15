package org.lazy.fmt.service;

import org.lazy.fmt.client.Fmt;
import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtClientCore;
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
import org.lazy.fmt.tool.FmtTool;

import java.io.InputStream;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;

/**
 * FMT 服务门面：地址与 token 都在 FmtTool 里，这里只做转发，业务代码从头到尾看不到 token（设计文档 §1、§4）。
 *
 * <p>用起来就三步：{@code new FmtService()} → 调方法 → {@code close()}。
 * 想换地址/token 就换一个构造器，或用 {@link #FmtService(FmtClientCore)} 接管外部配置。
 * </p>
 */
public class FmtService implements AutoCloseable {

    FmtClientCore core = new FmtTool().FmtConnection();
    FmtChannel fmt = Fmt.channel(core);

    /** 默认：FmtTool 里的地址 + token。 */
    public FmtService() {
    }

    /** 换参数（比如从配置文件读出来的地址/token）。 */
    public FmtService(FmtClientCore core) {
        this.core = core;
        this.fmt = Fmt.channel(core);
    }

    /** 换地址 + token。 */
    public FmtService(String baseUrl, String token) {
        this(new FmtTool().FmtConnection(baseUrl, token));
    }

    /** 通道描述：地址 + 掩码后的凭证，可以放心打日志（§9）。 */
    public String describe() {
        return fmt.describe();
    }

    // ------------------------------------------------------------------
    // 健康检测与只读信息（§5.1）
    // ------------------------------------------------------------------

    /** 健康检测：`GET /api/ping`，唯一不需要 token 的接口；token 配错时它照样通。 */
    public boolean ping() {
        return fmt.ping().isOk();
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
    public FmtResult<FmtPage> listFiles(String search, FileQuery.Sort sort, int pageSize) {
        return fmt.listFiles(FileQuery.builder()
                .search(search)
                .sort(sort)
                .page(1)
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

    // 上传流（chunked，不参与 401 重试；本机文件由调用方自己开流，SDK 不收 Path）
    public FmtResult<FmtUpload> upload(InputStream in, String fileName) {
        return fmt.upload(in, fileName);
    }

    // 下载（流式落盘，先写 .part 再改名；默认覆盖）
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

    // ------------------------------------------------------------------
    // 生命周期
    // ------------------------------------------------------------------

    /** 关通道（内部线程池会停掉）；进程结束前调一次。 */
    @Override
    public void close() {
        fmt.close();
    }
}