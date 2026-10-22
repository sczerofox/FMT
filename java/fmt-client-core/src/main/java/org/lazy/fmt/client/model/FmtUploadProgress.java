package org.lazy.fmt.client.model;

import java.util.List;

/**
 * 大文件上传进度快照。
 *
 * @param uploadId      会话 ID
 * @param fileName      目标文件名
 * @param size          总字节数
 * @param uploadedBytes 已上传字节数
 * @param chunkIndex    刚完成/即将上传的分片下标；-1 表示会话级事件
 * @param chunkCount    分片总数
 * @param state         服务端状态
 * @param received      已收到的分片下标
 */
public record FmtUploadProgress(
        String uploadId,
        String fileName,
        long size,
        long uploadedBytes,
        long chunkIndex,
        long chunkCount,
        String state,
        List<Long> received) {

    public int percent() {
        if (size <= 0) {
            return 0;
        }
        return (int) Math.min(100L, Math.max(0L, uploadedBytes * 100L / size));
    }
}
