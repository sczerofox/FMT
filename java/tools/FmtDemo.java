import org.lazy.fmt.client.Fmt;
import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtClientCore;
import org.lazy.fmt.client.FmtError;
import org.lazy.fmt.client.FmtException;
import org.lazy.fmt.client.FmtLoggers;
import org.lazy.fmt.client.FmtResult;
import org.lazy.fmt.client.FmtTransportException;
import org.lazy.fmt.client.FmtConfigurationException;
import org.lazy.fmt.client.model.FileQuery;
import org.lazy.fmt.client.model.FmtBucketListing;
import org.lazy.fmt.client.model.FmtConfig;
import org.lazy.fmt.client.model.FmtDeleteCheck;
import org.lazy.fmt.client.model.FmtFile;
import org.lazy.fmt.client.model.FmtPage;
import org.lazy.fmt.client.model.FmtPayload;
import org.lazy.fmt.client.model.FmtServerStatus;
import org.lazy.fmt.client.model.FmtShare;
import org.lazy.fmt.client.model.FmtTrashEmpty;
import org.lazy.fmt.client.model.FmtTrashEntry;
import org.lazy.fmt.client.model.FmtTrashListing;
import org.lazy.fmt.client.model.FmtUpload;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.function.Supplier;

/**
 * FmtDemo：fmt-client-core 的用法参考（**单文件**，不用 Maven 也能跑）。
 *
 * <p>调用流程只有三步：<b>调参数（FmtClientCore）→ 建通道（Fmt.channel）→ 调方法</b>：
 * <pre>{@code
 * FmtClientCore core = new FmtClientCore();          // 不调参数就用默认值
 * core.setBaseUrl("http://localhost:4122");
 * core.setDataRoot(Path.of("D:/Tools/fmt"));         // 或 core.setToken("<32 位十六进制>")
 * try (FmtChannel fmt = Fmt.channel(core)) {
 *     if (fmt.ping()) { fmt.listFiles(FileQuery.all()); }
 * }
 * }</pre>
 *
 * <p>用法（Windows 上类路径分隔符是 {@code ;}）：
 * <pre>
 *   # 直接用刚打好的 jar（数据根默认 D:/Data/Temp/JMT/fmt，token 从 data/user.json 读）
 *   java -cp java\fmt-client-core\target\fmt-client-core-2.2.1.jar java\tools\FmtDemo.java
 *
 *   # 显式指定：地址 + 数据根（推荐这个写法：服务端换 token 之后 Java 侧不用改）
 *   java -cp ...\fmt-client-core-2.2.1.jar java\tools\FmtDemo.java ^
 *        --data-root=D:/Data/Temp/JMT/fmt --out-dir=D:/tmp/fmt-demo
 *
 *   # 或直接给明文 token（占位符，别照抄：每个数据根一个）
 *   java -cp ...\fmt-client-core-2.2.1.jar java\tools\FmtDemo.java ^
 *        --base-url=http://localhost:4122 --token=&lt;32 位十六进制&gt;
 *
 *   # 配假 FMT（java/tools/fake-fmt/FakeFmt.java）联调，不碰真服务
 *   java java\tools\fake-fmt\FakeFmt.java 14122
 *   java -cp ...\fmt-client-core-2.2.1.jar java\tools\FmtDemo.java ^
 *        --base-url=http://127.0.0.1:14122 --token=test-token-1234567890abcdef --keep
 *
 *   # 上传本机某个文件（不给 --in 就上传一段内存字节）
 *   java -cp ...\fmt-client-core-2.2.1.jar java\tools\FmtDemo.java --in=D:/报表.xlsx
 * </pre>
 *
 * <p>可选参数：{@code --search=关键字}（列表页过滤）、{@code --keep}（不回滚，把演示文件留在
 * FMT 里）、{@code --verbose}（打开 SDK 日志，能看到地址回落与每次请求）。
 *
 * <p><b>编码注意</b>：JDK 17 的单文件源码模式按**平台编码**读源文件（中文 Windows 上是 GBK），
 * 所以本文件的**字符串字面量全部是 ASCII**，中文只出现在注释里（与 {@code tools/fake-fmt/FakeFmt.java}
 * 同一条纪律）。要用 {@code javac} 编译也一样安全：{@code javac -encoding UTF-8}。
 */
public final class FmtDemo {

    private FmtDemo() {
    }

    public static void main(String[] args) {
        Options options = Options.parse(args);
        System.out.println("== FMT Java SDK demo ==");

        try (FmtChannel fmt = options.open()) {
            // describe() 只给地址 + 掩码后的凭证，**永远不含 token 明文**：放心写日志
            System.out.println("  channel            " + fmt.describe());

            health(fmt);
            configuration(fmt);
            files(fmt, options);

            DemoUpload upload = upload(fmt, options);
            if (upload == null) {
                System.out.println();
                System.out.println("upload did not succeed, stop here.");
                return;
            }

            inspectAndDownload(fmt, upload, options);
            share(fmt, upload.fileId());
            errorModel(fmt, upload.fileId());
            escapeHatch(fmt);
            cleanup(fmt, upload, options);
        } catch (FmtTransportException unreachable) {
            // 连不上 / 超时 / 响应不是 FMT 信封
            System.out.println();
            System.out.println("[FAIL] cannot reach FMT: " + unreachable.getMessage());
            System.out.println("       check: fmt.exe service status");
            System.out.println("       check: <data-root>/config/server.json has \"enabled\": true");
            System.out.println("       check: Java and fmt.exe must run on the SAME machine");
        } catch (FmtConfigurationException misconfigured) {
            // token 没配 / baseUrl 非法 / user.json 损坏
            System.out.println();
            System.out.println("[FAIL] configuration: " + misconfigured.getMessage());
        }
    }

    // ------------------------------------------------------------------
    // 1. 探活
    // ------------------------------------------------------------------

    private static void health(FmtChannel fmt) {
        section("1. ping / status");

        // ping 是**唯一不需要 token** 的业务接口：token 配错时它还通，用来分清「服务没开」与「凭证不对」
        FmtResult<java.util.Map<String, Object>> ping = fmt.ping();
        System.out.println("  ping               "
                + (ping.isOk() ? "ok (public endpoint, no token needed)" : ping.error()));

        FmtServerStatus status = attempt("status", fmt::status);
        if (status != null) {
            System.out.println("  root               " + status.root());
            System.out.println("  version / pid      " + status.version() + " / " + status.pid()
                    + " (" + status.channel() + ")");
        }
    }

    // ------------------------------------------------------------------
    // 2. 配置与桶（都只读）
    // ------------------------------------------------------------------

    private static void configuration(FmtChannel fmt) {
        section("2. config / buckets (both read-only)");

        FmtConfig config = attempt("config", fmt::getConfig);
        if (config != null) {
            System.out.println("  user / bucket      " + config.currentUser() + " / " + config.currentBucket());
            // 上传前先拿上限做一次粗预检，省得白传一遍再拿 FMT-303
            System.out.println("  max upload size    " + config.maxUploadSize() + " bytes");
        }

        FmtBucketListing buckets = attempt("buckets", fmt::listBuckets);
        if (buckets != null) {
            System.out.println("  current bucket     " + buckets.currentBucket() + " (" + buckets.count() + " buckets)");
            for (FmtBucketListing.FmtBucket bucket : buckets.buckets()) {
                System.out.println("    - " + bucket.name() + (bucket.current() ? "   <- current" : ""));
            }
        }
    }

    // ------------------------------------------------------------------
    // 3. 列表（搜索 / 排序 / 分页）
    // ------------------------------------------------------------------

    private static void files(FmtChannel fmt, Options options) {
        section("3. list files (search / sort / page)");

        FmtPage page = attempt("listFiles", () -> fmt.listFiles(FileQuery.builder()
                .search(options.search)
                .sort(FileQuery.Sort.NAME)
                .page(1)
                .pageSize(10)
                .build()));
        if (page == null) {
            return;
        }
        System.out.println("  total              " + page.total()
                + "   (page " + page.page() + ", pageSize " + page.pageSize()
                + ", totalPages " + page.totalPages() + ")");
        for (FmtFile file : page.files()) {
            System.out.println("    - " + file.fileId() + "  " + file.fileName()
                    + "  " + file.size() + "B  " + file.fileType());
        }
    }

    // ------------------------------------------------------------------
    // 4. 上传：两种形态，**都不收 Path**
    // ------------------------------------------------------------------

    private static DemoUpload upload(FmtChannel fmt, Options options) {
        section("4. upload (the body IS the content; there is no upload(Path) by design)");

        FmtConfig config = attempt("config", fmt::getConfig);
        String stamp = Long.toString(System.currentTimeMillis());
        byte[] bytes = null;
        long size = 0;
        String name;
        if (options.input != null) {
            try {
                size = Files.size(options.input);
            } catch (IOException broken) {
                System.out.println("  read failed        " + broken);
                return null;
            }
            if (config != null && config.maxUploadSize() > 0 && size > config.maxUploadSize()) {
                System.out.println("  skipped            " + options.input + " is " + size
                        + "B > maxUploadSize " + config.maxUploadSize() + "B");
                return null;
            }
            name = options.input.getFileName().toString();
        } else {
            bytes = ("fmt-demo-" + stamp + "\n").getBytes(StandardCharsets.UTF_8);
            size = bytes.length;
            name = "fmt-demo-" + stamp + ".txt";
        }
        // 传进 lambda 的局部变量必须是「有效 final」：所以这里把分支结果收敛成三个不再改的变量
        final byte[] payload = bytes;
        final long payloadSize = size;
        final String fileName = name;

        FmtUpload uploaded;
        if (options.input != null) {
            // 流式上传：长度未知 → Transfer-Encoding: chunked；流只能读一次，所以**不参与 401 重试**
            // （token 配错会直接拿到 FMT-018，而不是一个读到一半的流）
            try (InputStream in = Files.newInputStream(options.input)) {
                uploaded = attempt("upload(stream)", () -> fmt.upload(in, fileName));
            } catch (IOException broken) {
                System.out.println("  open failed        " + broken);
                return null;
            }
        } else {
            // 内存里的字节：可以重放，因此参与 401 自动重试
            uploaded = attempt("upload(bytes)", () -> fmt.upload(payload, fileName));
        }
        if (uploaded == null || uploaded.file() == null) {
            return null;
        }

        FmtFile file = uploaded.file();
        System.out.println("  file_id            " + file.fileId());
        System.out.println("  file_name / size   " + file.fileName() + " / " + file.size() + "B");
        System.out.println("  md5 / type         " + file.md5() + " / " + file.fileType());
        System.out.println("  message            " + uploaded.message());
        return new DemoUpload(file.fileId(), payload, payloadSize);
    }

    // ------------------------------------------------------------------
    // 5. 单条信息 / 下载 / 预览
    // ------------------------------------------------------------------

    private static void inspectAndDownload(FmtChannel fmt, DemoUpload upload, Options options) {
        section("5. get / download / preview");

        FmtFile file = attempt("getFile", () -> fmt.getFile(upload.fileId()));
        if (file != null) {
            System.out.println("  name / size        " + file.fileName() + " / " + file.size() + "B");
            System.out.println("  path (relative)    " + file.path());
            System.out.println("  in trash?          " + file.inTrash());
        }

        // 下载：流式落盘，先写 .part 再改名；目标已存在时必须显式 overwrite=true
        Path target = options.outDir.resolve(upload.fileId() + ".dat");
        FmtPayload got = attempt("download", () -> fmt.download(upload.fileId(), target, true));
        if (got != null) {
            System.out.println("  written            " + got.target()
                    + "  (" + got.bytes() + "B, " + got.contentType() + ", inline=" + got.inline() + ")");
            try {
                byte[] bytes = Files.readAllBytes(got.target());
                boolean same = upload.payload() != null
                        ? Arrays.equals(bytes, upload.payload())
                        : bytes.length == upload.payloadSize();
                System.out.println("  bytes match        " + same);
            } catch (IOException broken) {
                System.out.println("  verify failed      " + broken);
            }
        }

        // 预览：只有图片与文本类能内联，其余 FMT-701；**下载不受这个限制**
        Path previewTarget = options.outDir.resolve("preview-" + upload.fileId() + ".txt");
        FmtPayload preview = attempt("preview", () -> fmt.preview(upload.fileId(), previewTarget, true));
        if (preview != null) {
            System.out.println("  preview            " + preview.target() + " (inline=" + preview.inline() + ")");
        }
    }

    // ------------------------------------------------------------------
    // 6. 分享
    // ------------------------------------------------------------------

    private static void share(FmtChannel fmt, String fileId) {
        section("6. share");

        FmtShare share = attempt("createShare", () -> fmt.createShare(fileId));
        if (share == null) {
            return;
        }
        System.out.println("  share_id           " + share.shareId());
        System.out.println("  quota / expire     " + share.downloadCount() + "/" + share.maxDownloadCount()
                + ", expire " + share.expireTime());
        // FmtShare 里**没有 url 字段**（服务端响应里有）：完整链接自己拼，
        // 或者用逃生舱 fmt.call("POST", "/api/share", null, Map.of("file_id", id)) 拿原始字段
        System.out.println("  link               " + fmt.baseUri() + "/api/share/"
                + share.shareId() + "/download");

        FmtShare looked = attempt("getShare", () -> fmt.getShare(share.shareId()));
        if (looked != null) {
            System.out.println("  state / available  " + looked.state() + " / " + looked.available());
        }
        if (attempt("revokeShare", () -> fmt.revokeShare(share.shareId())) != null) {
            System.out.println("  revoked            link is dead now (demo leaves no share behind)");
        }
    }

    // ------------------------------------------------------------------
    // 7. 错误模型
    // ------------------------------------------------------------------

    private static void errorModel(FmtChannel fmt, String fileId) {
        section("7. error model (business errors never throw)");

        FmtResult<FmtFile> missing = fmt.getFile("no-such-file-" + System.currentTimeMillis() + ".txt");
        if (missing.isFailure()) {
            FmtError error = missing.error();
            // code 用来做判断，message 只用来给人看（服务端措辞会变）
            System.out.println("  code / HTTP        " + error.code() + " / " + error.httpStatus()
                    + "   (exit code " + error.exitCode() + ")");
            System.out.println("  message            " + error.message());
            System.out.println("  isUnauthorized?    " + error.isUnauthorized());
        } else {
            System.out.println("  unexpected success " + missing.data());
        }

        // 「需要确认」的操作：先 dry_run 预检（零副作用），确认之后再 force（CLI 是 --yes）
        FmtTrashEmpty plan = attempt("emptyTrash(dryRun=true)", () -> fmt.emptyTrash(true, false));
        if (plan != null) {
            System.out.println("  trash plan         files=" + plan.files() + " buckets=" + plan.buckets()
                    + " bytes=" + plan.bytes() + " blocked=" + plan.blocked());
            System.out.println("  needs confirm      " + plan.needsConfirm() + "   (real run needs force=true)");
        }

        // 有内容又没给 force -> FMT-016，**不会真的删**；空站则直接成功
        FmtResult<FmtTrashEmpty> refused = fmt.emptyTrash(false, false);
        if (refused.isFailure() && refused.error().isConfirmRequired()) {
            System.out.println("  FMT-016 demo       " + refused.error().message());
            System.out.println("                     -> dryRun first, then call again with force=true");
        } else if (refused.isOk()) {
            System.out.println("  trash is empty     no confirmation needed");
        }

        // 删除预检：?dry_run=1，说清「会不会跨桶、有没有歧义」，零副作用
        FmtDeleteCheck check = attempt("checkDelete", () -> fmt.checkDelete(fileId));
        if (check != null) {
            System.out.println("  delete precheck    blocked=" + check.blocked()
                    + " otherBucket=" + check.otherBucket()
                    + " ambiguous=" + check.ambiguous() + " bucket=" + check.bucket());
        }
    }

    // ------------------------------------------------------------------
    // 8. 逃生舱
    // ------------------------------------------------------------------

    private static void escapeHatch(FmtChannel fmt) {
        section("8. escape hatch: call() for endpoints the SDK does not wrap");
        // SDK 没包装的接口（或 FMT 以后新增的）直接按 HTTP 语义调，返回信封里的 data 原始节点。
        // 不要用它调流式接口（上传/下载）。
        Object data = attempt("call GET /api/status", () -> fmt.call("GET", "/api/status", null, null));
        System.out.println("  raw data           " + data);
    }

    // ------------------------------------------------------------------
    // 9. 收尾：软删除 -> 永久删除（让数据根回到原样）
    // ------------------------------------------------------------------

    private static void cleanup(FmtChannel fmt, DemoUpload upload, Options options) {
        section("9. cleanup: soft delete -> purge");

        if (options.keep) {
            System.out.println("  skipped (--keep)   demo file left in FMT: " + upload.fileId());
            return;
        }
        FmtFile deleted = attempt("delete(force=false)", () -> fmt.delete(upload.fileId(), false));
        if (deleted != null) {
            System.out.println("  moved to trash     " + deleted.fileName() + " (" + deleted.fileId() + ")");
        }

        FmtTrashListing trash = attempt("listTrash", fmt::listTrash);
        if (trash == null) {
            return;
        }
        for (FmtTrashEntry entry : trash.entries()) {
            if (upload.fileId().equals(entry.id())) {
                // 永久删除不可恢复，必须 force=true
                if (attempt("deleteTrash(force=true)", () -> fmt.deleteTrash(entry.id(), true)) != null) {
                    System.out.println("  purged             " + entry.name() + "  (permanent)");
                }
                return;
            }
        }
        System.out.println("  not in trash       already gone?");
    }

    // ------------------------------------------------------------------
    // 小工具
    // ------------------------------------------------------------------

    private record DemoUpload(String fileId, byte[] payload, long payloadSize) {
    }

    private static void section(String title) {
        System.out.println();
        System.out.println("-- " + title);
    }

    /**
     * 调一次接口：业务错误（信封 {@code ok=false}）与传输/配置异常都**只打印不中断**，
     * 失败返回 {@code null}。真正的业务代码里应该按错误码决定要不要继续
     * （比如 {@link FmtError#isConfirmRequired()} 就补一个 force 再来一次）。
     */
    private static <T> T attempt(String label, Supplier<FmtResult<T>> call) {
        try {
            FmtResult<T> result = call.get();
            if (result.isFailure()) {
                FmtError error = result.error();
                System.out.println("  " + label + " -> " + error.code()
                        + " (HTTP " + error.httpStatus() + ") " + error.message());
                return null;
            }
            return result.data();
        } catch (FmtException failure) {
            System.out.println("  " + label + " -> " + failure.getClass().getSimpleName()
                    + ": " + failure.getMessage());
            return null;
        }
    }

    private static final class Options {

        private String baseUrl;                                  // 不给就用 SDK 默认值
        private Path dataRoot = Path.of("D:/Data/Temp/JMT/fmt"); // token 从这里的 data/user.json 读
        private String token;                                    // 给了就直接用（优先于 dataRoot）
        private Path input;                                      // --in=... 上传这个文件
        private Path outDir = Path.of(System.getProperty("java.io.tmpdir"), "fmt-demo");
        private String search = "";                              // 列表页的过滤关键字
        private boolean keep;                                    // 不回滚演示文件
        private boolean verbose;                                 // 打开 SDK 日志

        static Options parse(String[] args) {
            Options options = new Options();
            for (String arg : args) {
                if (arg.startsWith("--base-url=")) {
                    options.baseUrl = arg.substring("--base-url=".length());
                } else if (arg.startsWith("--data-root=")) {
                    options.dataRoot = Path.of(arg.substring("--data-root=".length()));
                } else if (arg.startsWith("--token=")) {
                    options.token = arg.substring("--token=".length());
                } else if (arg.startsWith("--in=")) {
                    options.input = Path.of(arg.substring("--in=".length()));
                } else if (arg.startsWith("--out-dir=")) {
                    options.outDir = Path.of(arg.substring("--out-dir=".length()));
                } else if (arg.startsWith("--search=")) {
                    options.search = arg.substring("--search=".length());
                } else if (arg.equals("--keep")) {
                    options.keep = true;
                } else if (arg.equals("--verbose")) {
                    options.verbose = true;
                }
            }
            return options;
        }

        /** 建通道：参数在 FmtClientCore 上调，凭证绑在通道上，之后业务代码再也看不到 token。 */
        FmtChannel open() {
            FmtClientCore core = new FmtClientCore();
            if (baseUrl != null) {
                core.setBaseUrl(baseUrl);
            }
            if (token != null && !token.isBlank()) {
                core.setToken(token);                       // 明文（占位符，别照抄）
            } else {
                core.setDataRoot(dataRoot);                 // 从 <数据根>/data/user.json 读，能自愈
            }
            if (verbose) {
                core.setLogger(FmtLoggers.stderr());        // token 永远以掩码出现
            }
            try {
                Files.createDirectories(outDir);
            } catch (IOException ignored) {
                // 建不出来就让后面的写入自己报错
            }
            return Fmt.channel(core);
        }
    }
}
