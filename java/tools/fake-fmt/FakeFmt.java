import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;

import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.Executors;

/**
 * 假 FMT：本地开发/联调用的最小 FMT HTTP 服务，不用装 Windows 服务、不用管理员权限。
 *
 * <p>用法（三步，见 java/README.md §6.1）：
 * <pre>
 * java java/tools/fake-fmt/FakeFmt.java 14122
 * java -jar java/fmt-client-demo/target/fmt-bridge.jar \
 *      --server.port=18080 --fmt.base-url=http://127.0.0.1:14122 \
 *      --fmt.token=test-token-1234567890abcdef --demo.smoke=true
 * curl http://127.0.0.1:18080/actuator/health
 * </pre>
 *
 * <p>协议照抄真服务端：除 {@code /api/ping} 与 {@code /api/share/<id>/download} 外，
 * 所有 {@code /api/*} 都要 {@code X-FMT-Token}（或 {@code Authorization: Bearer}），
 * 失败回 {@code 401 + FMT-018}；成功 {@code {"ok":true,"data":…}}、失败
 * {@code {"ok":false,"error":{"code":…,"message":…}}}；未登记路由回 {@code 404 + FMT-017}。
 *
 * <p>编码注意：JDK 17 的单文件源码模式按**平台编码**读源文件（中文 Windows 上是 GBK），
 * 所以这个文件里的非 ASCII 只允许出现在注释里；**字符串字面量必须保持 ASCII**，
 * 否则一个中文字符的字节序列会把后面的引号吞掉，直接编译不过（踩过一次）。
 * 要输出中文就转义成 {@code \\uXXXX}。
 */
public class FakeFmt {

    static final String TOKEN = "test-token-1234567890abcdef";

    public static void main(String[] args) throws Exception {
        int port = args.length > 0 ? Integer.parseInt(args[0]) : 14122;
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", port), 0);
        server.createContext("/", FakeFmt::handle);
        server.setExecutor(Executors.newFixedThreadPool(4));
        server.start();
        System.out.println("[fake-fmt] listening on 127.0.0.1:" + port + " token=" + TOKEN);
    }

    static void handle(HttpExchange exchange) throws IOException {
        String path = exchange.getRequestURI().getPath();
        String rawPath = exchange.getRequestURI().getRawPath();
        String rawQuery = exchange.getRequestURI().getRawQuery();
        String method = exchange.getRequestMethod();
        byte[] body = exchange.getRequestBody().readAllBytes();
        System.out.println("[fake-fmt] " + method + " " + rawPath
                + (rawQuery == null ? "" : "?" + rawQuery)
                + " token=" + mask(exchange.getRequestHeaders().getFirst("X-FMT-Token"))
                + " bytes=" + body.length);

        boolean publicPath = path.equals("/api/ping")
                || (path.startsWith("/api/share/") && path.endsWith("/download"));
        if (!publicPath) {
            String token = exchange.getRequestHeaders().getFirst("X-FMT-Token");
            if (token == null) {
                String authorization = exchange.getRequestHeaders().getFirst("Authorization");
                if (authorization != null && authorization.startsWith("Bearer ")) {
                    token = authorization.substring(7);
                }
            }
            if (!TOKEN.equals(token)) {
                send(exchange, 401, "{\"ok\":false,\"error\":{\"code\":\"FMT-018\","
                        + "\"message\":\"missing or invalid token\"}}");
                return;
            }
        }

        if (path.equals("/api/ping")) {
            send(exchange, 200, "{\"ok\":true,\"data\":{}}");
            return;
        }
        if (path.equals("/api/status")) {
            send(exchange, 200, "{\"ok\":true,\"data\":{\"root\":\"D:/FakeFmt\",\"pid\":999,"
                    + "\"version\":\"1.0.0\",\"channel\":\"http\"}}");
            return;
        }
        if (path.equals("/api/config") && method.equals("GET")) {
            // token 永远不返回（真服务端连 ?include_token=1 这种开关都不提供）：
            // 响应体会进日志、被代理、被贴进工单，凭证不该多一份副本。
            send(exchange, 200, "{\"ok\":true,\"data\":{\"current_user\":\"user\","
                    + "\"current_bucket\":\"work\",\"max_upload_size\":10485760,"
                    + "\"size_unit\":\"MB\",\"language\":\"zh-CN\",\"path\":\"config/config.json\","
                    + "\"user_id\":\"u-1\"}}");
            return;
        }
        if (path.equals("/api/bucket") && method.equals("GET")) {
            send(exchange, 200, "{\"ok\":true,\"data\":{\"buckets\":["
                    + "{\"name\":\"work\",\"is_current\":true},"
                    + "{\"name\":\"private\",\"is_current\":false}],"
                    + "\"count\":2,\"current_bucket\":\"work\"}}");
            return;
        }
        // 注意：这条必须放在任何 /api/trash/<name> 形式的路由之前（真服务端同理）
        if (path.equals("/api/trash/empty") && method.equals("DELETE")) {
            boolean dryRun = rawQuery != null && rawQuery.contains("dry_run=1");
            boolean force = rawQuery != null && rawQuery.contains("force=1");
            if (!force && !dryRun) {
                send(exchange, 400, "{\"ok\":false,\"error\":{\"code\":\"FMT-016\","
                        + "\"message\":\"trash has 2 items; add force to confirm\"}}");
                return;
            }
            if (dryRun) {
                send(exchange, 200, "{\"ok\":true,\"data\":{\"files\":2,\"buckets\":0,\"bytes\":1234,"
                        + "\"needs_confirm\":true,\"blocked\":false,"
                        + "\"message\":\"will permanently delete 2 items\"}}");
                return;
            }
            send(exchange, 200, "{\"ok\":true,\"data\":{\"files\":2,\"buckets\":0,\"bytes\":1234,"
                    + "\"message\":\"trash emptied\"}}");
            return;
        }
        if (path.equals("/api/file") && method.equals("GET")) {
            send(exchange, 200, "{\"ok\":true,\"data\":{\"files\":["
                    + "{\"file_id\":\"fmt-1\",\"file_name\":\"report.xlsx\",\"size\":11},"
                    + "{\"file_id\":\"fmt-2\",\"file_name\":\"b.txt\",\"size\":5}],"
                    + "\"count\":2,\"total\":2,\"page\":1,\"page_size\":0,\"total_pages\":0,"
                    + "\"sort\":\"name\"}}");
            return;
        }
        if (path.equals("/api/file/upload") && method.equals("POST")) {
            String name = queryParam(exchange.getRequestURI().getRawQuery(), "name");
            send(exchange, 200, "{\"ok\":true,\"data\":{\"file_id\":\"fmt-9\",\"file_name\":\""
                    + name + "\",\"size\":" + body.length + ",\"md5\":\"deadbeef\","
                    + "\"extension\":\"txt\",\"file_type\":\"text\",\"bucket\":\"work\","
                    + "\"message\":\"upload done\"}}");
            return;
        }
        if (path.startsWith("/api/file/") && path.endsWith("/download")) {
            byte[] content = "download-body".getBytes(StandardCharsets.UTF_8);
            exchange.getResponseHeaders().set("Content-Type", "application/octet-stream");
            exchange.getResponseHeaders().set("Content-Disposition",
                    "attachment; filename*=UTF-8''report.xlsx");
            exchange.sendResponseHeaders(200, content.length);
            try (OutputStream out = exchange.getResponseBody()) {
                out.write(content);
            }
            return;
        }
        send(exchange, 404, "{\"ok\":false,\"error\":{\"code\":\"FMT-017\","
                + "\"message\":\"no such route: " + path + "\"}}");
    }

    private static String queryParam(String rawQuery, String name) {
        if (rawQuery == null) {
            return "unnamed";
        }
        for (String pair : rawQuery.split("&")) {
            int equals = pair.indexOf('=');
            if (equals > 0 && pair.substring(0, equals).equals(name)) {
                return URLDecoder.decode(pair.substring(equals + 1).replace("+", "%2B"),
                        StandardCharsets.UTF_8);
            }
        }
        return "unnamed";
    }

    private static String mask(String token) {
        return token == null ? "(none)" : token.substring(0, Math.min(4, token.length())) + "...";
    }

    private static void send(HttpExchange exchange, int status, String json) throws IOException {
        byte[] body = json.getBytes(StandardCharsets.UTF_8);
        exchange.getResponseHeaders().set("Content-Type", "application/json; charset=utf-8");
        exchange.sendResponseHeaders(status, body.length);
        try (OutputStream out = exchange.getResponseBody()) {
            out.write(body);
        }
    }
}
