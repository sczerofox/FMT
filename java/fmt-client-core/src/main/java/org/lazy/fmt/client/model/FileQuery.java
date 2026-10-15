package org.lazy.fmt.client.model;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * {@code GET /api/file} 的查询条件。
 *
 * <p>服务端的顺序固定为**过滤 → 排序 → 分页**，所以翻页不会漏也不会重；
 * {@code pageSize} 给 0 表示不分页（一次给全，只多回一个 total）。
 */
public final class FileQuery {

    /** 排序方式，对应服务端的 {@code ?sort=}。 */
    public enum Sort {
        NAME("name"),
        SIZE("size"),
        ID("id");

        private final String wire;

        Sort(String wire) {
            this.wire = wire;
        }

        public String wire() {
            return wire;
        }
    }

    private final String search;
    private final Sort sort;
    private final int page;
    private final int pageSize;

    private FileQuery(Builder builder) {
        this.search = builder.search;
        this.sort = builder.sort;
        this.page = builder.page;
        this.pageSize = builder.pageSize;
    }

    public static Builder builder() {
        return new Builder();
    }

    public static FileQuery all() {
        return builder().build();
    }

    public String search() {
        return search;
    }

    public Sort sort() {
        return sort;
    }

    public int page() {
        return page;
    }

    public int pageSize() {
        return pageSize;
    }

    /** 只带上真正有值的参数，避免给服务端塞无意义的默认值。 */
    public Map<String, String> toQueryParams() {
        Map<String, String> params = new LinkedHashMap<>();
        if (search != null && !search.isBlank()) {
            params.put("search", search);
        }
        if (sort != null) {
            params.put("sort", sort.wire());
        }
        if (pageSize > 0) {
            params.put("page", Integer.toString(page));
            params.put("page_size", Integer.toString(pageSize));
        }
        return params;
    }

    public static final class Builder {

        private String search;
        private Sort sort;
        private int page = 1;
        private int pageSize = 0;

        private Builder() {
        }

        /** 关键字：不区分大小写的子串匹配，文件名与 file_id 都匹配。 */
        public Builder search(String search) {
            this.search = search;
            return this;
        }

        public Builder sort(Sort sort) {
            this.sort = sort;
            return this;
        }

        /** 从 1 开始。 */
        public Builder page(int page) {
            this.page = page;
            return this;
        }

        /** 0（默认）= 不分页；上限 1000（服务端会校验）。 */
        public Builder pageSize(int pageSize) {
            this.pageSize = pageSize;
            return this;
        }

        public FileQuery build() {
            return new FileQuery(this);
        }
    }
}
