package org.lazy.fmt.client;

/**
 * 日志出口。核心包**刻意不依赖 slf4j**：默认是 {@link FmtLoggers#NOOP}，
 * Spring Boot 环境下由 starter 桥接到 slf4j。
 *
 * <p>约定：SDK **永远不打印 token 明文**（只打印掩码）。实现方也不要自己去猜 token 值。
 */
public interface FmtLogger {

    void debug(String message);

    void info(String message);

    void warn(String message);

    void error(String message, Throwable cause);
}
