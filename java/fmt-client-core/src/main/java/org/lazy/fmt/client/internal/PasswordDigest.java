package org.lazy.fmt.client.internal;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

import java.nio.charset.StandardCharsets;
import java.util.HexFormat;

/**
 * 与 FMT C++ 侧完全一致的 PBKDF2-HMAC-SHA256：32 字节输出、100000 轮、
 * 盐是十六进制字符串解码后的原始字节。
 *
 * <p>登录时明文密码只在这里出现，算完摘要就丢弃；网络上只有 64 位十六进制摘要。
 * 算法必须与服务端 `fmt::password_digest()` 对齐，否则老账号的密码会验不过。
 */
public final class PasswordDigest {

    /** C++ 侧 kDefaultUserIterations。 */
    public static final int DEFAULT_ITERATIONS = 100_000;
    public static final int DEFAULT_LENGTH = 32;

    private PasswordDigest() {
    }

    /** 口令 + 十六进制盐 → 64 位十六进制摘要。 */
    public static String pbkdf2Sha256Hex(String password, String saltHex) {
        return pbkdf2Sha256Hex(password, saltHex, DEFAULT_ITERATIONS, DEFAULT_LENGTH);
    }

    public static String pbkdf2Sha256Hex(String password, String saltHex, int iterations, int length) {
        if (saltHex == null || saltHex.isBlank()) {
            throw new IllegalArgumentException("盐为空");
        }
        byte[] salt;
        try {
            salt = HexFormat.of().parseHex(saltHex.trim());
        } catch (IllegalArgumentException broken) {
            throw new IllegalArgumentException("盐不是合法的十六进制：" + saltHex, broken);
        }
        return HexFormat.of().formatHex(pbkdf2(password, salt, iterations, length));
    }

    static byte[] pbkdf2(String password, byte[] salt, int iterations, int length) {
        if (iterations <= 0 || length <= 0) {
            throw new IllegalArgumentException("iterations/length 必须为正数");
        }
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            // C++ 侧把口令的原始 UTF-8 字节交给 BCryptDeriveKeyPBKDF2；这里保持一致。
            mac.init(new SecretKeySpec(
                    (password == null ? "" : password).getBytes(StandardCharsets.UTF_8),
                    "HmacSHA256"));

            int blocks = (length + 31) / 32;
            byte[] output = new byte[length];
            for (int block = 1; block <= blocks; block++) {
                mac.update(salt);
                mac.update(new byte[]{
                        (byte) (block >>> 24), (byte) (block >>> 16),
                        (byte) (block >>> 8), (byte) block});
                byte[] u = mac.doFinal();
                byte[] t = u.clone();
                for (int round = 1; round < iterations; round++) {
                    u = mac.doFinal(u);
                    for (int i = 0; i < t.length; i++) {
                        t[i] ^= u[i];
                    }
                }
                int offset = (block - 1) * 32;
                System.arraycopy(t, 0, output, offset, Math.min(32, length - offset));
            }
            return output;
        } catch (java.security.GeneralSecurityException broken) {
            throw new IllegalStateException("当前 JDK 不支持 HmacSHA256", broken);
        }
    }
}
