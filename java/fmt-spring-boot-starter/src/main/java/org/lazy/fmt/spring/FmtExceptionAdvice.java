package org.lazy.fmt.spring;

import org.lazy.fmt.client.FmtApiException;
import org.lazy.fmt.client.FmtConfigurationException;
import org.lazy.fmt.client.FmtError;
import org.lazy.fmt.client.FmtErrorCodes;
import org.lazy.fmt.client.FmtException;
import org.lazy.fmt.client.FmtTransportException;
import org.springframework.core.Ordered;
import org.springframework.core.annotation.Order;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.RestControllerAdvice;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * 把 FMT 的错误翻成 JSON，HTTP 状态码优先沿用 FMT 给的那个。
 *
 * <p>由 {@link FmtAutoConfiguration} 在 Servlet Web 应用里自动注册，宿主不需要再复制这个类，
 * 也不需要自己写 {@code @ControllerAdvice}。
 *
 * <p>只处理 {@link FmtException} 及其子类，<b>不接管</b>
 * {@link IllegalArgumentException} 之类的宿主异常——starter 是公共库，不能因为接了 FMT
 * 就把无关控制器的异常语义一起改掉。空上传这类参数错误请在业务侧显式抛
 * {@link FmtApiException}（带 {@code FMT-001}），本类会把它翻成 400。
 *
 * <p>注册条件：Servlet Web 应用、classpath 上有 Spring Web、且宿主没有同类型的 bean；
 * 设置 {@code fmt.exception-advice=false} 可整体关闭。自己声明一个
 * {@code FmtExceptionAdvice} bean 也会覆盖默认实现。
 */
@RestControllerAdvice
@Order(Ordered.LOWEST_PRECEDENCE)
public class FmtExceptionAdvice {

    /** 业务错误（信封 ok=false）：FMT-002 文件不存在、FMT-016 需要确认…… */
    @ExceptionHandler(FmtApiException.class)
    public ResponseEntity<Map<String, Object>> onFmtError(FmtApiException failure) {
        return response(failure.error());
    }

    /** 连不上 / 超时 / 响应不是 FMT 信封：网关侧问题，502。 */
    @ExceptionHandler(FmtTransportException.class)
    public ResponseEntity<Map<String, Object>> onTransport(FmtTransportException failure) {
        return ResponseEntity.status(HttpStatus.BAD_GATEWAY)
                .body(envelope(failure.error().code(), failure.getMessage(), 0));
    }

    /** token 没配 / baseUrl 非法：这是我们自己的配置问题。 */
    @ExceptionHandler(FmtConfigurationException.class)
    public ResponseEntity<Map<String, Object>> onConfiguration(FmtConfigurationException failure) {
        return ResponseEntity.status(HttpStatus.INTERNAL_SERVER_ERROR)
                .body(envelope(FmtErrorCodes.CONFIG_ERROR, failure.getMessage(), 0));
    }

    /**
     * SDK 里其余 {@link FmtException} 子类的兜底（例如请求体序列化失败）。
     *
     * <p>Spring 会优先匹配上面三个更具体的 handler，这里是最后一道网，避免漏成默认 500。
     */
    @ExceptionHandler(FmtException.class)
    public ResponseEntity<Map<String, Object>> onOtherFmtError(FmtException failure) {
        return response(failure.error());
    }

    private static ResponseEntity<Map<String, Object>> response(FmtError error) {
        return ResponseEntity.status(statusFor(error))
                .body(envelope(error.code(), error.message(), error.httpStatus()));
    }

    /**
     * FMT 错误 → 桥接服务的 HTTP 状态码。
     *
     * <p>优先用服务端返回的状态码；服务端没给（客户端本地发现的错误，{@code httpStatus == 0}）
     * 时按错误码补一个。FMT-018 是「桥接自己没带对 token」，不是调用方要登录 → 502。
     */
    static HttpStatus statusFor(FmtError error) {
        if (FmtErrorCodes.isUnauthorized(error.code())) {
            return HttpStatus.BAD_GATEWAY;
        }
        if (error.httpStatus() > 0) {
            HttpStatus fromServer = HttpStatus.resolve(error.httpStatus());
            if (fromServer != null) {
                return fromServer;
            }
        }
        return switch (FmtErrorCodes.number(error.code())) {
            case 1, 12, 14, 100, 101, 102, 103, 104, 106, 202, 300, 303, 700, 701 -> HttpStatus.BAD_REQUEST;
            case 2, 17, 200, 305, 400, 402, 500 -> HttpStatus.NOT_FOUND;
            case 3, 16, 105, 201, 203, 304, 401, 403 -> HttpStatus.CONFLICT;
            case 4, 501, 502, 503, 603 -> HttpStatus.FORBIDDEN;
            case 8, 604 -> HttpStatus.INTERNAL_SERVER_ERROR;
            default -> HttpStatus.BAD_GATEWAY;
        };
    }

    private static Map<String, Object> envelope(String code, String message, int fmtStatus) {
        Map<String, Object> error = new LinkedHashMap<>();
        error.put("code", code);
        error.put("message", message);
        if (fmtStatus > 0) {
            error.put("fmtHttpStatus", fmtStatus);
        }
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("ok", false);
        body.put("error", error);
        return body;
    }
}
