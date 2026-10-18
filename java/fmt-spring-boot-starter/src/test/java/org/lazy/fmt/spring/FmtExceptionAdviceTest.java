package org.lazy.fmt.spring;

import org.lazy.fmt.client.FmtApiException;
import org.lazy.fmt.client.FmtConfigurationException;
import org.lazy.fmt.client.FmtError;
import org.lazy.fmt.client.FmtErrorCodes;
import org.lazy.fmt.client.FmtJsonException;
import org.lazy.fmt.client.FmtTransportException;

import org.junit.jupiter.api.Test;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;

import java.util.Map;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;

class FmtExceptionAdviceTest {

    private final FmtExceptionAdvice advice = new FmtExceptionAdvice();

    @Test
    void businessErrorKeepsTheServerHttpStatus() {
        ResponseEntity<Map<String, Object>> response = advice.onFmtError(
                new FmtApiException(new FmtError(FmtErrorCodes.FILE_NOT_FOUND, "文件不存在", 404)));

        assertEquals(HttpStatus.NOT_FOUND, response.getStatusCode());
        Map<String, Object> error = errorOf(response);
        assertEquals(FmtErrorCodes.FILE_NOT_FOUND, error.get("code"));
        assertEquals("文件不存在", error.get("message"));
        assertEquals(404, error.get("fmtHttpStatus"));
    }

    @Test
    void localErrorFallsBackToTheCodeMapping() {
        // 本地发现（httpStatus=0）：FMT-001 是参数错 → 400，而不是默认 502
        ResponseEntity<Map<String, Object>> response = advice.onFmtError(
                new FmtApiException(FmtError.local(FmtErrorCodes.INVALID_ARGUMENT, "上传的文件是空的")));

        assertEquals(HttpStatus.BAD_REQUEST, response.getStatusCode());
        Map<String, Object> error = errorOf(response);
        assertEquals(FmtErrorCodes.INVALID_ARGUMENT, error.get("code"));
        assertFalse(error.containsKey("fmtHttpStatus"));
    }

    @Test
    void unauthorizedIsReportedAsBadGateway() {
        // FMT-018 是桥接自己没带对 token，不是调用方要重新登录
        ResponseEntity<Map<String, Object>> response = advice.onFmtError(
                new FmtApiException(new FmtError(FmtErrorCodes.UNAUTHORIZED, "token 不对", 401)));

        assertEquals(HttpStatus.BAD_GATEWAY, response.getStatusCode());
    }

    @Test
    void confirmRequiredIsConflict() {
        assertEquals(HttpStatus.CONFLICT,
                FmtExceptionAdvice.statusFor(FmtError.local(FmtErrorCodes.CONFIRM_REQUIRED, "要确认")));
    }

    @Test
    void loginFailureIsUnauthorized() {
        ResponseEntity<Map<String, Object>> response = advice.onFmtError(
                new FmtApiException(new FmtError(FmtErrorCodes.LOGIN_FAILED, "用户名或密码不正确", 401)));
        assertEquals(HttpStatus.UNAUTHORIZED, response.getStatusCode());
        assertEquals(FmtErrorCodes.LOGIN_FAILED, errorOf(response).get("code"));
    }

    @Test
    void transportFailureIsBadGateway() {
        ResponseEntity<Map<String, Object>> response =
                advice.onTransport(new FmtTransportException("连不上 FMT"));

        assertEquals(HttpStatus.BAD_GATEWAY, response.getStatusCode());
        assertEquals(FmtErrorCodes.IO_ERROR, errorOf(response).get("code"));
    }

    @Test
    void configurationFailureIsInternalServerError() {
        ResponseEntity<Map<String, Object>> response =
                advice.onConfiguration(new FmtConfigurationException("没有可用的 FMT 访问 token"));

        assertEquals(HttpStatus.INTERNAL_SERVER_ERROR, response.getStatusCode());
        assertEquals(FmtErrorCodes.CONFIG_ERROR, errorOf(response).get("code"));
    }

    @Test
    void otherFmtExceptionsStillGetAnEnvelope() {
        ResponseEntity<Map<String, Object>> response =
                advice.onOtherFmtError(new FmtJsonException("请求体序列化失败"));

        assertEquals(HttpStatus.BAD_GATEWAY, response.getStatusCode());
        assertEquals(FmtErrorCodes.JSON_PARSE_ERROR, errorOf(response).get("code"));
    }

    @SuppressWarnings("unchecked")
    private static Map<String, Object> errorOf(ResponseEntity<Map<String, Object>> response) {
        Map<String, Object> body = response.getBody();
        assertNotNull(body);
        assertEquals(false, body.get("ok"));
        Map<String, Object> error = (Map<String, Object>) body.get("error");
        assertNotNull(error, "响应里必须有 error 节点：" + body);
        return error;
    }
}
