package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * {@code GET /api/file} 的一页结果。
 *
 * @param files      本页文件；超出末页时是空列表（服务端不报错）
 * @param count      本页条数
 * @param total      命中总数（过滤之后、分页之前）——**不是**本页条数
 * @param page       页码，从 1 开始
 * @param pageSize   每页条数；0 表示没分页
 * @param totalPages 总页数；没分页时为 0
 * @param sort       实际使用的排序方式
 * @param search     实际使用的搜索关键字（没搜就是空串）
 */
public record FmtPage(
        List<FmtFile> files,
        int count,
        long total,
        int page,
        int pageSize,
        int totalPages,
        String sort,
        String search) {

    public static FmtPage from(Object dataNode) {
        Map<String, Object> data = Json.castMap(dataNode);
        if (data == null) {
            return new FmtPage(List.of(), 0, 0L, 1, 0, 0, "", "");
        }
        List<FmtFile> files = new ArrayList<>();
        for (Map<String, Object> item : Json.maps(data, "files")) {
            files.add(FmtFile.from(item));
        }
        return new FmtPage(
                List.copyOf(files),
                Json.integer(data, "count", files.size()),
                Json.lng(data, "total", files.size()),
                Json.integer(data, "page", 1),
                Json.integer(data, "page_size", 0),
                Json.integer(data, "total_pages", 0),
                Json.str(data, "sort"),
                Json.str(data, "search"));
    }

    /** 是否还有下一页（只有分页查询才判断得出来）。 */
    public boolean hasMore() {
        return pageSize > 0 && page < totalPages;
    }
}
