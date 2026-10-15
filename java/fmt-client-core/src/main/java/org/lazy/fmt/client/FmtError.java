package org.lazy.fmt.client;

/**
 * 一次 FMT 业务错误的描述：错误码 + 中文消息 + HTTP 状态码。
 *
 * <p>对应服务端 {@code {"ok":false,"error":{"code":"FMT-0xx","message":"…"}}} 里的 error 节点。
 *
 * @param code       形如 {@code FMT-018}
 * @param message    可修改的自然语言消息（服务端会随着版本改措辞，不要拿它做判断）
 * @param httpStatus 服务端返回的 HTTP 状态码；客户端本地发现的错误为 0
 */
public record FmtError(String code, String message, int httpStatus) {

    public FmtError {
        if (code == null || code.isBlank()) {
            code = FmtErrorCodes.INVALID_ARGUMENT;
        }
        if (message == null) {
            message = "";
        }
    }

    /** 本地（非服务端）发现的错误：例如目标文件已存在、下载被截断。 */
    public static FmtError local(String code, String message) {
        return new FmtError(code, message, 0);
    }

    /** 错误码编号，例如 {@code FMT-018} -> 18。 */
    public int number() {
        return FmtErrorCodes.number(code);
    }

    /** 与 C++ 侧对齐的退出码。 */
    public int exitCode() {
        return FmtErrorCodes.exitCode(code);
    }

    public boolean is(String... candidates) {
        for (String candidate : candidates) {
            if (code.equalsIgnoreCase(candidate)) {
                return true;
            }
        }
        return false;
    }

    public boolean isUnauthorized() {
        return FmtErrorCodes.isUnauthorized(code);
    }

    public boolean isConfirmRequired() {
        return FmtErrorCodes.isConfirmRequired(code);
    }

    @Override
    public String toString() {
        return httpStatus > 0
                ? code + "(" + FmtErrorCodes.name(number()) + ", HTTP " + httpStatus + "): " + message
                : code + "(" + FmtErrorCodes.name(number()) + "): " + message;
    }
}
