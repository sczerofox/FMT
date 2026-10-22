package org.lazy.fmt.client.internal;

import org.lazy.fmt.client.FmtAuthScheme;
import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtChannelConfig;
import org.lazy.fmt.client.FmtCodec;
import org.lazy.fmt.client.FmtConfigurationException;
import org.lazy.fmt.client.FmtError;
import org.lazy.fmt.client.FmtErrorCodes;
import org.lazy.fmt.client.FmtException;
import org.lazy.fmt.client.FmtLogger;
import org.lazy.fmt.client.FmtResult;
import org.lazy.fmt.client.FmtTransportException;
import org.lazy.fmt.client.json.Json;
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
import org.lazy.fmt.client.model.FmtUploadSession;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ConnectException;
import java.net.InetAddress;
import java.net.URI;
import java.net.URISyntaxException;
import java.net.URLDecoder;
import java.net.URLEncoder;
import java.net.UnknownHostException;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Semaphore;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * {@link FmtChannel} 的默认实现：一个通道 = 一个 {@link HttpClient}（连接池）+ 一个凭证来源。
 *
 * <p><b>凭证注入点只有一处</b>：{@link #applyToken(HttpRequest.Builder)}。
 * 所有请求（JSON 调用、流式上传、流式下载）都经过 {@link #exchange} 构造请求，
 * 因此不存在「某个业务方法忘了带 token」这种事故——这和 FMT 服务端把认证放在
 * pre-routing 钩子里的思路是一模一样的。
 */
public final class DefaultFmtChannel implements FmtChannel {

    /** 错误响应体最多读这么多字节再解析，避免畸形响应把内存吃满。 */
    private static final int MAX_ERROR_BODY = 64 * 1024;

    private static final Duration RETRY_BACKOFF = Duration.ofMillis(150);

    private final FmtChannelConfig config;
    private final FmtCodec codec;
    private final FmtLogger logger;
    private final HttpClient client;
    private final ExecutorService executor;
    private final Semaphore permits;
    private final AtomicBoolean closed = new AtomicBoolean();
    /** 候选地址：第一个是配置里的，其余是主机名解析出来的字面地址（见 {@link #buildCandidates}）。 */
    private final List<URI> candidateUris;
    /** 最后连上的那个候选（下次请求直接从它开始，不再每次先撞一次墙）。 */
    private final java.util.concurrent.atomic.AtomicInteger activeUri =
            new java.util.concurrent.atomic.AtomicInteger();

    public DefaultFmtChannel(FmtChannelConfig config) {
        this.config = Objects.requireNonNull(config, "config 不能为空");
        this.codec = config.codec();
        this.logger = config.logger();
        this.permits = new Semaphore(config.maxConnections());
        this.candidateUris = List.copyOf(buildCandidates(config.baseUri()));
        this.executor = Executors.newCachedThreadPool(runnable -> {
            Thread thread = new Thread(runnable, "fmt-channel");
            thread.setDaemon(true);
            return thread;
        });
        HttpClient.Builder builder = HttpClient.newBuilder()
                // cpp-httplib 只说 HTTP/1.1：显式钉死，免掉 JDK 客户端的 HTTP/2 升级尝试
                .version(HttpClient.Version.HTTP_1_1)
                // 不做自动重定向：FMT 的 301/302 只可能来自中间设备，跟着走会把 token 带去别处
                .followRedirects(HttpClient.Redirect.NEVER)
                .executor(executor);
        if (isPositive(config.connectTimeout())) {
            builder.connectTimeout(config.connectTimeout());
        }
        this.client = builder.build();
        if (candidateUris.size() > 1) {
            logger.debug("通道候选地址（连接被拒时依次尝试）：" + candidateUris);
        }
    }

    /**
     * 候选地址：配置里给的是**主机名**时，把它的所有解析结果都变成候选。
     *
     * <p>为什么需要（真机踩过）：Windows 上 {@code localhost} 同时解析到
     * {@code 127.0.0.1} 与 {@code ::1}，而 FMT 的 {@code "host": "localhost"} 往往
     * **只绑了 {@code ::1}**；JDK 的 {@code HttpClient} 只会试**第一个**地址、
     * 不回落，于是"服务明明活着却连接被拒"。通道自己把另一族试一遍就自愈了。
     */
    static List<URI> buildCandidates(URI base) {
        List<URI> candidates = new ArrayList<>();
        candidates.add(base);
        String host = stripBrackets(base.getHost());
        if (host == null || host.isEmpty() || isLiteralAddress(host)) {
            return candidates;
        }
        try {
            for (InetAddress address : InetAddress.getAllByName(host)) {
                URI candidate = withLiteralHost(base, address.getHostAddress());
                if (!candidates.contains(candidate)) {
                    candidates.add(candidate);
                }
            }
        } catch (UnknownHostException unresolved) {
            // 解析不了就只留原始 URI：连接错误会如实报出来，不在这里吞掉
        }
        return candidates;
    }

    private static boolean isLiteralAddress(String host) {
        if (host.indexOf(':') >= 0) {
            return true; // IPv6 字面量
        }
        return host.matches("\\d{1,3}(\\.\\d{1,3}){3}");
    }

    private static String stripBrackets(String host) {
        if (host == null) {
            return null;
        }
        return host.startsWith("[") && host.endsWith("]")
                ? host.substring(1, host.length() - 1)
                : host;
    }

    private static URI withLiteralHost(URI base, String literal) {
        StringBuilder text = new StringBuilder();
        text.append(base.getScheme()).append("://");
        text.append(literal.indexOf(':') >= 0 ? "[" + literal + "]" : literal);
        if (base.getPort() > 0) {
            text.append(':').append(base.getPort());
        }
        String path = base.getRawPath();
        if (path != null && !path.isEmpty()) {
            text.append(path);
        }
        return URI.create(text.toString());
    }

    // ------------------------------------------------------------------
    // 对外方法
    // ------------------------------------------------------------------

    @Override
    public URI baseUri() {
        return config.baseUri();
    }

    @Override
    public String bucket() {
        return config.bucket();
    }

    @Override
    public String describe() {
        String token;
        try {
            token = config.tokenProvider().maskedToken();
        } catch (RuntimeException unavailable) {
            token = "(不可用：" + unavailable.getMessage() + ")";
        }
        return config.baseUri() + " [auth=" + config.authScheme().description()
                + ", bucket=" + (config.bucket() == null ? "(current)" : config.bucket())
                + ", token=" + token + "]";
    }

    @Override
    public FmtResult<Map<String, Object>> ping() {
        // ping 是公开接口：**故意不带凭证**，这样 token 配错时探活依然能说明「服务活着」
        return jsonCall("GET", "/api/ping", null, null, false).map(node -> {
            Map<String, Object> data = Json.castMap(node);
            return data == null ? Map.<String, Object>of() : data;
        });
    }

    @Override
    public FmtResult<FmtServerStatus> status() {
        return jsonCall("GET", "/api/status", null, null, true).map(FmtServerStatus::from);
    }

    @Override
    public FmtResult<FmtConfig> getConfig() {
        // 不带任何参数：服务端的 /api/config 一律不回 token，客户端也没有"要 token"这种诉求
        return jsonCall("GET", "/api/config", null, null, true).map(FmtConfig::from);
    }

    @Override
    public FmtResult<FmtBucketListing> listBuckets() {
        return jsonCall("GET", "/api/bucket", null, null, true).map(FmtBucketListing::from);
    }

    @Override
    public FmtResult<FmtPage> listFiles(FileQuery query) {
        FileQuery effective = query == null ? FileQuery.all() : query;
        return jsonCall("GET", "/api/file", effective.toQueryParams(), null, true).map(FmtPage::from);
    }

    @Override
    public FmtResult<FmtFile> getFile(String idOrName) {
        return jsonCall("GET", "/api/file/" + encodeSegment(idOrName), null, null, true)
                .map(FmtFile::fromRoot);
    }

    @Override
    public FmtResult<FmtUpload> upload(byte[] data, String fileName) {
        if (data == null) {
            return FmtResult.failure(FmtErrorCodes.INVALID_ARGUMENT, "上传内容不能为空");
        }
        String name;
        try {
            name = requireFileName(fileName);
        } catch (FmtConfigurationException invalid) {
            return FmtResult.failure(FmtErrorCodes.FILE_NAME_EMPTY, invalid.getMessage());
        }
        return uploadInternal(HttpRequest.BodyPublishers.ofByteArray(data), name, true);
    }

    @Override
    public FmtResult<FmtUpload> upload(InputStream in, String fileName) {
        if (in == null) {
            return FmtResult.failure(FmtErrorCodes.INVALID_ARGUMENT, "上传流不能为空");
        }
        String name;
        try {
            name = requireFileName(fileName);
        } catch (FmtConfigurationException invalid) {
            return FmtResult.failure(FmtErrorCodes.FILE_NAME_EMPTY, invalid.getMessage());
        }
        // 流只能读一次 → 不允许 401 重试（重试会读到已耗尽的流）。
        // 未知长度时走 chunked，服务端（cpp-httplib）支持。
        return uploadInternal(HttpRequest.BodyPublishers.ofInputStream(() -> in), name, false);
    }

    @Override
    public FmtResult<FmtUploadSession> initLargeUpload(String fileName, long size, long chunkSize,
                                                       String md5) {
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("file_name", fileName);
        body.put("size", size);
        if (chunkSize > 0) {
            body.put("chunk_size", chunkSize);
        }
        if (md5 != null && !md5.isBlank()) {
            body.put("md5", md5);
        }
        return jsonCall("POST", "/api/upload/init", null, body, true)
                .map(FmtUploadSession::from);
    }

    @Override
    public FmtResult<FmtUploadSession> largeUploadStatus(String uploadId) {
        return uploadSessionCall("GET", "/api/upload/" + encodeSegment(uploadId), null);
    }

    @Override
    public FmtResult<FmtUploadSession> uploadLargeChunk(String uploadId, int index, byte[] data,
                                                        String chunkMd5) {
        if (data == null) {
            return FmtResult.failure(FmtErrorCodes.INVALID_ARGUMENT, "分片内容不能为空");
        }
        String path = "/api/upload/" + encodeSegment(uploadId) + "/chunk";
        String encodedQuery = encodeQuery(Map.of("index", Integer.toString(index)));
        String label = "PUT " + path + "?" + encodedQuery;
        HttpResponse<String> response = exchange(label, true, true, false, config.streamTimeout(),
                () -> {
                    HttpRequest.Builder builder = HttpRequest.newBuilder(resolve(path, encodedQuery))
                            .PUT(HttpRequest.BodyPublishers.ofByteArray(data));
                    builder.setHeader("Content-Type", "application/octet-stream");
                    if (chunkMd5 != null && !chunkMd5.isBlank()) {
                        builder.setHeader("X-Chunk-MD5", chunkMd5);
                    }
                    return builder;
                },
                HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
        return parseEnvelope(response.statusCode(), response.body()).map(FmtUploadSession::from);
    }

    @Override
    public FmtResult<FmtUpload> completeLargeUpload(String uploadId) {
        String path = "/api/upload/" + encodeSegment(uploadId) + "/complete";
        String label = "POST " + path;
        HttpResponse<String> response = exchange(label, true, false, false, config.streamTimeout(),
                () -> HttpRequest.newBuilder(resolve(path, null))
                        .POST(HttpRequest.BodyPublishers.noBody()),
                HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
        return parseEnvelope(response.statusCode(), response.body()).map(FmtUpload::from);
    }

    @Override
    public FmtResult<FmtUploadSession> pauseLargeUpload(String uploadId) {
        return uploadSessionCall("POST", "/api/upload/" + encodeSegment(uploadId) + "/pause", null);
    }

    @Override
    public FmtResult<FmtUploadSession> resumeLargeUpload(String uploadId) {
        return uploadSessionCall("POST", "/api/upload/" + encodeSegment(uploadId) + "/resume", null);
    }

    @Override
    public FmtResult<FmtUploadSession> cancelLargeUpload(String uploadId) {
        return uploadSessionCall("DELETE", "/api/upload/" + encodeSegment(uploadId), null);
    }

    private FmtResult<FmtUploadSession> uploadSessionCall(String method, String path, Object body) {
        return jsonCall(method, path, null, body, true).map(FmtUploadSession::from);
    }

    @Override
    public FmtResult<FmtPayload> download(String idOrName, Path target) {
        return download(idOrName, target, true);
    }

    @Override
    public FmtResult<FmtPayload> download(String idOrName, Path target, boolean overwrite) {
        return content("/api/file/" + encodeSegment(idOrName) + "/download", target, false, overwrite, true);
    }

    @Override
    public FmtResult<FmtPayload> preview(String idOrName, Path target) {
        return preview(idOrName, target, true);
    }

    @Override
    public FmtResult<FmtPayload> preview(String idOrName, Path target, boolean overwrite) {
        return content("/api/file/" + encodeSegment(idOrName) + "/preview", target, true, overwrite, true);
    }

    @Override
    public FmtResult<FmtDeleteCheck> checkDelete(String idOrName) {
        Map<String, String> query = Map.of("dry_run", "1");
        return jsonCall("DELETE", "/api/file/" + encodeSegment(idOrName), query, null, true)
                .map(FmtDeleteCheck::from);
    }

    @Override
    public FmtResult<FmtFile> delete(String idOrName, boolean force) {
        Map<String, String> query = force ? Map.of("force", "1") : null;
        return jsonCall("DELETE", "/api/file/" + encodeSegment(idOrName), query, null, true)
                .map(FmtFile::fromRoot);
    }

    @Override
    public FmtResult<FmtTrashListing> listTrash() {
        return jsonCall("GET", "/api/trash", null, null, true).map(FmtTrashListing::from);
    }

    @Override
    public FmtResult<FmtTrashEntry> getTrash(String key) {
        return jsonCall("GET", "/api/trash/" + encodeSegment(key), null, null, true)
                .map(node -> FmtTrashEntry.from(Json.map(node, "entry")));
    }

    @Override
    public FmtResult<FmtTrashEntry> restoreTrash(String key) {
        return jsonCall("POST", "/api/trash/" + encodeSegment(key) + "/restore", null, null, true)
                .map(node -> FmtTrashEntry.from(Json.map(node, "entry")));
    }

    @Override
    public FmtResult<FmtTrashEntry> deleteTrash(String key, boolean force) {
        Map<String, String> query = force ? Map.of("force", "1") : null;
        return jsonCall("DELETE", "/api/trash/" + encodeSegment(key), query, null, true)
                .map(node -> FmtTrashEntry.from(Json.map(node, "entry")));
    }

    @Override
    public FmtResult<FmtTrashEmpty> emptyTrash(boolean dryRun, boolean force) {
        // 用 LinkedHashMap 而不是 Map.of：参数顺序固定，日志与抓包都好看
        Map<String, String> query = new LinkedHashMap<>();
        if (dryRun) {
            query.put("dry_run", "1");
        }
        if (force) {
            query.put("force", "1");
        }
        return jsonCall("DELETE", "/api/trash/empty", query.isEmpty() ? null : query, null, true)
                .map(FmtTrashEmpty::from);
    }

    @Override
    public FmtResult<FmtShare> createShare(String fileId) {
        if (fileId == null || fileId.isBlank()) {
            return FmtResult.failure(FmtErrorCodes.INVALID_ARGUMENT, "file_id 不能为空");
        }
        return jsonCall("POST", "/api/share", null, Map.of("file_id", fileId), true)
                .map(node -> FmtShare.from(Json.castMap(node)));
    }

    @Override
    public FmtResult<FmtShare> getShare(String shareId) {
        return jsonCall("GET", "/api/share/" + encodeSegment(shareId), null, null, true)
                .map(node -> FmtShare.from(Json.castMap(node)));
    }

    @Override
    public FmtResult<List<FmtShare>> listShares(String fileId) {
        Map<String, String> query = Map.of("file_id", fileId == null ? "" : fileId);
        return jsonCall("GET", "/api/share", query, null, true).map(node -> {
            List<FmtShare> shares = new ArrayList<>();
            for (Map<String, Object> item : Json.maps(node, "shares")) {
                FmtShare share = FmtShare.from(item);
                if (share != null) {
                    shares.add(share);
                }
            }
            return List.copyOf(shares);
        });
    }

    @Override
    public FmtResult<FmtShare> revokeShare(String shareId) {
        return jsonCall("DELETE", "/api/share/" + encodeSegment(shareId), null, null, true)
                .map(node -> FmtShare.from(Json.castMap(node)));
    }

    @Override
    public FmtResult<FmtPayload> downloadShare(String shareId, Path target, boolean overwrite) {
        // 公开端点：分享链接本身就是凭证，这里刻意不注入 token
        return content("/api/share/" + encodeSegment(shareId) + "/download", target, false, overwrite, false);
    }

    @Override
    public FmtResult<Object> call(String method, String path, Map<String, String> query, Object jsonBody) {
        if (method == null || method.isBlank()) {
            return FmtResult.failure(FmtErrorCodes.INVALID_ARGUMENT, "method 不能为空");
        }
        if (path == null || !path.startsWith("/")) {
            return FmtResult.failure(FmtErrorCodes.INVALID_ARGUMENT, "path 必须以 / 开头：" + path);
        }
        return jsonCall(method.trim().toUpperCase(Locale.ROOT), path, query, jsonBody, true);
    }

    @Override
    public void close() {
        if (!closed.compareAndSet(false, true)) {
            return;
        }
        executor.shutdownNow();
        logger.debug("FMT 通道已关闭：" + config.baseUri());
    }

    // ------------------------------------------------------------------
    // 凭证：整个 SDK 唯一注入 token 的地方
    // ------------------------------------------------------------------

    private void applyToken(HttpRequest.Builder builder) {
        String token = config.tokenProvider().token();
        if (token == null || token.isBlank()) {
            throw new FmtConfigurationException("凭证来源给出了空 token：" + describe());
        }
        switch (config.authScheme()) {
            case X_FMT_TOKEN -> builder.setHeader("X-FMT-Token", token);
            case BEARER -> builder.setHeader("Authorization", "Bearer " + token);
            case BOTH -> {
                builder.setHeader("X-FMT-Token", token);
                builder.setHeader("Authorization", "Bearer " + token);
            }
        }
        // 库名（Bucket）必须**每个请求都明显带上**：服务端会话 token 会校验它，
        // 旧账号 token 有它就按它走、没有就沿用服务端 current_bucket。
        String bucket = config.bucket();
        if (bucket != null && !bucket.isBlank()) {
            builder.setHeader("X-FMT-Bucket", bucket);
        }
    }

    private void applyCommonHeaders(HttpRequest.Builder builder) {
        builder.setHeader("User-Agent", config.userAgent());
        builder.setHeader("Accept", "application/json, */*");
        for (Map.Entry<String, String> header : config.extraHeaders().entrySet()) {
            builder.setHeader(header.getKey(), header.getValue());
        }
    }

    // ------------------------------------------------------------------
    // 请求管道
    // ------------------------------------------------------------------

    /** 只负责「方法 + 地址 + 请求体」；凭证、超时、公共头由 {@link #exchange} 统一加。 */
    @FunctionalInterface
    private interface RequestShape {
        HttpRequest.Builder builder();
    }

    /**
     * 所有 HTTP 调用的唯一出口。
     *
     * @param label      日志用的描述（**不含 token**）
     * @param authenticate 是否注入凭证（只有 ping 与公开分享下载是 false）
     * @param retryable  401 之后能否安全重发（请求体能否重新生成）
     * @param idempotent 传输失败能否重发（只有 GET 之类才为 true）
     */
    private <R> HttpResponse<R> exchange(String label,
                                         boolean authenticate,
                                         boolean retryable,
                                         boolean idempotent,
                                         Duration timeout,
                                         RequestShape shape,
                                         HttpResponse.BodyHandler<R> handler) {
        ensureOpen();
        boolean acquired = false;
        try {
            permits.acquire();
            acquired = true;
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new FmtTransportException("等待通道配额时被中断：" + label, interrupted);
        }
        try {
            int authLeft = (authenticate && retryable && config.retryOnUnauthorized()) ? 1 : 0;
            int ioLeft = idempotent ? config.idempotentRetries() : 0;
            final int startIndex = activeUri.get();
            int currentIndex = startIndex;
            int addressTries = 0;
            while (true) {
                long startedAt = System.nanoTime();
                HttpRequest request;
                try {
                    HttpRequest.Builder builder = shape.builder();
                    if (isPositive(timeout)) {
                        builder.timeout(timeout);
                    }
                    applyCommonHeaders(builder);
                    if (authenticate) {
                        applyToken(builder);
                    }
                    request = builder.build();
                } catch (FmtException alreadyMeaningful) {
                    throw alreadyMeaningful;
                } catch (IllegalArgumentException rejected) {
                    // JDK 客户端会拒绝 Host / Connection / Content-Length 这类受限头
                    throw new FmtConfigurationException(
                            "构造请求失败（" + label + "）：" + rejected.getMessage()
                                    + "；受限请求头（Host/Connection/Content-Length/Upgrade 等）"
                                    + "需要 -Djdk.httpclient.allowRestrictedHeaders 才能设置", rejected);
                }

                HttpResponse<R> response;
                try {
                    response = client.send(request, handler);
                } catch (IOException failure) {
                    // 连接**根本没建立**（ConnectException）→ 换一个候选地址重发是安全的：
                    // 请求体一个字节都没写出去。这就是 localhost 解析出 127.0.0.1 + ::1、
                    // 而 FMT 只绑了其中一个时的救命路径（JDK 客户端自己不会回落）。
                    if (failure instanceof ConnectException
                            && addressTries < candidateUris.size() - 1) {
                        addressTries++;
                        int next = (startIndex + addressTries) % candidateUris.size();
                        logger.warn("连接 " + candidateUris.get(currentIndex) + " 被拒，改用 "
                                + candidateUris.get(next) + " 重试（" + label + "）");
                        currentIndex = next;
                        activeUri.set(next);
                        continue;
                    }
                    if (ioLeft-- > 0) {
                        logger.warn("调用 FMT 传输失败，重试（" + label + "）：" + failure);
                        backoff();
                        continue;
                    }
                    throw new FmtTransportException(
                            "调用 FMT 失败（" + label + "）：" + failure
                                    + "；已尝试的地址：" + candidateUris
                                    + "。请确认 fmt.exe 服务在跑、config/server.json 里 HTTP 已打开"
                                    + "（host 写 localhost 时 httplib 可能只绑了 IPv6 的 ::1，"
                                    + "通道会自动回落；如果所有候选都失败，那就是真的连不上）", failure);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new FmtTransportException("调用 FMT 被中断（" + label + "）", interrupted);
                }

                logger.debug(String.format(Locale.ROOT, "FMT %s -> HTTP %d（%.1f ms）",
                        label, response.statusCode(), (System.nanoTime() - startedAt) / 1_000_000.0));

                if (response.statusCode() != 401 || !authenticate) {
                    return response;
                }
                if (authLeft-- <= 0) {
                    return response; // 已经重试过一次：把 401 交回调用方变成 FMT-018
                }
                closeQuietly(response.body());
                logger.warn("FMT 返回 401：作废缓存凭证并重试一次（" + label + "）");
                config.tokenProvider().invalidate();
            }
        } finally {
            if (acquired) {
                permits.release();
            }
        }
    }

    /** JSON 调用：请求体可选，响应体按 UTF-8 读成字符串再解析信封。 */
    private FmtResult<Object> jsonCall(String method, String path, Map<String, String> query,
                                       Object body, boolean authenticate) {
        HttpRequest.BodyPublisher publisher = body == null
                ? HttpRequest.BodyPublishers.noBody()
                : HttpRequest.BodyPublishers.ofString(codec.write(body), StandardCharsets.UTF_8);
        String encodedQuery = encodeQuery(query);
        String label = method + " " + path + (encodedQuery.isEmpty() ? "" : "?" + encodedQuery);

        HttpResponse<String> response = exchange(label, authenticate, true,
                isIdempotent(method), config.requestTimeout(),
                () -> {
                    HttpRequest.Builder builder =
                            HttpRequest.newBuilder(resolve(path, encodedQuery)).method(method, publisher);
                    if (body != null) {
                        builder.setHeader("Content-Type", "application/json; charset=utf-8");
                    }
                    return builder;
                },
                HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
        return parseEnvelope(response.statusCode(), response.body());
    }

    private FmtResult<FmtUpload> uploadInternal(HttpRequest.BodyPublisher publisher, String fileName,
                                               boolean retryable) {
        String encodedQuery = encodeQuery(Map.of("name", fileName));
        String label = "POST /api/file/upload?name=" + fileName;
        HttpResponse<String> response = exchange(label, true, retryable, false, config.streamTimeout(),
                () -> {
                    HttpRequest.Builder builder = HttpRequest.newBuilder(resolve("/api/file/upload", encodedQuery))
                            .POST(publisher);
                    builder.setHeader("Content-Type", "application/octet-stream");
                    // 服务端优先看 ?name=，这里再给一份 UTF-8 百分号编码的 Content-Disposition 作保险
                    builder.setHeader("Content-Disposition",
                            "attachment; filename*=UTF-8''" + encodeSegment(fileName));
                    return builder;
                },
                HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
        return parseEnvelope(response.statusCode(), response.body()).map(FmtUpload::from);
    }

    /** 下载/预览：流式落盘，先写 {@code .part} 再改名，绝不留半截文件。 */
    private FmtResult<FmtPayload> content(String path, Path target, boolean inline, boolean overwrite,
                                          boolean authenticate) {
        if (target == null) {
            return FmtResult.failure(FmtErrorCodes.INVALID_ARGUMENT, "目标路径不能为空");
        }
        Path absolute = target.toAbsolutePath().normalize();
        if (Files.exists(absolute) && !overwrite) {
            return FmtResult.failure(FmtErrorCodes.FILE_ALREADY_EXISTS,
                    "目标文件已存在：" + absolute + "（要覆盖请传 overwrite=true）");
        }

        String label = "GET " + path;
        HttpResponse<InputStream> response = exchange(label, authenticate, true, true, config.streamTimeout(),
                () -> HttpRequest.newBuilder(resolve(path, null)).GET(),
                HttpResponse.BodyHandlers.ofInputStream());

        if (response.statusCode() != 200) {
            String body = readText(response.body());
            // 非 200 的响应体是错误信封；解析失败会抛传输异常，这才是「不像 FMT」的信号
            return FmtResult.failure(parseEnvelope(response.statusCode(), body).error());
        }

        Path parent = absolute.getParent();
        if (parent != null) {
            try {
                Files.createDirectories(parent);
            } catch (IOException failure) {
                closeQuietly(response.body());
                throw new FmtTransportException("无法创建目录：" + parent, failure);
            }
        }
        Path part = absolute.resolveSibling(absolute.getFileName().toString() + ".part");
        long expected = response.headers().firstValueAsLong("Content-Length").orElse(-1L);
        long written;
        try (InputStream in = response.body();
             OutputStream out = Files.newOutputStream(part,
                     StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING,
                     StandardOpenOption.WRITE)) {
            written = in.transferTo(out);
        } catch (IOException failure) {
            deleteQuietly(part);
            throw new FmtTransportException("下载写入失败：" + absolute + "（" + failure + "）", failure);
        }
        if (expected >= 0 && written != expected) {
            deleteQuietly(part);
            throw new FmtTransportException(
                    "下载不完整：服务端声明 " + expected + " 字节，实际收到 " + written + " 字节", 0);
        }
        try {
            if (overwrite) {
                Files.move(part, absolute, StandardCopyOption.REPLACE_EXISTING);
            } else {
                Files.move(part, absolute);
            }
        } catch (IOException failure) {
            deleteQuietly(part);
            throw new FmtTransportException("下载落地失败：" + absolute, failure);
        }

        String serverName = parseFileName(response.headers().firstValue("Content-Disposition").orElse(null));
        String contentType = response.headers().firstValue("Content-Type")
                .orElse("application/octet-stream");
        String fileName = serverName.isEmpty() ? absolute.getFileName().toString() : serverName;
        return FmtResult.ok(new FmtPayload(absolute, written, fileName, contentType, inline));
    }

    // ------------------------------------------------------------------
    // 信封解析
    // ------------------------------------------------------------------

    /** {@code {"ok":true,"data":…}} / {@code {"ok":false,"error":{…}}} -> 结果。 */
    private FmtResult<Object> parseEnvelope(int status, String body) {
        Object node;
        try {
            node = codec.read(body == null ? "" : body);
        } catch (FmtException broken) {
            throw new FmtTransportException(
                    "响应不是 JSON 信封（HTTP " + status + "）：" + snippet(body), status, broken);
        }
        Map<String, Object> envelope = Json.castMap(node);
        Object okFlag = envelope == null ? null : envelope.get("ok");
        if (!(okFlag instanceof Boolean ok)) {
            throw new FmtTransportException(
                    "响应缺少 ok 字段（HTTP " + status + "）：" + snippet(body), status);
        }
        if (ok) {
            return FmtResult.ok(envelope.get("data"));
        }
        Map<String, Object> detail = Json.map(envelope, "error");
        String code = Json.str(detail, "code", FmtErrorCodes.HTTP_REQUEST_INVALID);
        String message = Json.str(detail, "message", "FMT 返回了未说明的错误");
        return FmtResult.failure(new FmtError(code, message, status));
    }

    // ------------------------------------------------------------------
    // 小工具
    // ------------------------------------------------------------------

    private void ensureOpen() {
        if (closed.get()) {
            throw new IllegalStateException("FMT 通道已关闭：" + config.baseUri());
        }
    }

    private URI resolve(String path, String encodedQuery) {
        String base = candidateUris.get(activeUri.get()).toString();
        StringBuilder url = new StringBuilder(base.length() + path.length() + 32);
        url.append(base);
        if (!path.startsWith("/")) {
            url.append('/');
        }
        url.append(path);
        if (encodedQuery != null && !encodedQuery.isEmpty()) {
            url.append('?').append(encodedQuery);
        }
        try {
            return new URI(url.toString());
        } catch (URISyntaxException broken) {
            throw new FmtConfigurationException("拼出的 URL 非法：" + url, broken);
        }
    }

    /** 路径段编码：文件名可能是中文，必须百分号编码（服务端再 url_decode 回来）。 */
    private static String encodeSegment(String value) {
        if (value == null) {
            throw new FmtConfigurationException("路径参数不能为空");
        }
        return URLEncoder.encode(value, StandardCharsets.UTF_8).replace("+", "%20");
    }

    private static String encodeQuery(Map<String, String> query) {
        if (query == null || query.isEmpty()) {
            return "";
        }
        StringBuilder out = new StringBuilder();
        for (Map.Entry<String, String> entry : query.entrySet()) {
            if (out.length() > 0) {
                out.append('&');
            }
            out.append(encodeSegment(entry.getKey())).append('=').append(encodeSegment(entry.getValue()));
        }
        return out.toString();
    }

    private static boolean isIdempotent(String method) {
        return "GET".equalsIgnoreCase(method) || "HEAD".equalsIgnoreCase(method);
    }

    private static boolean isPositive(Duration duration) {
        return duration != null && !duration.isZero() && !duration.isNegative();
    }

    private static String requireFileName(String fileName) {
        if (fileName == null || fileName.isBlank()) {
            throw new FmtConfigurationException("上传必须给文件名：查询串 ?name= 或 Content-Disposition 都行");
        }
        return fileName.trim();
    }

    /** 从 {@code Content-Disposition} 里取文件名，优先 RFC 5987 的 {@code filename*}。 */
    private static String parseFileName(String header) {
        if (header == null || header.isBlank()) {
            return "";
        }
        int star = header.indexOf("filename*=");
        if (star >= 0) {
            String value = stripQuotes(section(header.substring(star + "filename*=".length())));
            int charset = value.indexOf("''");
            if (charset >= 0) {
                value = value.substring(charset + 2);
            }
            return decodePercent(value);
        }
        int plain = header.indexOf("filename=");
        if (plain >= 0) {
            return stripQuotes(section(header.substring(plain + "filename=".length())));
        }
        return "";
    }

    private static String section(String value) {
        int end = value.indexOf(';');
        return (end >= 0 ? value.substring(0, end) : value).trim();
    }

    private static String stripQuotes(String value) {
        String text = value.trim();
        if (text.length() >= 2
                && (text.charAt(0) == '"' || text.charAt(0) == '\'')
                && text.charAt(text.length() - 1) == text.charAt(0)) {
            return text.substring(1, text.length() - 1);
        }
        return text;
    }

    private static String decodePercent(String value) {
        // URLDecoder 会把 '+' 解成空格，先把它保护起来；文件名里的 '+' 很常见
        return URLDecoder.decode(value.replace("+", "%2B"), StandardCharsets.UTF_8);
    }

    private static String readText(InputStream in) {
        if (in == null) {
            return "";
        }
        try (InputStream stream = in) {
            byte[] buffer = stream.readNBytes(MAX_ERROR_BODY);
            return new String(buffer, StandardCharsets.UTF_8);
        } catch (IOException failure) {
            return "";
        }
    }

    private static String snippet(String body) {
        if (body == null) {
            return "(空)";
        }
        String text = body.replaceAll("\\s+", " ").trim();
        if (text.length() > 200) {
            text = text.substring(0, 200) + "…";
        }
        return text.isEmpty() ? "(空)" : text;
    }

    private static void backoff() {
        try {
            Thread.sleep(RETRY_BACKOFF.toMillis());
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    private static void closeQuietly(Object body) {
        if (body instanceof InputStream stream) {
            try {
                stream.close();
            } catch (IOException ignored) {
                // 关闭失败无所谓：连接会被连接池回收或丢弃
            }
        }
    }

    private static void deleteQuietly(Path path) {
        try {
            Files.deleteIfExists(path);
        } catch (IOException ignored) {
            // 删不掉就留着，反正它叫 xxx.part，一眼能看出是半成品
        }
    }
}
