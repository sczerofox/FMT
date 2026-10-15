package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

import java.util.Map;

/**
 * 一条分享记录。
 *
 * <p>{@code share.get} 会额外带上 {@code state/available/message/file_name/size/bucket}：
 * 服务端对「已过期 / 次数用尽 / 已撤销 / 文件不可用」**如实报告**，
 * 只有 share_id 根本不存在才是 {@code FMT-500}。
 *
 * @param shareId           分享凭证（12 位十六进制），本身就是下载凭证
 * @param fileId            关联文件
 * @param maxDownloadCount  最多下载次数
 * @param downloadCount     已下载次数
 * @param expireTime        到期时间（服务端给的字符串）；没有到期时间时是 {@code null}
 * @param valid             是否还在有效状态（未撤销、未过期、次数没用尽）
 * @param state             服务端的状态名（{@code ok/expired/limit_reached/revoked/…}）
 * @param available         现在能不能下载
 * @param message           服务端说明
 * @param fileName          文件名（仅 share.get）
 * @param size              文件大小（仅 share.get，可能为 {@code null}）
 * @param bucket            文件所在 Bucket（仅 share.get）
 */
public record FmtShare(
        String shareId,
        String fileId,
        int maxDownloadCount,
        int downloadCount,
        String expireTime,
        boolean valid,
        String state,
        boolean available,
        String message,
        String fileName,
        Long size,
        String bucket) {

    public static FmtShare from(Map<String, Object> node) {
        if (node == null) {
            return null;
        }
        Object sizeNode = Json.path(node, "size");
        return new FmtShare(
                Json.str(node, "share_id"),
                Json.str(node, "file_id"),
                Json.integer(node, "max_download_count", 0),
                Json.integer(node, "download_count", 0),
                Json.str(node, "expire_time").isEmpty() ? null : Json.str(node, "expire_time"),
                Json.bool(node, "is_valid", true),
                Json.str(node, "state"),
                Json.bool(node, "available", true),
                Json.str(node, "message"),
                Json.str(node, "file_name"),
                sizeNode instanceof Number number ? number.longValue() : null,
                Json.str(node, "bucket"));
    }
}
