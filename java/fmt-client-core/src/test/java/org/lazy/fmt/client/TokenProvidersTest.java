package org.lazy.fmt.client;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class TokenProvidersTest {

    private static final String TOKEN_A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    private static final String TOKEN_B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    @Test
    void readsTokenFromFmtUserJson(@TempDir Path root) throws IOException {
        Path file = writeUserJson(root, TOKEN_A);
        assertEquals(TOKEN_A, TokenProviders.fromUserJson(file).token());
        // 数据根写法：<root>/data/user.json
        assertEquals(TOKEN_A, TokenProviders.fromDataRoot(root).token());
    }

    @Test
    void missingTokenFileExplainsWhatToDo(@TempDir Path root) {
        FmtConfigurationException failure = assertThrows(FmtConfigurationException.class,
                () -> TokenProviders.fromUserJson(root.resolve("data/user.json")).token());
        assertTrue(failure.getMessage().contains("fmt.exe config list"), failure.getMessage());
    }

    @Test
    void brokenTokenFileIsReportedClearly(@TempDir Path root) throws IOException {
        Path notJson = root.resolve("user.json");
        Files.writeString(notJson, "not json at all", StandardCharsets.UTF_8);
        assertThrows(FmtConfigurationException.class, () -> TokenProviders.fromUserJson(notJson).token());

        Path noToken = root.resolve("no-token.json");
        Files.writeString(noToken, "{\"user_id\":\"u1\"}", StandardCharsets.UTF_8);
        FmtConfigurationException failure = assertThrows(FmtConfigurationException.class,
                () -> TokenProviders.fromUserJson(noToken).token());
        assertTrue(failure.getMessage().contains("token 字段"), failure.getMessage());
    }

    @Test
    void readsTokenFromLegacyUsersArrayShape(@TempDir Path root) throws IOException {
        // 真机踩过：老部署的 data/user.json 还是 {"users":[{…}]}，客户端不能等它升级
        Path file = root.resolve("user.json");
        Files.writeString(file, "{\"version\":1,\"users\":[{\"user_id\":\"u-1\",\"username\":\"user\","
                + "\"token\":\"" + TOKEN_A + "\"}]}", StandardCharsets.UTF_8);
        assertEquals(TOKEN_A, TokenProviders.fromUserJson(file).token());
    }

    @Test
    void legacyShapeWithoutTokenStillFailsClearly(@TempDir Path root) throws IOException {
        Path file = root.resolve("user.json");
        Files.writeString(file, "{\"version\":1,\"users\":[]}", StandardCharsets.UTF_8);
        FmtConfigurationException failure = assertThrows(FmtConfigurationException.class,
                () -> TokenProviders.fromUserJson(file).token());
        assertTrue(failure.getMessage().contains("users"), failure.getMessage());
    }

    @Test
    void ttlCacheServesStaleValueUntilInvalidated(@TempDir Path root) throws IOException {
        Path file = writeUserJson(root, TOKEN_A);
        TokenProvider provider = TokenProviders.fromUserJson(file, Duration.ofMinutes(5));
        assertEquals(TOKEN_A, provider.token());

        Files.writeString(file, userJson(TOKEN_B), StandardCharsets.UTF_8);
        assertEquals(TOKEN_A, provider.token(), "TTL 内不该重新读文件");

        provider.invalidate();
        assertEquals(TOKEN_B, provider.token(), "invalidate 之后必须重新读文件");
    }

    @Test
    void zeroTtlAlwaysReadsTheFile(@TempDir Path root) throws IOException {
        Path file = writeUserJson(root, TOKEN_A);
        TokenProvider provider = TokenProviders.fromUserJson(file, Duration.ZERO);
        assertEquals(TOKEN_A, provider.token());
        Files.writeString(file, userJson(TOKEN_B), StandardCharsets.UTF_8);
        assertEquals(TOKEN_B, provider.token());
    }

    @Test
    void firstOfFallsThroughUnconfiguredSources() {
        TokenProvider provider = TokenProviders.firstOf(
                TokenProviders.failWith("没有配 fmt.token"),
                TokenProviders.of(TOKEN_B));
        assertEquals(TOKEN_B, provider.token());

        FmtConfigurationException failure = assertThrows(FmtConfigurationException.class,
                () -> TokenProviders.firstOf(
                        TokenProviders.failWith("没有配 fmt.token"),
                        TokenProviders.failWith("没有配 fmt.token-file")).token());
        assertTrue(failure.getMessage().contains("没有配 fmt.token-file"), failure.getMessage());
    }

    @Test
    void environmentProviderReadsSystemPropertyThenEnvironment() {
        String name = "FMT_TEST_TOKEN_" + System.nanoTime();
        System.setProperty(name, TOKEN_A);
        try {
            assertEquals(TOKEN_A, TokenProviders.fromEnvironment(name).token());
            assertThrows(FmtConfigurationException.class,
                    () -> TokenProviders.fromEnvironment("FMT_TEST_MISSING_" + System.nanoTime()).token());
        } finally {
            System.clearProperty(name);
        }
    }

    @Test
    void maskNeverLeaksTheMiddleOfAToken() {
        assertEquals("1a28…693f", TokenProviders.mask("1a2867378b112b8e7864218fd2fd693f"));
        assertEquals("(未设置)", TokenProviders.mask(""));
        assertEquals("(未设置)", TokenProviders.mask(null));
        assertEquals("***", TokenProviders.mask("short"));
    }

    private static Path writeUserJson(Path root, String token) throws IOException {
        Path file = root.resolve("data").resolve("user.json");
        Files.createDirectories(file.getParent());
        Files.writeString(file, userJson(token), StandardCharsets.UTF_8);
        return file;
    }

    private static String userJson(String token) {
        return "{\"version\":1,\"user_id\":\"u-1\",\"user_name\":\"user\",\"token\":\"" + token
                + "\",\"created_at\":\"2026-10-05 23:40:01\"}";
    }
}
