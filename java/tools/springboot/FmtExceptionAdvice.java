package com.hncgy.mybatis.exception;

import org.lazy.fmt.client.FmtApiException;
import org.lazy.fmt.client.FmtConfigurationException;
import org.lazy.fmt.client.FmtError;
import org.lazy.fmt.client.FmtErrorCodes;
import org.lazy.fmt.client.FmtTransportException;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.RestControllerAdvice;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * 把 FMT 的错误翻成 JSON，HTTP 状态码沿用 FMT 给的那个。
 *
 * <p>没有这个兜底，{@code fmtService.xxx().orThrow()} 抛出的异常会被 Spring 变成默认的 500
 * 错误页（{@code {"timestamp":…,"status":500,"error":"Internal Server Error"}}），
 * 调用方拿不到 {@code FMT-002} 这类错误码。
 */
@RestControllerAdvice
public class FmtExceptionAdvice {

    /** 业务错误（信封 ok=false）：FMT-002 文件不存在、FMT-016 需要确认…… */
    @ExceptionHandler(FmtApiException.class)
    public ResponseEntity<Map<String, Object>> onFmtError(FmtApiException failure) {
        FmtError error = failure.error();
        // FMT-018 是「桥接自己没带对 token」，不是调用方要登录 → 报 502 更准确
        HttpStatus status = FmtErrorCodes.isUnauthorized(error.code())
                ? HttpStatus.BAD_GATEWAY
                : (error.httpStatus() > 0
                        ? HttpStatus.valueOf(error.httpStatus())
                        : HttpStatus.BAD_GATEWAY);
        return ResponseEntity.status(status)
                .body(envelope(error.code(), error.message(), error.httpStatus()));
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

    /** 参数错（例如上传了空文件）。 */
    @ExceptionHandler(IllegalArgumentException.class)
    public ResponseEntity<Map<String, Object>> onBadRequest(IllegalArgumentException failure) {
        return ResponseEntity.badRequest()
                .body(envelope(FmtErrorCodes.INVALID_ARGUMENT, failure.getMessage(), 400));
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
