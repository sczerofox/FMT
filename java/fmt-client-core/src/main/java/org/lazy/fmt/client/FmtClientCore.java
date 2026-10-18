package org.lazy.fmt.client;

import java.nio.file.Path;
import java.time.Duration;

/**
 * 客户端参数载体：<b>可写的配置单</b>，建通道前在这里调参数，之后交给
 * {@link Fmt#channel(FmtClientCore)} 换成不可变的 {@link FmtChannelConfig}。
 *
 * <pre>{@code
 * // 最省事的调用流程：调参数 → 建通道 → 调方法
 * FmtClientCore core = new FmtClientCore();   // 地址默认 http://localhost:4122
 * core.setBaseUrl("http://localhost:4122");   // 想改就改，不改也行
 * core.setToken("1a2867378b112b8e7864218fd2fd693f");
 *
 * try (FmtChannel fmt = Fmt.channel(core)) {
 *     if (fmt.ping()) {                        // ping 公开，不需要 token
 *         System.out.println(fmt.status().data().version());
 *     }
 *     fmt.listFiles(FileQuery.all()).orThrow();
 * }
 * }</pre>
 *
 * <p>更短的写法（同样效果）：
 * <pre>{@code
 * FmtChannel fmt = Fmt.channel(new FmtClientCore("http://localhost:4122", token));
 * // 或者完全不动配置，用环境变量 / 数据根：
 * FmtChannel fmt = Fmt.channel(new FmtClientCore().setDataRoot(Path.of("D:/Tools/fmt")));
 * }</pre>
 *
 * <h2>两条边界（很重要）</h2>
 * <ol>
 *   <li><b>token 绑在通道上，不散落在调用里</b>：本类只把凭证来源交给
 *       {@link Fmt#channel(FmtClientCore)}，之后所有请求由通道在唯一一处
 *       {@code applyToken()} 注入 header。业务方法里没有 token 参数（见设计文档 §1、§4）。</li>
 *   <li><b>建完通道别再改本对象指望生效</b>：{@code Fmt.channel(core)} 内部会把当时的参数
 *       拷成不可变配置；改 {@code core} 只影响<b>之后再建</b>的通道。
 *       要换 token 又不想重启进程，请用 {@link #setDataRoot(Path)}（每次读文件）或
 *       自己实现 {@link TokenProvider}（可轮换），而不是改这个对象。</li>
 * </ol>
 *
 * <h2>参数与默认值</h2>
 * <table border="1">
 *   <tr><th>参数</th><th>默认值</th><th>说明</th></tr>
 *   <tr><td>{@link #setBaseUrl(String)}</td><td>{@code http://localhost:4122}</td>
 *       <td><b>写主机名 localhost</b>：SDK 会在 IPv4/IPv6 之间回落（§4.4）</td></tr>
 *   <tr><td>{@link #setToken(String)}</td><td>无</td><td>固定 token；也可用数据根 / 环境变量</td></tr>
 *   <tr><td>{@link #setDataRoot(Path)}</td><td>无</td><td>从 {@code <数据根>/data/user.json} 读 token（推荐）</td></tr>
 *   <tr><td>{@link #setAuthScheme(FmtAuthScheme)}</td><td>{@code X_FMT_TOKEN}</td>
 *       <td>{@code X-FMT-Token} 头；过网关可用 {@code BEARER}</td></tr>
 *   <tr><td>{@link #setConnectTimeout(Duration)}</td><td>3s</td><td></td></tr>
 *   <tr><td>{@link #setRequestTimeout(Duration)}</td><td>30s</td><td>JSON 调用</td></tr>
 *   <tr><td>{@link #setStreamTimeout(Duration)}</td><td>30m</td><td>上传/下载；{@code ZERO} = 不限制</td></tr>
 *   <tr><td>{@link #setMaxConnections(int)}</td><td>16</td><td>并发闸门，超出的排队</td></tr>
 *   <tr><td>{@link #setRetryOnUnauthorized(boolean)}</td><td>{@code true}</td>
 *       <td>401 → 作废缓存 → 重取 → 只重试一次（§4.2）</td></tr>
 *   <tr><td>{@link #setIdempotentRetries(int)}</td><td>0</td><td>只作用于 GET 的传输层重试</td></tr>
 *   <tr><td>{@link #setLogger(FmtLogger)}</td><td>不打日志</td>
 *       <td>{@link FmtLoggers#stderr()} 可以看到「换地址重试」这类日志</td></tr>
 *   <tr><td>{@link #setCodec(FmtCodec)}</td><td>内置 {@code SimpleJson}</td><td>Spring 环境下可换成 Jackson</td></tr>
 * </table>
 */
public final class FmtClientCore {

    /** 默认地址：FMT 只监听本机，且默认关闭（要在 {@code config/server.json} 里打开）。 */
    public static final String DEFAULT_BASE_URL = FmtChannelConfig.DEFAULT_BASE_URL;

    /** 默认取 token 的环境变量 / 系统属性名：{@code FMT_TOKEN}（{@code -DFMT_TOKEN=…} 也认）。 */
    public static final String DEFAULT_TOKEN_VARIABLE = "FMT_TOKEN";

    /** 取数据根的环境变量 / 系统属性名：{@code FMT_ROOT}。 */
    public static final String DEFAULT_ROOT_VARIABLE = "FMT_ROOT";

    private String baseUrl = DEFAULT_BASE_URL;
    private String token;
    private Path dataRoot;
    private TokenProvider tokenProvider;
    private String username;
    private String password;
    private String bucket;
    private FmtAuthScheme authScheme = FmtAuthScheme.X_FMT_TOKEN;
    private Duration connectTimeout = Duration.ofSeconds(3);
    private Duration requestTimeout = Duration.ofSeconds(30);
    private Duration streamTimeout = Duration.ofMinutes(30);
    private int maxConnections = 16;
    private boolean retryOnUnauthorized = true;
    private int idempotentRetries;
    private FmtLogger logger = FmtLoggers.NOOP;
    private FmtCodec codec;

    /** 默认参数：地址 {@code http://localhost:4122}，凭证等 {@link #setToken} / {@link #setDataRoot}。 */
    public FmtClientCore() {
    }

    /** 一步到位的常用写法：地址 + 固定 token。 */
    public FmtClientCore(String baseUrl, String token) {
        setBaseUrl(baseUrl);
        setToken(token);
    }

    // ------------------------------------------------------------------
    // 链式 setter（返回 this，也可以当普通 setter 用）
    // ------------------------------------------------------------------

    /** 通道地址，例如 {@code http://localhost:4122}；空值会被忽略（保留默认）。 */
    public FmtClientCore setBaseUrl(String baseUrl) {
        if (baseUrl != null && !baseUrl.isBlank()) {
            this.baseUrl = baseUrl.trim();
        }
        return this;
    }

    /** 固定 token（明文）。改一次要新建通道才生效。 */
    public FmtClientCore setToken(String token) {
        this.token = token == null ? null : token.trim();
        return this;
    }

    /**
     * 账号密码登录模式（像 MySQL 一样给账号、密码、库名）。
     *
     * <p>与 {@link #setToken(String)} 二选一；通道第一次需要认证时会自动
     * 取 salt → 算摘要 → 换临时会话 token，过期后自动重登。
     */
    public FmtClientCore setLogin(String username, String password, String bucket) {
        this.username = username == null || username.isBlank() ? null : username.trim();
        this.password = password;
        this.bucket = bucket == null || bucket.isBlank() ? null : bucket.trim();
        return this;
    }

    /** 账号（配合 {@link #setPassword} / {@link #setBucket}）。 */
    public FmtClientCore setUsername(String username) {
        this.username = username == null || username.isBlank() ? null : username.trim();
        return this;
    }

    public FmtClientCore setPassword(String password) {
        this.password = password;
        return this;
    }

    /** 库名（Bucket）；登录模式必填，token 模式可选（不填沿用服务端 current_bucket）。 */
    public FmtClientCore setBucket(String bucket) {
        this.bucket = bucket == null || bucket.isBlank() ? null : bucket.trim();
        return this;
    }

    /**
     * 从数据根的 {@code data/user.json} 读 token —— <b>推荐</b>：服务端换 token、重装服务都能自愈。
     *
     * <p>每次取凭证时读文件（token 只有几十字节），所以不需要重启 Java 应用。
     */
    public FmtClientCore setDataRoot(Path dataRoot) {
        this.dataRoot = dataRoot;
        return this;
    }

    public FmtClientCore setDataRoot(String dataRoot) {
        return setDataRoot(dataRoot == null || dataRoot.isBlank() ? null : Path.of(dataRoot.trim()));
    }

    /** 完全自定义的凭证来源（可轮换、可从配置中心取），优先级最高。 */
    public FmtClientCore setTokenProvider(TokenProvider tokenProvider) {
        this.tokenProvider = tokenProvider;
        return this;
    }

    /** 认证头写法：{@code X_FMT_TOKEN}（默认）/ {@code BEARER} / {@code BOTH}。 */
    public FmtClientCore setAuthScheme(FmtAuthScheme authScheme) {
        this.authScheme = authScheme == null ? FmtAuthScheme.X_FMT_TOKEN : authScheme;
        return this;
    }

    public FmtClientCore setConnectTimeout(Duration connectTimeout) {
        this.connectTimeout = connectTimeout;
        return this;
    }

    public FmtClientCore setRequestTimeout(Duration requestTimeout) {
        this.requestTimeout = requestTimeout;
        return this;
    }

    /** 上传/下载的整体超时；{@code Duration.ZERO} 表示不限制。 */
    public FmtClientCore setStreamTimeout(Duration streamTimeout) {
        this.streamTimeout = streamTimeout;
        return this;
    }

    public FmtClientCore setMaxConnections(int maxConnections) {
        this.maxConnections = maxConnections;
        return this;
    }

    /** 401 后是否自动作废凭证、重取并重试一次（默认开）。 */
    public FmtClientCore setRetryOnUnauthorized(boolean retryOnUnauthorized) {
        this.retryOnUnauthorized = retryOnUnauthorized;
        return this;
    }

    /** GET 在传输层失败时的重试次数，默认 0。 */
    public FmtClientCore setIdempotentRetries(int idempotentRetries) {
        this.idempotentRetries = idempotentRetries;
        return this;
    }

    /** 日志出口：默认不打日志；{@link FmtLoggers#stderr()} 便于排查连接问题。 */
    public FmtClientCore setLogger(FmtLogger logger) {
        this.logger = logger;
        return this;
    }

    /** JSON 出口：默认内置 {@code SimpleJson}，核心包零依赖。 */
    public FmtClientCore setCodec(FmtCodec codec) {
        this.codec = codec;
        return this;
    }

    // ------------------------------------------------------------------
    // 读取参数 / 建通道
    // ------------------------------------------------------------------

    public String baseUrl() {
        return baseUrl;
    }

    public FmtAuthScheme authScheme() {
        return authScheme;
    }

    public Duration connectTimeout() {
        return connectTimeout;
    }

    public Duration requestTimeout() {
        return requestTimeout;
    }

    public Duration streamTimeout() {
        return streamTimeout;
    }

    public int maxConnections() {
        return maxConnections;
    }

    public boolean retryOnUnauthorized() {
        return retryOnUnauthorized;
    }

    public int idempotentRetries() {
        return idempotentRetries;
    }

    public FmtLogger logger() {
        return logger;
    }

    public FmtCodec codec() {
        return codec == null ? null : codec;
    }

    /**
     * 把当前参数变成不可变的 {@link FmtChannelConfig}（{@link Fmt#channel(FmtClientCore)} 内部用它）。
     *
     * <p>这里<b>不解析 token</b>，只把「凭证从哪来」交出去：FMT 可能比 Java 应用后启动、
     * token 可能还没生成——取不到凭证的错误应该发生在第一次需要认证的调用上，
     * 而 {@code ping} 与健康检查依然能报告「服务活着，只是凭证不对」（设计文档 §4.1）。
     */
    public FmtChannelConfig toConfig() {
        FmtChannelConfig.Builder builder = FmtChannelConfig.builder()
                .baseUrl(baseUrl)
                .authScheme(authScheme)
                .connectTimeout(connectTimeout)
                .requestTimeout(requestTimeout)
                .streamTimeout(streamTimeout)
                .maxConnections(maxConnections)
                .retryOnUnauthorized(retryOnUnauthorized)
                .idempotentRetries(idempotentRetries)
                .logger(logger == null ? FmtLoggers.NOOP : logger);
        if (username != null || password != null) {
            builder.login(username, password, bucket);
        } else {
            builder.tokenProvider(resolveTokenProvider());
            if (bucket != null) {
                builder.bucket(bucket);
            }
        }
        if (codec != null) {
            builder.codec(codec);
        }
        return builder.build();
    }

    /** 一步建通道：{@code FmtChannel fmt = new FmtClientCore().setToken(t).channel();} */
    public FmtChannel channel() {
        return Fmt.channel(this);
    }

    /**
     * 凭证来源，按优先级拼：自定义 provider → 固定 token → 数据根 → 环境变量 → 明确报错。
     *
     * <p>只有「这个来源没配」才会跳到下一个；文件坏了、JSON 坏了会如实抛出。
     */
    private TokenProvider resolveTokenProvider() {
        return TokenProviders.firstOf(
                tokenProvider,
                token == null || token.isEmpty() ? null : TokenProviders.of(token),
                dataRoot == null ? null : TokenProviders.fromDataRoot(dataRoot),
                TokenProviders.fromEnvironment(DEFAULT_TOKEN_VARIABLE),
                fromRootVariable(),
                TokenProviders.failWith("没有可用的 FMT token：请用 setToken(\"…\") 直接给，"
                        + "或 setDataRoot(Path.of(\"<数据根>\")) 从 data/user.json 读，"
                        + "或设环境变量 " + DEFAULT_TOKEN_VARIABLE + " / " + DEFAULT_ROOT_VARIABLE
                        + "（token 也可以用 `fmt.exe config list` 查看）"));
    }

    /** {@code FMT_ROOT=<数据根>} → {@code <数据根>/data/user.json}。 */
    private static TokenProvider fromRootVariable() {
        return () -> {
            String root = System.getProperty(DEFAULT_ROOT_VARIABLE);
            if (root == null || root.isBlank()) {
                root = System.getenv(DEFAULT_ROOT_VARIABLE);
            }
            if (root == null || root.isBlank()) {
                throw new FmtConfigurationException("没有 " + DEFAULT_ROOT_VARIABLE);
            }
            return TokenProviders.fromDataRoot(Path.of(root.trim())).token();
        };
    }

    @Override
    public String toString() {
        return "FmtClientCore[baseUrl=" + baseUrl + ", auth=" + authScheme.description()
                + ", 凭证=" + (username != null ? "账号密码登录（user=" + username + ", bucket=" + bucket + "）"
                        : tokenProvider != null ? "自定义 TokenProvider"
                        : token != null && !token.isEmpty() ? "setToken(…)"
                        : dataRoot != null ? "data/user.json @ " + dataRoot
                        : "环境变量 " + DEFAULT_TOKEN_VARIABLE + " / " + DEFAULT_ROOT_VARIABLE)
                + "]";
    }
}
