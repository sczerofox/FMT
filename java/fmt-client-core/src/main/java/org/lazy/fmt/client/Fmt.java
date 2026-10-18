package org.lazy.fmt.client;

import org.lazy.fmt.client.internal.DefaultFmtChannel;

import java.nio.file.Path;
import java.time.Duration;

/**
 * SDK 入口。
 *
 * <pre>{@code
 * // ① 最直观：参数载体（想调什么就 set 什么，不调就用默认值）
 * FmtClientCore core = new FmtClientCore();
 * core.setBaseUrl("http://localhost:4122");
 * core.setToken(token);                       // 或用 core.setDataRoot(Path.of("D:/Tools/fmt"))
 * try (FmtChannel fmt = Fmt.channel(core)) {
 *     fmt.ping();
 *     FmtResult<FmtPage> page = fmt.listFiles(FileQuery.all());
 * }
 *
 * // ② 最省事：默认地址 + 从数据根读 token（推荐，服务端换 token 也能自愈）
 * try (FmtChannel fmt = Fmt.fromDataRoot(Path.of("D:/Tools/fmt"))) {
 *     FmtResult<FmtPage> page = fmt.listFiles(FileQuery.all());
 * }
 *
 * // ③ 完全显式：地址 + 明文 token（占位符，别照抄：每个数据根一个，
 * //   从 <数据根>/data/user.json 或 fmt.exe config list --show-token 拿）
 * try (FmtChannel fmt = Fmt.channel("http://127.0.0.1:4122", token)) { … }
 *
 * // ④ 细调：超时、连接数、认证头写法、日志、JSON 实现
 * FmtChannelConfig config = Fmt.builder("http://127.0.0.1:4122", token)
 *         .requestTimeout(Duration.ofSeconds(10))
 *         .authScheme(FmtAuthScheme.BEARER)
 *         .logger(FmtLoggers.stderr())
 *         .build();
 * }</pre>
 */
public final class Fmt {

    private Fmt() {
    }

    /** 由配置建通道（调用方负责 {@link FmtChannel#close()}，或放进 try-with-resources）。 */
    public static FmtChannel channel(FmtChannelConfig config) {
        return new DefaultFmtChannel(config);
    }

    /**
     * 由参数载体建通道：<b>先调参数、再建通道、然后只调方法</b>。
     *
     * <pre>{@code
     * FmtClientCore core = new FmtClientCore();
     * core.setBaseUrl("http://localhost:4122");
     * core.setToken(token);
     * try (FmtChannel fmt = Fmt.channel(core)) {
     *     fmt.ping();
     * }
     * }</pre>
     *
     * <p>建通道时把 {@code core} 里的参数<b>拷成不可变配置</b>，之后改 {@code core} 不再影响这个通道；
     * 凭证也从此绑在通道上（业务方法里没有 token 参数）。
     */
    public static FmtChannel channel(FmtClientCore core) {
        if (core == null) {
            throw new FmtConfigurationException("FmtClientCore 为空：先 new 一个并 setBaseUrl / setToken");
        }
        return channel(core.toConfig());
    }

    public static FmtChannel channel(String baseUrl, String token) {
        return channel(FmtChannelConfig.builder(baseUrl, token).build());
    }

    /**
     * 像 MySQL 一样用「账号 + 密码 + 库名」建通道（默认地址 {@code http://localhost:4122}）。
     *
     * <pre>{@code
     * try (FmtChannel fmt = Fmt.login("user", "password", "work")) {
     *     fmt.listFiles(FileQuery.all());   // 每个请求都自动带 X-FMT-Bucket: work
     * }
     * }</pre>
     */
    public static FmtChannel login(String username, String password, String bucket) {
        return channel(FmtChannelConfig.builder()
                .login(username, password, bucket)
                .build());
    }

    /** 像 MySQL 一样用「地址 + 账号 + 密码 + 库名」建通道。 */
    public static FmtChannel login(String baseUrl, String username, String password, String bucket) {
        return channel(FmtChannelConfig.builder()
                .baseUrl(baseUrl)
                .login(username, password, bucket)
                .build());
    }

    /** 旧 token 模式 + 显式库名：每个请求带 {@code X-FMT-Bucket}。 */
    public static FmtChannel channel(String baseUrl, String token, String bucket) {
        return channel(FmtChannelConfig.builder(baseUrl, token).bucket(bucket).build());
    }

    public static FmtChannel channel(String baseUrl, TokenProvider tokenProvider) {
        return channel(FmtChannelConfig.builder().baseUrl(baseUrl).tokenProvider(tokenProvider).build());
    }

    /** 默认地址（{@code http://127.0.0.1:4122}）+ 数据根下的 {@code data/user.json}。 */
    public static FmtChannel fromDataRoot(Path dataRoot) {
        return channel(FmtChannelConfig.builder()
                .tokenProvider(TokenProviders.fromDataRoot(dataRoot))
                .build());
    }

    public static FmtChannel fromDataRoot(String baseUrl, Path dataRoot) {
        return channel(FmtChannelConfig.builder()
                .baseUrl(baseUrl)
                .tokenProvider(TokenProviders.fromDataRoot(dataRoot))
                .build());
    }

    /** 环境变量 {@code FMT_TOKEN} / 系统属性 {@code -DFMT_TOKEN=…}。 */
    public static FmtChannel fromEnvironment(String baseUrl) {
        return channel(FmtChannelConfig.builder()
                .baseUrl(baseUrl)
                .tokenProvider(TokenProviders.fromEnvironment("FMT_TOKEN"))
                .build());
    }

    public static FmtChannelConfig.Builder builder() {
        return FmtChannelConfig.builder();
    }

    public static FmtChannelConfig.Builder builder(String baseUrl, String token) {
        return FmtChannelConfig.builder(baseUrl, token);
    }

    /** 常用默认值的说明性常量：FMT 的 JSON 调用给 30 秒通常够用。 */
    public static final Duration DEFAULT_REQUEST_TIMEOUT = Duration.ofSeconds(30);
}
