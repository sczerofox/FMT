package org.lazy.fmt.spring;

import org.lazy.fmt.client.FmtAuthScheme;
import org.lazy.fmt.client.FmtChannelConfig;

import org.springframework.boot.context.properties.ConfigurationProperties;

import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * {@code fmt.*} 配置。
 *
 * <p>只支持两种凭证：{@code fmt.token}，或 {@code fmt.username + fmt.password}。
 * 两者都没配时应用**启动失败**（不会静默尝试别处）。
 *
 * <pre>{@code
 * fmt:
 *   base-url: http://localhost:4122
 *   token: <32 位十六进制>          # 方式 A
 *   # 或
 *   username: lazy                 # 方式 B
 *   password: 123456
 *   bucket: pet                    # 可选；不填用服务端 current_bucket
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

    /** 直接给 token（32 位十六进制）。与账号密码二选一。 */
    private String token;

    /**
     * 账号密码登录（像 MySQL 一样）。配了 username + password 就**不再走 token**，
     * 第一次业务调用时自动换临时会话 token；会话过期自动重登。
     *
     * <p>明文密码只在本进程内存里，网络上只有 PBKDF2 摘要。
     */
    private String username;

    private String password;

    /** 库名（Bucket）。可选；不填用服务端 current_bucket。 */
    private String bucket;

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

    /** 是否配置了固定 token。 */
    public boolean hasToken() {
        return token != null && !token.isBlank();
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
        return username != null && !username.isBlank()
                && password != null && !password.isBlank();
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
