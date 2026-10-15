package org.lazy.fmt.tool;

import org.lazy.fmt.client.FmtClientCore;
import org.lazy.fmt.client.FmtLoggers;

import java.nio.file.Path;
import java.nio.file.Paths;
import java.time.Duration;

/**
 * FMT 连接参数：地址 + 凭证只在这里写一次，其余地方（含业务类）看不到 token。
 * 对应设计文档 §4.1 的凭证来源优先级。
 *
 * <h2>用之前改这 3 个常量（这里的值都是占位符，照抄会连不上）</h2>
 * <ol>
 *   <li>{@link #BASE_URL} —— 默认 {@code http://localhost:4122}，一般不用改；
 *       <b>必须写主机名 localhost</b>，写 127.0.0.1 时 SDK 没法在 IPv4/IPv6 之间回落（§2.1、§4.4）。</li>
 *   <li>{@link #DATA_ROOT} —— 改成你自己的 FMT 数据根（exe 所在目录）。</li>
 *   <li>{@link #TOKEN} —— 想固定 token 就填；留空则走 {@code <数据根>/data/user.json}（推荐）。</li>
 * </ol>
 * token 从哪来：在数据根下跑 {@code fmt.exe config list --show-token}，或直接读
 * {@code <数据根>/data/user.json} 的 {@code token} 字段。
 *
 * <p><b>不要把真实 token 提交进版本库</b>：留空 + 数据根读取即可，服务端换 token 后它自己会跟上。
 *
 * <p>兼容 Java 8：路径用 {@link Paths#get}，超时用 {@link Duration#ofSeconds}。
 */
public class FmtTool {

    /** FMT 地址：**必须写主机名 localhost**（见类注释）。 */
    public static final String BASE_URL = "http://localhost:4122";

    /** FMT 数据根（exe 所在目录）：改成你自己的，例如 {@code D:/Tools/fmt}。 */
    public static final String DATA_ROOT = "D:/Tools/fmt";

    /** 固定 token；留空表示从 {@code <DATA_ROOT>/data/user.json} 读（推荐，能自愈）。 */
    public static final String TOKEN = "";

    /** 数据根下的 data/user.json。 */
    public static final Path TOKEN_FILE = Paths.get(DATA_ROOT, "data", "user.json");

    /** 建一条通道配置：不传就全用 FmtTool 里的值（token 空则从数据根读）。 */
    public FmtClientCore FmtConnection() {
        return TOKEN == null || TOKEN.trim().isEmpty()
                ? FmtConnectionFromRoot()
                : FmtConnection(BASE_URL, TOKEN.trim());
    }

    public FmtClientCore FmtConnection(String baseUrl, String token) {
        return new FmtClientCore()
                .setBaseUrl(baseUrl)
                .setToken(token)
                .setConnectTimeout(Duration.ofSeconds(3))     // §4.3
                .setRequestTimeout(Duration.ofSeconds(30))
                .setStreamTimeout(Duration.ofMinutes(30))     // 读写大文件用
                .setMaxConnections(16)                        // 并发闸门
                .setRetryOnUnauthorized(true)                 // 401 → 换凭证 → 只重试一次（§4.2）
                .setIdempotentRetries(1)
                .setLogger(FmtLoggers.stderr());              // token 只以掩码出现，可去掉
    }

    /** 从数据根读 token 的写法：token 轮换后不用重启 Java。 */
    public FmtClientCore FmtConnectionFromRoot() {
        return new FmtClientCore()
                .setBaseUrl(BASE_URL)
                .setDataRoot(Paths.get(DATA_ROOT));
    }
}