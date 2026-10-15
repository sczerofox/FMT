package org.lazy.fmt.client;

import com.fasterxml.jackson.annotation.JsonIgnore;
import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.annotation.JsonProperty;

import java.util.function.Function;

/**
 * FMT 调用的结果：要么成功带数据，要么失败带 {@link FmtError}——两者互斥。
 *
 * <p>刻意照抄 FMT 服务端 {@code Result<T> = variant<T, Error>} 的语义：不需要调用方去读
 * 一个可能自相矛盾的「success + data + error」三件套。
 *
 * <pre>{@code
 * FmtResult<FmtPage> page = channel.listFiles(FileQuery.builder().search("报表").build());
 * if (!page.isOk()) {
 *     log.warn("列文件失败: {}", page.error());
 *     return;
 * }
 * for (FmtFile file : page.data().files()) { … }
 * }</pre>
 *
 * <h2>直接返回给前端时（Spring MVC / Jackson）</h2>
 * 注解把 JSON 形状钉成和 FMT 服务端一样的信封，而且**成功时不出现 error、失败时不出现 data**：
 * <pre>{@code
 * { "ok": true,  "data": { … } }
 * { "ok": false, "error": { "code": "FMT-002", "message": "…", "httpStatus": 404 } }
 * }</pre>
 * 没有这些注解会出真事故（真机踩过）：Jackson 按 bean 处理本类，{@code isFailure()} 与失败分支的
 * {@code data()} 抛异常被吞掉后，成功响应会缩水成 {@code {"ok":true}}——**data 整个丢了**。
 *
 * @param <T> 成功时的数据类型
 */
@JsonInclude(JsonInclude.Include.NON_NULL)
public final class FmtResult<T> {

    /** 字段名刻意不叫 data：避免和下面给 Jackson 的 {@code value()} 撞同一个属性名。 */
    private final T value;
    private final FmtError error;

    private FmtResult(T value, FmtError error) {
        this.value = value;
        this.error = error;
    }

    public static <T> FmtResult<T> ok(T data) {
        return new FmtResult<>(data, null);
    }

    public static <T> FmtResult<T> failure(FmtError error) {
        return new FmtResult<>(null, error == null
                ? FmtError.local(FmtErrorCodes.IO_ERROR, "未知错误")
                : error);
    }

    public static <T> FmtResult<T> failure(String code, String message) {
        return failure(FmtError.local(code, message));
    }

    /** 序列化成信封的 {@code ok} 字段。 */
    @JsonProperty("ok")
    public boolean isOk() {
        return error == null;
    }

    /** 内部判断用；**不是** JSON 字段（曾因为它让成功响应丢 data）。 */
    @JsonIgnore
    public boolean isFailure() {
        return error != null;
    }

    /**
     * 成功时的数据；失败时抛 {@link IllegalStateException}（用 {@link #isOk()} 先判断）。
     *
     * <p><b>这个方法没有 {@code @JsonProperty}</b>：它对失败结果会抛异常，绝不能交给 Jackson。
     * 给 Jackson 的是下面的 {@link #value()}——同样的值，但不抛。
     */
    public T data() {
        if (error != null) {
            throw new IllegalStateException("结果是失败，没有数据：" + error);
        }
        return value;
    }

    /**
     * 给 Jackson 的访问器：成功就是 data，失败是 {@code null}（配 {@code NON_NULL} 直接不出现）。
     *
     * <p>{@code @JsonProperty("data")} 与 {@code @JsonProperty("error")} 都是**必须**的：
     * 不写名字的话，Jackson 因为返回类型是类型变量 {@code T} 而推断不出属性类型，
     * 会把访问器<b>静默丢掉</b>——成功响应只剩 {@code {"ok":true}}，data 整个消失（真机踩过）。
     *
     * <p>{@code @JsonIgnore} 在字段上：属性 {@code data} 已经被显式认领，字段本身不必再被探测。
     */
    @JsonProperty("data")
    public T value() {
        return value;
    }

    /**
     * 失败时的错误；成功时为 {@code null}（为 null 时不会出现在 JSON 里）。
     */
    @JsonProperty("error")
    public FmtError error() {
        return error;
    }

    public T orElse(T fallback) {
        return error == null ? value : fallback;
    }

    /**
     * 异常风格出口：失败时抛 {@link FmtApiException}。
     * 适合「失败就是不该继续」的场景，让 Spring 的异常处理统一兜住。
     */
    public T orThrow() {
        if (error != null) {
            throw new FmtApiException(error);
        }
        return value;
    }

    public <R> FmtResult<R> map(Function<? super T, ? extends R> mapper) {
        if (error != null) {
            return FmtResult.failure(error);
        }
        return FmtResult.ok(mapper.apply(value));
    }

    @Override
    public String toString() {
        return error == null ? "FmtResult.ok(" + value + ")" : "FmtResult.failure(" + error + ")";
    }
}
