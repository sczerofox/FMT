package org.lazy.fmt.spring;

import org.lazy.fmt.client.FmtLogger;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 * 把通道的日志出口桥到 slf4j。logger 名固定为 {@code org.lazy.fmt.client}，
 * 想安静就调这个 logger 的级别即可。
 *
 * <p>通道只会打印地址、方法与状态码；**token 永远以掩码形式出现**。
 */
public class Slf4jFmtLogger implements FmtLogger {

    private static final Logger LOG = LoggerFactory.getLogger("org.lazy.fmt.client");

    @Override
    public void debug(String message) {
        LOG.debug(message);
    }

    @Override
    public void info(String message) {
        LOG.info(message);
    }

    @Override
    public void warn(String message) {
        LOG.warn(message);
    }

    @Override
    public void error(String message, Throwable cause) {
        if (cause == null) {
            LOG.error(message);
        } else {
            LOG.error(message, cause);
        }
    }
}
