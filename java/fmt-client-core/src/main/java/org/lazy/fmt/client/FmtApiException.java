package org.lazy.fmt.client;

/**
 * 由 {@link FmtResult#orThrow()} 抛出的业务错误异常。
 *
 * <p>只在调用方明确要求「异常风格」时出现；SDK 内部永远优先返回 {@link FmtResult}。
 */
public class FmtApiException extends FmtException {

    private static final long serialVersionUID = 1L;

    public FmtApiException(FmtError error) {
        super(error, error == null ? null : error.toString(), null);
    }
}
