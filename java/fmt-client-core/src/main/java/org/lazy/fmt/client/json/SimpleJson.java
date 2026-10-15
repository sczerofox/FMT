package org.lazy.fmt.client.json;

import org.lazy.fmt.client.FmtCodec;
import org.lazy.fmt.client.FmtJsonException;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * 零依赖的 JSON 编解码器：递归下降解析 + 手写序列化。
 *
 * <p>为什么不用 Jackson/Gson：核心包要做到「丢进任何 Java 17 工程都不打架」，
 * 而 SDK 真正需要处理的 JSON 只有两类——FMT 的固定信封，以及我们自己拼的小请求体。
 * 需要更强的能力时，换成宿主的 {@code ObjectMapper}（starter 会自动这么做）。
 *
 * <p>解析产物：{@code Map<String,Object>} / {@code List<Object>} / {@code String} /
 * {@code Boolean} / {@code Long} / {@code Double} / {@code null}。
 * 不保证对象键顺序之外的东西，也不做「未知字段丢弃」以外的任何映射。
 */
public final class SimpleJson implements FmtCodec {

    public static final SimpleJson INSTANCE = new SimpleJson();

    /** 嵌套上限：防止畸形输入把解析器打到 StackOverflowError。 */
    private static final int MAX_DEPTH = 200;

    @Override
    public Object read(String json) {
        if (json == null) {
            throw new FmtJsonException("JSON 文本为 null");
        }
        return new Parser(json).parseDocument();
    }

    @Override
    public String write(Object value) {
        StringBuilder out = new StringBuilder(128);
        writeValue(out, value, 0);
        return out.toString();
    }

    // ------------------------------------------------------------------
    // 序列化
    // ------------------------------------------------------------------

    private void writeValue(StringBuilder out, Object value, int depth) {
        if (depth > MAX_DEPTH) {
            throw new FmtJsonException("JSON 嵌套超过 " + MAX_DEPTH + " 层");
        }
        if (value == null) {
            out.append("null");
            return;
        }
        if (value instanceof CharSequence || value instanceof Character || value instanceof Enum<?>) {
            writeString(out, String.valueOf(value));
            return;
        }
        if (value instanceof Boolean bool) {
            out.append(bool.booleanValue());
            return;
        }
        if (value instanceof Number number) {
            writeNumber(out, number);
            return;
        }
        if (value instanceof Map<?, ?> map) {
            out.append('{');
            boolean first = true;
            for (Map.Entry<?, ?> entry : map.entrySet()) {
                if (!first) {
                    out.append(',');
                }
                first = false;
                writeString(out, String.valueOf(entry.getKey()));
                out.append(':');
                writeValue(out, entry.getValue(), depth + 1);
            }
            out.append('}');
            return;
        }
        if (value instanceof Iterable<?> iterable) {
            out.append('[');
            boolean first = true;
            for (Object item : iterable) {
                if (!first) {
                    out.append(',');
                }
                first = false;
                writeValue(out, item, depth + 1);
            }
            out.append(']');
            return;
        }
        if (value instanceof Object[] array) {
            out.append('[');
            for (int i = 0; i < array.length; i++) {
                if (i > 0) {
                    out.append(',');
                }
                writeValue(out, array[i], depth + 1);
            }
            out.append(']');
            return;
        }
        // 兜底：当成字符串写出去，宁可多一层引号，也不要生成非法 JSON。
        writeString(out, String.valueOf(value));
    }

    private void writeNumber(StringBuilder out, Number number) {
        if (number instanceof Double d && (d.isNaN() || d.isInfinite())) {
            throw new FmtJsonException("NaN / Infinity 不能写成 JSON：" + number);
        }
        if (number instanceof Float f && (f.isNaN() || f.isInfinite())) {
            throw new FmtJsonException("NaN / Infinity 不能写成 JSON：" + number);
        }
        out.append(number);
    }

    private void writeString(StringBuilder out, String text) {
        out.append('"');
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            switch (c) {
                case '"' -> out.append("\\\"");
                case '\\' -> out.append("\\\\");
                case '\n' -> out.append("\\n");
                case '\r' -> out.append("\\r");
                case '\t' -> out.append("\\t");
                case '\b' -> out.append("\\b");
                case '\f' -> out.append("\\f");
                default -> {
                    if (c < 0x20) {
                        out.append(String.format("\\u%04x", (int) c));
                    } else {
                        out.append(c); // 非 ASCII 原样输出：整体按 UTF-8 编码
                    }
                }
            }
        }
        out.append('"');
    }

    // ------------------------------------------------------------------
    // 解析
    // ------------------------------------------------------------------

    private static final class Parser {

        private final String text;
        private int index;

        Parser(String text) {
            this.text = text;
        }

        Object parseDocument() {
            skipWhitespace();
            if (index >= text.length()) {
                throw fail("JSON 是空的");
            }
            Object value = parseValue(0);
            skipWhitespace();
            if (index < text.length()) {
                throw fail("JSON 末尾有多余字符");
            }
            return value;
        }

        private Object parseValue(int depth) {
            if (depth > MAX_DEPTH) {
                throw fail("JSON 嵌套超过 " + MAX_DEPTH + " 层");
            }
            char c = peek();
            return switch (c) {
                case '{' -> parseObject(depth);
                case '[' -> parseArray(depth);
                case '"' -> parseString();
                case 't' -> literal("true", Boolean.TRUE);
                case 'f' -> literal("false", Boolean.FALSE);
                case 'n' -> literal("null", null);
                default -> parseNumber();
            };
        }

        private Map<String, Object> parseObject(int depth) {
            expect('{');
            Map<String, Object> result = new LinkedHashMap<>();
            skipWhitespace();
            if (peekOrZero() == '}') {
                index++;
                return result;
            }
            while (true) {
                skipWhitespace();
                String key = parseString();
                skipWhitespace();
                expect(':');
                skipWhitespace();
                result.put(key, parseValue(depth + 1));
                skipWhitespace();
                char c = next();
                if (c == ',') {
                    continue;
                }
                if (c == '}') {
                    return result;
                }
                throw fail("对象里期望 ',' 或 '}'，实际是 '" + c + "'");
            }
        }

        private List<Object> parseArray(int depth) {
            expect('[');
            List<Object> result = new ArrayList<>();
            skipWhitespace();
            if (peekOrZero() == ']') {
                index++;
                return result;
            }
            while (true) {
                skipWhitespace();
                result.add(parseValue(depth + 1));
                skipWhitespace();
                char c = next();
                if (c == ',') {
                    continue;
                }
                if (c == ']') {
                    return result;
                }
                throw fail("数组里期望 ',' 或 ']'，实际是 '" + c + "'");
            }
        }

        private String parseString() {
            expect('"');
            StringBuilder out = new StringBuilder();
            while (true) {
                char c = next();
                if (c == '"') {
                    return out.toString();
                }
                if (c != '\\') {
                    if (c < 0x20) {
                        throw fail("字符串里有未转义的控制字符 U+" + String.format("%04X", (int) c));
                    }
                    out.append(c);
                    continue;
                }
                char escaped = next();
                switch (escaped) {
                    case '"' -> out.append('"');
                    case '\\' -> out.append('\\');
                    case '/' -> out.append('/');
                    case 'b' -> out.append('\b');
                    case 'f' -> out.append('\f');
                    case 'n' -> out.append('\n');
                    case 'r' -> out.append('\r');
                    case 't' -> out.append('\t');
                    case 'u' -> {
                        if (index + 4 > text.length()) {
                            throw fail("\\u 转义不完整");
                        }
                        String hex = text.substring(index, index + 4);
                        index += 4;
                        try {
                            out.append((char) Integer.parseInt(hex, 16));
                        } catch (NumberFormatException ignored) {
                            throw fail("非法的 \\u 转义：" + hex);
                        }
                    }
                    default -> throw fail("未知转义字符：\\" + escaped);
                }
            }
        }

        private Object parseNumber() {
            int start = index;
            while (index < text.length()) {
                char c = text.charAt(index);
                if (c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E' || (c >= '0' && c <= '9')) {
                    index++;
                } else {
                    break;
                }
            }
            String token = text.substring(start, index);
            if (token.isEmpty()) {
                throw fail("期望一个值，实际是 '" + peekOrZero() + "'");
            }
            boolean fractional = token.indexOf('.') >= 0
                    || token.indexOf('e') >= 0
                    || token.indexOf('E') >= 0;
            if (!fractional) {
                try {
                    return Long.valueOf(token);
                } catch (NumberFormatException ignored) {
                    // 超出 long：退化成 double（FMT 的字段都在 long 范围内，这里只是兜底）
                }
            }
            try {
                return Double.valueOf(token);
            } catch (NumberFormatException ignored) {
                throw fail("非法的数字：" + token);
            }
        }

        private Object literal(String word, Object value) {
            if (!text.startsWith(word, index)) {
                throw fail("期望 " + word);
            }
            index += word.length();
            return value;
        }

        private void expect(char expected) {
            char c = next();
            if (c != expected) {
                throw fail("期望 '" + expected + "'，实际是 '" + c + "'");
            }
        }

        private char next() {
            if (index >= text.length()) {
                throw fail("JSON 意外结束");
            }
            return text.charAt(index++);
        }

        private char peek() {
            if (index >= text.length()) {
                throw fail("JSON 意外结束");
            }
            return text.charAt(index);
        }

        private char peekOrZero() {
            return index < text.length() ? text.charAt(index) : '\0';
        }

        private void skipWhitespace() {
            while (index < text.length()) {
                char c = text.charAt(index);
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    index++;
                } else {
                    break;
                }
            }
        }

        private FmtJsonException fail(String message) {
            int from = Math.max(0, index - 20);
            String around = text.substring(from, Math.min(text.length(), index + 20));
            return new FmtJsonException(message + "（位置 " + index + "，附近：" + around + "）");
        }
    }
}
