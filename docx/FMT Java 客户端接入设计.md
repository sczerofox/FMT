# FMT Java 客户端接入设计

> 目标：让 Java / Spring Boot 项目通过一个 **jar 包**和 FMT 的 HTTP 服务建立「通道」，
> 双向传数据，**认证头必须带 token，且 token 绑定在建立的通道上**而不是散落在每次调用里。
>
> 本文所有关于 FMT 的结论都来自仓库里的真实实现（`src/server/server.cpp`、
> `src/service/commands.cpp`、`include/fmt/common/error.hpp`），不是推测。
> 配套代码：`java/`（`fmt-client-core` + `fmt-spring-boot-starter` + `fmt-client-demo`）。

---

## 1. 先把「token 放进通道」这句需求翻译准确

需求原话是：**头必须带 token，token 放入建立的链接通道中。**

这句话里有一个必须先讲清的技术事实：

> **HTTP 协议里，请求头是逐请求（per-request）的，没有任何办法把一个 header 真正"焊死"在一条
> TCP 连接上。** 连接可以 keep-alive 复用，但每个请求都必须重新带上凭证——这是协议决定的，
> 换任何语言、任何 HTTP 库都一样。

所以「token 放进通道」在工程上只能落成下面这个**通道模型**（本设计就是这么实现的）：

| 层 | 谁负责 | 业务代码看到什么 |
|---|---|---|
| 凭证来源 | `TokenProvider`（固定串 / `data/user.json` / 环境变量） | 看不到 |
| 通道建立 | 建 `FmtChannel` 时把凭证交给通道；通道内部一个 `HttpClient`（连接池） | 只看到 `FmtChannel fmt` |
| 请求注入 | 通道内部**唯一一处** `applyToken()` 给每个请求写 `X-FMT-Token` | 看不到 token 参数 |
| 凭证失效 | 收到 `401 + FMT-018` → 作废缓存 → 重取 → **自动重试一次** | 看不到，业务无感 |
| 凭证过期 | 收到 `401 + FMT-019` → **不重试**（文件里那个 token 也已过期），抛出可识别的错误提示去跑 `fmt.exe user token renew` | 需要人工续期 |

也就是说：

```java
// 业务代码里没有 token，一次都没有
FmtResult<FmtPage> page = fmt.listFiles(FileQuery.builder().search("报表").build());
```

- **物理连接**：JDK `HttpClient` 的连接池让它「建立一次、多次复用」（`Connection: keep-alive`）；
- **凭证**：由通道在唯一注入点写到每个请求的 header 上；
- **跨调用一致性**：不存在「某个业务方法忘了带 token」这种事故——这跟 FMT 服务端把认证放在
  pre-routing 钩子（`src/server/server.cpp:631`）而不是每个路由里的思路是同一个。

### 如果你要的是「握手一次、之后连接内免 token」

那就**必须改 fmt.exe**，因为当前服务端是无状态的：

```cpp
// src/server/server.cpp —— 每个请求都重新验一次，服务端不记会话
impl_->server->set_pre_routing_handler([verifier](const Request& request, Response& response) {
    if (!starts_with(request.path, "/api/") || is_public_path(request.path)) return Unhandled;
    const std::string token = header_token(request);          // 每次都读 header
    const std::string username = (verifier && !token.empty()) ? verifier(token) : std::string{};
    ...
});
```

三种做法对比（详见 §8 演进）：

| 方案 | 服务端改动 | 是否真的更安全 | 建议 |
|---|---|---|---|
| A. 通道级注入（本设计） | 无 | 对「只监听本机、单用户」的场景够用 | **采用** |
| B. 加 `POST /api/auth/handshake` 换 session_id | 新路由 + 会话表 + 过期/清理 + 并发锁 | 本机场景几乎没收益（token 仍在内存/配置里） | 有远程接入需求时再做 |
| C. 双向 TLS / Windows 命名管道 | 大 | 收益最高但成本最高 | 只在真正跨机部署时考虑 |

---

## 2. FMT HTTP 服务的真实契约（接入前必须知道的事）

### 2.1 开关与可达性

| 项 | 事实 |
|---|---|
| 默认状态 | **关闭**。要在数据根的 `config/server.json` 里 `{"version":1,"enabled":true,"host":"localhost","port":4122}` |
| 生效方式 | 改完必须重启服务：`fmt.exe service stop` → `service start`（需要管理员） |
| 监听范围 | **只监听本机**（默认 `host: localhost`）。Java 进程在和 FMT 不同的机器上时打不通，得自己搭隧道 |
| **地址族（真机踩过）** | `localhost` 在 Windows 上常先解析到 **IPv6 `::1`**，于是 httplib **只绑了 `::1`**：`http://127.0.0.1:4122/api/ping` → 连接被拒绝，而 `http://localhost:4122/api/ping` → 200。**客户端一律用主机名 `localhost`**（SDK 的默认值就是 `http://localhost:4122`，写主机名让 JDK 把两个地址族都试一遍）；`Get-NetTCPConnection -LocalPort 4122` 可以看到到底绑在哪个地址上 |
| 协议 | HTTP/1.1（cpp-httplib）。SDK 显式钉死 HTTP/1.1，免得 JDK 客户端去做 HTTP/2 升级协商 |

### 2.2 认证

- 除两个公开路径外，**所有 `/api/*` 都要 token**：
  - `GET /api/ping` —— 健康检查；
  - `GET /api/share/<share_id>/download` —— 分享链接本身即凭证。
- 请求头两种写法（服务端 `header_token()`）：`X-FMT-Token: <token>` 优先，其次 `Authorization: Bearer <token>`。
- 校验失败：**`401` + `FMT-018`**（缺 token / token 不对）。
- **token 有时效**（2026-10-18 起）：过期后 **`401` + `FMT-019 TokenExpired`**。
  - 有效期在 `data/user.json` 的 `token_expires_at`；默认 **7 天**（`config set token_ttl 7d`）。
  - **`FMT-019` 不能靠"重读 user.json 再试一次"自愈**：文件里那个 token 本身就已经过期了。
    这一条与 §4.2 的 401 自愈是两码事，SDK 必须把它当**不可恢复**处理。
  - 唯一的续期路径是**用密码**在 CLI 执行 `fmt.exe user token renew`
    （换完旧 token 立即失效）。也就是说：**Java 侧无法自行续期**，过期后需要人介入。
    如果将来要「像 MySQL 那样用 jar 登录」，那是新增一个 HTTP 登录/换发接口，
    不改变现有的 token 通路（见 §2.2 末条与 §8.1）。
- token 来源：数据根 `data/user.json`。**两种形状都认**：现行是单用户文件、字段平铺
  （`{"user_id":…,"token":…}`），老版本是 `{"users":[{…}]}` 取第一条——客户端不能等服务端先升级
  （真机踩过：部署的 exe 比工作区旧，文件还是 `users` 数组）。人拿 token 靠
  `fmt.exe config list --show-token`（默认隐藏，要显式加开关；会打印用户 ID / 访问 token /
  建议的请求头）。
  **HTTP 侧拿不到 token**：`GET /api/config` 的响应里永远没有它（见 §2.4 第 2 条）。
- **CLI 与 HTTP 都支持账号密码登录**（HTTP 侧见 §2.2.1，2.1.0 起）：
  - CLI 支持 `fmt.exe -u <用户名> -p`（回车后隐藏输入口令、回显 `*`）与窗口内
    `login` / `logout`，**业务命令要求先登录**（`service` / `log` / `help` / `version`
    与 `user get` / `user create` 除外）；登录态**只在 CLI 内存里**，窗口一关就要重新登录。
  - 两条链路用的是同一套 `salt + 本地 password_digest() + 摘要比对`：**口令明文不出
    客户端进程**，过管道/HTTP 的只有 PBKDF2 摘要。
  - **旧 token 通路必须保持可用**：已发布的 SDK 不会被一次性打翻。登录是新增能力，
    不是替换现有 token 认证。

### 2.2.1 账号密码登录（2.1.0 起；2.2.0 起桶名可选）

像 MySQL 一样用「账号 + 密码 + 库名（Bucket）」执行业务，不用先手工拿 token：

```text
GET  /api/auth/salt?username=<用户名>          （公开）→ {username, salt}
POST /api/auth/login                            （公开）
     {"username":"…","password_digest":"<64 hex>","bucket":"work"}   // bucket 可省略
  → 200 {"ok":true,"data":{"token":"<会话 token>","token_type":"session",
                           "expires_at":"…","username":"…","bucket":"work"}}
POST /api/auth/logout                           （要 token）→ 会话立即失效
```

- **口令**: 客户端先取 salt，本地 PBKDF2-SHA256（100000 轮、32 字节）算摘要，
  只发摘要；服务端用 `data/user.json` 里存的摘要做常量时间比对。
- **会话 token**: 服务端内存表里的临时密钥，**2 小时**有效；服务重启即失效。
  它与 `data/user.json` 的账号 token **完全独立**（后者仍是 7 天，给旧客户端/CLI 续期用）。
- **库名可选**: 登录时不填 `bucket` → 用服务端 `current_bucket`；业务请求头
  `X-FMT-Bucket` 也可省略（默认会话桶）。带了就必须与会话桶一致，不匹配 → 403 `FMT-004`。
- **旧账号 token**: 带 `X-FMT-Bucket` 就按它走；不带就沿用服务端 `current_bucket`。
  显式带库名的连接写法：
  ```java
  FmtChannel fmt = Fmt.channel("http://localhost:4122", token, "work");
  ```
- **Java 侧**:
  ```java
  try (FmtChannel fmt = Fmt.login("http://localhost:4122", "user", "password", null)) {
      fmt.listFiles(FileQuery.all());   // 不填桶 = 服务端当前桶
  }
  ```
  Spring Boot 配 `fmt.username` / `fmt.password`（`fmt.bucket` 可选）。会话过期
  （`FMT-019`）时 SDK 用保存的账号密码自动重登一次；账号密码错误则报 `FMT-020`。
- **Spring Boot starter 只支持两种凭证**：`fmt.token`，或 `fmt.username + fmt.password`；
  两者都没配会**启动失败**。`fmt.data-root` / `fmt.token-file` / `FMT_TOKEN` / `FMT_ROOT`
  不再被 starter 读取（核心包 `FmtClientCore` 仍保留这些底层能力）。

### 2.2.2 FmtClient 与统一上传包（2.3.0）

`FmtClient` 是给业务代码用的简化入口：构造器/setter 建连接，业务方法直接返回领域对象；
Spring Boot starter 同时注册 `FmtClient` bean（包装 `FmtChannel`，不重复建连接）。

```java
try (FmtClient fmt = new FmtClient("lazy", "123456", "pet")) {
    fmt.ping();
    FmtFilePackage pkg = fmt.upload(inputStream, "avatar.png");
    String fileId = pkg.fileId();
    FmtMedia preview = pkg.previewOrNull();   // 不支持预览时为 null
    FmtMedia download = pkg.download();
}
```

`upload(...)` 是**一次调用**，内部按顺序处理：

1. 成功 → 返回；
2. `FMT-304`（同内容已存在）→ 查已有记录（先按名，再按 MD5 翻页）→ 返回；
3. `FMT-105`（同名不同内容）→ 自动改名 `名字 + 01 + 扩展名`（如 `1234567.jpg` →
   `123456701.jpg`）→ 重传；再次冲突继续 02、03…（最多 100 次）。

`FmtFilePackage` 含 `FmtFile` 基本信息；`preview()` / `download()` 惰性取内容到临时文件，
`FmtMedia` 提供 `contentType/fileName/bytes/openStream/readBytes`，Spring Boot 可直接流式返回；
`close()` 负责清理临时文件。

Spring Boot starter 另外提供 `FmtFileResponse`，把「文件基本信息 + 预览响应 + 下载响应」封成一个对象：

```java
FmtFileResponse response = FmtFileResponse.of(pkg);
response.fileId();
response.fileName();
response.preview();    // ResponseEntity<Resource>，inline
response.download();   // ResponseEntity<Resource>，attachment
```

响应里的临时文件在 Spring 关闭流时自动删除，Controller 不再自己写临时文件清理代码。
已有文件可用 `FmtFileResponse.of(fmtClient, fileId)` 重新构造。



```json
{"ok": true,  "data": { … }}
{"ok": false, "error": {"code": "FMT-002", "message": "文件不存在：a.txt"}}
```

| HTTP | 含义 | 典型错误码 |
|---|---|---|
| 400 | 参数/校验 | FMT-001、FMT-303、FMT-701 |
| 401 | 认证 | FMT-018（缺/错 token）、**FMT-019（token 过期）**、**FMT-020（账号/密码/库名登录失败）**；旧账号 token 过期不可自愈，登录模式的会话 token 会自动重登 |
| 403 | 权限与不可用 | FMT-004、FMT-501/502/503 |
| 404 | 不存在（含"故意不提供"） | FMT-002、FMT-017、FMT-200 |
| 409 | 冲突 | FMT-003、FMT-105（同桶同名但内容不同）、FMT-304（同桶同名且内容相同）、FMT-401、FMT-403（回退桶时的合并撞名，见 `FMT 技术文档.md` 18.44） |
| 501 | **已登记但未实现** | FMT-602 |
| 500 | 服务内部错误 | 其它 |

> `501` 是重点：**已登记但没实现**的模块/子路径（`/api/server/*`、`/api/preview/*`、`/api/config/x`）
> 走兜底路由返回 501，不是 404 也不是 500。要分清三种"没有"：
> ① 打错的路径（`GET /api/nosuch`）→ **404 + `FMT-017`**；
> ② **故意不提供**的桶写接口（`GET /api/bucket/x`）→ 也是 **404 + `FMT-017`**（"故意不要"≠"还没做"）；
> ③ 非 GET 的未登记路径（`POST /api/bucket`）→ 落到通用错误处理器 → **404 + `FMT-002`**（只有 GET 有 `/api/.*` 兜底）。
> SDK 一律保留服务端给的状态码与错误码，不做二次解释。

### 2.4 路由清单与"故意没有的接口"（照抄实现，不含猜测）

| 方法 | 路径 | SDK 方法 |
|---|---|---|
| GET | `/api/ping` | `ping()` |
| GET | `/api/status` | `status()` |
| GET | `/api/config` | `getConfig(...)`（**只读**，默认不回 token） |
| GET | `/api/bucket` | `listBuckets()`（**只读**） |
| GET | `/api/auth/salt` | `Fmt.login(...)` 内部：取口令盐（**公开**） |
| POST | `/api/auth/login` | `Fmt.login(...)` 内部：摘要 + Bucket → 会话 token（**公开**） |
| POST | `/api/auth/logout` | 注销当前会话（要 token；2.1.0） |
| GET | `/api/file` | `listFiles(query)` |
| POST | `/api/file/upload` | `upload(...)` |
| GET | `/api/file/<id或名>` | `getFile(...)` |
| GET | `/api/file/<…>/download` | `download(...)` |
| GET | `/api/file/<…>/preview` | `preview(...)` |
| DELETE | `/api/file/<…>` | `checkDelete(...)` / `delete(...)` |
| GET | `/api/trash` | `listTrash()` |
| GET | `/api/trash/<key>` | `getTrash(key)` |
| POST | `/api/trash/<key>/restore` | `restoreTrash(key)` |
| DELETE | `/api/trash/<key>` | `deleteTrash(key, force)` |
| DELETE | `/api/trash/empty` | `emptyTrash(dryRun, force)`（**有内容必须 force**） |
| POST | `/api/share` | `createShare(fileId)` |
| GET | `/api/share?file_id=` | `listShares(fileId)` |
| GET | `/api/share/<share_id>` | `getShare(shareId)` |
| DELETE | `/api/share/<share_id>` | `revokeShare(shareId)` |
| GET | `/api/share/<share_id>/download` | `downloadShare(...)`（**公开**） |

**故意没有的接口**（设计上就要说清楚，不能让调用方以为有）：

1. **Bucket 只读**。只有 `GET /api/bucket` 这个列表；创建 / 切换 / 删除桶由 CLI 管——
   `current_bucket` 是服务端的全局状态，多个 HTTP 调用方一起改会互相踩，而且删桶有确认流程。
   未登记的桶路径仍是 `404 + FMT-017`。
2. **配置只读，而且响应里永远没有 token**。`GET /api/config` 能读到 `max_upload_size`、
   当前桶、用户 ID 等，**但没有写入口**（改配置会动服务进程的全局状态，那是 CLI 的活）；
   token **连"要不要带"的开关都不提供**——响应体会进日志、被代理、被贴进工单，
   凭证多一份副本就多一处泄漏面。要 token 走 CLI `fmt config list` 或直接读 `data/user.json`。
3. **没有 URL 上传**（`POST /api/file` 那套 `upload_args_from_body` 在实现里存在但未登记路由）。

> 上面第 1、2 条和「清空回收站」是**本轮新补的**：此前 `/api/bucket*` 一整套被删掉、
> `/api/config` 没开、`DELETE /api/trash/empty` 会被 `R"(/api/trash/([^/]+))"` 匹配成
> 「名字叫 `empty` 的条目」走 `trash.delete`（`FMT-016` 或 `FMT-400`）。
> 现在的形态是「**能读的都开，会变成第二个写入者的不开**」。

---

## 3. jar 的划分与依赖关系

```text
                 ┌──────────────────────────────┐
                 │   fmt-client-core  (零依赖)   │
                 │  FmtChannel / TokenProvider  │
                 │  FmtResult / FmtError        │
                 │  SimpleJson / 模型 / 流式     │
                 └───────────┬──────────────────┘
                             │ (compile)
                 ┌───────────▼──────────────────┐
                 │ fmt-spring-boot-starter      │
                 │ 自动配置 + SLF4J 桥 + Jackson │
                 │ + Actuator 健康检查           │
                 └───────────┬──────────────────┘
                             │
                 ┌───────────▼──────────────────┐
                 │ fmt-client-demo（可执行 jar）  │
                 │ REST 桥接 + 定时拉取 + 自检     │
                 └──────────────────────────────┘
```

| 模块 | 依赖 | 用途 | 谁该引入 |
|---|---|---|---|
| `fmt-client-core` | **零第三方依赖**（HTTP 用 JDK `java.net.http`，JSON 用内置 `SimpleJson`） | 通道、凭证、模型、流式上传下载 | 任何 Java 17 项目（非 Spring 也行） |
| `fmt-spring-boot-starter` | core + `spring-boot-autoconfigure` + `slf4j-api`（Jackson / Actuator 可选） | 一个 `FmtChannel` bean + `fmt.*` 配置 + `/actuator/health` | Spring Boot 3 项目 |
| `fmt-client-demo` | starter + `starter-web` + actuator | 可执行的桥接服务示例 | 想直接跑通/给别的系统调用时 |

**为什么 core 坚持零依赖**：放在任何工程的 classpath 上都不会和宿主的 Jackson/OkHttp/Netty 版本打架；
而且 JSON 出口是可替换的接口（`FmtCodec`），Spring 环境自动换成宿主的 `ObjectMapper`。

---

## 4. 通道的内部结构

```text
        FmtChannel（单例，随 Spring 容器生命周期）
        ├── FmtChannelConfig      地址 / 超时 / 连接数 / 认证头写法 / JSON / 日志
        ├── TokenProvider         凭证来源（可缓存、可作废、可轮换）
        ├── HttpClient            连接池（keep-alive 复用），HTTP/1.1，不跟随重定向
        ├── Semaphore             maxConnections 并发闸门
        └── exchange()            所有请求的唯一出口
                ├── RequestShape            只给「方法 + 地址 + body」
                ├── applyCommonHeaders()    UA / Accept / 自定义头
                ├── applyToken()            ★ 唯一注入 token 的地方
                └── 401 → invalidate() → 重发一次
```

### 4.1 凭证来源与优先级（starter 的解析顺序）

1. `fmt.token`（配置里直接写）
2. `fmt.token-file`（指定 `user.json`）
3. `fmt.data-root` → `<data-root>/data/user.json` ← **推荐**（服务端换 token、重装服务都能自愈）
4. 环境变量 / 系统属性 `FMT_TOKEN`（`-DFMT_TOKEN=…` 也认）
5. 环境变量 `FMT_ROOT` → `$FMT_ROOT/data/user.json`
6. 都不行 → 抛 `FmtConfigurationException`，消息里直接写清 ①②③④ 怎么配

> 刻意**不在启动时读文件**：FMT 可能比 Java 应用后启动、token 可能还没生成。
> 取不到凭证时报错发生在「第一次需要认证的调用」上，而 `ping` 与健康检查依然能报告
> 「服务活着，只是凭证不对」——这两件事必须能分开看。

### 4.2 401 自愈时序

```text
业务: fmt.listFiles(...)
   │
   ├─ 通道: GET /api/file   X-FMT-Token: <缓存里的旧 token>
   │         ← 401 {"ok":false,"error":{"code":"FMT-018",…}}
   │
   ├─ 通道: tokenProvider.invalidate()        // 清缓存，不读网络
   ├─ 通道: 重新取 token（例如重读 data/user.json）
   ├─ 通道: GET /api/file   X-FMT-Token: <新 token>   ← 只重试一次
   │         ← 200 {"ok":true,"data":{…}}
   └─ 业务: 拿到 FmtResult.ok(...)，全程不知道发生过 401
```

重试的**边界**（写死在实现里，避免埋雷）：

- 只有**可安全重发**的请求才重试：JSON 调用、`ofByteArray` 上传、下载（body 还没开始消费）；
- **`InputStream` 上传不重试**（流只能读一次，重发会读到已耗尽的流）→ 直接返回 `FMT-018`；
- 重试**最多一次**，避免"token 永远错"时打爆日志；
- 放弃重试时把 `401 + FMT-018` 原样交给调用方，不做静默降级。

### 4.3 线程与资源

| 项 | 取值 | 说明 |
|---|---|---|
| 连接池 | 一个 `HttpClient`（每通道） | keep-alive 复用；`close()` 关线程池 |
| 并发闸门 | `fmt.max-connections`（默认 16） | `Semaphore`，超出的请求排队而不是堆连接 |
| 连接超时 | 默认 3s | `fmt.connect-timeout` |
| JSON 调用超时 | 默认 30s | `fmt.request-timeout` |
| 流式调用超时 | 默认 30m | `fmt.stream-timeout`（上传/下载）；设 `0s` 表示不限制 |
| 重定向 | **不跟随** | FMT 不该有 3xx；跟着走可能把 token 带去别的地址 |
| 幂等重试 | 默认 0 | `fmt.idempotent-retries`，只作用于 GET，传输层失败才重试 |

### 4.4 地址回落（真机教训换来的能力）

配置里给的是**主机名**时，通道把它解析出的**所有**地址都当候选，连接被拒就换下一个重试：

```text
GET /api/ping
  ├─ http://localhost:4122      ← 配置原样，JDK 只试它的第一个解析结果 127.0.0.1 → 连接被拒
  ├─ http://127.0.0.1:4122      ← 候选 2 → 连接被拒
  └─ http://[0:0:0:0:0:0:0:1]:4122  ← 候选 3 → 200，记住它，后续请求直接从这里开始
```

为什么必须有这层：FMT 的 `"host": "localhost"` 在 Windows 上常让 httplib **只绑 `::1`**，
而 JDK 的 `HttpClient` 对多地址主机名**只试第一个、不回落**——于是「服务活着却连接被拒」。
只在 `ConnectException`（连接根本没建立、请求体一个字节都没写出去）时换地址，
所以对上传/写操作也是安全的；全部候选都失败才抛传输异常，错误信息里会列出**试过哪些地址**。

---

## 5. 数据的双向互传

### 5.1 Java → FMT（主动调用，能力最全）

| 业务 | SDK 调用 | HTTP |
|---|---|---|
| 读配置 | `fmt.getConfig()` | `GET /api/config`（**只读**；响应里永远没有 token） |
| 列桶 / 看当前桶 | `fmt.listBuckets()` | `GET /api/bucket`（**只读**） |
| 清空回收站 | `fmt.emptyTrash(dryRun, force)` | `DELETE /api/trash/empty[?dry_run=1\|force=1]` |
| 上传字节/流 | `fmt.upload(byte[], name)` / `fmt.upload(InputStream, name)` | `POST /api/file/upload?name=<编码后的文件名>`，**请求体就是文件内容**；流走 `Transfer-Encoding: chunked`。**SDK 不收 `Path`**（安全口径，见下）。服务端**也收** `multipart/form-data`（取第一个带 `filename` 的 part，Postman / 浏览器表单 / Spring 的 `MultipartFile` 直接可用），但 **SDK 仍然只推裸字节**——这样大文件不必先拼进内存、也不过一遍 multipart 编解码；Demo 从 `MultipartFile` 取出流再交给 SDK（见下） |
| 大文件分片 | `fmt.uploadLarge(Path, name, options)` / `resumeLargeUpload` / `pauseLargeUpload` / `cancelLargeUpload` / `largeUploadStatus` | `POST /api/upload/init` → `PUT /api/upload/<id>/chunk?index=N` → `POST /api/upload/<id>/complete`；暂停/继续/取消/查询各一条（v3.0.0） |
| 列文件 | `fmt.listFiles(FileQuery)` | `GET /api/file?search=&sort=&page=&page_size=` |
| 单条信息 | `fmt.getFile(id)` | `GET /api/file/<id或名>`（含相对数据根的 `path`） |
| 下载 | `fmt.download(id, target, overwrite)` | `GET /api/file/<…>/download`（流式落盘，先 `.part` 再改名） |
| 预览 | `fmt.preview(id, target)` | `GET /api/file/<…>/preview`（图片/文本；其余 `FMT-701`） |
| 删除预检 | `fmt.checkDelete(id)` | `DELETE /api/file/<…>?dry_run=1` |
| 软删除 | `fmt.delete(id, force)` | `DELETE /api/file/<…>[?force=1]` |
| 回收站 | `listTrash/getTrash/restoreTrash/deleteTrash` | `/api/trash*` |
| 分享 | `createShare/getShare/listShares/revokeShare` | `/api/share*` |
| 逃生舱 | `fmt.call(method, path, query, body)` | 任意 JSON 接口（FMT 以后加接口也不用等 SDK 升级） |

**上传的四个细节**（都是实现里踩过的地方）：

1. 文件名用 `?name=` 给（服务端会 `url_decode` 并做文件名校验），同时再带一份
   `Content-Disposition: attachment; filename*=UTF-8''<百分号编码>` 作保险；
   **文件名必须由调用方显式给**（没有「从路径推断」这条捷径了，见下面第 5 条）；
2. 路径段必须百分号编码：`GET /api/file/%E6%8A%A5%E8%A1%A8.xlsx` —— 服务端的捕获组拿的是
   **原始（编码）路径**，它自己再 `url_decode`；
3. 超过 `max_upload_size` 时服务端**边收边判**、超了立刻中止接收：客户端可能拿到
   `FMT-303`（响应可读），也可能拿到连接被重置的 IO 异常——**这不是 bug，是流式上传的固有形态**。
   建议在业务侧先用本地文件大小做一次粗预检（HTTP 侧拿不到上限，得靠配置或经验值）；
4. 未知长度的流走 `chunked`（cpp-httplib 支持，已在 `third_party/cpp-httplib/httplib.h:4321` 确认）。
5. **SDK 只收「字节 / 流」，不收 `Path`**（用户要求，安全口径）——曾经的
   `upload(Path)` / `upload(Path, String)` 两个重载**已删除**。理由：接口一旦收路径，
   调用方很容易把**服务端**的路径当参数递进来（FMT 只监听 localhost，这时客户端与服务端
   常常在同一台机器上），那等于让一次上传把服务进程能读到的任意文件拖出去——而且这种误用
   在编译期完全看不出来。现在内容只能由调用方自己读出来：
   `upload(Files.readAllBytes(path), name)` 或
   `upload(Files.newInputStream(path), name)`；**文件名也必须显式给**
   （没有「从源文件名推断」这条捷径，因为那正是路径进参数的第一步）。

### 5.2 FMT → Java（**只能拉，不能推**）

这是本设计里最需要跟业务方对齐的一点：

> **FMT 没有任何出站能力**：没有 webhook、没有 SSE/WebSocket、没有回调 URL 配置。
> 所以「FMT 有新文件就通知 Java」在现有协议上**不存在**。

可用的正确做法——**增量拉取（polling with cursor）**，`fmt-client-demo` 里的
`FmtInboxPoller` 就是参考实现：

```text
每 60 秒（initialDelay 5s）
   │
   ├─ GET /api/file?sort=id            // 按 file_id 排序（file_id 里的日期+序号天然递增）
   ├─ 与本地游标（inbox/.fmt-cursor 里的 file_id 集合）比对
   ├─ 只对**没见过**的 file_id 发 GET /api/file/<id>/download → 落盘到 inbox/
   └─ 成功后才把 file_id 追加进游标（写失败只影响"下次可能重复拉"，不影响本轮）
```

必须处理的四件事（否则轮询会变成事故）：

| 问题 | 做法 |
|---|---|
| 重复消费 | 游标持久化到文件，重启不重复拉；下载成功才记账 |
| 漏文件 | 每轮都拉**全量列表**再比对（`page_size=0` 不分页）；不要只拉"最新 N 条" |
| 顺序 | `sort=id`；下载可以并发，但业务侧要能容忍乱序到达 |
| 频率 | 默认 60s，且**默认关闭**（`demo.pull-enabled=false`）——轮询是资源，不是免费的 |

如果业务确实需要**秒级/事件级**的 FMT → Java 通知，只有两条路：
① 给 fmt.exe 加出站 webhook（见 §8.2，要改 C++）；② 换共享存储/消息队列，让 FMT 把文件
放到一个 Java 也能读到的地方（例如 FMT 数据根本身就在本机，直接读 `repository/`——
但**不要**绕过服务去写 `data/*.json`，那会破坏单写入者模型）。

### 5.3 大文件

| 方向 | 内存 | 断点/完整性 |
|---|---|---|
| 上传（`byte[]`） | 不整块进内存（`BodyPublishers.ofByteArray`） | 带 `Content-Length`，服务端可提前拦超限 |
| 上传（`InputStream`） | 不整块进内存 | `chunked`；**不参与 401 重试** |
| 下载/预览 | 流式写盘（`BodyHandlers.ofInputStream` + `transferTo`） | 先写 `<name>.part`，校验 `Content-Length` 一致后 `Files.move` 到目标；不一致/中断则删掉半成品，抛 `FmtTransportException` |

#### 5.3.1 大文件分片（v3.0.0）

`upload(...)` 仍然是一次 HTTP 请求；超过 FMT 的 `max_upload_size`（默认 50 MB）时用
`uploadLarge(...)`：

```java
try (FmtClient fmt = new FmtClient("lazy", "123456", "pet")) {
    FmtFilePackage pkg = fmt.uploadLarge(
            Path.of("D:/big.rar"), "big.rar",
            FmtLargeUploadOptions.builder()
                    .chunkSize(8L * 1024 * 1024)
                    .progress(p -> System.out.printf("%d%% %d/%d%n",
                            p.percent(), p.chunkIndex() + 1, p.chunkCount()))
                    .build());
}
```

流程：客户端先算整文件 MD5 → `POST /api/upload/init`（带 `file_name/size/md5`）→ 按
`received`/`missing` 逐片 `PUT`（每片带 `X-Chunk-MD5`）→ `POST .../complete`。
服务端会拼接、逐片复核、整文件 MD5 校验，再按与普通上传相同的规则入库（同内容秒传、
同名不同内容自动 `name01.ext`）。

断点续传与暂停：

```java
FmtUploadSession s = fmt.largeUploadStatus(uploadId);
if ("paused".equals(s.state()) || s.failed()) {
    fmt.resumeLargeUpload(path, uploadId);       // 只补 missing 里的分片
}
fmt.pauseLargeUpload(uploadId);
fmt.cancelLargeUpload(uploadId);                 // 取消并清理临时分片
```

边界：文件大小必须与会话一致；`complete` 失败（分片损坏 / MD5 不符）服务端返回
`409 + FMT-806`，不会用 200 假装成功；HTTP 会话的 `resume` 只切状态，缺片由客户端重传；
会话默认 24h 过期、单文件 8 GB、每用户 4 个活动会话。

### 5.4 并发与幂等

- 通道可跨线程共享；并发上限由 `maxConnections` 控制（默认 16，对本地服务足够）。
- 幂等性边界：`GET` 与 `DELETE ?dry_run=1` 天然幂等；`upload` 每次都是新的一次提交
  （**本次改动（未提交） 起服务端只看同一个 Bucket，而且先看内容**：桶内已有相同内容
  （不管名字同不同）→ `FMT-304`「文件已存在」，**不要求改文件名**；内容不同而文件名相同
  → `FMT-105`「文件名重复」，提示改文件名；别的桶里有同内容/同名完全不干扰），
  所以**不要**在业务层无脑重试上传——重放同一个文件会拿到 `FMT-304`
  （「这份内容已经有了」）而不是「已经存过、跳过」。
- 服务端所有写操作都走"单一业务进程 + 文件锁"，客户端不需要自己加分布式锁；但**跨请求的多步
  业务**（例如"列出→下载→标记已处理"）要在 Java 侧自己保证幂等。

---

## 6. 错误模型（两层，边界写死）

```java
// 1) 业务错误：服务端明确给了结论 —— 不抛异常
FmtResult<FmtFile> got = fmt.getFile("a.txt");
if (got.isFailure()) {
    FmtError error = got.error();
    error.code();        // "FMT-002"
    error.message();     // 服务端的中文说明
    error.httpStatus();  // 404
    error.exitCode();    // 3（与 C++ 侧 exit_code() 对齐，便于脚本分类）
    error.isConfirmRequired(); // FMT-016：补一个 force 就能继续
}

// 2) 传输/配置错误：连不上、响应不是信封、token 没配 —— 直接抛
//    FmtTransportException / FmtConfigurationException
```

- `FmtResult.orThrow()` 给"失败就该中断"的场景一个异常出口（抛 `FmtApiException`），
  由 starter 自动注册的 `FmtExceptionAdvice` 统一兜住（Servlet Web 应用；`fmt.exception-advice=false`
  可关闭，自己声明同类型 bean 也会覆盖）。宿主不用再复制这个类。
- 错误码表 `FmtErrorCodes` 与 `include/fmt/common/error.hpp` **一一对应**（编号照抄，仅追加）。
- **不要**用 `message` 做判断（服务端会改措辞），要判断就判断 `code`。

---

## 7. Spring Boot 集成

```yaml
fmt:
  enabled: true
  base-url: http://localhost:4122   # 别写 127.0.0.1（见 §2.1 地址族那条）
  # ---- 方式 A：账号 + 密码（2.1.0 起；与 token 二选一）----
  username: user
  password: your-password
  # bucket: work                  # 可选；不填用服务端 current_bucket
  # ---- 方式 B：固定 token ----
  # token: <32 位十六进制>
  # bucket: work                  # 可选：token 模式也显式带库名
  auth-scheme: X_FMT_TOKEN       # X_FMT_TOKEN | BEARER | BOTH
  connect-timeout: 3s
  request-timeout: 30s
  stream-timeout: 30m            # 0s = 不限制
  max-connections: 16
  retry-on-unauthorized: true
  exception-advice: true         # Servlet Web 应用里自动注册 FMT 异常 → JSON（默认开）
  headers:                       # 过网关时可能需要
    X-Request-From: billing
```

> **2.2.0 起**：starter 只认 `fmt.token` 或 `fmt.username + fmt.password`；
> 两者都没配会**启动失败**。`fmt.data-root` / `fmt.token-file` / `FMT_TOKEN` / `FMT_ROOT`
> 不再由 starter 读取（核心包 `FmtClientCore` 仍保留）。

| bean | 类型 | 覆盖方式 |
|---|---|---|
| `fmtChannel` | `FmtChannel`（`destroyMethod=close`） | 自己声明一个 `FmtChannel` bean |
| `fmtClient` | `FmtClient`（包装上面的通道，生命周期不重复关闭） | 自己声明一个 `FmtClient` bean |
| `fmtCodec` | `FmtCodec` | 自己声明；有 `ObjectMapper` 时默认用 Jackson |
| `fmtLogger` | `FmtLogger` | 自己声明；默认桥到 slf4j（`org.lazy.fmt.client`） |
| `fmtHealthIndicator` | Actuator 健康检查 | `management.health.fmt.enabled=false` |
| `fmtExceptionAdvice` | `FmtExceptionAdvice`（Servlet Web） | `fmt.exception-advice=false`，或自己的同类型 bean |

健康检查的输出分两件事，便于运维分流：

```json
{"status":"UP",
 "details":{"fmt":{"status":"UP","baseUrl":"http://127.0.0.1:4122",
                   "authenticated":true,"root":"D:/Tools/fmt","version":"2.0.0","pid":4321}}}
```

- `ping` 失败 → `DOWN`（服务没开 / HTTP 没打开 / 端口不对）；
- `ping` 通过但 `authenticated:false` → `DOWN`（**token 配错**，details 里有 `FMT-018` 与提示）。

---

## 8. 演进：什么时候该改 fmt.exe

### 8.1 会话模式（2.1.0 已落地）

2.1.0 起实现了「账号密码换临时会话 token」，但**没有**另造 `X-FMT-Session` 头：
会话 token 与账号 token 共用 `X-FMT-Token` / `Authorization: Bearer`，服务端先查内存
会话表，未命中再按旧账号 token 校验。

```text
GET  /api/auth/salt?username=<用户名>     （公开）
POST /api/auth/login                      （公开）
     {"username":"…","password_digest":"<64 hex>","bucket":"work"}
  → {"token":"<会话 token>","token_type":"session","expires_at":"…","bucket":"work"}
POST /api/auth/logout                      （要 token）→ 会话立即失效
```

落地细节：

| 项 | 实现 |
|---|---|
| 会话表 | `ServerRuntime` 内存 `token → {username, bucket, expires}`，`session_mutex_` 保护 |
| TTL | **2 小时**；过期返回 `401 + FMT-019`；登录/校验时顺手清过期项 |
| 服务重启 | 会话全丢（内存），客户端用保存的账号密码自动重登 |
| 旧 token | 完整保留；不带 `X-FMT-Bucket` 仍用 `current_bucket` |
| 库名 | 登录时可选；业务请求 `X-FMT-Bucket` 也可省略（默认会话桶），带了就必须与会话桶一致 |
| 并发 | 会话锁与业务锁分开；登录/校验不长时间持业务锁 |

在「只监听 localhost」的现状下，它不增加传输层安全性，但把「账号密码 → 临时钥匙」的
使用方式做成了与 MySQL 类似的体验，且与 7 天账号 token 解耦。

### 8.2 FMT → Java 的真推送（webhook）

```text
POST /api/hook/register   {"url":"http://127.0.0.1:8080/hooks/fmt","events":["file.uploaded","file.deleted"],"secret":"…"}
→ 服务端保存 hook；事件发生时：
POST <url>  X-FMT-Signature: HMAC-SHA256(secret, body)   负载 {"event":"file.uploaded","file_id":"…"}
   失败重试（指数退避）、超时、并发上限、失败落 log/
```
需要动 C++ 的地方：业务层事件埋点（file/share/trash 各写操作）、出站 HTTP 客户端
（WinHTTP 已在用）、重试队列（服务重启后的持久化）、开关配置。工作量明显大于会话模式，
**除非业务明确要事件驱动，否则用 §5.2 的轮询**。

### 8.3 HTTP 接口的补齐情况

**已补（本轮，服务端 + SDK 同步）**：

1. `DELETE /api/trash/empty` —— 客户端终于能清空回收站；有内容必须 `force`，
   空站直接成功、不为空操作要确认（`?dry_run=1` 预检）。
2. `GET /api/config` —— 客户端上传前能先看 `max_upload_size`，少踩一次 `FMT-303`；
   **响应里永远没有 token**（连"要不要带"的开关都不提供）。
3. `GET /api/bucket` —— 客户端能知道自己正在操作哪个桶（只读列表）。

**仍然建议保留在 CLI（故意不做写接口）**：桶的创建 / 切换 / 删除。理由见 §2.4 第 1 条。

---

## 9. 安全清单

| 项 | 做法 |
|---|---|
| token 不进日志 | 通道只打方法/路径/状态码/耗时；`describe()` 与健康检查里都是掩码 `1a28…693f`；有专门的用例钉住这一点 |
| token 不进 URL | 一律走 header（放 query 会被日志、代理、浏览器历史记录） |
| token 不进异常消息 | 传输异常只带方法、路径与底层原因，不带 header |
| 不跟随重定向 | `Redirect.NEVER`，避免把 token 带到别的地址 |
| 传输范围 | 默认 `localhost:4122`（见 §2.1 的地址族说明）；**不要**把 FMT 的 `host` 改成 `0.0.0.0` 去"方便远程调试" |
| 远程接入（如果非要） | ① SSH 隧道 / VPN；② 前置反向代理做 TLS + IP 白名单 + 限流；③ 那时再上 §8.1 的会话模式 |
| 明文 HTTP 的边界 | 本机回环不出网卡，可接受；跨机必须 TLS（FMT 自己不提供 TLS，靠前置代理） |
| 凭证轮换 | 换 token 后无需重启 Java：`token-cache-ttl` 到期自动重读，或任意请求收到 401 立即刷新 |

---

## 10. 测试与验证

### 10.1 单测覆盖（`mvn test`，不需要真的 FMT）

`StubFmtServer` **照抄真服务端的认证与信封行为**（`/api/*` 先验 token、公开路径、`401+FMT-018`、
`ok/data|error` 信封、未登记路由 `404+FMT-017`），因此下面这些是**真 HTTP 实测**：

| 用例 | 验证什么 |
|---|---|
| `pingIsPublicAndCarriesNoCredential` | `ping` 不带凭证也能通（token 配错时探活仍有效） |
| `channelInjectsTokenOnEveryAuthenticatedCall` | 每个认证请求都带上通道的 token |
| `bearerScheme…` / `bothScheme…` | `Authorization: Bearer` 与两种都发 |
| `unauthorizedTriggersCredentialRefreshAndExactlyOneRetry` | 401 → 作废 → 重取 → 只重试一次，且两次用的 token 不同 |
| `unauthorizedIsReportedAsFmt018WhenRetryIsDisabled` | 关掉重试就只发一次，错误码/状态码/退出码正确 |
| `businessErrorBecomesResultFailureAndOrThrowThrows` | 业务错误不重试、`orThrow()` 抛 `FmtApiException` |
| `responseWithoutEnvelopeBecomesTransportException` | 响应不是 FMT 信封时明确报错 |
| `connectionRefusedGivesActionableTransportError` | 连不上时的提示里带"检查 fmt.exe 与 `config/server.json`" |
| `listFilesEncodesQueryAndParsesPage` | 查询串百分号编码 + 分页字段解析 |
| `chineseFileNameIsPercentEncodedInThePath` | 中文文件名的路径段编码（服务端看到的是编码后的路径） |
| `uploadBytesCarriesNameAndBody` / `uploadLocalFileViaStreamUsesItsName` | 上传的字节、`?name=`、`Content-Disposition`（本机文件由调用方开成流，**SDK 不收路径**） |
| `uploadStreamIsChunkedAndArrivesIntact` | 128KB 流式上传 `chunked` 后字节一字不差 |
| `downloadStreamsToTargetAndReadsUtf8FileName` | 落盘内容 + 从 `filename*=UTF-8''` 解析中文名 |
| `downloadFailureLeavesNoPartialFile` | 失败不留文件、不留 `.part` |
| `downloadRefusesToOverwriteUnlessToldTo` | 覆盖策略与"本地就拦住，不发请求" |
| `truncatedDownloadIsRejectedAndCleanedUp` | `Content-Length` 与实际不符 → 拒绝 + 清理 |
| `checkDeleteUsesDryRunAndReportsAmbiguity` / `deleteSendsForce…` | `dry_run` / `force` 语义 |
| `createSharePostsJsonBodyAndParsesShare` | 请求体 JSON 与 `Content-Type` |
| `downloadShareNeedsNoToken` | 公开分享下载**故意**不带 token |
| `emptyTrashSendsConfirmFlagsAndParsesCounts` / `emptyTrashDryRunReportsWhatWouldGo` / `emptyTrashWithoutForceSurfacesFmt016` | 清空回收站：必须打 `/api/trash/empty` 这条独立路由、`dry_run`/`force` 语义、缺确认就是 `FMT-016` |
| `getConfigNeverAsksForOrCarriesTheToken` | 配置接口既不索取也不带回 token（就算服务端塞回来，模型里也没这个字段） |
| `listBucketsParsesCurrentBucket` | 桶列表与 `currentBucket` 解析 |
| `SimpleJsonTest` / `TokenProvidersTest` | 编解码正确性、凭证读取与缓存/作废；**老 `users[]` 形状也能读** |
| `fallsBackToTheOtherLoopbackFamilyWhenLocalhostResolvesToTheDeadOne` | 真机教训固化：stub **只绑 `::1`**，通道用 `localhost` 也要能通（换地址重试） |
| `CandidateAddressesTest` | 候选地址计算：字面 IP 只有一个、主机名保留原样并补上解析结果、端口/路径不丢 |
| `SdkModelJacksonTest`（starter） | record 模型能被宿主 Jackson 干净序列化，布尔助手不会覆盖同名字段（`bucket` 不再是 `false`） |
| `FmtLargeUploadTest`（v3.0.0） | 大文件分片：init / 逐片 PUT / 暂停 / 取消 / 断点续传 / 进度回调 / 失败会话 |

> **v3.0.0 当前**：Java 侧共 **111 项**（core 83 + starter 28），全绿；C++ 侧 **200 项**，
> `ctest` 100% 通过。

### 10.2 真机 smoke（**已实测**，2026-10-10）

现场：FMT 服务已安装并运行（服务宿主/数据根 `D:/Data/Temp/JMT/fmt`），
`config/server.json` = `{"enabled":true,"host":"localhost","port":4122}`。
本轮**部署了三次**、每次都在真机上复验（真机冒烟始终是 113 通过 / 0 失败）：

```text
第一次部署：含三条新路由的构建（旧 exe 上先跑了基线 102/10，部署后 113/0）
  SHA256 = F029F0BA9102D8C3EE1CD050F7935694C263D698F36F8BD1EC8C6D9D93D9BE75
  这一版 /api/config 还保留着 ?include_token=1 这个开关

第二次部署：按用户要求把 token 从 /api/config 彻底删掉（连开关都不提供）
  SHA256 = 02FAA2AAD3E03E51C49629A1FF6BB35A8ECA58B9F92DE24E7F7444024270C6AF
  已实测（见下）：/api/config 与 /api/config?include_token=1 **两次响应都没有 token 字段**
  真机全量冒烟再次 **113 通过 / 0 失败**；桥接自检 pid=3056

第三次部署：CLI 文案与命令总览顺序（`help share` 改说"公开下载端点已实现"；
        `(share)` 移回 `(trash)` 之后）——顺带新增的三条分享单测只动 tests/，fmt.exe 行为不变
  SHA256 = A59DB7CB961FB3024708A1232519CEAF4DD36DEC387EE4071300DE8A0D2971B6
  实测：**部署目录那个 exe** 跑 `help` → 新顺序；`help share` → 新文案；
        真机全量冒烟仍 **113 通过 / 0 失败**（第 10 节确认机主原始数据未变）；桥接自检 pid=18240

data/user.json 由新 exe 在启动时自动改写成单用户平铺形状（token 不变）
```

```powershell
# 1) HTTP 默认就是开的（本次改动（未提交）把 enabled 默认改成 true）
#    数据根/config/server.json -> {"version":1,"enabled":true,"host":"localhost","port":4122}
#    要关/改地址用 CLI（改完会自动重启服务；提权被取消时会提示手动执行）：
#      fmt.exe config set server_enabled false
#      fmt.exe config set server_host 127.0.0.1
#      fmt.exe config set server_port 4321
# 2) 手动改过 config/server.json 的话自己重启一次服务（需要管理员）
fmt.exe service stop
fmt.exe service start

# 3) 取 token
fmt.exe config list          # 打印「用户 ID / 访问 token / 建议的请求头」

# 4) 用 demo jar 打一遍 ping / status / 列文件（**不用给地址**，默认就是 localhost:4122）
java -jar java\fmt-client-demo\target\fmt-bridge.jar `
     --fmt.data-root=D:\Data\Temp\JMT\fmt --demo.smoke=true --server.port=8080
```

**实测输出**（真机原样，只有中文被控制台编码弄花的地方我按原意还原）：

```text
[fmt-smoke] 通道：http://localhost:4122 [auth=X-FMT-Token, token=1a28…693f]
WARN org.lazy.fmt.client : 连接 http://localhost:4122 被拒，改用 http://127.0.0.1:4122 重试（GET /api/ping）
WARN org.lazy.fmt.client : 连接 http://127.0.0.1:4122 被拒，改用 http://[0:0:0:0:0:0:0:1]:4122 重试（GET /api/ping）
[fmt-smoke] GET /api/ping：ok（公开接口，不需要 token）
[fmt-smoke] GET /api/status：数据根=D:/Data/Temp/JMT/fmt，版本=2.0.0，pid=23420
[fmt-smoke] GET /api/file：2 个文件（共 2）
[fmt-smoke]   · fmt-20261008-2  17364485.jpg  3889891 B
[fmt-smoke]   · fmt-20261008-0  asdva.jpg  130184 B
```

**真机全量冒烟**（`pwsh -File tools/smoke-test.ps1`，对部署的新 exe）：

```text
结果：通过 113 ，失败 0
  … 第 10 节逐项确认机主原始数据未变：asdva.jpg / 17364485.jpg / 回收站 a7.jpg 都还在，
    user.json 是单用户形状且带账号，share.json 没留测试分享，temp/ 无残留，上传上限已恢复 50MB
```

**三次部署各跑过一次，都是 113 通过 / 0 失败**（桥接自检 pid 依次 23420 / 3056 / 18240，结果一致）。

（部署前对**旧 exe** 跑同一条脚本是 102 通过 / 10 失败——那 10 项见 §10.2.2。）

**三条新接口在真机上的实测响应**（都经桥接 `fmt-bridge.jar`，不是原始 curl）：

| 调用 | 真机响应 |
|---|---|
| `GET /demo/config` | `200`，**没有 `token` 字段**、`maxUploadSize:52428800`、`currentBucket:"lazy"`、`userId:"u-b1e7c28f"` |
| `GET /demo/config?include_token=true` | **与上一行逐字节相同**：200，同样**没有 `token` 字段**（第二次部署后已复验——那个开关在源码里根本不存在了） |
| `GET /demo/buckets` | `200`，`{buckets:[{name:"lazy",current:true}],count:1,currentBucket:"lazy"}` |
| `GET /demo/trash` | `200`，条目 `a7.jpg` 的 `"bucket":"lazy"`（**Jackson 覆盖 bug 已在真机上确认修好**） |
| `DELETE /demo/trash/empty?dry_run=true` | `200`，`{files:1,buckets:0,bytes:114724,needsConfirm:true,message:"永久删除回收站里的全部 1 项…"}`——**这是 `trash.empty` 的文案，证明确实走了新路由**而不是 `trash.delete("empty")` |
| `DELETE /demo/trash/empty`（缺 `force`） | `409` + `FMT-016`（`fmtHttpStatus:400`），**回收站条目一条没少**（安全约定：只测拒绝路径，绝不对真实数据根做 force 清空） |

`/actuator/health`：

```json
{"status":"UP","components":{"fmt":{"status":"UP","details":{
  "baseUrl":"http://localhost:4122","authenticated":true,
  "root":"D:/Data/Temp/JMT/fmt","version":"2.0.0","pid":23420}},"ping":{"status":"UP"}}}
```

自检失败（连不上、token 不对）只会打印「自检未通过」，**不会**让应用起不来——
这是刻意的：FMT 是本地服务，可能后启动、可能没打开 HTTP，把那当成启动失败是错的。

#### 10.2.1 真机暴露的三个客户端缺陷（都已修 + 都补了用例）

这一趟真机 smoke 的价值几乎全在这三条上——它们在**假 FMT**（绑 `127.0.0.1`、新形状 user.json）
上永远暴露不出来：

| # | 现象 | 根因 | 修法 |
|---|---|---|---|
| 1 | `Connection refused`，但服务明明在跑 | FMT 的 `host: localhost` 让 httplib **只绑了 `::1`**；而 JDK 的 `HttpClient` 对多地址主机名**只试第一个解析结果（127.0.0.1）、不回落**。实测：`localhost`/`127.0.0.1` 都拒，只有 `[::1]` 通 | 通道把主机名的**所有**解析结果做成候选地址，`ConnectException` 时依次换一个重试并记住成功的那个（见 §4.4）；日志里就是上面那两行 WARN |
| 2 | `token 文件里没有 token 字段` | 部署的 `data/user.json` 还是**老形状** `{"version":1,"users":[{…}]}`（部署的 exe 早于「单用户文件」那次改动） | 凭证读取**两种形状都认**：先读平铺 `token`，再读 `users[0].token`。客户端不能等服务端先升级 |
| 3 | `/demo/trash` 里 `"bucket":false` | record 上的布尔助手 `isFile()` / `isBucket()` / `isTrashed()` 被 Jackson 当成 getter，与同名 record 组件**抢属性名并覆盖真实值**（`isBucket()` 赢了 `bucket()`） | 助手改名避开 getter 命名：`fileEntry()` / `bucketEntry()` / `inTrash()`；starter 里加 `SdkModelJacksonTest` 钉住「模型可以直接 return 给前端」 |

#### 10.2.2 部署的 exe 落后于工作区时，三个新接口会怎么表现

现场那台机器上的 `fmt.exe` 是 **2026-10-09 21:48** 构建的，早于本次三条路由（也早于「单用户
user.json」）。所以对着**旧 exe** 跑，桥接侧看到的是（都是**如实透传**，没有掩盖）：

| 桥接接口 | 旧 exe 的实际响应 | 原因 |
|---|---|---|
| `GET /demo/config` | `502 + FMT-602`（`"接口尚未实现：/api/config"`，`fmtHttpStatus:501`） | 旧 exe 没有这条路由，落兜底 501 |
| `GET /demo/buckets` | `404 + FMT-017`（`"没有这个接口：/api/bucket"`） | 同上，被整体删除过 |
| `DELETE /demo/trash/empty` | `409 + FMT-016`（`"永久删除不可恢复，需要确认（force = true）"`） | 旧 exe 上它落进 `trash.delete("empty")`；**没有删任何数据** |

> **当时的结论**：这三条接口必须先把新 exe 部署上去才能验（数据根不变，就是把新二进制覆盖过去
> 再重启服务，需要管理员）：
> ```powershell
> fmt.exe service stop                                                    # 需要管理员
> Copy-Item cmake-build-debug\bin\fmt.exe D:\Data\Temp\JMT\fmt\fmt.exe -Force
> fmt.exe service start
> ```
> **这一步已经做完并复验**（2026-10-10，二进制 SHA256 与工作区构建一致）：全量冒烟从
> `102 通过 / 10 失败` 变成 **`113 通过 / 0 失败`**——8 项新接口 + 2 项 `user.json` 形状
> 一起转绿（新 exe 启动时把 `data/user.json` 自动改写成单用户形状，token 不变）；
> 三条接口的真机响应见上面 §10.2 的表。**这张表留着当"旧 exe 长什么样"的对照**：
> 客户端在这种情况下的表现是**如实透传**（501/404/FMT-016），没有掩盖，也没有误删数据。

### 10.3 已经实测到的端到端结果（无需真机）

用 `java/tools/fake-fmt/FakeFmt.java`（协议照抄真服务端的最简假 FMT）
→ `fmt-bridge.jar`（打好的可执行 jar）→ 真实 HTTP，实测结果：

| 验证点 | 命令 | 实测结果 |
|---|---|---|
| 自动配置 + 凭证注入 | `GET /actuator/health` | `fmt.status=UP`、**`authenticated:true`**、`root/version/pid` 都拿到了 |
| 桥接 REST | `GET /demo/status` | `{"baseUrl":"http://127.0.0.1:14122","ping":true,"root":"D:/FakeFmt",…}` |
| 列表解析 | `GET /demo/files` | 2 条记录，字段映射正确 |
| 流式上传 | `POST /demo/files`（multipart） | 假 FMT 侧看到 `POST /api/file/upload token=test... bytes=24`，返回 `file_id=fmt-9` |
| 流式下载 | `GET /demo/files/fmt-1/download` | 落盘内容 `download-body`，`filename*=UTF-8''` 解析正常 |
| 错误映射 | `GET /demo/files/nope.txt` | 假 FMT 回 `FMT-017/404` → 桥接回 **HTTP 404** + 同形状信封（含 `fmtHttpStatus`） |
| 读配置 | `GET /demo/config` | 响应里**没有** token 字段（假 FMT 也照新口径：任何参数都不给） |
| 列桶 | `GET /demo/buckets` | `currentBucket=work` + 两个桶的 `current` 标记 |
| 清空回收站·缺确认 | `DELETE /demo/trash/empty` | 假 FMT 回 `FMT-016/400` → 桥接回 **HTTP 409** + `fmtHttpStatus:400` |
| 清空回收站·预检/执行 | `?dry_run=true` / `?force=true` | `needsConfirm:true` / `files:2`（执行分支该字段为 `null`） |

假 FMT 侧打印的每一行请求，正是「token 放进通道」的直接证据：

```text
[fake-fmt] GET /api/ping token=(none) bytes=0            ← 公开接口，刻意不带凭证
[fake-fmt] GET /api/status token=test... bytes=0         ← 其余请求都由通道注入
[fake-fmt] GET /api/file token=test... bytes=0
[fake-fmt] POST /api/file/upload token=test... bytes=24
[fake-fmt] GET /api/file/fmt-1/download token=test... bytes=0
[fake-fmt] GET /api/config token=test... bytes=0
[fake-fmt] DELETE /api/trash/empty?dry_run=1 token=test... bytes=0
[fake-fmt] DELETE /api/trash/empty?force=1 token=test... bytes=0
```


---

## 11. 落地方式与运维

| 场景 | 做法 |
|---|---|
| Maven 多模块工程内 | `mvn -s tools/maven-settings.xml clean install`，然后依赖 `org.lazy:fmt-spring-boot-starter:3.0.0` |
| 不想动私服 | `mvn install:install-file` 把两个 jar 装进本地仓库，或直接 `<scope>system</scope>`（不推荐） |
| 只在非 Spring 项目用 | 只依赖 `fmt-client-core`（零依赖，Java 17+） |
| 当独立桥接服务跑 | `java -jar fmt-bridge.jar`（demo 模块打成可执行 jar），别的系统调它的 REST |
| 版本策略 | 跟随 FMT 主版本；错误码表随 `error.hpp` 追加，**不复用编号** |

> 本机 `D:\apache-maven-3.8.4\conf\settings.xml` 里有一个 **HTTP** 的阿里云镜像，
> Maven 3.8+ 会拦截外部 HTTP 仓库。仓库里放了 `java/tools/maven-settings.xml`（HTTPS 的 Central）
> 用于覆盖：`mvn -s tools/maven-settings.xml …`。

---

## 12. 一句话总结

**jar 提供一个「通道」对象：建通道时把凭证交给它，之后所有数据（JSON 调用、流式上传、流式下载）
都从这一个通道进出，token 由通道在唯一注入点写进每个请求的 header，401 时自动换凭证重试一次。**
Java → FMT 是全能力的；FMT → Java 只能由 Java 侧增量拉取——除非你决定给 fmt.exe 加 webhook。
