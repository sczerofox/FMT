package org.lazy.fmt.client;

/** {@link FmtLogger} 的现成实现。 */
public final class FmtLoggers {

    /** 什么都不做——核心包的默认值，避免给宿主塞日志依赖。 */
    public static final FmtLogger NOOP = new FmtLogger() {
        @Override
        public void debug(String message) {
        }

        @Override
        public void info(String message) {
        }

        @Override
        public void warn(String message) {
        }

        @Override
        public void error(String message, Throwable cause) {
        }
    };

    /** 写到标准错误的极简实现，适合没有日志框架的小工具。 */
    public static FmtLogger stderr() {
        return new FmtLogger() {
            @Override
            public void debug(String message) {
                System.err.println("[FMT][DEBUG] " + message);
            }

            @Override
            public void info(String message) {
                System.err.println("[FMT][INFO ] " + message);
            }

            @Override
            public void warn(String message) {
                System.err.println("[FMT][WARN ] " + message);
            }

            @Override
            public void error(String message, Throwable cause) {
                System.err.println("[FMT][ERROR] " + message
                        + (cause == null ? "" : " <- " + cause));
            }
        };
    }

    private FmtLoggers() {
    }
}
