package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

import java.util.Map;

/**
 * 一条文件记录。
 *
 * <p>字段对齐服务端：{@code file.list} 的条目只有
 * {@code file_id/file_name/extension/file_type/size/md5}；
 * {@code file.get} 会多给 {@code bucket/is_trash/path/trash_path/trash_reason}。
 * 列表里拿不到的那些字段是 {@code null} 或空串——**不要**把 {@code null} 当 false 用。
 *
 * @param fileId     形如 {@code fmt-20261009-0}，全局唯一
 * @param fileName   文件名（可能含中文）
 * @param bucket     所在 Bucket（仅单条查询有）
 * @param extension  扩展名（不含点）
 * @param fileType   文件类型（服务端判定，预览策略按它走）
 * @param size       字节数
 * @param md5        MD5 十六进制
 * @param trashed    是否在回收站（仅单条查询有；{@code null} = 未知）
 * @param path       相对数据根的路径（仅单条查询有）
 * @param trashPath  回收站里的相对路径（仅单条查询、且在回收站时有）
 * @param trashReason 进回收站的原因（桶级删除标记等）
 */
public record FmtFile(
        String fileId,
        String fileName,
        String bucket,
        String extension,
        String fileType,
        long size,
        String md5,
        Boolean trashed,
        String path,
        String trashPath,
        String trashReason) {

    public static FmtFile from(Map<String, Object> node) {
        if (node == null) {
            return null;
        }
        return new FmtFile(
                Json.str(node, "file_id"),
                Json.str(node, "file_name"),
                Json.str(node, "bucket"),
                Json.str(node, "extension"),
                Json.str(node, "file_type"),
                Json.lng(node, "size", 0L),
                Json.str(node, "md5"),
                Json.boolOrNull(node, "is_trash"),
                Json.str(node, "path"),
                Json.str(node, "trash_path"),
                Json.str(node, "trash_reason"));
    }

    /** 上传接口返回的节点里没有 is_trash/path，这里照样能接。 */
    public static FmtFile fromRoot(Object dataNode) {
        return from(Json.castMap(dataNode));
    }

    /**
     * 是否在回收站。
     *
     * <p>**故意不叫 {@code isTrashed()}**：Jackson 会把 {@code isXxx()} 当成 getter，
     * 与 record 组件 {@code trashed} 抢同一个属性名并**覆盖**真实值
     * （真机踩过：`"trashed":false` 把本来缺失的 null 顶掉了）。
     * 在 record 上写 {@code isXxx()} 会污染宿主 ObjectMapper 的序列化结果。
     */
    public boolean inTrash() {
        return Boolean.TRUE.equals(trashed);
    }
}
