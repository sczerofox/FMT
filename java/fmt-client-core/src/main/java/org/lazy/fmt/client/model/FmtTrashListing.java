package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * {@code GET /api/trash} 的结果。
 *
 * @param entries 条目（文件级 + 桶级）
 * @param count   条目总数
 * @param files   文件级条目数
 * @param buckets 桶级条目数
 */
public record FmtTrashListing(List<FmtTrashEntry> entries, int count, int files, int buckets) {

    public static FmtTrashListing from(Object dataNode) {
        Map<String, Object> data = Json.castMap(dataNode);
        if (data == null) {
            return new FmtTrashListing(List.of(), 0, 0, 0);
        }
        List<FmtTrashEntry> entries = new ArrayList<>();
        for (Map<String, Object> item : Json.maps(data, "entries")) {
            FmtTrashEntry entry = FmtTrashEntry.from(item);
            if (entry != null) {
                entries.add(entry);
            }
        }
        return new FmtTrashListing(
                List.copyOf(entries),
                Json.integer(data, "count", entries.size()),
                Json.integer(data, "files", 0),
                Json.integer(data, "buckets", 0));
    }
}
