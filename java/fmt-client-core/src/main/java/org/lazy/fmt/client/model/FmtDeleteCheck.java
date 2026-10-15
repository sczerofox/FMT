package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * {@code DELETE /api/file/<id>?dry_run=1} 的预检结论。
 *
 * <p>服务端把「要不要确认」和「确认也解决不了」分得很清楚，客户端照着办即可：
 * <ul>
 *   <li>{@link #needsConfirm()} —— 跨 Bucket 删除，补一次 {@code force} 就能继续；</li>
 *   <li>{@link #blocked()} —— 同名歧义等，**y/N 解决不了**，必须先让用户选具体对象。</li>
 * </ul>
 *
 * @param candidates 歧义时给出的候选文件（让用户选一个）
 */
public record FmtDeleteCheck(
        boolean ambiguous,
        boolean otherBucket,
        boolean blocked,
        boolean needsConfirm,
        String fileId,
        String fileName,
        String bucket,
        String path,
        String message,
        List<FmtFile> candidates) {

    public static FmtDeleteCheck from(Object dataNode) {
        Map<String, Object> data = Json.castMap(dataNode);
        if (data == null) {
            return new FmtDeleteCheck(false, false, false, false, "", "", "", "", "", List.of());
        }
        List<FmtFile> candidates = new ArrayList<>();
        for (Map<String, Object> item : Json.maps(data, "candidates")) {
            candidates.add(FmtFile.from(item));
        }
        return new FmtDeleteCheck(
                Json.bool(data, "ambiguous", false),
                Json.bool(data, "other_bucket", false),
                Json.bool(data, "blocked", false),
                Json.bool(data, "needs_confirm", false),
                Json.str(data, "file_id"),
                Json.str(data, "file_name"),
                Json.str(data, "bucket"),
                Json.str(data, "path"),
                Json.str(data, "message"),
                List.copyOf(candidates));
    }
}
