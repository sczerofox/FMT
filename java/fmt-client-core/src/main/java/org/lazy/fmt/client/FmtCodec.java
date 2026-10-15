package org.lazy.fmt.client;

/**
 * JSON 编解码出口：把「发出去的请求体」和「收到的信封」在 JSON 文本与 Java 对象之间转换。
 *
 * <p>约定的对象模型就是最朴素的那套，任何 JSON 库都能直接对上：
 * <ul>
 *   <li>对象 → {@code Map<String,Object>}（键顺序不保证）</li>
 *   <li>数组 → {@code List<Object>}</li>
 *   <li>字符串 → {@code String}；布尔 → {@code Boolean}；null → {@code null}</li>
 *   <li>数字 → 能装进 {@code long} 的整数用 {@code Long}，否则 {@code Double}</li>
 * </ul>
 *
 * <p>默认实现是零依赖的 {@link org.lazy.fmt.client.json.SimpleJson}。Spring Boot 环境下
 * starter 会自动换成基于宿主 {@code ObjectMapper} 的 {@code JacksonFmtCodec}；
 * 也可以自己实现本接口并用 {@code @Bean} 覆盖。
 */
public interface FmtCodec {

    /** JSON 文本 → 上述对象模型。解析失败抛 {@link FmtJsonException}。 */
    Object read(String json);

    /** 上述对象模型 → JSON 文本（UTF-8，不转义非 ASCII 字符）。 */
    String write(Object value);
}
