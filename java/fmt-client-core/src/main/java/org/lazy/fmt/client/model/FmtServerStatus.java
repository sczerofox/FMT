package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

/**
 * {@code GET /api/status} 的结果（**这个接口要 token**，只有 {@code /api/ping} 是公开的）。
 *
 * @param root    FMT 的数据根绝对路径（exe 所在目录）
 * @param pid     服务进程号
 * @param version FMT 版本
 * @param channel 通道类型，HTTP 时是 {@code "http"}
 */
public record FmtServerStatus(String root, long pid, String version, String channel) {

    public static FmtServerStatus from(Object dataNode) {
        return new FmtServerStatus(
                Json.str(dataNode, "root"),
                Json.lng(dataNode, "pid", 0L),
                Json.str(dataNode, "version"),
                Json.str(dataNode, "channel"));
    }
}
