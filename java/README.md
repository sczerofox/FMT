# FMT Java 客户端（jar）

> 当前版本：**3.0.0**（v3.0.0 新增 `FmtClient.uploadLarge(...)` 大文件分片续传）。

把 FMT 的 HTTP 服务接进 Java / Spring Boot 项目：**建一条通道，token 交给通道**，
之后业务代码里再也看不到 token。

完整设计（包括「为什么 token 不能真的绑在 TCP 连接上」「FMT 有哪些接口其实没有」）见
[FMT Java 客户端接入设计](../docx/FMT%20Java%20客户端接入设计.md)。

```text
java/
├── fmt-client-core/        零依赖 SDK：通道、凭证、模型、流式上传下载   → 任何人可用（Java 17+）
├── fmt-spring-boot-starter/  Spring Boot 3 自动配置：FmtChannel bean + fmt.* + 健康检查
├── fmt-client-demo/        可执行示例 jar（fmt-bridge.jar）：REST 桥接 + 定时拉取 + 自检
└── tools/maven-settings.xml  干净的 Maven settings（避开本机 conf 里那个 HTTP 阿里云镜像）
```

---

## 0. 最简单：FmtClient（普通 Java / Spring Boot 通用）

不写 builder、不碰 token、不自己判断 FMT-304/FMT-105：

```java
// 账号密码 + 桶（地址默认 http://localhost:4122；桶不填 = current_bucket）
try (FmtClient fmt = new FmtClient("lazy", "123456", "pet")) {

    fmt.ping();                 // boolean
    fmt.status();               // FmtServerStatus（失败抛 FmtApiException）

    // 一次调用：上传 / 同内容复用 / 同名不同内容自动改名重传
    FmtFilePackage pkg = fmt.upload(
            Files.newInputStream(Path.of("avatar.png")), "avatar.png");

    String fileId = pkg.fileId();       // fmt-20261021-0
    String fileName = pkg.fileName();
    FmtMedia preview = pkg.preview();   // 图片/文本；不支持时 previewOrNull()
    FmtMedia download = pkg.download();
}
```

- token 模式：`FmtClient.token(token, "pet")`（桶可省）。
- set 方式：`new FmtClient().setBaseUrl(...).setUsername(...).setPassword(...).setBucket(...)`。
- Spring Boot 直接注入 `FmtClient`（starter 自动配置），不需要自己 new。
- 预览/下载是惰性取内容，`close()` 会清理临时文件。

---

## 1. 前置条件（FMT 侧）

1. **确认 HTTP 已打开**（新数据根默认就是开的）：数据根下 `config/server.json`

   ```json
   { "version": 1, "enabled": true, "host": "localhost", "port": 4122 }
   ```

   关闭过就用 `fmt.exe config set server_enabled true`（CLI 会自动重启服务），或手工改文件后
   执行 `service stop` → `service start`。

2. **拿到 token**：

   ```powershell
   fmt.exe config list --show-token   # 默认隐藏 token，显式加 --show-token 才打印
   ```

3. 确认服务在：

   ```powershell
   Invoke-WebRequest http://localhost:4122/api/ping    # 公开接口，不需要 token
   ```

> FMT **只监听本机**。Java 进程必须和 `fmt.exe` 在同一台机器上；跨机要自己搭隧道
> （SSH / VPN / 反向代理 + TLS），见设计文档第 9 节。

---

## 2. 快速开始：Spring Boot 3 项目

**① 引入依赖**（先用 `mvn install` 把 jar 装进本地仓库，或发布到私服）

```xml
<dependency>
  <groupId>org.lazy</groupId>
  <artifactId>fmt-spring-boot-starter</artifactId>
  <version>3.0.0</version>
</dependency>
```

**② 配置**（只支持两种凭证，二选一；两者都不填会**启动失败**）

```yaml
fmt:
  base-url: http://localhost:4122   # 别写 127.0.0.1：FMT 默认 host=localhost，Windows 上常只绑 IPv6 的 ::1

  # A. 固定 token
  token: <32 位十六进制>

  # 或 B. 账号 + 密码（bucket 可选；不填用服务端 current_bucket）
  # username: lazy
  # password: 123456
  # bucket: pet
```

> Spring Boot starter **不再读取** `fmt.data-root` / `fmt.token-file` / `FMT_TOKEN` / `FMT_ROOT`。
> 普通 Java 核心包（`FmtClientCore`）仍保留 `setDataRoot` 等底层能力，见 §3；
> 但 Spring Boot 宿主请只用 token 或账号密码。

**③ 用**

```java
@Service
public class ReportService {

    private final FmtChannel fmt;                       // 通道里带着凭证，注入即用

    public ReportService(FmtChannel fmt) {
        this.fmt = fmt;
    }

    /** 把本地报表推到 FMT，并返回 file_id。**自己开流，SDK 不收路径**。 */
    public String push(Path report) throws IOException {
        try (InputStream in = Files.newInputStream(report)) {
            return fmt.upload(in, report.getFileName().toString()).orThrow().file().fileId();
        }
    }

    /** 按关键字搜文件。 */
    public List<FmtFile> search(String keyword) {
        return fmt.listFiles(FileQuery.builder().search(keyword).build()).orThrow().files();
    }

    /** 取回文件（流式落盘，先写 .part 再改名）。 */
    public Path fetch(String fileId, Path target) {
        return fmt.download(fileId, target, true).orThrow().target();
    }

    /** 生成分享链接的凭证。 */
    public String share(String fileId) {
        return fmt.createShare(fileId).orThrow().shareId();
    }
}
```

---

## 2.1 像 MySQL 一样：账号 + 密码 + 库名

2.1.0 起，starter 可以直接用 **账号、密码、库名（Bucket）** 登录，不需要再配 token：

```yaml
fmt:
  base-url: http://localhost:4122
  username: user
  password: your-password
  bucket: work          # 可选；不填用服务端 current_bucket
```

`fmt.username` + `fmt.password` 与 `fmt.token` **二选一**（2.2.0 起 starter 不再支持 data-root）：

- 登录模式：第一次业务调用时自动 `GET /api/auth/salt` → 本地 PBKDF2 摘要 →
  `POST /api/auth/login`；拿到的是 **2 小时有效的内存会话 token**，不是 `data/user.json`
  里那个 7 天账号 token。
- 会话过期（401 + `FMT-019`）时通道会用保存的账号密码**自动重登一次**；账号密码被改过
  则如实报 `FMT-020`，不会无限重试。
- 每个业务请求都带 `X-FMT-Bucket: <bucket>`；服务端会校验会话只能访问登录时选的那个库。
- 明文密码只在 Java 进程里，网络上只有 PBKDF2 摘要。

普通 Java 项目：

```java
try (FmtChannel fmt = Fmt.login("http://localhost:4122", "user", "password", "work")) {
    fmt.listFiles(FileQuery.all());   // 请求自动带 X-FMT-Token + X-FMT-Bucket
}
```

如果继续用旧 token，也可以显式绑定库名：

```java
FmtChannel fmt = Fmt.channel("http://localhost:4122", token, "work");
```

> 登录失败是 `FMT-020`；库不存在是 `FMT-200`；会话过期是 `FMT-019`（登录模式会自动重登）。
> 旧 token 通路保持可用：不带 `X-FMT-Bucket` 时仍用服务端 `current_bucket`。

---

## 3. 快速开始：普通 Java 项目（不带 Spring）

> 这一节是**核心包**用法，保留 `setDataRoot` / 环境变量等底层能力；
> **Spring Boot starter 只支持 token 或账号密码**（见 §2），两者都没有会启动失败。

调用流程只有三步：**调参数（`FmtClientCore`）→ 建通道（`Fmt.channel`）→ 调方法**。

```java
// 1) 参数：不调就用默认值（http://localhost:4122、超时 3s/30s/30m、16 连接、401 自动重试）
FmtClientCore core = new FmtClientCore();
core.setBaseUrl("http://localhost:4122");
core.setDataRoot(Path.of("D:/Tools/fmt"));        // 从 data/user.json 读 token（推荐）
// core.setToken("<你的 token>");                 // 或直接给固定 token
// core.setRequestTimeout(Duration.ofSeconds(10)); core.setLogger(FmtLoggers.stderr());

// 2) 建通道：参数在这里被拷成不可变配置，token 从此绑在通道上（业务方法里没有它）
try (FmtChannel fmt = Fmt.channel(core)) {
    // 3) 只调方法
    if (fmt.ping()) {                             // ping 是公开接口，不需要 token
        System.out.println(fmt.status().orThrow().root());
        Path report = Path.of("D:/报表.xlsx");
        try (InputStream in = Files.newInputStream(report)) {
            fmt.upload(in, report.getFileName().toString()).orThrow();
        }
    }
}
```

> 也可以一行建通道：`Fmt.channel(new FmtClientCore("http://localhost:4122", token))`，
> 或者链式：`new FmtClientCore().setDataRoot(Path.of("D:/Tools/fmt")).channel()`。
> 参数默认值与含义见 [`FmtClientCore`](fmt-client-core/src/main/java/org/lazy/fmt/client/FmtClientCore.java) 的类注释。

只依赖 `fmt-client-core` 即可；它**没有任何第三方依赖**（HTTP 用 JDK 17 的
`java.net.http`，JSON 用内置实现，也可以用 `FmtCodec` 换成 Jackson）。

**完整可运行的参考**：[`tools/FmtDemo.java`](tools/FmtDemo.java)（单文件、不用 Maven；
中文只在注释里、字符串字面量全是 ASCII，所以 GBK 控制台下也能用单文件模式直接跑）：

```powershell
# 对着假 FMT 联调（不碰真服务）
java java\tools\fake-fmt\FakeFmt.java 14122
java -cp java\fmt-client-core\target\fmt-client-core-3.0.0.jar java\tools\FmtDemo.java `
     --base-url=http://127.0.0.1:14122 --token=test-token-1234567890abcdef --keep

# 对着真服务：地址 + 数据根（token 从 <数据根>/data/user.json 读）
java -cp java\fmt-client-core\target\fmt-client-core-3.0.0.jar java\tools\FmtDemo.java `
     --data-root=D:\Data\Temp\JMT\fmt --out-dir=D:\tmp\fmt-demo
```

它按顺序演示：ping/status → 配置与桶（只读）→ 列表（搜索/排序/分页）→ 上传（**内存字节与流两种形态**，
先按 `max_upload_size` 粗预检）→ 单条 / 下载（校验字节数）→ 预览（`FMT-701` 也是演示的一部分）→
分享链接 → 错误模型（业务错误不抛异常、`dry_run` 预检、`FMT-016` 要 `force`）→ 逃生舱 `call()` →
收尾（软删除 + 永久删除，`--keep` 可跳过）。参数：`--search=` / `--in=<文件>` / `--keep` / `--verbose`。

---

## 4. 关键用法

### 4.1 token 只在建通道时给一次

```java
// ① 参数载体（推荐）：想调什么就 set 什么，其余用默认值
FmtClientCore core = new FmtClientCore()
        .setBaseUrl("http://localhost:4122")
        .setDataRoot(Path.of("D:/Tools/fmt"));     // 从 <数据根>/data/user.json 读，能自愈
FmtChannel fmt = Fmt.channel(core);

// ② 一行：固定 token（**占位符，别照抄**：每个数据根一个，从 <数据根>/data/user.json 或
//   `fmt.exe config list --show-token` 拿；抄错会拿到 401 + FMT-018）
FmtChannel fmt = Fmt.channel("http://localhost:4122", "<你的 token>");

// ③ 也可以完全不动配置：环境变量 FMT_TOKEN，或 FMT_ROOT=<数据根>
FmtChannel fmt = Fmt.channel(new FmtClientCore());

// 头写法：默认 X-FMT-Token；过网关想要 Bearer 就切一下
new FmtClientCore().setAuthScheme(FmtAuthScheme.BEARER);
```

> `Fmt.channel(core)` 会把当时的参数**拷成不可变配置**：建完通道再改 `core` 不影响这个通道，
> 要换 token 又不想重启进程，用 `setDataRoot(...)`（每次读文件）或自定义 `TokenProvider`。

### 4.2 错误处理：业务错误不抛异常，传输/配置错误才抛

```java
FmtResult<FmtFile> got = fmt.getFile("a.txt");
if (got.isFailure()) {
    FmtError error = got.error();
    // error.code()      -> "FMT-002"
    // error.message()   -> 服务端中文说明（不要拿它做判断）
    // error.httpStatus()-> 404
    // error.exitCode()  -> 3（与 C++ 侧 exit_code 对齐）
    if (error.isConfirmRequired()) {          // FMT-016：补一个 force 就能继续
        got = fmt.delete("a.txt", true);
    }
}
```

```java
try {
    fmt.listFiles(FileQuery.all());
} catch (FmtTransportException notReachable) {
    // 连不上 / 响应不是 FMT 信封 / 请求被中断
} catch (FmtConfigurationException misconfigured) {
    // token 没配、baseUrl 非法、token 文件损坏
}
```

> Spring Boot 里**不用自己写 `@ControllerAdvice`**：starter 在 Servlet Web 应用里自动注册
> `FmtExceptionAdvice`，把上面的异常翻成 `{"ok":false,"error":{"code":"FMT-002",…}}`——
> 业务错误沿用 FMT 给的 HTTP 状态码，连不上/超时 → 502，配置错 → 500 + `FMT-008`。
> 不想要就设 `fmt.exception-advice=false`；声明自己的 `FmtExceptionAdvice` bean 也会覆盖默认实现。
> 它只处理 FMT 异常，不接管 `IllegalArgumentException` 之类的宿主异常。

### 4.3 上传：只有两种形态，**都不收路径**

```java
fmt.upload(bytes, "报表.xlsx");                          // 内存里的字节
try (InputStream in = file.getInputStream()) {           // 流（chunked，不参与 401 重试）
    fmt.upload(in, file.getOriginalFilename());
}
// 本机文件：FmtClient 的 Path 便捷方法（客户端本地读文件，不是把路径给服务端）
Path report = Path.of("D:/a.xlsx");
fmt.upload(report);
```

> **底层 `FmtChannel.upload` 仍然只收 byte[] / InputStream**：`FmtClient.upload(Path)`
> 只是客户端本地打开文件，内容照样由调用方进程读出；服务端从不接收路径。

> 超过 FMT 的 `max_upload_size` 时服务端**边收边判、立刻中止**：你可能拿到 `FMT-303`，
> 也可能拿到连接被重置的 IO 异常。建议业务侧先用文件大小做一次粗预检。

### 4.3.1 大文件分片上传

`upload(...)` 仍然是一次 HTTP 请求，适合普通文件；大文件用 `uploadLarge(...)`：

```java
try (FmtClient fmt = new FmtClient("lazy", "123456", "pet")) {
    FmtFilePackage pkg = fmt.uploadLarge(
            Path.of("D:/Application/minecraft.rar"),
            "minecraft.rar",
            FmtLargeUploadOptions.builder()
                    .chunkSize(8L * 1024 * 1024)
                    .progress(p -> System.out.printf(
                            "%d%%  分片 %d/%d%n",
                            p.percent(), p.chunkIndex() + 1, p.chunkCount()))
                    .build());
    System.out.println(pkg.fileId());
}
```

它会自动算整文件 MD5、建会话、逐片 PUT（带 `X-Chunk-MD5`）、回调进度并完成入库。

断点续传：

```java
FmtUploadSession status = fmt.largeUploadStatus(uploadId);
fmt.resumeLargeUpload(Path.of("D:/Application/minecraft.rar"), uploadId,
        FmtLargeUploadOptions.builder()
                .progress(p -> System.out.println(p.percent() + "%"))
                .build());
```

暂停/取消：

```java
fmt.pauseLargeUpload(uploadId);
fmt.cancelLargeUpload(uploadId);
```

`uploadLarge` 开始前会读一遍源文件算 MD5；续传不会重算，只按服务端的 `received/missing` 传缺片。
文件大小必须与会话一致，否则抛配置异常。

### 4.4 FMT → Java 只能拉，不能推

FMT 没有 webhook / SSE / 回调。要「新文件自动进 Java」，就得**定时拉 + 游标去重**，
参考实现是 `fmt-client-demo` 的 `FmtInboxPoller`（默认关闭）：

```yaml
demo:
  pull-enabled: true
  pull-interval-ms: 60000
  inbox-dir: ./inbox          # 已拉过的 file_id 记在 ./inbox/.fmt-cursor
```

### 4.5 读配置 / 列桶 / 清空回收站

```java
// 上传前先看上限，少踩一次 FMT-303（响应里不带 token）
FmtConfig config = fmt.getConfig().orThrow();
if (Files.size(report) > config.maxUploadSize()) {
    throw new IllegalStateException("超过 FMT 上传上限：" + config.maxUploadSize());
}

// 我到底在往哪个桶里放东西？（HTTP 的文件类接口都作用在当前桶上）
FmtBucketListing buckets = fmt.listBuckets().orThrow();
System.out.println("当前桶：" + buckets.currentBucket());

// 清空回收站：先预检，再确认（不可恢复）
FmtTrashEmpty plan = fmt.emptyTrash(true, false).orThrow();     // ?dry_run=1
if (plan.total() > 0) {
    fmt.emptyTrash(false, true).orThrow();                      // ?force=1
}
```

三条对应的 HTTP 语义（都是这次新补的）：

| 调用 | HTTP | 要点 |
|---|---|---|
| `getConfig()` | `GET /api/config` | **只读**；**响应里永远没有 token**（服务端连开关都不提供，要 token 走 CLI `fmt config list` 或读 `data/user.json`） |
| `listBuckets()` | `GET /api/bucket` | **只读**列表（含 `currentBucket`） |
| `emptyTrash(dryRun, force)` | `DELETE /api/trash/empty[?dry_run=1\|force=1]` | 有内容不给 `force` → `FMT-016`；空站直接成功 |

### 4.6 那些「FMT 其实没有」的接口

| 你可能想调 | 现实 |
|---|---|
| 改配置（上传上限等） | HTTP 侧**没有写接口**（会动服务进程的全局状态）：`fmt.exe config set max_upload_size 10MB` |
| 创建 / 切换 / 删除 Bucket | HTTP 侧**只读**：写操作是 CLI 的活（`current_bucket` 是全局状态，多调用方一起改会互相踩） |
| 用 URL 上传 | HTTP 侧只接受**请求体就是文件内容**（`fmt.upload(...)`），不支持 `{"url": …}` |

---

## 5. 构建、测试与打包

```powershell
cd java

# 本机 Maven conf 里有一个 HTTP 的阿里云镜像，Maven 3.8+ 会拦，所以显式指定这份 settings
mvn -s tools/maven-settings.xml clean install        # 构建 + 跑测试（不需要真的 FMT）

# 只出一个可执行桥接 jar
mvn -s tools/maven-settings.xml -pl fmt-client-demo -am package
# 产物：java/fmt-client-demo/target/fmt-bridge.jar
```

测试用的是 **JDK 自带的 HttpServer 起的假 FMT 服务**，它照抄了真服务端的认证与信封行为
（`/api/*` 先验 token、`401 + FMT-018`、`ok/data` 与 `error` 信封、未登记路由 `404 + FMT-017`），
所以「token 有没有真的进请求头」「401 有没有真的自愈」「上传的字节有没有真的到」都是**真 HTTP 实测**：

```powershell
mvn -s tools/maven-settings.xml -pl fmt-client-core test
```

---

## 6. 连上真的 FMT 自检

```powershell
# 确保 fmt.exe 服务在跑、config/server.json 已打开 HTTP
java -jar fmt-client-demo/target/fmt-bridge.jar `
     --fmt.data-root=D:\Tools\fmt --demo.smoke=true
```

预期输出：

```text
[fmt-smoke] 通道：http://localhost:4122 [auth=X-FMT-Token, token=1a28…693f]
WARN org.lazy.fmt.client : 连接 http://localhost:4122 被拒，改用 http://127.0.0.1:4122 重试（GET /api/ping）
WARN org.lazy.fmt.client : 连接 http://127.0.0.1:4122 被拒，改用 http://[0:0:0:0:0:0:0:1]:4122 重试（GET /api/ping）
[fmt-smoke] GET /api/ping：ok（公开接口，不需要 token）
[fmt-smoke] GET /api/status：数据根=D:/Data/Temp/JMT/fmt，版本=3.0.0，pid=23420
[fmt-smoke] GET /api/file：2 个文件（共 2）
[fmt-smoke]   · fmt-20261008-2  17364485.jpg  3889891 B
[fmt-smoke]   · fmt-20261008-0  asdva.jpg  130184 B
```

（上面是 2026-10-10 的**真机原样输出**：那两行 WARN 就是地址回落在工作——
FMT 只绑了 `::1`，JDK 从 `localhost` 出发先撞了 `127.0.0.1`。）

跑起来之后还可以当桥接服务用（默认 8080）：

```powershell
curl http://127.0.0.1:8080/demo/status
curl http://127.0.0.1:8080/demo/files?search=报表
curl -F "file=@D:\报表.xlsx" http://127.0.0.1:8080/demo/files
curl -OJ http://127.0.0.1:8080/demo/files/fmt-20261009-0/download
curl http://127.0.0.1:8080/demo/config          # 读配置：**不含** token（任何参数都不给）
curl http://127.0.0.1:8080/demo/buckets         # 桶列表 + 当前桶（只读）
curl -X DELETE "http://127.0.0.1:8080/demo/trash/empty?dry_run=true"   # 先预检
curl -X DELETE "http://127.0.0.1:8080/demo/trash/empty?force=true"     # 再确认
curl http://127.0.0.1:8080/actuator/health     # 看 fmt 那一段：authenticated 是不是 true
```

> 自检失败（连不上 FMT、token 不对）**不会**让应用起不来：`FmtSmokeRunner` 只会打印
> 「自检未通过」并继续启动，`/demo/*` 等你把 FMT 弄好再用即可。
>
> 真机实测（2026-10-10）已经跑过一次并**复验通过**：`authenticated:true`、真实数据根/文件/回收站条目都读通，
> 部署含三条新路由的 exe 后 `tools/smoke-test.ps1` **113 通过 / 0 失败**。
> 完整输出与**真机暴露并修掉的三个客户端缺陷**写在
> [设计文档 §10.2](../docx/FMT%20Java%20客户端接入设计.md)。
>
> 注意 `/demo/config`、`/demo/buckets`、`/demo/trash/empty` 需要**已部署包含这三条路由的 fmt.exe**；
> 服务端比工作区旧时它们会如实报 `501 / 404 / FMT-016`（桥接透传，不掩盖）。

### 6.1 没有 fmt.exe 也能验证（假 FMT）

`java/tools/fake-fmt/FakeFmt.java` 是一个只有几十行的假 FMT，协议照抄真服务端
（`/api/*` 先验 token、`401 + FMT-018`、`ok/data` 与 `error` 信封、未登记路由 `404 + FMT-017`），
不用装服务、不用管理员权限：

```powershell
# 终端 1：假 FMT（token 固定为 test-token-1234567890abcdef）
java java\tools\fake-fmt\FakeFmt.java 14122

# 终端 2：桥接 jar 连它
java -jar java\fmt-client-demo\target\fmt-bridge.jar `
     --server.port=18080 --fmt.base-url=http://127.0.0.1:14122 `
     --fmt.token=test-token-1234567890abcdef --demo.smoke=true

# 终端 3：打一圈
curl http://127.0.0.1:18080/actuator/health
curl http://127.0.0.1:18080/demo/files
curl -F "file=@README.md" http://127.0.0.1:18080/demo/files
curl -o out.bin http://127.0.0.1:18080/demo/files/fmt-1/download
```

假 FMT 会把每个请求连同 token 掩码打出来，可以直接看到「谁带了凭证、谁没带」：

```text
[fake-fmt] GET /api/ping token=(none) bytes=0            ← ping 是公开接口，刻意不带
[fake-fmt] GET /api/status token=test... bytes=0         ← 其余请求都由通道注入
[fake-fmt] POST /api/file/upload token=test... bytes=24
```

---

## 7. 排查表

| 现象 | 原因 / 处理 |
|---|---|
| `FmtTransportException: 调用 FMT 失败 … Connection refused` | 服务没跑或 HTTP 没开：`fmt.exe service status` + 检查 `config/server.json`，改完要重启服务 |
| 上面这条但**服务明明在跑** | 地址族不对：FMT 默认 `host: localhost`，Windows 上常只绑 **IPv6 `::1`**（`Get-NetTCPConnection -LocalPort 4122` 能看到 `::1`），而 JDK 客户端对多地址主机名**只试第一个解析结果、不回落**。SDK 已经内置**地址回落**（候选地址依次重试，日志里会看到 `连接 … 被拒，改用 … 重试`），所以默认配置 `http://localhost:4122` 就能用；要把服务固定成 IPv4 就改 `server.json` 的 `host` 并重启（需要管理员） |
| `token 文件里没有 token 字段` | 部署的 `data/user.json` 还是**老形状** `{"users":[{…}]}`（部署的 exe 比工作区旧）。SDK **两种形状都认**（先读平铺 `token`，再读 `users[0].token`）；服务端升级后会自己改写成单用户形状，token 不变 |
| 三个新接口报 `501 / 404 / FMT-016` | 部署的 `fmt.exe` **早于这三条路由**（`GET /api/config`→501、`GET /api/bucket`→404、`DELETE /api/trash/empty` 落进 `trash.delete`）。重新部署 exe 并重启服务即可（见下） |
| `FMT-018` 且重试后仍失败 | token 不对：`fmt.exe config list` 重新取；starter 只认 `fmt.token` 或账号密码，不再读 `fmt.data-root` |
| 健康检查 `authenticated:false` 但 `status:UP` | 服务活着、凭证不对——正是这两个字段分开报告的意义 |
| `FMT-303` | 超过 `max_upload_size`（默认见 `config/config.json`），或上传中途被服务端中止 |
| `FMT-701` | 预览只支持图片与 `.txt/.md/.json/.csv/.log/.xml`；**下载不受限制** |
| `FMT-016` | 需要显式确认：`delete`/`emptyTrash` 没带 `force`。先 `?dry_run=1` 预检再补 `force` |
| `FMT-602` + HTTP 501 | 这个接口 FMT 登记了但没实现（例如 `/api/preview/*`） |
| 日志里看不到 token | 这是设计如此：通道只打方法/路径/状态码/耗时，token 永远以 `1a28…693f` 掩码出现 |
| `Blocked mirror for repositories` 之类的 Maven 报错 | 用 `-s tools/maven-settings.xml`（见第 5 节） |
