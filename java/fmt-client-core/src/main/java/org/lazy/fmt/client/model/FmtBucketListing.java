package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * {@code GET /api/bucket} 的结果：Bucket 列表（**只读**）。
 *
 * <p>HTTP 侧只有这一个桶接口：创建 / 切换 / 删除都由 CLI 管。原因是「当前 Bucket」是
 * 服务端的全局状态，多个 HTTP 调用方一起改会互相踩；而且桶的删除有确认流程，
 * 那属于 CLI 的交互。
 *
 * @param buckets       桶列表
 * @param count         桶数量
 * @param currentBucket 当前桶（HTTP 的文件接口全都作用在它上面）
 */
public record FmtBucketListing(List<FmtBucket> buckets, int count, String currentBucket) {

    /**
     * 一个桶。
     *
     * @param name      桶名（服务端统一小写）
     * @param current   是否是当前桶
     */
    public record FmtBucket(String name, boolean current) {

        public static FmtBucket from(Map<String, Object> node) {
            if (node == null) {
                return null;
            }
            return new FmtBucket(
                    Json.str(node, "name"),
                    Json.bool(node, "is_current", false));
        }
    }

    public static FmtBucketListing from(Object dataNode) {
        Map<String, Object> data = Json.castMap(dataNode);
        if (data == null) {
            return new FmtBucketListing(List.of(), 0, "");
        }
        List<FmtBucket> buckets = new ArrayList<>();
        for (Map<String, Object> item : Json.maps(data, "buckets")) {
            FmtBucket bucket = FmtBucket.from(item);
            if (bucket != null) {
                buckets.add(bucket);
            }
        }
        return new FmtBucketListing(
                List.copyOf(buckets),
                Json.integer(data, "count", buckets.size()),
                Json.str(data, "current_bucket"));
    }
}
