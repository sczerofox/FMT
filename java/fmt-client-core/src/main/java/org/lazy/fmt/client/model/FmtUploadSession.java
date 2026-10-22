package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

import java.util.ArrayList;
import java.util.List;

/**
 * 大文件分片上传会话。
 *
 * <p>对应 HTTP 的 {@code /api/upload/*}；字段来自服务端 session.json 的公开视图。
 * {@code received} 是已经收到的分片下标，{@code missing} 是还缺的下标。
 */
public record FmtUploadSession(
        String uploadId,
        String fileName,
        long size,
        long chunkSize,
        long chunkCount,
        String state,
        long uploadedBytes,
        long receivedCount,
        long verifiedCount,
        String md5,
        String finalFileId,
        String finalFileName,
        String error,
        String message,
        boolean reused,
        List<Long> received,
        List<Long> missing) {

    public static FmtUploadSession from(Object node) {
        List<Long> received = longs(node, "received");
        List<Long> missing = longs(node, "missing");
        return new FmtUploadSession(
                Json.str(node, "upload_id"),
                Json.str(node, "file_name"),
                Json.lng(node, "size", 0L),
                Json.lng(node, "chunk_size", 0L),
                Json.lng(node, "chunk_count", 0L),
                Json.str(node, "state"),
                Json.lng(node, "uploaded_bytes", 0L),
                Json.lng(node, "received_count", 0L),
                Json.lng(node, "verified_count", 0L),
                Json.str(node, "md5"),
                Json.str(node, "final_file_id"),
                Json.str(node, "final_file_name"),
                Json.str(node, "error"),
                Json.str(node, "message"),
                Json.bool(node, "reused", false),
                received,
                missing);
    }

    private static List<Long> longs(Object node, String field) {
        List<Object> values = Json.list(node, field);
        if (values == null || values.isEmpty()) {
            return List.of();
        }
        List<Long> result = new ArrayList<>(values.size());
        for (Object value : values) {
            if (value instanceof Number number) {
                result.add(number.longValue());
            }
        }
        return List.copyOf(result);
    }

    public boolean completed() {
        return "completed".equalsIgnoreCase(state);
    }

    public boolean failed() {
        return "error".equalsIgnoreCase(state);
    }

    public boolean canceled() {
        return "canceled".equalsIgnoreCase(state);
    }

    /** 0～100；大小未知时为 0。 */
    public int percent() {
        if (size <= 0) {
            return completed() ? 100 : 0;
        }
        return (int) Math.min(100L, Math.max(0L, uploadedBytes * 100L / size));
    }
}
