package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

import java.util.Map;

/**
 * 一条回收站条目：文件级与桶级都在这里，用 {@link #type()} 区分。
 *
 * @param type        {@code file} 或 {@code bucket}
 * @param id          文件级是 file_id；桶级是回收站目录名（restore/delete 都用它当标识）
 * @param name        原始名字（文件名 / 桶名）
 * @param bucket      所属 Bucket
 * @param deletedAt   删除时间
 * @param bytes       占用字节数
 * @param files       桶级条目里包含的文件数
 * @param present     数据是否真的还在磁盘上（服务端如实报告）
 * @param restorable  是否可以回退
 * @param trashPath   回收站里的相对路径
 * @param reason      不可回退 / 不可用的原因（没有就是空串）
 */
public record FmtTrashEntry(
        String type,
        String id,
        String name,
        String bucket,
        String deletedAt,
        long bytes,
        int files,
        boolean present,
        boolean restorable,
        String trashPath,
        String reason) {

    public static FmtTrashEntry from(Map<String, Object> node) {
        if (node == null) {
            return null;
        }
        return new FmtTrashEntry(
                Json.str(node, "type"),
                Json.str(node, "id"),
                Json.str(node, "name"),
                Json.str(node, "bucket"),
                Json.str(node, "deleted_at"),
                Json.lng(node, "bytes", 0L),
                Json.integer(node, "files", 0),
                Json.bool(node, "present", false),
                Json.bool(node, "restorable", false),
                Json.str(node, "trash_path"),
                Json.str(node, "reason"));
    }

    /**
     * 是不是文件级条目。
     *
     * <p>**故意不叫 {@code isFile()} / {@code isBucket()}**：Jackson 把 {@code isXxx()}
     * 当 getter，会与 record 组件 {@code bucket} 抢属性名并**覆盖**真实值
     * （真机踩过：桶名字段被序列化成 `false`）。要加布尔助手就避开 getter 命名。
     */
    public boolean fileEntry() {
        return "file".equals(type);
    }

    public boolean bucketEntry() {
        return "bucket".equals(type);
    }
}
