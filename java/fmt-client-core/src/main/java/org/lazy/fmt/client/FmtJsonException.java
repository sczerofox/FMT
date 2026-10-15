package org.lazy.fmt.client;

/** JSON 读写错误（内部编解码器抛出）。 */
public class FmtJsonException extends FmtException {

    private static final long serialVersionUID = 1L;

    public FmtJsonException(String message) {
        super(new FmtError(FmtErrorCodes.JSON_PARSE_ERROR, message, 0), message, null);
    }

    public FmtJsonException(String message, Throwable cause) {
        super(new FmtError(FmtErrorCodes.JSON_PARSE_ERROR, message, 0), message, cause);
    }
}
