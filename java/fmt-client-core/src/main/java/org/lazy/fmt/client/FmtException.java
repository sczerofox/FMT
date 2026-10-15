package org.lazy.fmt.client;

/**
 * SDK 异常基类。
 *
 * <p>错误分两类，边界在 {@link FmtChannel} 的文档里写死了：
 * <ul>
 *   <li><b>业务错误</b>（服务端信封 {@code ok=false}）—— 不抛异常，作为
 *       {@link FmtResult#error()} 返回；调用方{@link FmtResult#orThrow() 显式要求}时才抛
 *       {@link FmtApiException}。</li>
 *   <li><b>传输/协议/配置错误</b>（连不上、响应不是 JSON、token 没配）—— 直接抛
 *       {@link FmtTransportException} / {@link FmtConfigurationException}。</li>
 * </ul>
 */
public class FmtException extends RuntimeException {

    private static final long serialVersionUID = 1L;

    private final FmtError error;

    public FmtException(FmtError error) {
        this(error, error == null ? null : error.message(), null);
    }

    public FmtException(FmtError error, String message, Throwable cause) {
        super(message, cause);
        this.error = error == null ? FmtError.local(FmtErrorCodes.IO_ERROR, message) : error;
    }

    public FmtException(String message) {
        this(FmtError.local(FmtErrorCodes.IO_ERROR, message), message, null);
    }

    public FmtException(String message, Throwable cause) {
        this(FmtError.local(FmtErrorCodes.IO_ERROR, message), message, cause);
    }

    /** 关联的 FMT 错误（总有值，便于统一上报）。 */
    public FmtError error() {
        return error;
    }
}
