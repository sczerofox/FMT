package org.lazy.fmt.client;

import org.lazy.fmt.client.internal.SessionTokenProvider;
import org.lazy.fmt.client.json.SimpleJson;

import java.net.URI;
import java.net.URISyntaxException;
import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * 通道配置：**连接信息 + 凭证 + 超时 + 编解码/日志出口**。
 *
 * <pre>{@code
 * FmtChannelConfig config = FmtChannelConfig.builder()
 *         .baseUrl("http://127.0.0.1:4122")
 *         .tokenProvider(TokenProviders.fromDataRoot(Path.of("D:/Tools/fmt")))
 *         .requestTimeout(Duration.ofSeconds(15))
 *         .build();
 * }</pre>
 *
 * <p>配置是不可变的：建好通道后想换 token 或超时，就换一个通道。
 */
public final class FmtChannelConfig {

    /**
     * FMT 默认监听地址（只监听本机，默认关闭，要在 config/server.json 里打开）。
     *
     * <p>写主机名 {@code localhost} 而不是某个字面 IP，是因为 FMT 自己的默认配置就是
     * {@code "host": "localhost"}——而 Windows 上 {@code localhost} 同时有
     * {@code 127.0.0.1} 与 {@code ::1} 两个地址，**httplib 只绑其中一个**
     * （真机实测：`Get-NetTCPConnection -LocalPort 4122` 显示只绑了 `::1`）。
     *
     * <p>而 JDK 的 {@code HttpClient} 对多地址主机名**只试第一个解析结果、不回落**
     * （真机实测：`localhost` → 先 127.0.0.1 → 连接被拒绝；`127.0.0.1` 同样被拒；
     * 只有 `[::1]` 通）。所以通道自己做了回落：把主机名的**所有**解析结果都当候选地址，
     * 连接被拒时换下一个重试，并记住最后成功的那个。见
     * {@code DefaultFmtChannel} 的候选地址逻辑。
     */
    public static final String DEFAULT_BASE_URL = "http://localhost:4122";

    private final URI baseUri;
    private final TokenProvider tokenProvider;
    private final FmtAuthScheme authScheme;
    private final Duration connectTimeout;
    private final Duration requestTimeout;
    private final Duration streamTimeout;
    private final int maxConnections;
    private final boolean retryOnUnauthorized;
    private final int idempotentRetries;
    private final FmtCodec codec;
    private final FmtLogger logger;
    private final Map<String, String> extraHeaders;
    private final String userAgent;
    private final String bucket;

    private FmtChannelConfig(Builder builder) {
        this.baseUri = builder.baseUri;
        this.tokenProvider = builder.tokenProvider;
        this.authScheme = builder.authScheme;
        this.connectTimeout = builder.connectTimeout;
        this.requestTimeout = builder.requestTimeout;
        this.streamTimeout = builder.streamTimeout;
        this.maxConnections = builder.maxConnections;
        this.retryOnUnauthorized = builder.retryOnUnauthorized;
        this.idempotentRetries = builder.idempotentRetries;
        this.codec = builder.codec;
        this.logger = builder.logger;
        this.extraHeaders = Map.copyOf(builder.extraHeaders);
        this.userAgent = builder.userAgent;
        this.bucket = builder.bucket;
    }

    public static Builder builder() {
        return new Builder();
    }

    /** 最常见的一行式：地址 + 明文 token。 */
    public static Builder builder(String baseUrl, String token) {
        return new Builder().baseUrl(baseUrl).token(token);
    }

    public URI baseUri() {
        return baseUri;
    }

    public TokenProvider tokenProvider() {
        return tokenProvider;
    }

    public FmtAuthScheme authScheme() {
        return authScheme;
    }

    public Duration connectTimeout() {
        return connectTimeout;
    }

    /** 普通 JSON 调用的整体超时；{@link Duration#ZERO} 表示不设。 */
    public Duration requestTimeout() {
        return requestTimeout;
    }

    /** 上传/下载这类流式调用的整体超时；{@link Duration#ZERO} 表示不设。 */
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

    public FmtCodec codec() {
        return codec;
    }

    public FmtLogger logger() {
        return logger;
    }

    public Map<String, String> extraHeaders() {
        return extraHeaders;
    }

    public String userAgent() {
        return userAgent;
    }

    /**
     * 业务请求要访问的 Bucket（库名）；空表示沿用服务端 {@code current_bucket}（旧行为）。
     *
     * <p>登录模式下它同时是登录时选的库，且每个请求都会带 {@code X-FMT-Bucket}。
     */
    public String bucket() {
        return bucket;
    }

    public static final class Builder {

        private URI baseUri = URI.create(DEFAULT_BASE_URL);
        private TokenProvider tokenProvider;
        private FmtAuthScheme authScheme = FmtAuthScheme.X_FMT_TOKEN;
        private Duration connectTimeout = Duration.ofSeconds(3);
        private Duration requestTimeout = Duration.ofSeconds(30);
        private Duration streamTimeout = Duration.ofMinutes(30);
        private int maxConnections = 16;
        private boolean retryOnUnauthorized = true;
        private int idempotentRetries = 0;
        private FmtCodec codec = SimpleJson.INSTANCE;
        private FmtLogger logger = FmtLoggers.NOOP;
        private final Map<String, String> extraHeaders = new LinkedHashMap<>();
        private String userAgent = "fmt-client-java/1.0";
        private String bucket;
        private String username;
        private String password;

        private Builder() {
        }

        public Builder baseUrl(String url) {
            if (url == null || url.isBlank()) {
                throw new FmtConfigurationException("baseUrl 为空");
            }
            String text = url.trim();
            while (text.endsWith("/")) { // 允许写 http://host:4122/，内部统一不带尾斜杠
                text = text.substring(0, text.length() - 1);
            }
            try {
                return baseUri(new URI(text));
            } catch (URISyntaxException broken) {
                throw new FmtConfigurationException("baseUrl 不是合法 URL：" + url, broken);
            }
        }

        public Builder baseUri(URI uri) {
            if (uri == null) {
                throw new FmtConfigurationException("baseUri 为空");
            }
            String scheme = uri.getScheme();
            if (scheme == null
                    || !(scheme.equalsIgnoreCase("http") || scheme.equalsIgnoreCase("https"))) {
                throw new FmtConfigurationException("baseUri 只支持 http/https：" + uri);
            }
            if (uri.getHost() == null || uri.getHost().isBlank()) {
                throw new FmtConfigurationException("baseUri 缺少主机名：" + uri);
            }
            this.baseUri = uri;
            return this;
        }

        /** 明文 token（会被包成固定来源的 {@link TokenProvider}）。 */
        public Builder token(String token) {
            this.tokenProvider = TokenProviders.of(token);
            return this;
        }

        public Builder tokenProvider(TokenProvider provider) {
            this.tokenProvider = provider;
            return this;
        }

        public Builder authScheme(FmtAuthScheme scheme) {
            this.authScheme = scheme == null ? FmtAuthScheme.X_FMT_TOKEN : scheme;
            return this;
        }

        public Builder connectTimeout(Duration timeout) {
            this.connectTimeout = timeout == null ? Duration.ZERO : timeout;
            return this;
        }

        public Builder requestTimeout(Duration timeout) {
            this.requestTimeout = timeout == null ? Duration.ZERO : timeout;
            return this;
        }

        public Builder streamTimeout(Duration timeout) {
            this.streamTimeout = timeout == null ? Duration.ZERO : timeout;
            return this;
        }

        public Builder maxConnections(int maxConnections) {
            this.maxConnections = maxConnections;
            return this;
        }

        public Builder retryOnUnauthorized(boolean retry) {
            this.retryOnUnauthorized = retry;
            return this;
        }

        /** 对**幂等**调用（GET）在连接层异常时的重试次数，默认 0（不重试）。 */
        public Builder idempotentRetries(int retries) {
            this.idempotentRetries = retries;
            return this;
        }

        public Builder codec(FmtCodec codec) {
            this.codec = codec == null ? SimpleJson.INSTANCE : codec;
            return this;
        }

        public Builder logger(FmtLogger logger) {
            this.logger = logger == null ? FmtLoggers.NOOP : logger;
            return this;
        }

        public Builder header(String name, String value) {
            if (name != null && value != null) {
                this.extraHeaders.put(name, value);
            }
            return this;
        }

        public Builder headers(Map<String, String> headers) {
            if (headers != null) {
                headers.forEach(this::header);
            }
            return this;
        }

        public Builder userAgent(String userAgent) {
            this.userAgent = userAgent;
            return this;
        }

        /**
         * 业务请求要访问的 Bucket（库名）。旧 token 模式下可不给（沿用服务端 current_bucket）；
         * 登录模式下必须给，并且每个请求都会带 {@code X-FMT-Bucket}。
         */
        public Builder bucket(String bucket) {
            this.bucket = bucket == null || bucket.isBlank() ? null : bucket.trim();
            return this;
        }

        /**
         * 账号密码登录（像 MySQL 一样给账号、密码、库名）。
         *
         * <p>通道会在第一次需要认证的调用时：
         * 取 salt → 本地 PBKDF2 摘要 → {@code POST /api/auth/login} → 拿临时会话 token。
         * 会话过期时通道会自动用同一份账号密码重登；明文密码不会出现在网络上。
         */
        public Builder login(String username, String password, String bucket) {
            if (username == null || username.isBlank()) {
                throw new FmtConfigurationException("登录用户名不能为空");
            }
            if (password == null) {
                throw new FmtConfigurationException("登录密码不能为空");
            }
            if (bucket == null || bucket.isBlank()) {
                throw new FmtConfigurationException("登录必须给库名（Bucket）");
            }
            this.username = username.trim();
            this.password = password;
            this.bucket = bucket.trim();
            return this;
        }

        public FmtChannelConfig build() {
            if (username != null || password != null) {
                if (tokenProvider != null) {
                    throw new FmtConfigurationException(
                            "不能同时配置 token 与账号密码：二选一（登录模式用 login(...)）");
                }
                tokenProvider = new SessionTokenProvider(
                        baseUri, username, password, bucket, codec,
                        logger == null ? FmtLoggers.NOOP : logger,
                        connectTimeout, requestTimeout);
            }
            if (tokenProvider == null) {
                throw new FmtConfigurationException(
                        "没有配置凭证：用 .login(user, password, bucket)、.token(...) 或 "
                                + ".tokenProvider(...) 给通道一个凭证");
            }
            if (maxConnections <= 0) {
                throw new FmtConfigurationException("maxConnections 必须大于 0");
            }
            if (idempotentRetries < 0) {
                throw new FmtConfigurationException("idempotentRetries 不能是负数");
            }
            return new FmtChannelConfig(this);
        }
    }
}
