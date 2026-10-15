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

import java.io.InputStream;
import java.net.URI;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;

/**
 * FMT HTTP 通道：**一个通道 = 一份连接配置 + 一份绑定在通道上的凭证**。
 *
 * <h2>token 在哪里</h2>
 * token 在**通道**里，不在方法参数里。业务代码从头到尾看不到 token：
 * <pre>{@code
 * // 建立通道时把凭证交给它（来源可以是固定串、user.json、环境变量）
 * try (FmtChannel fmt = Fmt.channel(FmtChannelConfig.builder()
 *         .baseUrl("http://127.0.0.1:4122")
 *         .tokenProvider(TokenProviders.fromDataRoot(Path.of("D:/Tools/fmt")))
 *         .build())) {
 *
 *     // 之后每一次调用，token 都由通道自动写进请求头
 *     FmtResult<FmtPage> page = fmt.listFiles(FileQuery.all());
 * }
 * }</pre>
 *
 * <p>实现细节（很重要，决定了能承诺什么）：HTTP 协议里请求头是**逐请求**的，
 * 没有任何办法把 header 真正「焊死」在一条 TCP 连接上。所以这里的做法是：
 * <ol>
 *   <li>通道持有 {@link TokenProvider}，业务层不接触凭证；</li>
 *   <li>通道内部只有一个 {@link java.net.http.HttpClient}，TCP 连接是**池化并复用**的
 *       （keep-alive）——物理连接确实被「建立一次、多次使用」；</li>
 *   <li>每次请求由通道在**唯一一个地方**注入 {@code X-FMT-Token} / {@code Authorization}；</li>
 *   <li>收到 401（{@code FMT-018}）时通道会作废缓存凭证、重新取一次并**自动重试一次**，
 *       这样服务端换 token、user.json 被改写都不需要重启 Java 应用。</li>
 * </ol>
 * 想要「握手一次、之后连接内免 token」的会话语义，必须让 fmt.exe 支持（当前不支持），
 * 见 {@code docx/FMT Java 客户端接入设计.md} 的「演进」一节。
 *
 * <h2>错误模型</h2>
 * <ul>
 *   <li>业务错误（服务端信封 {@code ok=false}）→ {@link FmtResult#error()}；要异常风格就
 *       {@link FmtResult#orThrow()}（抛 {@link FmtApiException}）。</li>
 *   <li>连不上、超时、响应不是 JSON 信封 → {@link FmtTransportException}（直接抛）。</li>
 *   <li>配置不对（token 没配、baseUrl 非法）→ {@link FmtConfigurationException}（直接抛）。</li>
 * </ul>
 *
 * <h2>FMT HTTP 侧的真实边界（SDK 不假装有）</h2>
 * <ul>
 *   <li><b>Bucket 只读</b>：只有 {@code GET /api/bucket}（{@link #listBuckets()}）。
 *       创建/切换/删除桶由 CLI 管——「当前 Bucket」是服务端的全局状态，多个 HTTP 调用方
 *       一起改会互相踩，而删除桶还有确认流程。</li>
 *   <li><b>配置只读且不带凭证</b>：{@code GET /api/config}（{@link #getConfig()}）能读
 *       {@code max_upload_size}、当前桶等，但改不了；而且**响应里永远没有 token**
 *       （连「要不要带」的开关都不提供）。</li>
 *   <li><b>清空回收站要确认</b>：{@code DELETE /api/trash/empty}（{@link #emptyTrash}）
 *       在有内容时必须 {@code force=true}，否则 {@code FMT-016}；先 {@code dryRun=true} 预检。</li>
 *   <li>服务**只监听本机**且默认关闭：Java 进程必须和 FMT 在同一台机器上（或自己做端口转发）。</li>
 * </ul>
 *
 * <p>线程安全：实现是无状态的（状态都在 HttpClient 与凭证缓存里），可以跨线程共享一个通道。
 */
public interface FmtChannel extends AutoCloseable {

    /** 通道的连接地址，例如 {@code http://127.0.0.1:4122}。 */
    URI baseUri();

    /** 便于日志的一行描述：地址 + 掩码后的凭证。**不含 token 明文。** */
    String describe();

    // ------------------------------------------------------------------
    // 健康检查
    // ------------------------------------------------------------------

    /** {@code GET /api/ping}：**唯一不需要 token** 的业务探活接口。 */
    FmtResult<Map<String, Object>> ping();

    /** {@code GET /api/status}：数据根、进程号、版本（这个要 token）。 */
    FmtResult<FmtServerStatus> status();

    /**
     * {@code GET /api/config}：服务端配置（**只读**）。
     *
     * <p>**响应里永远没有 token**：HTTP 侧的 `/api/config` 连「要不要带 token」的开关都不提供
     * （响应体会进日志、被代理、被贴进工单）。要 token 只有两条路——CLI
     * {@code fmt.exe config list --show-token}（默认不打印，见 README），或直接读数据根下的
     * {@code data/user.json}（{@link TokenProviders#fromDataRoot} 就是这么干的）。
     */
    FmtResult<FmtConfig> getConfig();

    /**
     * {@code GET /api/bucket}：Bucket 列表（**只读**，含当前桶）。
     *
     * <p>HTTP 侧没有创建/切换/删除桶的接口——那是 CLI 的活（见类注释）。
     */
    FmtResult<FmtBucketListing> listBuckets();

    // ------------------------------------------------------------------
    // 文件
    // ------------------------------------------------------------------

    /** {@code GET /api/file}：列出**当前 Bucket** 的文件（支持搜索/排序/分页）。 */
    FmtResult<FmtPage> listFiles(FileQuery query);

    /** {@code GET /api/file/<file_id 或文件名>}：单条信息（含相对数据根的 path）。 */
    FmtResult<FmtFile> getFile(String idOrName);

    /**
     * 上传一段内存里的字节（{@code fileName} 必填）。
     *
     * <p><b>我们只提供流式/内存上传，不提供「上传本机路径」</b>（安全口径）：接口一旦收
     * {@code Path}，调用方就会把**服务端**的路径当成参数递进来——服务端只监听 localhost 时
     * 那等于把服务进程能读到的文件直接拖出来。内容只能由调用方自己读成字节或流交进来，
     * 路径永远不进入请求。
     */
    FmtResult<FmtUpload> upload(byte[] data, String fileName);

    /**
     * 上传一个流（Spring 的 {@code MultipartFile}、网络流、本机文件自己开的流等）。
     *
     * <p>流的长度未知，因此客户端走 {@code Transfer-Encoding: chunked}（服务端
     * cpp-httplib 支持）；也因为流只能读一次，**这一条不参与 401 自动重试**——
     * token 配错时你会拿到 {@code FMT-018}，而不是一个读到一半的流。
     *
     * <p>想传本机文件：{@code upload(Files.newInputStream(path), path.getFileName().toString())}
     * ——由**调用方**决定读哪个文件，SDK 不接受路径参数（见 {@link #upload(byte[], String)}）。
     */
    FmtResult<FmtUpload> upload(InputStream in, String fileName);

    FmtResult<FmtPayload> download(String idOrName, Path target);

    FmtResult<FmtPayload> download(String idOrName, Path target, boolean overwrite);

    /**
     * 预览：图片与文本类（{@code .txt/.md/.json/.csv/.log/.xml}）内联，其余 {@code FMT-701}。
     * **下载不受这个限制**，任何类型都能 {@link #download}。
     */
    FmtResult<FmtPayload> preview(String idOrName, Path target);

    FmtResult<FmtPayload> preview(String idOrName, Path target, boolean overwrite);

    /** {@code DELETE /api/file/<…>?dry_run=1}：只预检，零副作用。 */
    FmtResult<FmtDeleteCheck> checkDelete(String idOrName);

    /**
     * {@code DELETE /api/file/<…>}：软删除进回收站。
     *
     * @param force 跨 Bucket 删除时必须为 {@code true}，否则服务端返回 {@code FMT-016}
     */
    FmtResult<FmtFile> delete(String idOrName, boolean force);

    // ------------------------------------------------------------------
    // 回收站
    // ------------------------------------------------------------------

    /** {@code GET /api/trash}：文件级与桶级条目都会列出来。 */
    FmtResult<FmtTrashListing> listTrash();

    FmtResult<FmtTrashEntry> getTrash(String key);

    FmtResult<FmtTrashEntry> restoreTrash(String key);

    /** {@code DELETE /api/trash/<key>}：**永久删除，不可恢复**，必须 {@code force=true}。 */
    FmtResult<FmtTrashEntry> deleteTrash(String key, boolean force);

    /**
     * {@code DELETE /api/trash/empty}：清空整个回收站（**不可恢复**）。
     *
     * <p>必须显式确认；回收站本来就空时服务端不要求确认。
     *
     * @param dryRun 只预检：报「几项、多少字节」，零副作用
     * @param force  已确认执行（有内容时不给会拿到 {@code FMT-016}）
     */
    FmtResult<FmtTrashEmpty> emptyTrash(boolean dryRun, boolean force);

    // ------------------------------------------------------------------
    // 分享
    // ------------------------------------------------------------------

    /** {@code POST /api/share}：默认 20 次、7 天过期（服务端策略）。 */
    FmtResult<FmtShare> createShare(String fileId);

    FmtResult<FmtShare> getShare(String shareId);

    FmtResult<List<FmtShare>> listShares(String fileId);

    FmtResult<FmtShare> revokeShare(String shareId);

    /** {@code GET /api/share/<share_id>/download}：**公开端点，不带 token**（分享链接即凭证）。 */
    FmtResult<FmtPayload> downloadShare(String shareId, Path target, boolean overwrite);

    // ------------------------------------------------------------------
    // 兜底
    // ------------------------------------------------------------------

    /**
     * 逃生舱：直接按 HTTP 语义调一个 JSON 接口，返回信封里的 {@code data} 原始节点。
     *
     * <p>用于 SDK 还没包装的接口，或 FMT 以后新增的接口（不必等 SDK 升级）。
     * 不要用它调流式接口（上传/下载）。
     *
     * @param method   GET / POST / DELETE / PATCH / PUT
     * @param path     以 {@code /} 开头的路径，例如 {@code /api/file}
     * @param query    查询参数，值会被 URL 编码；可为 {@code null}
     * @param jsonBody 请求体对象（会被编成 JSON）；GET 或无需请求体时传 {@code null}
     */
    FmtResult<Object> call(String method, String path, Map<String, String> query, Object jsonBody);

    /** 关闭通道：停掉内部线程池，之后再用这个通道会抛 {@link IllegalStateException}。 */
    @Override
    void close();
}
