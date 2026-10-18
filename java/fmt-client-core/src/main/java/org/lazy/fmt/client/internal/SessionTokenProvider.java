package org.lazy.fmt.client.internal;

import org.lazy.fmt.client.FmtApiException;
import org.lazy.fmt.client.FmtCodec;
import org.lazy.fmt.client.FmtConfigurationException;
import org.lazy.fmt.client.FmtError;
import org.lazy.fmt.client.FmtLogger;
import org.lazy.fmt.client.FmtLoggers;
import org.lazy.fmt.client.FmtTransportException;
import org.lazy.fmt.client.TokenProvider;
import org.lazy.fmt.client.json.Json;

import java.io.IOException;
import java.net.ConnectException;
import java.net.URI;
import java.net.URLEncoder;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.time.LocalDateTime;
import java.time.ZoneId;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * 用「账号 + 密码 + Bucket」换临时会话 token 的 {@link TokenProvider}。
 *
 * <p>登录分两步，与 CLI 完全一致：
 * <ol>
 *   <li>{@code GET /api/auth/salt?username=...} 取口令盐；</li>
 *   <li>本地算 PBKDF2-SHA256 摘要，{@code POST /api/auth/login} 发摘要 + Bucket；</li>
 *   <li>服务端回会话 token（短 TTL）；后续每个业务请求都带
 *       {@code X-FMT-Token} 与 {@code X-FMT-Bucket}。</li>
 * </ol>
 *
 * <p>明文密码只活在 Java 进程里；网络上看不到它。会话过期（401 + FMT-019）时，
 * 通道会调用 {@link #invalidate()}，下次请求自动用同一份账号密码重登。
 */
public final class SessionTokenProvider implements TokenProvider {

    private static final Duration EXPIRY_MARGIN = Duration.ofSeconds(60);

    private final String username;
    private final String password;
    private final String bucket;
    private final FmtCodec codec;
    private final FmtLogger logger;
    private final List<URI> candidates;
    private final HttpClient client;
    private final Duration requestTimeout;
    private final AtomicInteger activeCandidate = new AtomicInteger();

    private String cachedToken;
    private Instant expiresAt;

    public SessionTokenProvider(URI baseUri,
                                String username,
                                String password,
                                String bucket,
                                FmtCodec codec,
                                FmtLogger logger,
                                Duration connectTimeout,
                                Duration requestTimeout) {
        if (baseUri == null) {
            throw new FmtConfigurationException("baseUri 为空");
        }
        if (username == null || username.isBlank()) {
            throw new FmtConfigurationException("没有配置 FMT 用户名");
        }
        if (password == null) {
            throw new FmtConfigurationException("没有配置 FMT 密码");
        }
        if (bucket == null || bucket.isBlank()) {
            throw new FmtConfigurationException(
                    "登录模式必须配置库名（Bucket）：fmt.bucket / FmtChannelConfig.bucket(...)");
        }
        this.username = username;
        this.password = password;
        this.bucket = bucket;
        this.codec = codec == null ? org.lazy.fmt.client.json.SimpleJson.INSTANCE : codec;
        this.logger = logger == null ? FmtLoggers.NOOP : logger;
        this.candidates = List.copyOf(DefaultFmtChannel.buildCandidates(baseUri));
        this.requestTimeout = requestTimeout == null ? Duration.ofSeconds(30) : requestTimeout;

        HttpClient.Builder builder = HttpClient.newBuilder()
                .version(HttpClient.Version.HTTP_1_1)
                .followRedirects(HttpClient.Redirect.NEVER);
        if (connectTimeout != null && !connectTimeout.isZero() && !connectTimeout.isNegative()) {
            builder.connectTimeout(connectTimeout);
        }
        this.client = builder.build();
    }

    @Override
    public synchronized String token() {
        Instant now = Instant.now();
        if (cachedToken != null
                && (expiresAt == null || now.isBefore(expiresAt.minus(EXPIRY_MARGIN)))) {
            return cachedToken;
        }
        Login login = login();
        cachedToken = login.token;
        expiresAt = login.expiresAt;
        return cachedToken;
    }

    @Override
    public synchronized void invalidate() {
        cachedToken = null;
        expiresAt = null;
    }

    private Login login() {
        String salt = fetchSalt();
        String digest = PasswordDigest.pbkdf2Sha256Hex(password, salt);

        Map<String, String> body = new LinkedHashMap<>();
        body.put("username", username);
        body.put("password_digest", digest);
        body.put("bucket", bucket);

        HttpResponse<String> response = send("POST", "/api/auth/login", codec.write(body));
        Map<String, Object> root = envelope(response);
        String token = Json.str(root, "data.token", "");
        if (token.isBlank()) {
            throw new FmtTransportException("登录响应里没有 token：" + snippet(response.body()));
        }
        Instant expires = parseInstant(Json.str(root, "data.expires_at", ""));
        if (logger != null) {
            logger.info("FMT 登录成功：user=" + username + "，bucket=" + bucket + "，token="
                    + mask(token));
        }
        return new Login(token, expires);
    }

    private String fetchSalt() {
        HttpResponse<String> response =
                send("GET", "/api/auth/salt?username=" + encodeQuery(username), null);
        Map<String, Object> root = envelope(response);
        String salt = Json.str(root, "data.salt", "");
        if (salt.isBlank()) {
            throw new FmtTransportException("登录响应里没有 salt：" + snippet(response.body()));
        }
        return salt;
    }

    private HttpResponse<String> send(String method, String path, String body) {
        if (body == null) {
            logger.debug("FMT 登录请求：" + method + " " + path);
        } else {
            logger.debug("FMT 登录请求：" + method + " " + path + "（请求体含摘要，不打印）");
        }

        IOException lastFailure = null;
        for (int attempt = 0; attempt < candidates.size(); attempt++) {
            URI base = candidates.get(activeCandidate.get());
            HttpRequest.Builder builder = HttpRequest.newBuilder(resolve(base, path))
                    .header("User-Agent", "fmt-client-java/1.0")
                    .header("Accept", "application/json, */*")
                    .method(method, body == null
                            ? HttpRequest.BodyPublishers.noBody()
                            : HttpRequest.BodyPublishers.ofString(body, StandardCharsets.UTF_8));
            if (body != null) {
                builder.header("Content-Type", "application/json; charset=utf-8");
            }
            if (requestTimeout != null && !requestTimeout.isZero() && !requestTimeout.isNegative()) {
                builder.timeout(requestTimeout);
            }
            try {
                return client.send(builder.build(),
                        HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
            } catch (ConnectException refused) {
                lastFailure = refused;
                if (attempt + 1 < candidates.size()) {
                    int next = (activeCandidate.get() + 1) % candidates.size();
                    logger.warn("登录连接 " + base + " 被拒，改用 " + candidates.get(next));
                    activeCandidate.set(next);
                    continue;
                }
            } catch (IOException failure) {
                lastFailure = failure;
                break;
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                throw new FmtTransportException("登录请求被中断：" + path, interrupted);
            }
        }
        throw new FmtTransportException(
                "登录失败（" + path + "）：" + (lastFailure == null ? "未知错误" : lastFailure)
                        + "；已尝试的地址：" + candidates,
                lastFailure);
    }

    private Map<String, Object> envelope(HttpResponse<String> response) {
        int status = response.statusCode();
        Object parsed;
        try {
            parsed = codec.read(response.body());
        } catch (RuntimeException broken) {
            throw new FmtTransportException(
                    "登录响应不是 JSON 信封（HTTP " + status + "）：" + snippet(response.body()),
                    status, broken);
        }
        Map<String, Object> root = Json.castMap(parsed);
        if (root == null) {
            throw new FmtTransportException(
                    "登录响应顶层不是 JSON 对象（HTTP " + status + "）：" + snippet(response.body()),
                    status);
        }
        Object okFlag = root.get("ok");
        if (!(okFlag instanceof Boolean ok)) {
            throw new FmtTransportException(
                    "登录响应没有 ok 布尔字段（HTTP " + status + "）：" + snippet(response.body()),
                    status);
        }
        if (!ok) {
            Map<String, Object> error = Json.castMap(root.get("error"));
            String code = Json.str(error, "code", "FMT-018");
            String message = Json.str(error, "message", "登录失败");
            throw new FmtApiException(new FmtError(code, message, status));
        }
        return root;
    }

    private static URI resolve(URI base, String path) {
        String text = base.toString();
        while (text.endsWith("/")) {
            text = text.substring(0, text.length() - 1);
        }
        return URI.create(text + (path.startsWith("/") ? path : "/" + path));
    }

    private static String encodeQuery(String value) {
        return URLEncoder.encode(value, StandardCharsets.UTF_8).replace("+", "%20");
    }

    private static String mask(String token) {
        if (token == null || token.length() <= 8) {
            return "***";
        }
        return token.substring(0, 4) + "…" + token.substring(token.length() - 4);
    }

    private static String snippet(String body) {
        if (body == null) {
            return "";
        }
        return body.length() <= 256 ? body : body.substring(0, 256) + "…";
    }

    private static Instant parseInstant(String text) {
        if (text == null || text.isBlank()) {
            return null;
        }
        try {
            return LocalDateTime.parse(text).atZone(ZoneId.systemDefault()).toInstant();
        } catch (RuntimeException ignored) {
            // 解析不了就退化成「缓存到 401 为止」：服务端仍会拒绝过期 token。
            return null;
        }
    }

    private record Login(String token, Instant expiresAt) {
    }
}
