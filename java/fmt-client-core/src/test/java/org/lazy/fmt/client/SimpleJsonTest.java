package org.lazy.fmt.client;

import org.lazy.fmt.client.json.Json;
import org.lazy.fmt.client.json.SimpleJson;

import org.junit.jupiter.api.Test;

import java.util.List;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class SimpleJsonTest {

    private final SimpleJson json = SimpleJson.INSTANCE;

    @Test
    void parsesObjectsArraysAndScalars() {
        Object parsed = json.read("""
                {"ok":true,"data":{"files":[{"file_id":"fmt-1","size":12}],"total":1,"path":null}}
                """);
        Map<String, Object> root = Json.castMap(parsed);
        assertEquals(Boolean.TRUE, root.get("ok"));
        assertEquals("fmt-1", Json.str(root, "data.files.0.file_id"));
        assertEquals(12L, Json.lng(root, "data.files.0.size", -1));
        assertEquals(1L, Json.lng(root, "data.total", -1));
        assertNull(Json.path(root, "data.path"));
        assertTrue(Json.path(root, "data.missing.deeper") == null);
    }

    @Test
    void parsesEscapesAndUnicodeSurrogatePairs() {
        Object parsed = json.read("""
                {"text":"引号\\" 反斜杠\\\\ 换行\\n 制表\\t","emoji":"\\uD83D\\uDE00","slash":"a\\/b"}
                """);
        assertEquals("引号\" 反斜杠\\ 换行\n 制表\t", Json.str(parsed, "text"));
        assertEquals("\uD83D\uDE00", Json.str(parsed, "emoji"));
        assertEquals("a/b", Json.str(parsed, "slash"));
    }

    @Test
    void keepsIntegersAsLongAndFractionsAsDouble() {
        Object parsed = json.read("{\"big\":9007199254740993,\"small\":-4,\"ratio\":1.5,\"exp\":1e3}");
        assertEquals(9007199254740993L, Json.path(parsed, "big"));
        assertEquals(-4L, Json.path(parsed, "small"));
        assertEquals(1.5d, Json.path(parsed, "ratio"));
        assertEquals(1000.0d, Json.path(parsed, "exp"));
    }

    @Test
    void writesNonAsciiVerbatimAndRoundTrips() {
        String text = json.write(Map.of("file_name", "报表 v2.xlsx", "size", 12));
        assertTrue(text.contains("报表 v2.xlsx"), "非 ASCII 应原样输出：" + text);
        Map<String, Object> back = Json.castMap(json.read(text));
        assertEquals("报表 v2.xlsx", Json.str(back, "file_name"));
        assertEquals(12L, Json.lng(back, "size", 0));
    }

    @Test
    void writeEscapesControlCharactersAndQuotes() {
        String text = json.write(Map.of("k", "a\"b\\c\nd\u0001"));
        // 去掉最外层 {"k": …} 的壳，只看值的编码
        assertEquals("\"a\\\"b\\\\c\\nd\\u0001\"", text.substring(text.indexOf(':') + 1, text.length() - 1));
    }

    @Test
    void writesNestedStructuresAndRejectsNan() {
        Map<String, Object> value = new java.util.LinkedHashMap<>();
        value.put("files", List.of(Map.of("id", 1), Map.of("id", 2)));
        value.put("ok", true);
        assertEquals("{\"files\":[{\"id\":1},{\"id\":2}],\"ok\":true}", json.write(value));
        assertThrows(FmtJsonException.class, () -> json.write(Double.NaN));
    }

    @Test
    void rejectsMalformedJsonWithPosition() {
        FmtJsonException broken = assertThrows(FmtJsonException.class, () -> json.read("{\"a\":}"));
        assertTrue(broken.getMessage().contains("位置"), broken.getMessage());
        assertThrows(FmtJsonException.class, () -> json.read("{\"a\":1} trailing"));
        assertThrows(FmtJsonException.class, () -> json.read(""));
        assertThrows(FmtJsonException.class, () -> json.read("{\"a\" 1}"));
        assertThrows(FmtJsonException.class, () -> json.read("[1,]"));
    }

    @Test
    void refusesPathologicallyDeepNesting() {
        String deep = "[".repeat(500) + "]".repeat(500);
        assertThrows(FmtJsonException.class, () -> json.read(deep));
    }

    @Test
    void pathHelperHandlesIndexesAndWrongTypes() {
        Map<String, Object> node = Json.castMap(json.read("{\"shares\":[{\"share_id\":\"a\"},{\"share_id\":\"b\"}]}"));
        assertEquals("b", Json.str(node, "shares.1.share_id"));
        assertEquals("", Json.str(node, "shares.9.share_id"));
        assertEquals("fallback", Json.str(node, "shares.0.nope", "fallback"));
        assertInstanceOf(List.class, Json.path(node, "shares"));
        assertEquals(0, Json.maps(node, "shares.0").size(), "非数组路径应给空列表而不是抛异常");
    }
}
