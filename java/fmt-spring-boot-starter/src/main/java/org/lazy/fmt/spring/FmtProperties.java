package org.lazy.fmt.spring;

import org.lazy.fmt.client.FmtAuthScheme;
import org.lazy.fmt.client.FmtChannelConfig;
import org.lazy.fmt.client.FmtConfigurationException;
import org.lazy.fmt.client.TokenProvider;
import org.lazy.fmt.client.TokenProviders;

import org.springframework.boot.context.properties.ConfigurationProperties;

import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * {@code fmt.*} 配置。
 *
 * <pre>{@code
 * fmt:
 *   base-url: http://127.0.0.1:4122
 *   data-root: D:/Tools/fmt        # token 从 <data-root>/data/user.json 读，服务端换 token 能自愈
 * }</pre>
 */
@ConfigurationProperties(prefix = "fmt")
public class FmtProperties {

    /** 关掉就完全不注册任何 FMT bean（适合只有部分环境需要连 FMT 的场景）。 */
    private boolean enabled = true;

    /**
     * 是否在 Servlet Web 应用里自动注册 FMT 异常的 JSON 翻译（{@code FmtExceptionAdvice}）。
     * 默认开；宿主自己声明同类型 bean 时自动让位。
     */
    private boolean exceptionAdvice = true;

    /** FMT HTTP 地址；只监听本机，默认 4122。 */
    private String baseUrl = FmtChannelConfig.DEFAULT_BASE_URL;

    /** 直接给 token（32 位十六进制）。优先级最高。 */
    private String token;

    /**
     * 账号密码登录（像 MySQL 一样）。配了 username + password 就**不再走 token/data-root**，
     * 第一次业务调用时自动换临时会话 token；会话过期自动重登。
     *
     * <p>明文密码只在本进程内存里，网络上只有 PBKDF2 摘要。
     */
    private String username;

    private String password;

    /** 库名（Bucket）。登录模式必填；token 模式可选（不填沿用服务端 current_bucket）。 */
    private String bucket;

    /** 指定 {@code data/user.json} 文件。 */
    private Path tokenFile;

    /** FMT 数据根（exe 所在目录）；token 取 {@code <data-root>/data/user.json}。**推荐**。 */
    private Path dataRoot;

    /** 从文件读 token 的缓存时长；服务端换 token 时最多旧这么久，401 会立刻触发刷新。 */
    private Duration tokenCacheTtl = Duration.ofSeconds(30);

    private FmtAuthScheme authScheme = FmtAuthScheme.X_FMT_TOKEN;

    private Duration connectTimeout = Duration.ofSeconds(3);

    private Duration requestTimeout = Duration.ofSeconds(30);

    private Duration streamTimeout = Duration.ofMinutes(30);

    /** 通道允许的并发请求数（同时也是连接池上限）。 */
    private int maxConnections = 16;

    /** 收到 401（FMT-018）时作废凭证、重取并重试一次。 */
    private boolean retryOnUnauthorized = true;

    /** 幂等调用（GET）在传输失败时的重试次数，默认 0。 */
    private int idempotentRetries = 0;

    /** 附加请求头（例如过网关时需要的 X-Request-From）。 */
    private Map<String, String> headers = new LinkedHashMap<>();

    /**
     * 按优先级解析凭证来源。
     *
     * <p>顺序：{@code fmt.token} → {@code fmt.token-file} → {@code fmt.data-root} →
     * 环境变量/系统属性 {@code FMT_TOKEN} → 环境变量 {@code FMT_ROOT} 下的 user.json →
     * 给出「你该怎么配」的报错。
     *
     * <p>刻意**不在启动时**读取：FMT 可能比 Java 应用后启动，凭证也可能还没生成；
     * 真正取不到时抛 {@link FmtConfigurationException}，并且 {@code /api/ping} 与健康检查
     * 依然能报告「服务活着，只是凭证不对」。
     */
    public TokenProvider resolveTokenProvider() {
        List<TokenProvider> sources = new ArrayList<>();
        if (token != null && !token.isBlank()) {
            sources.add(TokenProviders.of(token));
        }
        if (tokenFile != null) {
            sources.add(TokenProviders.fromUserJson(tokenFile, tokenCacheTtl));
        }
        if (dataRoot != null) {
            sources.add(TokenProviders.fromUserJson(dataRoot.resolve("data").resolve("user.json"),
                    tokenCacheTtl));
        }
        sources.add(TokenProviders.fromEnvironment("FMT_TOKEN", "fmt.token"));
        sources.add(() -> {
            String root = System.getenv("FMT_ROOT");
            if (root == null || root.isBlank()) {
                throw new FmtConfigurationException("没有设置环境变量 FMT_ROOT");
            }
            return TokenProviders
                    .fromUserJson(Path.of(root, "data", "user.json"), tokenCacheTtl)
                    .token();
        });
        sources.add(TokenProviders.failWith("""
                没有可用的 FMT 访问 token。任选一种配法：
                  ① fmt.token=<32 位十六进制，占位符别照抄；每个数据根一个>
                  ② fmt.token-file=D:/Tools/fmt/data/user.json
                  ③ fmt.data-root=D:/Tools/fmt          （推荐：token 轮换能自愈）
                  ④ 环境变量 FMT_TOKEN，或 FMT_ROOT 指向数据根
                token 从哪来：在数据根下跑 `fmt.exe config list --show-token`，
                或直接读 <数据根>/data/user.json 的 token 字段（抄错 token 会拿到 FMT-018）。"""));

        return TokenProviders.firstOf(sources.toArray(new TokenProvider[0]));
    }

    public boolean isEnabled() {
        return enabled;
    }

    public void setEnabled(boolean enabled) {
        this.enabled = enabled;
    }

    public boolean isExceptionAdvice() {
        return exceptionAdvice;
    }

    public void setExceptionAdvice(boolean exceptionAdvice) {
        this.exceptionAdvice = exceptionAdvice;
    }

    public String getBaseUrl() {
        return baseUrl;
    }

    public void setBaseUrl(String baseUrl) {
        this.baseUrl = baseUrl;
    }

    public String getToken() {
        return token;
    }

    public void setToken(String token) {
        this.token = token;
    }

    /** 是否配置了账号密码登录。 */
    public boolean hasLogin() {
        return username != null && !username.isBlank() && password != null;
    }

    public String getUsername() {
        return username;
    }

    public void setUsername(String username) {
        this.username = username;
    }

    public String getPassword() {
        return password;
    }

    public void setPassword(String password) {
        this.password = password;
    }

    public String getBucket() {
        return bucket;
    }

    public void setBucket(String bucket) {
        this.bucket = bucket;
    }

    public Path getTokenFile() {
        return tokenFile;
    }

    public void setTokenFile(Path tokenFile) {
        this.tokenFile = tokenFile;
    }

    public Path getDataRoot() {
        return dataRoot;
    }

    public void setDataRoot(Path dataRoot) {
        this.dataRoot = dataRoot;
    }

    public Duration getTokenCacheTtl() {
        return tokenCacheTtl;
    }

    public void setTokenCacheTtl(Duration tokenCacheTtl) {
        this.tokenCacheTtl = tokenCacheTtl;
    }

    public FmtAuthScheme getAuthScheme() {
        return authScheme;
    }

    public void setAuthScheme(FmtAuthScheme authScheme) {
        this.authScheme = authScheme;
    }

    public Duration getConnectTimeout() {
        return connectTimeout;
    }

    public void setConnectTimeout(Duration connectTimeout) {
        this.connectTimeout = connectTimeout;
    }

    public Duration getRequestTimeout() {
        return requestTimeout;
    }

    public void setRequestTimeout(Duration requestTimeout) {
        this.requestTimeout = requestTimeout;
    }

    public Duration getStreamTimeout() {
        return streamTimeout;
    }

    public void setStreamTimeout(Duration streamTimeout) {
        this.streamTimeout = streamTimeout;
    }

    public int getMaxConnections() {
        return maxConnections;
    }

    public void setMaxConnections(int maxConnections) {
        this.maxConnections = maxConnections;
    }

    public boolean isRetryOnUnauthorized() {
        return retryOnUnauthorized;
    }

    public void setRetryOnUnauthorized(boolean retryOnUnauthorized) {
        this.retryOnUnauthorized = retryOnUnauthorized;
    }

    public int getIdempotentRetries() {
        return idempotentRetries;
    }

    public void setIdempotentRetries(int idempotentRetries) {
        this.idempotentRetries = idempotentRetries;
    }

    public Map<String, String> getHeaders() {
        return headers;
    }

    public void setHeaders(Map<String, String> headers) {
        this.headers = headers == null ? new LinkedHashMap<>() : headers;
    }
}
