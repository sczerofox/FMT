package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

import java.util.Map;

/**
 * {@code DELETE /api/trash/empty} 的结果：清空整个回收站。
 *
 * <p>两个分支共用一个结构：
 * <ul>
 *   <li>{@code ?dry_run=1} —— 只预检，{@link #needsConfirm()} 会是 {@code true}（有东西要删），
 *       {@link #blocked()} 为 false（清空**没有**"确认也解决不了"的情况）；</li>
 *   <li>{@code ?force=1} —— 真删完了，{@link #needsConfirm()} 为 {@code null}（服务端不给这个字段）、
 *       条数与字节数是刚刚删掉的数量。</li>
 * </ul>
 *
 * <p>回收站本来就空时，服务端**不会**要求确认，直接回 {@code files=0,buckets=0}。
 *
 * @param files        文件级条目数
 * @param buckets      桶级条目数
 * @param bytes        字节数
 * @param needsConfirm 是否需要确认；只有预检分支会给值，其余情况是 {@code null}
 * @param blocked      是否存在"确认也解决不了"的阻碍
 * @param message      服务端说明
 */
public record FmtTrashEmpty(
        int files,
        int buckets,
        long bytes,
        Boolean needsConfirm,
        boolean blocked,
        String message) {

    public static FmtTrashEmpty from(Object dataNode) {
        Map<String, Object> data = Json.castMap(dataNode);
        if (data == null) {
            return new FmtTrashEmpty(0, 0, 0L, null, false, "");
        }
        return new FmtTrashEmpty(
                Json.integer(data, "files", 0),
                Json.integer(data, "buckets", 0),
                Json.lng(data, "bytes", 0L),
                Json.boolOrNull(data, "needs_confirm"),
                Json.bool(data, "blocked", false),
                Json.str(data, "message"));
    }

    public int total() {
        return files + buckets;
    }
}
