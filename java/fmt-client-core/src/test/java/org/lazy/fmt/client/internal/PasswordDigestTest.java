package org.lazy.fmt.client.internal;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;

/** PBKDF2 必须与 C++ 侧 `fmt::password_digest()` 逐字节一致。 */
class PasswordDigestTest {

    @Test
    void matchesTheStandardPbkdf2Sha256Vector() {
        // RFC 7914 / 常见 PBKDF2-HMAC-SHA256 向量：password / salt / 1 轮 / 32 字节
        assertEquals("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b",
                PasswordDigest.pbkdf2Sha256Hex("password", "73616c74", 1, 32));
    }

    @Test
    void matchesTheCppIterationCountAndUtf8Password() {
        // 与 C++ 相同的 100000 轮、32 字节；口令含中文（UTF-8 原始字节）。
        assertEquals("8311414396192b4a109f0f247c08680e8e06f7d0c8871ff7c0faa3582d7a18a0",
                PasswordDigest.pbkdf2Sha256Hex(
                        "密码", "00112233445566778899aabbccddeeff", 100_000, 32));
    }
}
