package org.lazy.fmt.client;

/**
 * token 放进请求头的方式。FMT 服务端两种都认（见 {@code src/server/server.cpp} 的
 * {@code header_token()}）：{@code X-FMT-Token: <token>} 优先，其次
 * {@code Authorization: Bearer <token>}。
 */
public enum FmtAuthScheme {

    /** 只发 {@code X-FMT-Token}（FMT 原生写法，默认）。 */
    X_FMT_TOKEN("X-FMT-Token"),

    /** 只发 {@code Authorization: Bearer …}（过网关/代理时更通用）。 */
    BEARER("Authorization"),

    /** 两个都发：优先兼容各种中间件，代价是 token 出现在两个头上。 */
    BOTH("X-FMT-Token + Authorization");

    private final String description;

    FmtAuthScheme(String description) {
        this.description = description;
    }

    public String description() {
        return description;
    }
}
