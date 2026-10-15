package org.lazy.fmt.client;

import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executors;

/**
 * 测试用的假 FMT 服务：**照抄真服务端的行为**，而不是一个「什么都答应」的桩。
 *
 * <p>刻意复刻的部分（这样测试才算数）：
 * <ul>
 *   <li>{@code /api/*} 一律先验 token，且优先看 {@code X-FMT-Token}、其次
 *       {@code Authorization: Bearer}，失败回 <b>401 + FMT-018</b>；</li>
 *   <li>只有 {@code /api/ping} 与 {@code /api/share/<id>/download} 是公开路径；</li>
 *   <li>成功信封 {@code {"ok":true,"data":…}}、失败信封
 *       {@code {"ok":false,"error":{"code":…,"message":…}}}；</li>
 *   <li>没登记的接口回 <b>404 + FMT-017</b>（和真服务端的兜底路由一致）。</li>
 * </ul>
 * 请求体用 {@code getRequestBody().readAllBytes()} 读——{@code chunked} 请求
 * 由 JDK 的 HttpServer 自动解码，所以流式上传也能被真实校验。
 */
final class StubFmtServer implements AutoCloseable {

    /** 收到的一次请求。 */
    record Recorded(String method, String path, String rawQuery,
                    Map<String, List<String>> headers, byte[] body) {

        String header(String name) {
            List<String> values = headers.get(name);
            return values == null || values.isEmpty() ? null : values.get(0);
        }

        String query() {
            return rawQuery;
        }

        /** 按参数名取值（已 URL 解码）；不存在返回 {@code null}。 */
        String queryParam(String name) {
            for (String pair : rawQuery.split("&")) {
                int equals = pair.indexOf('=');
                if (equals < 0) {
                    continue;
                }
                String key = URLDecoder.decode(pair.substring(0, equals), StandardCharsets.UTF_8);
                if (key.equals(name)) {
                    return URLDecoder.decode(pair.substring(equals + 1).replace("+", "%2B"),
                            StandardCharsets.UTF_8);
                }
            }
            return null;
        }

        String bodyText() {
            return new String(body, StandardCharsets.UTF_8);
        }
    }

    /** 一次响应。 */
    static final class Reply {

        private final int status;
        private final byte[] body;
        private final Map<String, String> headers;
        /** 声明的 Content-Length；-1 表示按实际 body 长度。用来模拟被截断的下载。 */
        private final long declaredLength;

        private Reply(int status, byte[] body, Map<String, String> headers, long declaredLength) {
            this.status = status;
            this.body = body;
            this.headers = headers;
            this.declaredLength = declaredLength;
        }

        static Reply ok(String dataJson) {
            return raw(200, "{\"ok\":true,\"data\":" + dataJson + "}");
        }

        static Reply error(int status, String code, String message) {
            return raw(status, "{\"ok\":false,\"error\":{\"code\":\"" + escape(code)
                    + "\",\"message\":\"" + escape(message) + "\"}}");
        }

        static Reply raw(int status, String text) {
            Map<String, String> headers = new LinkedHashMap<>();
            headers.put("Content-Type", "application/json; charset=utf-8");
            return new Reply(status, text.getBytes(StandardCharsets.UTF_8), headers, -1);
        }

        static Reply bytes(int status, byte[] data, Map<String, String> headers) {
            return new Reply(status, data, new LinkedHashMap<>(headers), -1);
        }

        Reply header(String name, String value) {
            headers.put(name, value);
            return this;
        }

        int status() {
            return status;
        }

        private static String escape(String text) {
            return text == null ? "" : text.replace("\\", "\\\\").replace("\"", "\\\"");
        }
    }

    /** 需要声明一个和实际写出字节数不一致的 {@code Content-Length} 时用（模拟截断）。 */
    static final class ReplyBuilder {

        private int status = 200;
        private long declaredLength = -1;
        private byte[] body = new byte[0];
        private final Map<String, String> headers = new LinkedHashMap<>();

        ReplyBuilder status(int status) {
            this.status = status;
            return this;
        }

        ReplyBuilder declaredLength(long declaredLength) {
            this.declaredLength = declaredLength;
            return this;
        }

        ReplyBuilder header(String name, String value) {
            headers.put(name, value);
            return this;
        }

        ReplyBuilder body(byte[] body) {
            this.body = body;
            return this;
        }

        Reply build() {
            return new Reply(status, body, headers, declaredLength);
        }
    }

    @FunctionalInterface
    interface Responder {
        Reply reply(Recorded request);
    }

    private final HttpServer server;
    private final List<Recorded> requests = new CopyOnWriteArrayList<>();
    private final Map<String, Responder> routes = new ConcurrentHashMap<>();
    private volatile String expectedToken;

    private StubFmtServer(String bindHost, String expectedToken) throws IOException {
        this.expectedToken = expectedToken;
        this.server = HttpServer.create(new InetSocketAddress(bindHost, 0), 0);
        this.server.createContext("/", this::handle);
        this.server.setExecutor(Executors.newFixedThreadPool(4, runnable -> {
            Thread thread = new Thread(runnable, "stub-fmt");
            thread.setDaemon(true);
            return thread;
        }));
        this.server.start();
    }

    static StubFmtServer start(String expectedToken) {
        return startOn("127.0.0.1", expectedToken);
    }

    /** 绑到指定地址：测「只绑了 IPv6 ::1 而客户端从 localhost 过来」时需要。 */
    static StubFmtServer startOn(String bindHost, String expectedToken) {
        try {
            return new StubFmtServer(bindHost, expectedToken);
        } catch (IOException failure) {
            throw new IllegalStateException("起不了 stub 服务（" + bindHost + "）", failure);
        }
    }

    int port() {
        return server.getAddress().getPort();
    }

    String baseUrl() {
        return "http://127.0.0.1:" + server.getAddress().getPort();
    }

    /** 换掉当前有效 token（用来模拟服务端轮换凭证）。 */
    void expectToken(String token) {
        this.expectedToken = token;
    }

    /** 登记一条路由：**前缀匹配**，同方法下最长前缀胜出。 */
    StubFmtServer on(String method, String pathPrefix, Responder responder) {
        routes.put(method.toUpperCase(java.util.Locale.ROOT) + " " + pathPrefix, responder);
        return this;
    }

    List<Recorded> requests() {
        return List.copyOf(requests);
    }

    Recorded lastRequest() {
        return requests.isEmpty() ? null : requests.get(requests.size() - 1);
    }

    int requestCount() {
        return requests.size();
    }

    void clearRequests() {
        requests.clear();
    }

    @Override
    public void close() {
        server.stop(0);
    }

    // ------------------------------------------------------------------

    private void handle(HttpExchange exchange) throws IOException {
        byte[] body = exchange.getRequestBody().readAllBytes();
        // path 记的是**百分号编码后的原始路径**（真服务端看到的也是这个，
        // 它拿到捕获组之后才 url_decode），所以测试能验证客户端有没有正确编码
        String rawPath = exchange.getRequestURI().getRawPath();
        String decodedPath = exchange.getRequestURI().getPath();
        String rawQuery = exchange.getRequestURI().getRawQuery();
        Map<String, List<String>> headers = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);
        headers.putAll(exchange.getRequestHeaders());
        Recorded recorded = new Recorded(exchange.getRequestMethod(), rawPath,
                rawQuery == null ? "" : rawQuery, headers, body);
        requests.add(recorded);

        if (!isPublicPath(decodedPath)) {
            String token = extractToken(recorded);
            String expected = expectedToken;
            if (expected != null && (token == null || !expected.equals(token))) {
                send(exchange, Reply.error(401, FmtErrorCodes.UNAUTHORIZED, "缺少或无效的访问 token"));
                return;
            }
        }

        Responder responder = route(exchange.getRequestMethod(), decodedPath);
        if (responder == null) {
            send(exchange, Reply.error(404, FmtErrorCodes.ROUTE_NOT_FOUND, "没有这个接口：" + decodedPath));
            return;
        }
        send(exchange, responder.reply(recorded));
    }

    private static boolean isPublicPath(String path) {
        return "/api/ping".equals(path)
                || (path.startsWith("/api/share/") && path.endsWith("/download"));
    }

    private static String extractToken(Recorded request) {
        String token = request.header("X-FMT-Token");
        if (token != null && !token.isBlank()) {
            return token.trim();
        }
        String authorization = request.header("Authorization");
        if (authorization != null && authorization.startsWith("Bearer ")) {
            return authorization.substring(7).trim();
        }
        return null;
    }

    private Responder route(String method, String path) {
        String key = method.toUpperCase(java.util.Locale.ROOT) + " ";
        String best = null;
        for (String candidate : routes.keySet()) {
            if (!candidate.startsWith(key)) {
                continue;
            }
            String prefix = candidate.substring(key.length());
            if (path.startsWith(prefix) && (best == null || prefix.length() > best.length())) {
                best = prefix;
            }
        }
        return best == null ? null : routes.get(key + best);
    }

    private static void send(HttpExchange exchange, Reply reply) throws IOException {
        List<Map.Entry<String, String>> headers = new ArrayList<>(reply.headers.entrySet());
        for (Map.Entry<String, String> header : headers) {
            exchange.getResponseHeaders().set(header.getKey(), header.getValue());
        }
        byte[] body = reply.body == null ? new byte[0] : reply.body;
        long declared = reply.declaredLength >= 0 ? reply.declaredLength : body.length;
        exchange.sendResponseHeaders(reply.status, declared == 0 ? -1 : declared);
        if (body.length == 0) {
            exchange.close();
            return;
        }
        try (OutputStream out = exchange.getResponseBody()) {
            out.write(body);
        }
        exchange.close();
    }
}
