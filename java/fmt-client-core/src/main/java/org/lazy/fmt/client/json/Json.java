package org.lazy.fmt.client.json;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * 解析产物（{@code Map/List/String/Number/Boolean/null}）的读取助手。
 *
 * <p>路径写法是「点号 + 可选数字下标」：{@code "error.code"}、{@code "files.0.file_name"}。
 * 任何一段缺失都返回给定的默认值，绝不抛 {@code NullPointerException}——
 * 远端多加一个字段或少一个字段，不应该把调用方炸掉。
 */
public final class Json {

    private Json() {
    }

    /** 按路径取值；不存在返回 {@code null}。 */
    public static Object path(Object root, String dottedPath) {
        if (root == null || dottedPath == null || dottedPath.isEmpty()) {
            return root;
        }
        Object current = root;
        for (String segment : dottedPath.split("\\.")) {
            if (current == null) {
                return null;
            }
            if (current instanceof Map<?, ?> map) {
                current = map.get(segment);
            } else if (current instanceof List<?> list) {
                int i = parseIndex(segment);
                current = (i >= 0 && i < list.size()) ? list.get(i) : null;
            } else {
                return null;
            }
        }
        return current;
    }

    public static Map<String, Object> map(Object root, String dottedPath) {
        return castMap(path(root, dottedPath));
    }

    public static List<Object> list(Object root, String dottedPath) {
        Object value = path(root, dottedPath);
        return value instanceof List<?> items ? new ArrayList<>(items) : List.of();
    }

    /** 取「对象数组」：非对象元素直接跳过，而不是让整次解析失败。 */
    public static List<Map<String, Object>> maps(Object root, String dottedPath) {
        List<Map<String, Object>> result = new ArrayList<>();
        for (Object item : list(root, dottedPath)) {
            Map<String, Object> map = castMap(item);
            if (map != null) {
                result.add(map);
            }
        }
        return result;
    }

    public static String str(Object root, String dottedPath) {
        return str(root, dottedPath, "");
    }

    public static String str(Object root, String dottedPath, String fallback) {
        Object value = path(root, dottedPath);
        if (value == null) {
            return fallback;
        }
        return value instanceof String text ? text : String.valueOf(value);
    }

    public static long lng(Object root, String dottedPath, long fallback) {
        Object value = path(root, dottedPath);
        if (value instanceof Number number) {
            return number.longValue();
        }
        if (value instanceof String text) {
            try {
                return Long.parseLong(text.trim());
            } catch (NumberFormatException ignored) {
                return fallback;
            }
        }
        return fallback;
    }

    public static int integer(Object root, String dottedPath, int fallback) {
        long value = lng(root, dottedPath, fallback);
        return value > Integer.MAX_VALUE || value < Integer.MIN_VALUE ? fallback : (int) value;
    }

    public static boolean bool(Object root, String dottedPath, boolean fallback) {
        Boolean value = boolOrNull(root, dottedPath);
        return value == null ? fallback : value;
    }

    /** 「服务器如实报告」的布尔：字段缺失时返回 {@code null}，而不是硬塞一个 false。 */
    public static Boolean boolOrNull(Object root, String dottedPath) {
        Object value = path(root, dottedPath);
        if (value instanceof Boolean bool) {
            return bool;
        }
        if (value instanceof String text) {
            if ("true".equalsIgnoreCase(text)) {
                return Boolean.TRUE;
            }
            if ("false".equalsIgnoreCase(text)) {
                return Boolean.FALSE;
            }
        }
        return null;
    }

    /** 缺失或不是对象时返回 {@code null}。 */
    @SuppressWarnings("unchecked")
    public static Map<String, Object> castMap(Object value) {
        return value instanceof Map<?, ?> map ? (Map<String, Object>) map : null;
    }

    private static int parseIndex(String segment) {
        try {
            return Integer.parseInt(segment);
        } catch (NumberFormatException ignored) {
            return -1;
        }
    }
}
