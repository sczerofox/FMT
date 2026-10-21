package org.lazy.fmt.client;

import org.lazy.fmt.client.model.FileQuery;
import org.lazy.fmt.client.model.FmtBucketListing;
import org.lazy.fmt.client.model.FmtConfig;
import org.lazy.fmt.client.model.FmtDeleteCheck;
import org.lazy.fmt.client.model.FmtFile;
import org.lazy.fmt.client.model.FmtFilePackage;
import org.lazy.fmt.client.model.FmtPage;
import org.lazy.fmt.client.model.FmtPayload;
import org.lazy.fmt.client.model.FmtServerStatus;
import org.lazy.fmt.client.model.FmtShare;
import org.lazy.fmt.client.model.FmtTrashListing;
import org.lazy.fmt.client.model.FmtUpload;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.security.DigestInputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Duration;
import java.util.HexFormat;
import java.util.List;

/**
 * 最简单的 FMT 客户端：**先建连接，再调业务**。
 *
 * <pre>{@code
 * // 账号密码 + 桶（地址默认 http://localhost:4122）
 * try (FmtClient fmt = new FmtClient("lazy", "123456", "pet")) {
 *     fmt.ping();
 *     FmtFilePackage pkg = fmt.upload(Files.newInputStream(Path.of("avatar.png")), "avatar.png");
 *     String fileId = pkg.fileId();
 *     String fileName = pkg.fileName();
 *     FmtMedia preview = pkg.preview();   // Spring Boot 可直接流式返回
 *     FmtMedia download = pkg.download();
 * }
 *
 * // token + 桶（不填桶 = current_bucket）
 * try (FmtClient fmt = FmtClient.token(token, "pet")) {
 *     fmt.status();
 * }
 *
 * // set 方式；地址/超时都有默认值
 * FmtClient fmt = new FmtClient()
 *         .setBaseUrl("http://localhost:4122")
 *         .setUsername("lazy")
 *         .setPassword("123456")
 *         .setBucket("pet");
 * }</pre>
 *
 * <p>业务方法直接返回领域对象，失败抛 {@link FmtApiException}（业务）/
 * {@link FmtTransportException}（连接）/ {@link FmtConfigurationException}（配置）；
 * 不再要求调用方到处写 {@code .orThrow()}。
 *
 * <p>要底层 {@link FmtChannel}（例如新接口、逃生舱），用 {@link #channel()}。
 */
public final class FmtClient implements AutoCloseable {

    /** 默认地址：写 localhost 而不是字面 IP，SDK 会自动在 IPv4/IPv6 之间回落。 */
    public static final String DEFAULT_BASE_URL = FmtChannelConfig.DEFAULT_BASE_URL;

    private String baseUrl = DEFAULT_BASE_URL;
    private String username;
    private String password;
    private String token;
    private String bucket;

    private Duration connectTimeout = Duration.ofSeconds(3);
    private Duration requestTimeout = Duration.ofSeconds(30);
    private Duration streamTimeout = Duration.ofMinutes(30);
    private int maxConnections = 16;
    private FmtAuthScheme authScheme = FmtAuthScheme.X_FMT_TOKEN;

    private final boolean ownsChannel;
    private FmtChannel channel;
    private boolean closed;

    /** set 方式：先 new，再 set，第一次业务调用时自动连接。 */
    public FmtClient() {
        this.ownsChannel = true;
    }

    /** 账号 + 密码；桶不填 = 服务端 current_bucket。 */
    public FmtClient(String username, String password) {
        this(username, password, null);
    }

    /** 单参数 = 临时/固定 token；桶用 {@link #setBucket(String)} 或 {@link #token(String, String)}。 */
    public FmtClient(String token) {
        this();
        setToken(token);
    }

    /** 账号 + 密码 + 桶；默认地址 {@value #DEFAULT_BASE_URL}。 */
    public FmtClient(String username, String password, String bucket) {
        this();
        setUsername(username);
        setPassword(password);
        setBucket(bucket);
    }

    /** token + 桶（桶可为 null）。 */
    public static FmtClient token(String token, String bucket) {
        FmtClient client = new FmtClient();
        client.setToken(token);
        client.setBucket(bucket);
        return client;
    }

    public static FmtClient token(String token) {
        return token(token, null);
    }

    /**
     * 包装一个已有的通道（Spring Boot starter 用它暴露 {@code FmtClient} bean）。
     * 生命周期仍由外面管理，{@link #close()} 不会关闭被包装的通道。
     */
    public static FmtClient wrap(FmtChannel channel) {
        if (channel == null) {
            throw new FmtConfigurationException("FmtChannel 为空");
        }
        FmtClient client = new FmtClient(channel, false);
        return client;
    }

    private FmtClient(FmtChannel channel, boolean ownsChannel) {
        this.channel = channel;
        this.ownsChannel = ownsChannel;
    }

    // ------------------------------------------------------------------
    // 配置（连接前）
    // ------------------------------------------------------------------

    public FmtClient setBaseUrl(String baseUrl) {
        ensureConfigurable();
        this.baseUrl = baseUrl == null || baseUrl.isBlank() ? DEFAULT_BASE_URL : baseUrl.trim();
        return this;
    }

    public FmtClient setUsername(String username) {
        ensureConfigurable();
        this.username = username;
        this.token = null;
        return this;
    }

    public FmtClient setPassword(String password) {
        ensureConfigurable();
        this.password = password;
        this.token = null;
        return this;
    }

    public FmtClient setToken(String token) {
        ensureConfigurable();
        this.token = token;
        this.username = null;
        this.password = null;
        return this;
    }

    /** 桶名可选；不填/空串 = 服务端 current_bucket。 */
    public FmtClient setBucket(String bucket) {
        ensureConfigurable();
        this.bucket = bucket == null || bucket.isBlank() ? null : bucket.trim();
        return this;
    }

    public FmtClient setConnectTimeout(Duration timeout) {
        ensureConfigurable();
        this.connectTimeout = timeout;
        return this;
    }

    public FmtClient setRequestTimeout(Duration timeout) {
        ensureConfigurable();
        this.requestTimeout = timeout;
        return this;
    }

    public FmtClient setStreamTimeout(Duration timeout) {
        ensureConfigurable();
        this.streamTimeout = timeout;
        return this;
    }

    public FmtClient setMaxConnections(int maxConnections) {
        ensureConfigurable();
        this.maxConnections = maxConnections;
        return this;
    }

    public FmtClient setAuthScheme(FmtAuthScheme authScheme) {
        ensureConfigurable();
        this.authScheme = authScheme;
        return this;
    }

    /**
     * 显式连接；不调用也可以——第一次业务方法会自动连接。
     * 连接后不能再改配置，需先 {@link #close()}。
     */
    public FmtClient connect() {
        if (closed) {
            throw new IllegalStateException("FmtClient 已关闭");
        }
        if (channel != null) {
            return this;
        }
        FmtChannelConfig.Builder builder = FmtChannelConfig.builder()
                .baseUrl(baseUrl)
                .connectTimeout(connectTimeout)
                .requestTimeout(requestTimeout)
                .streamTimeout(streamTimeout)
                .maxConnections(maxConnections)
                .authScheme(authScheme);

        if (token != null && !token.isBlank()) {
            builder.token(token);
            if (bucket != null) {
                builder.bucket(bucket);
            }
        } else {
            if (username == null || username.isBlank()) {
                throw new FmtConfigurationException(
                        "没有凭证：用账号密码构造器，或 setUsername/setPassword，或 setToken");
            }
            if (password == null) {
                throw new FmtConfigurationException("密码不能为空");
            }
            builder.login(username, password, bucket);
        }
        this.channel = Fmt.channel(builder.build());
        return this;
    }

    /** 底层通道（新接口/逃生舱用）；会自动 {@link #connect()}。 */
    public FmtChannel channel() {
        connect();
        return channel;
    }

    public String baseUrl() {
        return baseUrl;
    }

    public String bucket() {
        return bucket;
    }

    // ------------------------------------------------------------------
    // 业务：探活/配置
    // ------------------------------------------------------------------

    /** 探活；失败返回 {@code false}。 */
    public boolean ping() {
        return channel().ping().isOk();
    }

    public FmtServerStatus status() {
        return channel().status().orThrow();
    }

    public FmtConfig config() {
        return channel().getConfig().orThrow();
    }

    public FmtBucketListing buckets() {
        return channel().listBuckets().orThrow();
    }

    // ------------------------------------------------------------------
    // 业务：文件
    // ------------------------------------------------------------------

    public FmtPage listFiles() {
        return listFiles(FileQuery.all());
    }

    public FmtPage listFiles(FileQuery query) {
        return channel().listFiles(query).orThrow();
    }

    public FmtFile getFile(String idOrName) {
        return channel().getFile(idOrName).orThrow();
    }

    /**
     * 一次调用完成「上传 / 同内容复用 / 同名自动改名重传」，返回统一文件包。
     *
     * <p>判断顺序：
     * <ol>
     *   <li>成功 → 直接返回；</li>
     *   <li>{@code FMT-304}（同内容已存在，名字可能相同也可能不同）→ 用现有记录返回；</li>
     *   <li>{@code FMT-105}（名字相同但内容不同）→ 自动改成
     *       {@code 原文件名 + 01 + 扩展名}（如 {@code 1234567.jpg -> 123456701.jpg}），
     *       再传；再次冲突就 02、03…，最多 100 次。</li>
     * </ol>
     */
    public FmtFilePackage upload(Path file) throws IOException {
        if (file == null) {
            throw new FmtConfigurationException("上传文件为空");
        }
        return upload(file, file.getFileName().toString());
    }

    /** 文件 + 显式文件名（文件名可以与磁盘名不同）。 */
    public FmtFilePackage upload(Path file, String fileName) throws IOException {
        if (file == null) {
            throw new FmtConfigurationException("上传文件为空");
        }
        String md5 = md5(Files.newInputStream(file));
        return uploadInternal(() -> Files.newInputStream(file), fileName, md5);
    }

    /** 普通 java.io.File。 */
    public FmtFilePackage upload(java.io.File file) throws IOException {
        return upload(file == null ? null : file.toPath());
    }

    public FmtFilePackage upload(java.io.File file, String fileName) throws IOException {
        return upload(file == null ? null : file.toPath(), fileName);
    }

    /** 字节数组上传；文件名必填。 */
    public FmtFilePackage upload(byte[] data, String fileName) {
        if (data == null) {
            throw new FmtConfigurationException("上传数据为空");
        }
        String md5 = md5(data);
        return uploadInternal(() -> new ByteArrayInputStream(data), fileName, md5);
    }

    /** 流上传；文件名必填。流只读一次，SDK 会先落到本地临时文件以便冲突重传。 */
    public FmtFilePackage upload(InputStream in, String fileName) throws IOException {
        if (in == null) {
            throw new FmtConfigurationException("上传流为空");
        }
        Path spool = Files.createTempFile("fmt-upload-", ".bin");
        try {
            // 边落临时文件边算 MD5：原来落盘后再完整读一遍，大文件会多一次 I/O。
            MessageDigest digest = digest();
            Files.copy(new DigestInputStream(in, digest), spool,
                    StandardCopyOption.REPLACE_EXISTING);
            String md5 = hex(digest.digest());
            return uploadInternal(() -> Files.newInputStream(spool), fileName, md5);
        } finally {
            Files.deleteIfExists(spool);
        }
    }

    private FmtFilePackage uploadInternal(InputStreamFactory factory, String requestedName,
                                          String md5) {
        String original = sanitizeName(requestedName);
        String name = original;
        for (int attempt = 0; attempt < 100; attempt++) {
            FmtResult<FmtUpload> result;
            try (InputStream in = factory.open()) {
                result = channel().upload(in, name);
            } catch (IOException failure) {
                throw new UncheckedIOException("读取待上传内容失败", failure);
            }

            if (result.isOk()) {
                FmtFile file = result.value().file();
                return new FmtFilePackage(channel(), file, file.fileName());
            }

            FmtError error = result.error();
            if (error.is(FmtErrorCodes.MD5_DUPLICATE)) {
                FmtFile existing = resolveDuplicate(name, md5, error);
                return new FmtFilePackage(channel(), existing, existing.fileName());
            }
            if (error.is(FmtErrorCodes.FILE_NAME_CONFLICT)) {
                name = nextName(original, attempt + 1);
                continue;
            }
            throw new FmtApiException(error);
        }
        throw new FmtApiException(FmtError.local(
                FmtErrorCodes.FILE_NAME_CONFLICT, "自动改名重试 100 次仍冲突：" + original));
    }

    /** FMT-304 时找已有记录：先按名字，再按 md5 翻页找（同内容可能叫别的名字）。 */
    private FmtFile resolveDuplicate(String name, String md5, FmtError originalError) {
        try {
            FmtFile byName = channel().getFile(name).orThrow();
            if (md5 == null || md5.equalsIgnoreCase(byName.md5())) {
                return byName;
            }
        } catch (FmtException ignored) {
            // 名字可能不是本次这个名字；继续按 md5 找
        }

        int page = 1;
        while (true) {
            FmtPage listing = channel().listFiles(FileQuery.builder()
                    .page(page)
                    .pageSize(200)
                    .build()).orThrow();
            for (FmtFile candidate : listing.files()) {
                if (md5 != null && md5.equalsIgnoreCase(candidate.md5())) {
                    return candidate;
                }
            }
            if (!listing.hasMore()) {
                break;
            }
            page++;
        }
        throw new FmtApiException(originalError);
    }

    public FmtPayload download(String idOrName, Path target) {
        return channel().download(idOrName, target).orThrow();
    }

    public FmtPayload preview(String idOrName, Path target) {
        return channel().preview(idOrName, target).orThrow();
    }

    public FmtDeleteCheck checkDelete(String idOrName) {
        return channel().checkDelete(idOrName).orThrow();
    }

    public FmtFile delete(String idOrName, boolean force) {
        return channel().delete(idOrName, force).orThrow();
    }

    public FmtTrashListing trash() {
        return channel().listTrash().orThrow();
    }

    // ------------------------------------------------------------------
    // 业务：分享
    // ------------------------------------------------------------------

    public FmtShare createShare(String fileId) {
        return channel().createShare(fileId).orThrow();
    }

    public FmtShare getShare(String shareId) {
        return channel().getShare(shareId).orThrow();
    }

    public List<FmtShare> shares(String fileId) {
        return channel().listShares(fileId).orThrow();
    }

    public FmtShare revokeShare(String shareId) {
        return channel().revokeShare(shareId).orThrow();
    }

    // ------------------------------------------------------------------
    // 生命周期
    // ------------------------------------------------------------------

    @Override
    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        if (ownsChannel && channel != null) {
            channel.close();
        }
        channel = null;
    }

    private void ensureConfigurable() {
        if (closed) {
            throw new IllegalStateException("FmtClient 已关闭");
        }
        if (channel != null) {
            throw new IllegalStateException("已连接，不能再改配置；先 close() 再新建");
        }
    }

    @FunctionalInterface
    private interface InputStreamFactory {
        InputStream open() throws IOException;
    }

    private static String sanitizeName(String name) {
        String text = name == null ? "" : name.trim().replace('\\', '/');
        int slash = text.lastIndexOf('/');
        if (slash >= 0) {
            text = text.substring(slash + 1);
        }
        return text.isBlank() ? "upload.bin" : text;
    }

    /** 1234567.jpg + 1 → 123456701.jpg；保持扩展名。 */
    private static String nextName(String original, int sequence) {
        int dot = original.lastIndexOf('.');
        String base = dot > 0 ? original.substring(0, dot) : original;
        String extension = dot > 0 ? original.substring(dot) : "";
        return base + String.format("%02d", sequence) + extension;
    }

    private static String md5(byte[] data) {
        return hex(digest().digest(data));
    }

    private static String md5(InputStream in) throws IOException {
        try (InputStream source = in) {
            MessageDigest digest = digest();
            byte[] buffer = new byte[8192];
            int read;
            while ((read = source.read(buffer)) >= 0) {
                if (read > 0) {
                    digest.update(buffer, 0, read);
                }
            }
            return hex(digest.digest());
        }
    }

    private static MessageDigest digest() {
        try {
            return MessageDigest.getInstance("MD5");
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException("JDK 缺少 MD5", impossible);
        }
    }

    private static String hex(byte[] bytes) {
        return HexFormat.of().formatHex(bytes);
    }
}
