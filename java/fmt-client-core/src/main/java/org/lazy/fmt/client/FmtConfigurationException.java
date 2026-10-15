package org.lazy.fmt.client;

/**
 * 配置错误：token 没给、baseUrl 非法、token 文件不存在或格式不对。
 *
 * <p>这类错误在**建立通道**或第一次取凭证时立刻抛出，不拖到发请求时才发现。
 */
public class FmtConfigurationException extends FmtException {

    private static final long serialVersionUID = 1L;

    public FmtConfigurationException(String message) {
        super(new FmtError(FmtErrorCodes.CONFIG_ERROR, message, 0), message, null);
    }

    public FmtConfigurationException(String message, Throwable cause) {
        super(new FmtError(FmtErrorCodes.CONFIG_ERROR, message, 0), message, cause);
    }
}
