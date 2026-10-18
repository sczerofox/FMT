package org.lazy.fmt.client;

import org.lazy.fmt.client.internal.PasswordDigest;
import org.lazy.fmt.client.internal.SessionTokenProvider;
import org.lazy.fmt.client.json.SimpleJson;
import org.lazy.fmt.client.model.FileQuery;

import org.junit.jupiter.api.Test;

import java.net.URI;
import java.time.Duration;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * 账号密码登录：salt → PBKDF2 → 会话 token → 每个业务请求带库名。
 *
 * <p>跑真 HTTP（StubFmtServer），所以摘要格式、请求头与缓存行为都是实测。
 */
class SessionTokenProviderTest {

    private static final String SALT = "00112233445566778899aabbccddeeff";
    private static final String SESSION = "session-token-0123456789abcdef";
    private static final String LOGIN_OK =
            "{\"token\":\"" + SESSION + "\",\"token_type\":\"session\","
                    + "\"expires_at\":\"2099-01-01T00:00:00\",\"username\":\"user\","
                    + "\"bucket\":\"work\"}";

    @Test
    void logsInWithDigestAndCachesTheSessionToken() {
        try (StubFmtServer stub = StubFmtServer.start(SESSION)) {
            stub.on("GET", "/api/auth/salt",
                    request -> StubFmtServer.Reply.ok(
                            "{\"username\":\"user\",\"salt\":\"" + SALT + "\"}"));
            stub.on("POST", "/api/auth/login", request -> StubFmtServer.Reply.ok(LOGIN_OK));

            SessionTokenProvider provider = new SessionTokenProvider(
                    URI.create(stub.baseUrl()), "user", "password", "work",
                    SimpleJson.INSTANCE, FmtLoggers.NOOP,
                    Duration.ofSeconds(2), Duration.ofSeconds(2));

            assertEquals(SESSION, provider.token());
            assertEquals(SESSION, provider.token(), "第二次应命中缓存");
            assertEquals(2, stub.requestCount(), "salt + login，只各一次");

            StubFmtServer.Recorded login = stub.requests().get(1);
            assertTrue(login.bodyText().contains("\"bucket\":\"work\""), login.bodyText());
            assertTrue(login.bodyText().contains(PasswordDigest.pbkdf2Sha256Hex("password", SALT)),
                    "登录请求里应只有 PBKDF2 摘要，没有明文密码：" + login.bodyText());
            assertTrue(!login.bodyText().contains("\"password\":\"password\""),
                    "不能把明文密码发出去：" + login.bodyText());

            provider.invalidate();
            assertEquals(SESSION, provider.token());
            assertEquals(4, stub.requestCount(), "作废后应重新 salt + login");
        }
    }

    @Test
    void channelLogsInThenSendsBucketOnBusinessRequests() {
        try (StubFmtServer stub = StubFmtServer.start(SESSION);
             FmtChannel fmt = Fmt.login(stub.baseUrl(), "user", "password", "work")) {
            stub.on("GET", "/api/auth/salt",
                    request -> StubFmtServer.Reply.ok(
                            "{\"username\":\"user\",\"salt\":\"" + SALT + "\"}"));
            stub.on("POST", "/api/auth/login", request -> StubFmtServer.Reply.ok(LOGIN_OK));
            stub.on("GET", "/api/file", request -> StubFmtServer.Reply.ok(
                    "{\"files\":[],\"count\":0,\"total\":0,\"page\":1,\"page_size\":0,"
                            + "\"total_pages\":0,\"sort\":\"name\"}"));

            assertTrue(fmt.listFiles(FileQuery.all()).isOk());

            StubFmtServer.Recorded business = stub.lastRequest();
            assertEquals("/api/file", business.path());
            assertEquals(SESSION, business.header("X-FMT-Token"));
            assertEquals("work", business.header("X-FMT-Bucket"));
        }
    }

    @Test
    void badPasswordBecomesFmtApiException() {
        try (StubFmtServer stub = StubFmtServer.start(SESSION)) {
            stub.on("GET", "/api/auth/salt",
                    request -> StubFmtServer.Reply.ok(
                            "{\"username\":\"user\",\"salt\":\"" + SALT + "\"}"));
            stub.on("POST", "/api/auth/login", request ->
                    StubFmtServer.Reply.error(401, FmtErrorCodes.LOGIN_FAILED, "用户名或密码不正确"));

            SessionTokenProvider provider = new SessionTokenProvider(
                    URI.create(stub.baseUrl()), "user", "wrong", "work",
                    SimpleJson.INSTANCE, FmtLoggers.NOOP,
                    Duration.ofSeconds(2), Duration.ofSeconds(2));

            FmtApiException failure = assertThrows(FmtApiException.class, provider::token);
            assertEquals(FmtErrorCodes.LOGIN_FAILED, failure.error().code());
        }
    }

    @Test
    void loginModeRequiresBucket() {
        assertThrows(FmtConfigurationException.class,
                () -> Fmt.login("http://127.0.0.1:1", "user", "password", ""));
        assertThrows(FmtConfigurationException.class,
                () -> Fmt.channel(Fmt.builder().baseUrl("http://127.0.0.1:1")
                        .login("user", "password", " ").build()));
    }
}
