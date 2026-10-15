package org.lazy.fmt.client;

import org.lazy.fmt.client.json.Json;
import org.lazy.fmt.client.json.SimpleJson;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * {@link TokenProvider} 的现成实现。
 *
 * <p>FMT 的 token 有几个常见来源，按「离配置文件最近」到「离得最远」排列：
 * <ol>
 *   <li>{@link #of(String)} —— 配置里直接写了（Spring 的 {@code fmt.token}）；</li>
 *   <li>{@link #fromUserJson(Path)} / {@link #fromDataRoot(Path)} —— 从数据根读
 *       {@code data/user.json}，**推荐**，服务改 token 后能自愈；</li>
 *   <li>{@link #fromEnvironment(String...)} —— 环境变量 / 系统属性（容器里常用）；</li>
 *   <li>{@link #firstOf(TokenProvider...)} —— 上面几种按优先级串起来；</li>
 *   <li>{@link #failWith(String)} —— 兜底：给出「你该怎么配」的报错。</li>
 * </ol>
 */
public final class TokenProviders {

    private TokenProviders() {
    }

    /** 固定 token。改 token 必须重建通道。 */
    public static TokenProvider of(String token) {
        String value = token == null ? "" : token.trim();
        if (value.isEmpty()) {
            throw new FmtConfigurationException("token 为空");
        }
        return () -> value;
    }

    /**
     * 从环境变量或 JVM 系统属性取 token，按给出的顺序找，先找到的先用。
     *
     * <p>每个名字都会先查 {@link System#getProperty(String)} 再查 {@link System#getenv(String)}，
     * 这样 {@code -DFMT_TOKEN=…} 和容器环境变量都能用。
     */
    public static TokenProvider fromEnvironment(String... names) {
        String[] candidates = (names == null || names.length == 0)
                ? new String[]{"FMT_TOKEN"}
                : names.clone();
        return () -> {
            for (String name : candidates) {
                String value = System.getProperty(name);
                if (value == null || value.isBlank()) {
                    value = System.getenv(name);
                }
                if (value != null && !value.isBlank()) {
                    return value.trim();
                }
            }
            throw new FmtConfigurationException(
                    "环境变量/系统属性里没有 token，找过：" + String.join(", ", candidates));
        };
    }

    /** 数据根下的 {@code data/user.json}（**默认带 30 秒缓存**）。 */
    public static TokenProvider fromDataRoot(Path dataRoot) {
        if (dataRoot == null) {
            throw new FmtConfigurationException("dataRoot 为空");
        }
        return fromUserJson(dataRoot.resolve("data").resolve("user.json"));
    }

    public static TokenProvider fromUserJson(Path userJsonFile) {
        return fromUserJson(userJsonFile, Duration.ofSeconds(30));
    }

    /**
     * 直接指定 {@code user.json} 文件。
     *
     * @param ttl 缓存时长；{@code null} 或非正数表示每次都读文件（文件很小，读它不贵）
     */
    public static TokenProvider fromUserJson(Path userJsonFile, Duration ttl) {
        if (userJsonFile == null) {
            throw new FmtConfigurationException("user.json 路径为空");
        }
        Path file = userJsonFile.toAbsolutePath().normalize();
        TokenProvider reader = () -> readTokenFromUserJson(file);
        return (ttl == null || ttl.isZero() || ttl.isNegative()) ? reader : cached(reader, ttl);
    }

    /** 给任意 provider 套一层 TTL 缓存；{@link TokenProvider#invalidate()} 会穿透到被包装者。 */
    public static TokenProvider cached(TokenProvider delegate, Duration ttl) {
        if (delegate == null) {
            throw new FmtConfigurationException("被包装的 TokenProvider 为空");
        }
        long ttlNanos = (ttl == null ? Duration.ZERO : ttl).toNanos();
        if (ttlNanos <= 0) {
            return delegate;
        }
        return new CachedTokenProvider(delegate, ttlNanos);
    }

    /**
     * 依次尝试多个来源，返回第一个能给出非空 token 的。
     *
     * <p>只有 {@link FmtConfigurationException}（「这个来源没配」）会跳到下一个；
     * 其它异常一律原样抛出——文件坏了就该让人看见，而不是悄悄换个来源。
     */
    public static TokenProvider firstOf(TokenProvider... candidates) {
        TokenProvider[] providers = candidates == null ? new TokenProvider[0] : candidates.clone();
        return () -> {
            List<String> reasons = new ArrayList<>();
            for (TokenProvider provider : providers) {
                if (provider == null) {
                    continue;
                }
                try {
                    String value = provider.token();
                    if (value != null && !value.isBlank()) {
                        return value.trim();
                    }
                    reasons.add("返回了空 token");
                } catch (FmtConfigurationException notConfigured) {
                    reasons.add(notConfigured.getMessage());
                }
            }
            throw new FmtConfigurationException("没有任何可用的 FMT token 来源：" + String.join("；", reasons));
        };
    }

    /** 永远失败的来源，用来说明「该怎么配」。放在 {@link #firstOf} 最后一项最好用。 */
    public static TokenProvider failWith(String message) {
        return () -> {
            throw new FmtConfigurationException(message);
        };
    }

    /** 日志里用的掩码：{@code 1a2867378b112b8e7864218fd2fd693f -> 1a28…693f}。 */
    public static String mask(String token) {
        if (token == null || token.isBlank()) {
            return "(未设置)";
        }
        String value = token.trim();
        if (value.length() <= 8) {
            return "***";
        }
        return value.substring(0, 4) + "…" + value.substring(value.length() - 4);
    }

    /**
     * 读 {@code data/user.json} 里的 token。
     *
     * <p>**两种形状都认**（真机踩过：老部署里还是 {@code users} 数组）：
     * <ul>
     *   <li>现行（单用户文件，字段平铺）：{@code {"version":1,"token":"…",…}}</li>
     *   <li>老版本（多用户集合）：{@code {"version":1,"users":[{"token":"…"}]}} —— 取第一条；
     *       服务端下一次启动会把它改写成单用户形状，但<strong>客户端不能等它</strong>。</li>
     * </ul>
     */
    private static String readTokenFromUserJson(Path file) {
        if (!Files.isRegularFile(file)) {
            throw new FmtConfigurationException(
                    "token 文件不存在：" + file + "（先跑 `fmt.exe config list --show-token`"
                            + "确认数据根与 token，或改用 fmt.token / FMT_TOKEN 显式给出 token）");
        }
        String text;
        try {
            text = Files.readString(file, StandardCharsets.UTF_8);
        } catch (IOException failure) {
            throw new FmtConfigurationException("读 token 文件失败：" + file, failure);
        }
        if (text.startsWith("\uFEFF")) {
            text = text.substring(1);
        }
        Object parsed;
        try {
            parsed = SimpleJson.INSTANCE.read(text);
        } catch (FmtJsonException broken) {
            throw new FmtConfigurationException("token 文件不是合法 JSON：" + file, broken);
        }
        Map<String, Object> root = Json.castMap(parsed);
        if (root == null) {
            throw new FmtConfigurationException("token 文件顶层不是 JSON 对象：" + file);
        }
        String token = Json.str(root, "token", "");
        if (token.isBlank()) {
            // 老形状：users 数组，取第一条
            token = Json.str(root, "users.0.token", "");
        }
        if (token.isBlank()) {
            throw new FmtConfigurationException(
                    "token 文件里没有 token 字段：" + file
                            + "（现行形状是字段平铺在顶层 {\"user_id\":\"…\",\"token\":\"…\"}；"
                            + "老形状 {\"users\":[{\"token\":\"…\"}]} 也认，取第一条）");
        }
        return token.trim();
    }

    /** 带 TTL 的缓存实现：只在过期后重新取，`invalidate()` 立刻作废。 */
    private static final class CachedTokenProvider implements TokenProvider {

        private final TokenProvider delegate;
        private final long ttlNanos;
        private volatile String value;
        private volatile long expiresAt;

        CachedTokenProvider(TokenProvider delegate, long ttlNanos) {
            this.delegate = delegate;
            this.ttlNanos = ttlNanos;
        }

        @Override
        public String token() {
            String cached = value;
            if (cached != null && System.nanoTime() - expiresAt < 0) {
                return cached;
            }
            synchronized (this) {
                if (value != null && System.nanoTime() - expiresAt < 0) {
                    return value;
                }
                String fresh = delegate.token();
                if (fresh == null || fresh.isBlank()) {
                    throw new FmtConfigurationException("token 来源返回了空值");
                }
                value = fresh.trim();
                expiresAt = System.nanoTime() + ttlNanos;
                return value;
            }
        }

        @Override
        public void invalidate() {
            synchronized (this) {
                value = null;
                expiresAt = 0L;
                delegate.invalidate();
            }
        }
    }
}
