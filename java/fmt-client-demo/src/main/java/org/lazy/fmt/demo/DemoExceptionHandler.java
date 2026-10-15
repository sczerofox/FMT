package org.lazy.fmt.demo;

import org.lazy.fmt.client.FmtApiException;
import org.lazy.fmt.client.FmtConfigurationException;
import org.lazy.fmt.client.FmtErrorCodes;
import org.lazy.fmt.client.FmtTransportException;

import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.RestControllerAdvice;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * 把 FMT 的错误翻译成 HTTP 响应，并且**沿用 FMT 的信封形状**，
 * 这样调桥接 jar 的人看到的错误结构跟他直接调 FMT 时是一样的：
 * {@code {"ok":false,"error":{"code":"FMT-002","message":"…"}}}。
 */
@RestControllerAdvice
public class DemoExceptionHandler {

    @ExceptionHandler(FmtApiException.class)
    public ResponseEntity<Map<String, Object>> handleApi(FmtApiException failure) {
        String code = failure.error().code();
        return ResponseEntity.status(statusFor(code)).body(envelope(code, failure.error().message(),
                failure.error().httpStatus()));
    }

    @ExceptionHandler(FmtTransportException.class)
    public ResponseEntity<Map<String, Object>> handleTransport(FmtTransportException failure) {
        // 连不上 FMT / 响应不是信封：这是网关侧问题，502 比 500 更准确
        return ResponseEntity.status(HttpStatus.BAD_GATEWAY)
                .body(envelope(failure.error().code(), failure.getMessage(), 0));
    }

    @ExceptionHandler(FmtConfigurationException.class)
    public ResponseEntity<Map<String, Object>> handleConfiguration(FmtConfigurationException failure) {
        return ResponseEntity.status(HttpStatus.INTERNAL_SERVER_ERROR)
                .body(envelope(FmtErrorCodes.CONFIG_ERROR, failure.getMessage(), 0));
    }

    @ExceptionHandler(IllegalArgumentException.class)
    public ResponseEntity<Map<String, Object>> handleBadRequest(IllegalArgumentException failure) {
        return ResponseEntity.badRequest()
                .body(envelope(FmtErrorCodes.INVALID_ARGUMENT, failure.getMessage(), 400));
    }

    private static Map<String, Object> envelope(String code, String message, int upstreamStatus) {
        Map<String, Object> error = new LinkedHashMap<>();
        error.put("code", code);
        error.put("message", message);
        if (upstreamStatus > 0) {
            error.put("fmtHttpStatus", upstreamStatus);
        }
        Map<String, Object> body = new LinkedHashMap<>();
        body.put("ok", false);
        body.put("error", error);
        return body;
    }

    /**
     * FMT 错误码 → 桥接服务的 HTTP 状态码。这是**桥接自己的策略**（FMT 的状态码是它俩之间的事），
     * 按「调用方该做什么」来分：参数错就 400、找不到就 404、要确认就 409、凭证不对是我们的问题就 502。
     */
    private static HttpStatus statusFor(String code) {
        return switch (FmtErrorCodes.number(code)) {
            case 1, 12, 14, 100, 101, 102, 103, 104, 106, 202, 300, 303, 700, 701 -> HttpStatus.BAD_REQUEST;
            case 2, 17, 200, 305, 400, 402, 500 -> HttpStatus.NOT_FOUND;
            case 3, 16, 105, 201, 203, 304, 401 -> HttpStatus.CONFLICT;
            case 4, 501, 502, 503, 603 -> HttpStatus.FORBIDDEN;
            case 6, 11, 15 -> HttpStatus.CONFLICT;
            case 8, 604 -> HttpStatus.INTERNAL_SERVER_ERROR;
            case 18 -> HttpStatus.BAD_GATEWAY; // 桥接自己没带对 token：不是调用方的错
            default -> HttpStatus.BAD_GATEWAY;
        };
    }
}
