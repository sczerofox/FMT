package org.lazy.fmt.client;

/**
 * 传输层错误：连不上 FMT、超时、连接被断开、响应不是约定的 JSON 信封。
 *
 * <p>这类错误**不是** FMT 业务结论，重试或修配置之前不会靠重发解决。
 */
public class FmtTransportException extends FmtException {

    private static final long serialVersionUID = 1L;

    private final int httpStatus;

    public FmtTransportException(String message) {
        this(message, 0, null);
    }

    public FmtTransportException(String message, Throwable cause) {
        this(message, 0, cause);
    }

    public FmtTransportException(String message, int httpStatus) {
        this(message, httpStatus, null);
    }

    public FmtTransportException(String message, int httpStatus, Throwable cause) {
        super(new FmtError(statusToCode(httpStatus), message, httpStatus), message, cause);
        this.httpStatus = httpStatus;
    }

    public int httpStatus() {
        return httpStatus;
    }

    private static String statusToCode(int status) {
        return switch (status) {
            case 401 -> FmtErrorCodes.UNAUTHORIZED;
            case 404 -> FmtErrorCodes.ROUTE_NOT_FOUND;
            case 501 -> FmtErrorCodes.SERVICE_OPERATION_FAILED;
            case 0 -> FmtErrorCodes.IO_ERROR;
            default -> FmtErrorCodes.HTTP_REQUEST_INVALID;
        };
    }
}
