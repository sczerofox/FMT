# FMT 重构设计

> 项目名称：FMT
> 项目类型：Windows 文件管理系统
> 文档定位：**本次重构（V1 重构版）的差异说明与冻结决策索引**
> 状态：冻结，作为阶段 2 / 阶段 3 的编码依据；**阶段 2～5（上）已实现并提交**
> （阶段 4 = Bucket，commit 32249ea；**Bucket 删除/回退的回收站形状与桶级 `trash list` / `trash restore`
> 随后落地，commit c2d545d**；桶级 `trash get` / `trash delete` 收尾于 `4fee290`；
> **阶段 5 的 `file` 四条命令与上传两段式在 commit `188e85d` 落地**），
> 下一步是阶段 5 剩下的 `share` 与阶段 7 的**文件级** trash 读取侧
> 分支：`arch-restart`（基线 `bdbe33d`，旧实现保留在 `dev`）

本文档不替代三份主文档，只回答一个问题：**相对旧设计，这次改了什么、为什么改、按什么顺序做。**

| 文档 | 定位 |
|---|---|
| `FMT 开发文档.md` | 规范主体：模块职责、数据结构、命令、流程、测试要求 |
| `FMT 技术文档.md` | 技术实现：技术栈、接口、协议、实现方式 |
| `FMT 项目架构.md` | 总体架构、分层、冻结决策、错误码清单 |
| `FMT 重构设计.md`（本文档） | 本次重构的差异与决策索引 |
| `项目基本提示词.txt` | 项目背景，不是规范依据 |

---

## 1. 重构背景与范围

旧设计（`dev` 分支）把 Windows Service 排在开发顺序的第 9 阶段，且把 CLI 当成「HTTP 客户端」：
CLI 与服务之间的唯一通道是 `127.0.0.1:4122`，数据根固定为「服务 exe 所在目录」。

本次重构把「双击即用」提到最前面，并重新划分了进程与通道：

- **服务生命周期与 CLI 交互成为阶段 2 / 3 的内容**，业务功能（bucket / file / share / trash）排在后面。
- CLI 不再走 HTTP，改走**命名管道**直连服务；HTTP 只留给浏览器。
- 数据根不再由服务自己决定，改由 **CLI 声明**，因此「把 exe 复制到新目录双击」就会得到一套新的数据目录。
- 服务生命周期命令**四条动作命令都通过 UAC 提权**（`install` / `uninstall` / `start` / `stop`），
  并在 CLI 里以固定格式回显；**`status` 是查询命令，不提权、不弹 UAC**。
- CLI 不再只是「只读客户端」：**双击时它先对自己所在数据根做一次幂等体检与补齐**（只补缺失的目录与
  默认 JSON，不碰业务数据内容），服务与 CLI 共用同一套 `ensure_root` / `check_root` 规则。
- 服务启动失败时把 FMT 编号写进 `dwServiceSpecificExitCode`，CLI 能读回并打印
  `服务启动失败：FMT-008 配置错误`，而不是只会说「启动失败」。
- `hello` 响应新增 `switched` / `previous_root`，CLI 据此记一行日志 `[Service] 数据根切换：旧 -> 新`（只进日志）。

范围外（本阶段不做）：`pause`（本次确认不需要）、HTTP 客户端 CLI、多用户与权限系统。
业务命令**按阶段推进**：阶段 4 已完成 `bucket`（决策 19～23 与第 4.2、4.3 节的参数形状就来自它），
并在同一个阶段内落地了**桶级回收站的全部四条命令**（`trash list` / `get` / `restore` / `delete`、
`trash/<user>/.original`、`file.json` 的 `trash_reason`，决策 19 已按此改口径；
`get` / `delete` 在收尾提交 `4fee290` 补齐，永久删除要显式确认）；
阶段 5 做 `file` / `upload` / `share` 与**文件级**回收站条目（**上半已完成**：提交 `188e85d`
落地了 `file` 四条命令、上传两段式、MD5 去重与软删除写文件级条目；`share` 未开始），
阶段 6 做 HTTP 浏览器侧与 Preview，
阶段 7 做**文件级**的 `trash get` / `trash delete`（永久删除）、文件级恢复，以及永久删除时的
share 清理（第 10 节）。

> 第 2 节冻结决策一览里的 **19～21** 来自阶段 4（Bucket 与参数形状），
> **22～23** 来自阶段 5 上半（长任务不持锁、上传来源与文件名作用域，提交 `188e85d`）。

---

## 2. 冻结决策一览

| # | 决策 |
|---|---|
| 1 | 单一 `fmt.exe`，三种形态：CLI 形态、Service 形态（被 SCM 启动）、提权短命副本。manifest 为 `asInvoker`，**不是** `requireAdministrator` |
| 2 | service 命令有**五条**：`install` / `uninstall` / `start` / `stop` / `status`，**无 pause、无 delete**（`delete` 更名 `uninstall`），命令不带 `--` 前缀；`status` 是只读查询 |
| 3 | **四条动作命令**（`install` / `uninstall` / `start` / `stop`）都走 UAC 提权（不做「已满足状态就免提权」的优化）；`status` **不提权、不弹 UAC** |
| 4 | CLI ↔ 服务：命名管道 `\\.\pipe\fmt.control`；浏览器 ↔ 服务：HTTP `127.0.0.1:4122`；两者进同一个 service 层 |
| 5 | 数据根（`FMT_ROOT`）由 CLI 连接时声明，服务维护「当前数据根」；切换不删旧数据 |
| 6 | 初始化（建目录 + 默认 JSON）**由 Service 与 CLI 共用同一套幂等规则**（同一份 `ensure_root` / `check_root`）：服务在启动/换根时执行，CLI 在双击时对自己的数据根执行；**两边都只补缺失、都不删不改不覆盖**，已有的 JSON 会被读一遍确认完整性，损坏只报告不重置（见第 5.2、8 节） |
| 7 | 服务自身状态写在 `%ProgramData%\FMT\service.json`，不属于业务数据 |
| 8 | 服务：名 `FMT`，显示名 `FMT File Management Service`，`SERVICE_AUTO_START`，`LocalSystem`，Recovery 5s/10s/30s、失败计数 1 天重置 |
| 9 | 服务不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`；`HandlerEx` 只处理 `STOP` / `SHUTDOWN` / `INTERROGATE` |
| 10 | CLI 单实例：互斥体 `Local\FMT.CLI.v1`，已有实例则激活已有窗口，不新建窗口 |
| 11 | 服务宿主 = 首次安装时注册的 exe 绝对路径（不复制到别处）；**移动请用复制** |
| 12 | 依赖：vendor `nlohmann/json` + `cpp-httplib` 到 `third_party/`，`/MT` 静态链接 CRT，产物只有一个 `fmt.exe`。**URL 下载另用系统库 `winhttp`**（提交 `a2b6cd1`：WinHTTP + Schannel，系统组件、不产生额外 DLL，见第 2 节第 23 项） |
| 13 | CLI 横幅 `File Manager Tool  v1.0  ( build  2026.10.08 )`（与 `--version` 同一串）+ `Service Running...`，提示符 `fmt> `（打印前先空一行，空命令不重复空行），正常→stdout / 错误→stderr |
| 14 | 输出固定为 `执行成功...` / `错误码：0`，失败为 `执行失败：FMT-601 ...` / `错误码：8` |
| 15 | 运行目录是**六个**：`repository/` `trash/` `config/` `data/` `log/` `temp/`。`temp/` 是临时文件目录（既不是业务数据、也不是日志，随时可清）；提权结果文件与上传暂存都放这里，数据根不可写时才退回 `%TEMP%` |
| 16 | 程序横幅名 `File Manager Tool`，版本部分 `v<MAJOR>.<MINOR>`（当前 `v1.0`），构建日期由 CMake 配置时生成（`FMT_BUILD_DATE`，`%Y.%m.%d` 本地时间）→ `banner_text()` 一处产出，横幅与 `--version` 共用；用法标题是 `用法：fmt.exe [命令]` |
| 17 | **控制台只留交互**：双击时数据根体检结果（新建目录 / 新建文件 / 完整）与「服务当前状态」**只进日志**；只有异常（无法补齐、文件损坏）走 stderr |
| 18 | 帮助有两个入口：`--help` 打印带横幅与退出码表的完整用法；`help` 不带参数只列命令总览、`help <组>` 打印该组详情（`service` / `bucket` / `file` / `share` / `trash` / `help` / `exit`），交互与一次性都支持；`help <未知组>` → stderr 一行 + `FMT-001` / 退出码 2。`exit` / `quit` 是正式命令 |
| 19 | **Bucket 就是目录，没有独立 ID**：`repository/<user>/<bucket>/` 这个目录就是 Bucket，存在性 = 目录存在；`current_bucket` 存在 `config.json` 里（只存名称）。删除 = 把整个目录移到 `trash/<user>/<bucket>_<YYYYMMDDHHMMSS>/`，**不丢弃数据**；回收站目录名**一律带删除时间戳**（同一秒内删第二次、或目录恰好同名时再加 `_2` 序号），所以同一个桶删多少次都不会互相覆盖。**桶级身份记录的唯一权威是 `trash/<user>/.original`**（`{"version":1,"buckets":[{"trashed","original","deleted_at"}]}`）：一律查这张表，**绝不靠剥离时间戳反推原名**；`data/trash.json` 不再记桶级条目。回退是**整单判定**：目标 `repository/<user>/<原名>` 已存在就整单拒绝（`FMT-401 RestoreConflict`，不覆盖、不改名、不把不冲突的文件先塞进去），不存在就整个目录一次 `std::filesystem::rename` 搬回（阶段 4 已落地 `trash list` / `trash restore`，commit c2d545d；**`trash get` / `trash delete` 在收尾提交 `4fee290` 补齐**：`get` 复用同一套定位并报 `{present, path, files, bytes}`（索引有、目录没了不报错，只如实报 `present:false`），`delete` 是**永久删除、必须显式确认**，顺序为「先删目录 → 再清 `file.json` 里 `trash_reason="bucket"` 的记录 → 最后摘 `.original`」，文件级（`"file"`）记录绝不动）。**文件级条目另有落点**：`trash/<user>/.files/<bucket>/YYYY/MM/DD/<file>`（提交 `4fee290`）——`trash/<user>/` 顶层只留给桶级条目 `trash/<user>/<名字>_<14 位时间戳>/`，桶级扫描跳过点开头条目、且只认这个形状的目录，所以文件级的桶目录不会被误当成「孤儿桶条目」 |
| 20 | **`current_user` 用占位名**：V1 没有用户系统，数据根初始化时若 `current_user` 为空就自动置 `user` 并保存（需求原文「不做用户先用 user 代替」），磁盘上是 `repository/user/<bucket>/…`。**不需要用户先设置**；`FMT-604 NoCurrentUser` 保留给「用户被显式清空」 |
| 21 | **两条入口一套参数**：管道 `op = "<组>.<动作>"`（如 `bucket.create`）+ 位置参数放 `args.argv`；HTTP 路由固定，请求体接受 `{"name":"工作"}` 或 `{"argv":["工作"]}`，**路径参数里的中文由服务端 `url_decode` 解码**（`common/string` 的 `url_encode` / `url_decode`）。错误码 → HTTP 状态码的映射一并冻结（400 / 403 / 404 / 409 / 500 五档，见技术文档 12.5） |
| 22 | **长任务不持锁（提交 `188e85d` 落地）**：上传拆成两段——`prepare_upload()`（下载/复制到 `temp/`、边写边算 MD5、边判大小上限）在**业务锁外**跑；`commit_upload()`（MD5 去重 → 文件名冲突 → 分配 `file_id` → 搬到仓库 → 写 `file.json`）在**业务锁内**跑。运行体 `ServerRuntime::run_upload()` 编排这三步（锁下取快照 → 锁外 prepare → 锁内 commit），**管道与 HTTP 共用这一份**；`file.upload` 是唯一不落在 `execute_business()` 里的 op。没有引入按 JSON / 按 `file_id` 的细分锁 |
| 23 | **上传支持 `http://`、`https://` 或本机路径（提交 `188e85d`，`a2b6cd1` 再改口径）**：**`https://` 现在支持**——原口径「`https://` 返回 `FMT-300 UrlInvalid`（V1 不引入 OpenSSL）」**已被 `a2b6cd1` 推翻**。下载改由 `include/fmt/common/http_client.hpp` + `src/common/http_client.cpp` 的 **WinHTTP + Schannel** 流式 GET 客户端完成（`src/common/CMakeLists.txt` 链 `winhttp`；`src/file/CMakeLists.txt` **不再链 cpp-httplib**，浏览器服务端仍用它）。**为什么换**：`cpp-httplib` 的 `Client` 走 https 必须 OpenSSL，自己编要 Perl + NASM，破坏「只依赖 vendored 单头文件、离线可构建」；静态链接 OpenSSL 虽不引 DLL，但 `/MT` 与常见 `/MD` 静态包混 CRT 会出问题。WinHTTP 是**系统组件**，TLS 走 **Schannel**（系统证书库），**不分发任何 DLL**，还自动使用系统代理（`AUTOMATIC_PROXY`，失败退回 `DEFAULT_PROXY`）。**判定顺序**：`http://` 或 `https://` → 下载；含 `"://"` 但不是 http/https → `FMT-300`（**不是 `FMT-002`**）；其余 → 本地路径，存在性不过 → `FMT-002`。行为：只有 2xx 的响应体交给 sink（非 2xx 直接丢弃）、sink 返回 false 即中止（返回成功且 `aborted=true`，被拒的那块不计入 `bytes`）、显式要求 `Accept-Encoding: identity`（服务器仍压缩 → `FMT-301`）、超时 → `FMT-302`、证书类失败读 `WINHTTP_OPTION_SECURITY_FLAGS` 给准提示、URL 带用户名密码不接受。`file.upload` 用 `ipc::kUploadTimeoutMs = 30 分钟`（其余命令仍 30 秒，见第 9 节末）。**文件名可省略**：省略时从来源推断（砍 `scheme://host` → 去查询串/锚点/结尾斜杠 → 取最后一段 → 百分号解码），**绝不拿主机名当文件名**；推断为空 → `FMT-100`，要求用户显式给名字。**MD5 去重与重名判断的作用域都是「同用户 + 任何 Bucket + 正常文件」**（不是桶内），分别回 `FMT-304` / `FMT-105`，都不自动改名 |
| 24 | **Bucket 名称统一小写（提交 `9c3d2cb`）**：`BucketService::create()` 先把名称按 **ASCII 折叠**转小写、再校验、再建目录（`create WORK` 建成 `work`），返回值从 `Status` 改成 `Result<BucketCreation>`（`{requested, name, renamed, became_current}`），`renamed` 为真时 `bucket.create` 的 `data` 带 `note`（「Bucket 名称统一使用小写：已把 WORK 转为 work」）、CLI 单独打一行 `提示：…`；`use` / `get` / `delete` 一律经 `canonical_name()` 规范化到**磁盘上的实际名字**（不区分大小写地扫桶目录，找不到才退回小写形式），所以 `current_bucket`、`file.json` 的 `bucket`、`.original` 的 `original` **只有一份拼写**；`bucket.use` 的 `bucket` 字段回的是规范化后的名字，被改过时也给 `note` |
| 25 | **`fmt-YYYYMMDD-N` 是保留形状（提交 `9c3d2cb`）**：文件名与 `file_id` 同形会被 `validate_file_name()` 拒绝（新错误码 `FMT-106 FileNameLikeFileId`，退出码 2，判定函数 `looks_like_file_id()`），理由与 Windows 保留设备名同类——定位是「先按 `file_id` 查、查不到再按名字查」，同名会遮住名字这一路。旧数据里已经存在的这种名字，`file delete` 在两个索引命中**不同**记录时报 `FMT-001`，消息里点名两条记录并让用户直接用 `file_id`；**只给 delete 加**（`locate_record()` 只被 `remove()` 用），`file get` 的两种查询范围保持不变（只读查询最坏是把 id 命中的那条给用户看，破坏性操作不能猜）。`file get` 对回收站里的记录另返回 `trash_path`（相对数据根、正斜杠），仓库里找不到时**不返回** `path` |


---

## 3. 架构总览

```text
                       ┌──────────────────────────────────────────────┐
浏览器 HTTP/HTTPS ─────→│  HTTP Server  127.0.0.1:4122                 │
                       │        │                                      │
CLI 窗口 ──命名管道────→│        ▼                                      │
(\\.\pipe\fmt.control)  │   fmt service 层（Bucket / File / Share /     │
                       │                   Trash / Config）            │
                       │        ▼                                      │
                       │   fmt core 层（路径 / 事务 / file_id / MD5）   │
                       │        ▼                                      │
                       │   storage（当前数据根下的 JSON 与文件）        │
                       └──────────────────────────────────────────────┘
                                 ↑ 同一个 fmt.exe 的 Service 形态
                                 └ 由 SCM 启动，开机自启，全局唯一一个

CLI 形态：不碰 core 业务操作、不写业务 JSON；双击时先对自己所在数据根跑一次共用的幂等体检与补齐
          （只补缺失的六个目录与六个默认 JSON，见第 5.2 节），此外只解析命令 + 走管道 + 写日志
service 命令：本机直连 SCM；`install` / `uninstall` / `start` / `stop` 走 UAC 提权，
              `status` 是只读查询、不提权（两者都不走管道、不走 HTTP）
```

要点：

- **服务是全局单例**，CLI 只是客户端。任何时刻只有一个进程写 `data/*.json`，旧设计里「CLI 与服务同时改同一个 JSON」的问题从根上消失。CLI 唯一会碰目录结构的动作，是双击时对自己数据根跑一次共用的幂等补齐（只补缺失，不改业务数据内容）。
- 两条通道汇入同一个 service 层，所以浏览器与 CLI 的业务行为天然一致（旧文档冻结规则「CLI 与 HTTP 使用统一业务核心」继续有效）。
- `service` 五条命令**必须直连 SCM**：服务尚未安装时走任何进程间通道都是死锁；其中 `status` 只是查询，连提权都不需要。

---

## 4. 进程与通道

### 4.1 入口分发

```text
wmain
 ├─ StartServiceCtrlDispatcherW({ L"FMT", ServiceMain })
 │     ├─ 成功                                            → Service 形态
 │     ├─ 失败且 ERROR_FAILED_SERVICE_CONTROLLER_CONNECT  → 用户启动 → CLI 形态
 │     └─ 其它错误                                        → FMT-602 / 退出码 8
```

- SCM 只给 30 秒连接窗口，所以 `wmain` 必须**尽早**调用 `StartServiceCtrlDispatcherW`。
- `ServiceMain` 里初始化慢时先 `SetServiceStatus(SERVICE_START_PENDING)` 并上报 `dwCheckPoint`，否则 SCM 报 1053。
- `ServiceMain` 里初始化**失败**时**不把异常抛给 SCM**：把 FMT 编号（**不是退出码**）写进
  `SERVICE_STATUS::dwServiceSpecificExitCode`，置 `dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR`、
  `dwCurrentState = SERVICE_STOPPED`。CLI 通过统一查询接口读回该编号并映射成错误码，
  于是能打印 `服务启动失败：FMT-008 配置错误`。判定「数据根/配置类问题」的错误码集合见第 5.4 节。
- 提权副本不是独立的入口分支，而是 CLI 形态收到 `--elevated` 内部参数后的短命分支。

**统一的状态查询接口与「等它落定」**（所有读 SCM 状态的地方都走这里）：

```text
service::State { NotInstalled, Stopped, StartPending, StopPending, Running,
                 ContinuePending, PausePending, Paused, Unknown }
service::StatusInfo { state, wait_hint_ms, win32_exit_code, service_exit_code }
service::query_status()  -> Result<StatusInfo>
       「未安装」是**正常结果**（state = State::NotInstalled），不是错误；
       只有查询本身失败（打不开服务控制管理器等）才返回错误（FMT-602 / 8）
service::query_state()         只是 query_status() 的薄封装（取 state，失败给 Unknown）
service::installed_binary_path() -> Result<std::string>（服务宿主 binPath，去掉引号）
service::last_start_failure()  -> Result<ErrorCode>
       也基于 query_status()，读 win32_exit_code / service_exit_code；没有失败信息则返回错误
```

> 名字别混：`State` 是运行状态枚举；`ServiceState` 是 `service.json` 的结构体
> （`current_root` / `host_path` / `installed_at`，由 `load_state()` 读取）。

旧的「`QueryServiceStatus` + `map_state`」写法已被取代。`service status` 命令、
`service start` / `service stop` 的落定判定、双击引导读失败编号，三处都只走这一个入口。

**等待落定按 SCM 的 `dwWaitHint` 自适应，不写死时长**（实现是 CLI 内的 `settle_state()`）：

```text
服务处于 State::StartPending / State::StopPending 时，每轮查询读 SCM 给的 dwWaitHint，
把它夹在 100 ms – 2000 ms 之间作为下次查询的间隔；兜底上限 30 秒（kSettleCapMs）。
状态一旦不是等待类（Running / Stopped / Paused / NotInstalled …）就立即结束等待；
settle_state() 返回落定后的 State，回来仍是等待类就说明「没起来」。
```

理由是写死 8 秒在慢机器上会把「还在启动」误判成「启动失败」，然后白弹一次 UAC 去做根本
解决不了问题的重装。**等一下多久，由 SCM 自己说**——`dwWaitHint` 就是服务自己上报的
「预计还要多久」。

### 4.2 CLI ↔ 服务：命名管道

```text
管道名   \\.\pipe\fmt.control
类型     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT
实例数   PIPE_UNLIMITED_INSTANCES（但服务端一次只 accept 一条连接，见下）
帧格式   [4 字节小端长度][UTF-8 JSON]，一请求一响应，用 id 配对
请求     {"id":7,"op":"hello","root":"D:\\FMT2","pid":1234}
         {"id":8,"op":"bucket.create","args":{"argv":["工作"]}}
响应     {"ok":true,"data":{...}}
         {"ok":false,"error":{"code":"FMT-201","message":"Bucket 已存在：工作"}}
```

**连接是严格串行的**（阶段 4 实现为准）：服务端只有一条 accept 循环，
`accept` 一次建立一条连接，然后在这条连接上「读一个请求 → 处理 → 写一个响应」，
**客户端断开才回到 accept**——所以同一时刻只有一条 CLI 连接（配套决策 10），
第二个客户端会拿到 `ERROR_PIPE_BUSY`，客户端等 3 秒重试、最终归为 `FMT-601`。
**没有「每连接一个线程」这回事**（HTTP 那边才是线程池并发）。

**`args.argv` 是位置参数的唯一形状**（决策 21）：`op = "<组>.<动作>"`，
`fmt> bucket create 工作` → `{"op":"bucket.create","args":{"argv":["工作"]}}`。
阶段 4 落地的 op 有九种：`bucket.create` / `bucket.list` / `bucket.get` /
`bucket.use` / `bucket.delete` / `trash.list` / `trash.restore` / `trash.get` /
`trash.delete`（后两种在收尾提交 `4fee290` 补齐）；响应的 `data` 形状见技术文档 12.3.2.1
（`list` 回 `{buckets, count, current_bucket}`，`create` / `use` / `delete` 回
`message` + 相关字段，`get` 回 `{bucket, is_current, path}`；
**`bucket.delete` 的 `data` 增加 `trashed_name`，`message` 是
`Bucket 已删除（移入回收站）：<原名>  ->  <回收站名>`**；
`trash.list` 回 `{deleted_buckets:[{trashed, original, deleted_at, present}], count}`，
`trash.restore` 回 `{trashed, original, restored_to, message}`；
**`trash.get` 回 `{trashed, original, deleted_at, present, path, files, bytes}`，
`trash.delete` 回 `{trashed, original, removed_files, removed_records, message}`**）。
**提交 `9c3d2cb` 起 `bucket.create` / `bucket.use` 的 `data` 增加 `note`**（只在名称被规范化时出现，
形如 `"note": "Bucket 名称统一使用小写：已把 WORK 转为 work"`），`create` 回的 `bucket` 是**实际建成的名字**、
`use` 回的 `bucket` 是**规范化后的名字**；CLI 把 `note` 打成单独一行 `提示：…`。

**提交 `188e85d` 又冻结了四种 `file.*`**，形状同样进 `args.argv`：

```text
file.upload   args.argv = [<来源>] 或 [<来源>, <文件名>]   ← 来源 = http:// URL 或本机路径；
                                                            文件名可省略，服务端从来源推断；
                                                            显式名与推断名都不能与 file_id
                                                            同形（fmt-YYYYMMDD-N）→ FMT-106
file.list     无参数
file.get      args.argv = [<file_id 或 文件名>]            ← 先当 file_id 查（全局含回收站）、
                                                            查不到再当文件名查（当前用户 +
                                                            正常文件）；命中回收站记录时
                                                            另回 trash_path
file.delete   args.argv = [<file_id 或 文件名>]            ← 软删除，**不需要 force**；
                                                            提交 0ad9efc 起与 file.get 同一套
                                                            定位规则（locate_record()）；
                                                            提交 9c3d2cb 起两者同时命中
                                                            **不同**记录 → FMT-001 报歧义
```

`file.upload` 是唯一不落在 `execute_business()` 里的 op：运行体 `ServerRuntime::run_upload()`
把它拆成两段（锁下取快照 → 锁外下载 → 锁内登记），管道与 HTTP 共用这一份
（`FMT 技术文档.md` 第 10.2、15.1 ④、18.19 节）。`commands.cpp` 的 `file_command()`
只处理 `file.list` / `file.get` / `file.delete`。

> 原口径「`trash.get` / `trash.delete`（永久删除）仍未实现，返回 `FMT-602`（阶段 7）」
> **已作废**（提交 `4fee290`）：桶级四条 trash 命令全部可用。**提交 `188e85d` 之后范围再收一次**：
> 仍未实现的只有**文件级**条目（`file delete` 产生的）的 list/get/restore/delete，
> 属阶段 7——注意 `file delete` 自己**已经可用**，它只是「写得进、读不出」。
> **永久删除必须先确认**：服务端要求请求带 `force == true`（否则 `FMT-001` / 退出码 2），
> CLI 交互窗口问一次、一次性命令必须 `--yes`，HTTP 用 `?force=1` 或 `{"force":true}`；
> 删除顺序是「先删目录、再清 `trash_reason="bucket"` 的 file.json 记录、最后摘
> `trash/<user>/.original`」——**文件级（`trash_reason="file"`）的记录绝不动**。

`hello` 的响应 `data` 里带两个新字段：

```json
{ "id":1, "ok":true, "data":{ "root":"D:/FMT2", "switched":true, "previous_root":"D:/FMT" } }
```

- `switched` = 这次声明是否**导致服务切换了数据根**；未切换时不含 `previous_root`（`switched` 缺省视为 false）。
- `switched` 为真时 CLI 额外记一行日志 `[Service] 数据根切换：旧 -> 新`（只进日志），所以双击一个新目录就能看见服务跟过来了。

响应信封与 HTTP 完全一致，跨进程错误码用同一个字符串还原函数，**一份信封两处复用**。

两个必须处理的 Windows 坑：

1. **DACL**：服务以 `LocalSystem` 运行，管道默认只有 SYSTEM / Administrators 能连。要用 `ConvertStringSecurityDescriptorToSecurityDescriptorW` 显式授权交互用户：
   `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`
2. **强制完整性级别（MIC）**：高完整性进程创建的对象带高完整性标签，中完整性的普通 CLI 会被「禁止向上写」挡住。必须再给管道加 MIC 标签 `S:(ML;;NW;;;ME)`，否则非提权 CLI 连接报 `ERROR_ACCESS_DENIED`。

超时：连接时管道不存在最多重试 **5 秒**（权限类错误不重试）、普通命令 **30 秒**；**`file.upload` 用 `ipc::kUploadTimeoutMs = 30 分钟`**（提交 `a2b6cd1`，第 12 节）。连不上（`ERROR_FILE_NOT_FOUND`）→「无法连接 FMT Service，请先执行 service install」+ 退出码 8（`FMT-601`）。

### 4.3 浏览器 ↔ 服务：HTTP

保持旧设计：`server.json` 控制 `enabled` / `host` / `port`（默认 `127.0.0.1:4122`），由 cpp-httplib 提供。HTTP 请求作用于**当前数据根**。

**阶段 4 已落地的业务路由（`/api/bucket` 五条 + `/api/trash` 四条，形状已冻结），
提交 `188e85d` 又追加 `/api/file` 四条**：

```text
GET    /api/bucket                → bucket.list
POST   /api/bucket                → bucket.create   body: {"name":"工作"} 或 {"argv":["工作"]}
GET    /api/bucket/<name>         → bucket.get
POST   /api/bucket/<name>/use     → bucket.use
DELETE /api/bucket/<name>         → bucket.delete
GET    /api/trash                 → trash.list
POST   /api/trash/<名字>/restore  → trash.restore <名字>   （路径参数百分号解码）
GET    /api/trash/<名字>          → trash.get <名字>       （提交 4fee290）
DELETE /api/trash/<名字>          → trash.delete <名字>    （提交 4fee290）
                                    需 ?force=1（或 force=true）或 body {"force":true}，
                                    否则 400 + FMT-001
GET    /api/file                  → file.list               （提交 188e85d）
POST   /api/file                  → file.upload             body: {"url":"…"} 或 {"path":"…"}，
                                    可带 "file_name"（可省略 → 服务端从来源推断）
GET    /api/file/<id_or_name>     → file.get                （路径参数百分号解码）
DELETE /api/file/<file_id_or_name> → file.delete            （**软删除、不需要 force**；
                                    提交 0ad9efc 起路径参数也可以是文件名，
                                    路由正则没改，仍是 ([^/]+) + url_decode()）
```

- **路径参数里的中文会被客户端百分号编码**（`/api/bucket/%E5%B7%A5%E4%BD%9C`），
  服务端必须先用 `common/string` 的 `url_decode` 还原成 UTF-8 再当业务参数用；
  对应地 `url_encode` 给客户端拼路径用。不解码会得到 `FMT-200 Bucket 不存在：%E5%B7%A5…`。
- 请求体两种写法等价：`{"name":"工作"}`（好写）或 `{"argv":["工作"]}`（与管道一致）；
  两者最终都变成 `args.argv`，**交给同一个 `execute_business()`**，所以两条入口的行为、
  错误码、信封完全一致。
- **错误码 → HTTP 状态码**已冻结（技术文档 12.5）：`400` 参数/名称/路径/URL 类、
  `403` 权限与分享不可用、`404` 对象不存在、`409` 冲突、其余 `500`；
  管道没有这一层，CLI 只看信封里的 `FMT-NNN`。
- HTTP 是**线程池并发**（cpp-httplib 默认 `max(8, hardware_concurrency-1)`），
  `/api/ping`、`/api/status` 不碰业务锁；业务路由与管道共用运行体的同一把互斥锁（第 4.2 节）。
  **提交 `188e85d` 的例外**：`file.upload` 走两段式，下载/复制那一段**不在锁内**，
  锁只保护「去重 → 重名 → file_id → 搬文件 → 写 file.json」那一下，
  所以上传不会再把 `bucket list` 与其它 HTTP 业务请求一起堵住。

`file` / `share` / `config` 的 `/api/*` 路由仍是**设计约定**，随阶段 6、7 落地细化；
**桶级回收站的四条路由已落地**：`GET /api/trash`（列条目）、`POST /api/trash/<名字>/restore`
（回退）、`GET /api/trash/<名字>`（条目详情）、`DELETE /api/trash/<名字>`（永久删除，需要
`?force=1` 或请求体 `{"force":true}`，否则 `400 + FMT-001`）——路径参数由服务端 `url_decode`
解码；后两条在收尾提交 `4fee290` 补齐（原口径「属阶段 7，现在返回 `FMT-602`」**已作废**）；
**`/api/file` 四条已在提交 `188e85d` 落地**（`GET` / `POST /api/file`、
`GET /api/file/<id_or_name>`、`DELETE /api/file/<file_id_or_name>`——路径参数名在
`0ad9efc` 放宽成「也可以是文件名」），所以上面那句「设计约定」
现在只剩 `/api/share`（四条）与 `/api/config`（两条）——**打到它们仍返回 `FMT-602`**
（`share.*` / `config.*` 被 `is_known_business()` 认得但没有实现）；
**「service 层是唯一业务执行者、两条入口共用信封」这一结构不变**。

### 4.4 service 命令：直连 SCM + UAC

```text
fmt >service stop
需要管理员权限            ← FMT-603 AdminRequired
正在提权...               ← ShellExecuteExW(lpVerb=L"runas")
执行成功...
错误码：0                 ← 提权子进程的退出码

fmt >                     ← 提示符前先空一行（横幅之后、以及每条命令之后都如此）
```

命令行形状（**已定稿**）：

```text
结果文件：<数据根>\temp\fmt-elev-<父进程 pid>.json
          固定名、跟着 exe 走，用户一眼能找到、随时可清
          数据根不可写时退回 %TEMP%\fmt-elev-<父进程 pid>.json，并记一行 WARN
命令行  ：fmt.exe --elevated <operation> --result "<结果文件的绝对路径>"
operation ∈ install | uninstall | start | stop | reinstall
            （五种，全部是「动作」命令。`service status` 是查询、不提权，所以**不在**这个清单里，
              它直接 OpenSCManagerW + OpenServiceW + QueryServiceStatusEx，见第 6 节）
```

`reinstall` = 先卸载（服务未安装时忽略该错误）再安装并启动，用于「服务宿主 exe 已丢失、
需要重新指向当前目录」的场景，**一次 UAC 做完**。

- 是否已提权：`OpenProcessToken` + `GetTokenInformation(TokenElevation)`。
- 提权：`ShellExecuteExW(lpVerb=L"runas", fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC|SEE_MASK_FLAG_NO_UI, nShow=SW_HIDE)`，
  父进程 `WaitForSingleObject(hProcess, 60000)` + `GetExitCodeProcess`，再读结果文件（读完即删除）。
- **`runas` 启动的控制台程序会另开一个控制台窗口**，所以提权副本必须 `SW_HIDE` 无窗口，结果写进结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 回传，由父进程读回打印——这是「只留一个窗口」的前提。CLI 在执行提权命令前会尝试创建 `<数据根>/temp`；建不出来（例如 exe 放在只读位置）就退回系统临时目录 `%TEMP%`，并写一行 WARN 说明原因与改用后的路径；提权副本在写结果文件前也会确保目录存在（它有权限）。
- **回传不用命名管道**：提权副本是高完整性进程，它创建的管道带高完整性标签，中完整性父进程受 MIC「禁止向上写」限制，**连接和读取都会被拒**（与技术文档 13.9.2「坑 2」同一机制）。结果文件写在数据根下的 `temp/` 里（同一个用户、只是令牌不同），父子两边都能读写，不需要放宽安全描述符；只有数据根不可写时才退回用户自己的 `%TEMP%`。**命名管道方案作废，结果文件是唯一通道。**
- 结果文件内容 `{ "ok": …, "code": "FMT-NNN", "message": …, "exit": … }`：`exit` 与提权副本的进程退出码一致（成功 0 / 需要管理员权限 5 / SCM 操作失败与等待超时 8）。
- `SHELLEXECUTEINFOW::lpFile` 必须指向具名变量：`info.lpFile = path_from_utf8(self).c_str();` 会指向语句结束即析构的临时 `std::wstring`，实测表现为 Win32 1155 `ERROR_NO_ASSOCIATION`，完全看不出是提权的问题。
- 用户取消 UAC（`ERROR_CANCELLED` 1223）→ `FMT-004` / 退出码 5；非管理员或策略禁止提权（`ERROR_ACCESS_DENIED` 5）→ `FMT-603` / 退出码 5；`ShellExecuteExW` 其它失败、等待超时、结果文件不存在（提权副本崩了）→ `FMT-602` / 退出码 8。
- 提权副本只做 SCM 操作（五种 operation），短命、不进循环。
- **Service 启动时清理 `temp/`**：删除 `<数据根>/temp` 下以 `fmt-` 开头的遗留文件（上次异常退出留下的提权结果等），**用户手放进去的其它文件一律不动**；删除数量记一行 INFO（见第 6 节 `ServiceMain` 序列）。根切换时不做这件事——那是另一个根的数据。

---

## 5. 数据根

### 5.1 由 CLI 声明

```text
CLI 启动
  → GetModuleFileNameW 取自身路径 → 父目录 = root（如 D:\FMT2）
  → 先对 root 跑一次共用的幂等体检与补齐（第 5.2 节，只补缺失、不碰业务数据内容）
  → 连管道，首帧 {"op":"hello","root":"D:\\FMT2","pid":1234}
  → 服务：
       root != 当前数据根 ?
         ├─ 是 → 切换数据根（旧根数据原样保留），对 root 做幂等初始化，
         │        记 INFO「数据根切换: D:\FMT → D:\FMT2」，
         │        响应里置 switched=true 并回填 previous_root
         └─ 否 → 直接进入命令循环，响应不含 switched/previous_root
  → 该连接上的所有业务命令都在该根下执行
  → switched=true 时 CLI 记一行日志「数据根切换：D:\FMT -> D:\FMT2」（只进日志）
```

只有同时一个 CLI 窗口（决策 10），所以同时只有一个数据根，不会出现两个根互相打架。

### 5.2 初始化职责

`repository/`、`trash/`、`config/`、`data/`、`log/`、`temp/` 与六个默认 JSON
（`config/config.json`、`config/server.json`、`data/file.json`、`data/share.json`、
`data/trash.json`、`data/user.json`）由 **Service 与 CLI 共用的同一套幂等规则**处理
（同一份 `ensure_root` / `check_root`）：

```text
谁执行   Service：启动时、以及 hello 触发换根时，对自己的数据根执行
         CLI    ：双击时，对自己 exe 所在的数据根执行（在打开日志器之前，所以 log/ 也由它建）
共同规则 只补缺失
         六个目录缺则建，已存在一律不动（不删除、不覆盖、不改名）
         默认 JSON 缺则写（关键 JSON 先写 .tmp 再原子替换）
         已有的 JSON 会被真正读一遍（解析 + 版本检查）确认完整性
         读不出来或版本不受支持 → 只报告、绝不重置（沿用「JSON 损坏不能静默重置」）
         都不碰业务数据内容：不写 data/*.json 的内容、不删文件、不改名
阶段 4 加的一步（在共用规则之后，由 initialize_root 做）
         加载 config.json → current_user 为空？→ 置占位名 "user" 并保存（决策 20）
         幂等：第二次读到 "user" 什么都不写
```

CLI 侧的结果**只进日志**（`log/fmt.log`，模块 `Cli`），控制台一行都不打：

```text
[Cli] 数据根检查：D:/FMT2
[Cli] 数据根新建目录：repository, trash, config, data, log, temp
[Cli] 数据根新建文件：D:/FMT2/config/config.json, ...
```

第二次及以后双击（什么都没缺）只是日志里换成 `[Cli] 数据根完整`；
发现损坏时日志记 `[Cli] 数据根损坏（未自动修复）：data/file.json`，
并把 `数据根文件损坏（未自动修复）：data/file.json` 送到 **stderr**（异常才进控制台）。

`log/` 与 `temp/` 曾经被写成「CLI 只读的两处例外」；现在 CLI 建目录不再有特例，只有「补缺失」
这一条规则（`--help` / `--version` / `help` / `exit` 仍然不写日志、不创建任何目录）。

`temp/` 是**临时文件目录**：**不是业务数据、也不是日志**，内容随时可以清空。它放两类东西——
提权结果文件 `temp/fmt-elev-<父进程 pid>.json`（父进程读完立刻删除）与以后上传时的暂存文件
（旧文写「temp/ 或者系统临时目录」，现在明确为 `temp/`）。**Service 启动时**会删除 `temp/` 下
以 `fmt-` 开头的遗留文件（上次异常退出留下的提权结果等），**用户手放进去的其它文件一律不动**，
删除数量记一行 INFO。

这条同时修掉了旧文档的内部矛盾：旧 §4.4 说「CLI 只读不建目录」，旧 §18.2 实测却写「首次运行就自动建出了目录」。当时的口径是「服务建，CLI 只读」，只加了两条例外（CLI 为写日志可以创建 `log/`，为提权可以创建 `temp/`）；**现在的口径更进一步**：两边跑同一套幂等规则，CLI 双击时补的就是它自己数据根缺的那部分，所以既不与旧实测打架，也不再把「建目录」当成 CLI 的禁忌——真正的禁忌是**碰业务数据内容**。

### 5.3 服务状态文件

```text
%ProgramData%\FMT\service.json     ← 只存「当前数据根路径」与安装信息
```

它不属于业务数据，因此不放进任何数据根，也不放进 `log/`。服务开机自启且没有 CLI 连接时，数据根取这里的记录值；从未记录过则取服务宿主 exe 所在目录。

### 5.4 把 exe 复制到新目录

```text
双击 D:\FMT2\fmt.exe
 1. 单实例互斥体：若旧窗口还开着 → 激活旧窗口，不开第二个
    （所以换目录前必须先关掉旧窗口）
 2. 数据根体检与补齐：对自己所在数据根跑共用的 ensure_root/check_root
    （六个目录 + 六个默认 JSON，只补缺失；已有 JSON 读一遍确认，损坏只报告不重置，第 5.2 节）
    这一步在打开日志器之前完成，所以 log/ 也在同一条「新建目录」清单里；
    结果**只进日志**，控制台一行都不打，异常才走 stderr
 3. 查 SCM 状态（走统一接口 `service::query_status()`，第 6 节；「未安装」是**正常结果**）：
       ├─ 未安装        → 提权 install（装 + 启动，一次 UAC）
       ├─ 运行中        → 不动、不弹 UAC
       └─ 已安装未运行  → 提权 start → **等它落定**（第 4.1 节：按 SCM 的 `dwWaitHint` 自适应）
             ├─ 起来了       → 完成
             └─ 仍然没起     → 读服务留下的失败编号 dwServiceSpecificExitCode（第 4.1 节）
                   ├─ 数据根/配置类问题 → **不重装**（重装也解决不了），打印原因让用户先处理
                   └─ 其它             → 提权 reinstall 一次（卸载 + 安装，一次 UAC），仍失败则报错
 4. 比较服务 binPath 与自身路径：
      不同但文件存在   → 作为客户端继续
      不同且文件已丢失 → 询问是否重新安装服务（提权副本的 reinstall operation：先卸载、再安装
                          并启动，服务未安装时忽略卸载错误，一次 UAC，见第 4.4 节）
 5. 服务在运行 → 连管道发 hello 声明 root=D:\FMT2（第 5.1 节）；
      switched=true 时记一行日志「数据根切换：旧 -> 新」（只进日志）
 6. 打印横幅与 Service Running... → 空一行 → fmt> 提示符
```

第 3 步判定「数据根/配置类问题」的错误码集合（命中就**跳过重装**、只打印原因让用户先处理）：

| 错误码 | 含义 |
|---|---|
| `FMT-006` | `JsonParseError` JSON 格式错误 |
| `FMT-007` | `JsonWriteError` JSON 写入失败 |
| `FMT-008` | `ConfigError` 配置错误 |
| `FMT-011` | `JsonUnsupportedVersion` JSON 数据版本不受支持 |
| `FMT-013` | `DirectoryCreateFailed` 目录创建失败 |
| `FMT-009` | `StorageError` 存储操作失败 |
| `FMT-005` | `IoError` 磁盘/IO 错误 |
| `FMT-014` | `PathEscape` 路径穿越 |

这些都不是「服务注册坏了」，重装服务解决不了；集合之外的失败（如 `FMT-602 ServiceOperationFailed`）
才走「提权 reinstall 一次」。

服务宿主就是**首次安装时那份 exe**：复制走没事，剪切或删除会让服务起不来（SCM 报 1053），需要重新 `service install`。

---

## 6. 服务生命周期

| 命令 | 行为 |
|---|---|
| `service install` | 提权 → `OpenSCManagerW` → `CreateServiceW`（`SERVICE_AUTO_START`、`LocalSystem`）→ 配 Recovery → `StartServiceW`。已存在 → `FMT-600`，不重复创建 |
| `service uninstall` | 提权 → 先 `ControlService(STOP)` → `DeleteService`。**不得删除** `repository` / `trash` / `data` / `config` / `log` / `temp` |
| `service start` | 提权 → 存在则 `StartServiceW`；不存在 → `FMT-601` |
| `service stop` | 提权 → `ControlService(SERVICE_CONTROL_STOP)` → 停止接受新请求 → 等在途事务 → 停 HTTP → 退出 |
| `service status` | **不提权** → `OpenSCManagerW(SC_MANAGER_CONNECT)` + `OpenServiceW(SERVICE_QUERY_STATUS)` + `QueryServiceStatusEx(SERVICE_STATUS_PROCESS)`，打印状态 / 宿主 / 数据根 / 错误码。状态名用 `未安装` / `已停止` / `正在启动` / `正在停止` / `运行中` / … 映射 `dwCurrentState`；未安装 → `FMT-601` / 退出码 8，已安装未运行 → 照常打印、退出码 0 |
| `service reinstall` | 提权副本的 operation（`--elevated reinstall --result "<路径>"`）：卸载（服务不存在则忽略 `FMT-601`）→ 安装 → 启动，一次 UAC；用于宿主 exe 已丢失、需要重新指向当前目录 |

`service status` 的输出样例：

```text
服务状态：运行中
服务宿主：D:/FMT2/fmt.exe
服务数据根：D:/FMT2
错误码：0
```

状态名由 `service::state_name()` 一处映射（`service::State` → 中文），不在命令里各写一套。
「服务宿主」取 `service::installed_binary_path()`（注册的 `binPath`，去掉引号）；
「服务数据根」取 `service::load_state()` 的 `current_root`。

**服务状态与失败编号统一走 `service::query_status()`**（第 4.1 节）：`status` 命令、
`service start` / `service stop` 的落定判定、双击引导读失败编号都用它，不再各自调
`QueryServiceStatusEx` 再自己映射状态。

五个命令里只有前四条（`install` / `uninstall` / `start` / `stop`）走 UAC，加上提权副本专用的
组合操作 `reinstall` 共五种 operation；**`status` 不属于 operation 集合**。

Recovery：`ChangeServiceConfig2W(SERVICE_CONFIG_FAILURE_ACTIONS)`，第一次失败 5 秒、第二次 10 秒、后续 30 秒重启，失败计数 1 天重置；不自建 watchdog。

`ServiceMain` 关键序列：

```text
RegisterServiceCtrlHandlerExW
  → SetServiceStatus(START_PENDING[, dwCheckPoint])
  → 读取 %ProgramData%\FMT\service.json，确定数据根
  → 初始化存储与业务服务（幂等建目录含 temp/ + 默认 JSON；与服务/CLI 共用的 ensure_root）
  → 清理 temp/ 下以 fmt- 开头的遗留文件（用户手放的其它文件不动），删除数量记一行 INFO
  → 起 HTTP 线程
  → SetServiceStatus(RUNNING, ACCEPT_STOP | ACCEPT_SHUTDOWN)
  → 等停止事件
  → 停 HTTP → 等在途操作 → SetServiceStatus(STOPPED)
```

**初始化失败时**（第 4.1 节的编号上报，CLI 双击引导第 3 步依赖它）：

```text
失败（如 FMT-008 ConfigError）
  → SetServiceStatus { dwCurrentState = SERVICE_STOPPED,
                       dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR,
                       dwServiceSpecificExitCode = 8 }    ← FMT-008 的数字部分
  → 记一行 ERROR 日志（含完整 FMT-NNN 与消息）
CLI 侧 service::last_start_failure() 读回该编号 → code_from_number → 「服务启动失败：FMT-008 配置错误」
```

**等到什么时候才算「没起来」**：先 `settle_state()` 按 SCM 的 `dwWaitHint` 等
（夹在 100 ms – 2000 ms，兜底 30 秒，不是等待类就立即结束，见第 4.1 节），
再判失败编号——不写死时长，避免在慢机器上把「还在启动」误判成「启动失败」。

---

## 7. 单实例与窗口

- 互斥体 `Local\FMT.CLI.v1`；`GetLastError() == ERROR_ALREADY_EXISTS` → 已有实例。
- 激活已有窗口：`EnumWindows` 找 `ConsoleWindowClass` → `SetForegroundWindow`；受前台锁定限制失败时退化 `FlashWindowEx`。
- **UIPI**：高完整性进程的窗口无法被中完整性进程置前，因此提权副本必须无窗口（见 4.4）。

---

## 8. 目录与文件布局

```text
<数据根>/                     ← CLI 声明，等于 CLI 的 fmt.exe 所在目录
├── fmt.exe
├── repository/              <user>/<bucket>/YYYY/MM/DD/<file_name>
│                            Bucket 就是这一层目录，没有独立 ID（决策 19）
├── trash/                   回收站：桶级条目在顶层，文件级条目收在 .files/ 下
│                            Bucket 删除 → trash/<user>/<bucket>_<YYYYMMDDHHMMSS>/
│                            目录名**一律带删除时间戳**，同一秒再删一次加 _2 序号，
│                            绝不覆盖已有条目（不再区分有没有重名）
│                            trash/<user>/.original = 桶级身份记录的唯一权威，
│                            data/*.json 丢了原名也还在（不靠剥离时间戳猜原名）
│                            文件删除（**提交 188e85d 起已落地写入侧**）→ trash/<user>/.files/
│                            <bucket>/YYYY/MM/DD/<file_name>（提交 4fee290 定下落点）：中间那层
│                            .files 是刻意的——trash/<user>/ 的顶层留给桶级条目，否则阶段 5
│                            落地文件级删除后，桶级扫描会把 trash/<user>/<bucket>/
│                            当成「孤儿桶条目」列出来；桶级扫描跳过点开头的条目，
│                            并且只认 <名字>_<14 位时间戳>（可带 _<1-3 位序号>）形状的目录
│                            代价：文件级条目也因此一条都列不出来（读取侧属阶段 7）
├── config/
│   ├── config.json          version / current_user / current_bucket /
│   │                        max_upload_size / size_unit / language
│   │                        current_user 由初始化补成占位名 user（决策 20）
│   └── server.json          version / enabled / host / port(4122)
├── data/
│   ├── user.json            {"version":1,"users":[]}
│   ├── file.json            {"version":1,"files":[]}
│   │                        （阶段 4 起每条记录在进回收站时带 trash_reason：
│   │                          "bucket" = 桶被删、"file" = 文件自己删的；
│   │                          提交 188e85d 起 file delete 会写 "file"）
│   │                        **不存路径也不存时间**：落在哪一天由 file_id 的
│   │                          fmt-YYYYMMDD-N 片段推出（技术文档 8.5）
│   ├── share.json           {"version":1,"shares":[]}
│   └── trash.json           {"version":1,"trash":[]}
│                            只为**文件级**条目服务（阶段 5 起）；
│                            提交 188e85d 起 file delete 真的会往这里追加
│                            type="file" 的记录（file_id / file_name /
│                            original_path / trash_path / deleted_at）；
│                            但**读取侧还没做**：trash list/get/restore 只认桶级条目
│                            Bucket 级记录（旧 type=bucket）**作废**：
│                            桶级身份改记 trash/<user>/.original
├── log/
│   ├── fmt.log              Service 与 CLI 追加同一个文件
│   └── error.log            仅 ERROR 级
└── temp/                    临时文件，随时可以清空（既不是业务数据、也不是日志）
    ├── fmt-upload-<随机>-<序号>.tmp   上传暂存（提交 188e85d；fmt- 前缀让启动清理收走）
    └── fmt-elev-<父进程 pid>.json   提权结果文件，父进程读完立刻删除

%ProgramData%\FMT\service.json  ← 服务自身状态，不属于业务数据
```

`log/` 是唯一由**两个进程共同追加**的目录：Service 与 CLI 都以「追加」方式打开同一个
`<数据根>/log/fmt.log`（`error.log` 仅 ERROR 级），每行一次写入，MSVC 文件流是共享模式，
因此不会争抢。这条口径推翻过旧写法「只有 Service 写日志文件」——用户在 CLI 里敲
`service stop`，服务随即被停掉，旧写法下这次操作在日志里一个字都没有，日志跟不上用户
做过什么；日志要能回答「谁在什么时候对服务做了什么、结果如何」。四条边界：CLI 双击时先对自身数据根
跑一次共用的幂等补齐（只补缺失的目录与默认 JSON，第 5.2 节），这一步在打开日志器之前完成，所以
`log/` 也在其中，**其结果只进日志**；除此之外 CLI 只允许创建写日志用的 `log/` 与执行提权命令前要用的 `temp/`
（`repository` / `data` / `config` 的**内容**一概不碰，损坏 JSON 也只报告不重置）；
`--help` / `--version` / `help` / `exit` 不写日志、不创建任何目录；两个进程的数据根可能不同时，各写各自数据根下的 `log/fmt.log`，
CLI 会额外写一行 WARN 指明服务当前数据根与服务侧日志的位置。CLI 侧模块短名为 `Cli`
与 `Elevated`。CLI 连管道时若管道还不存在，最多重试 5 秒（权限类错误不重试）。

**控制台只留交互**：横幅（`File Manager Tool  v1.0  ( build  <日期> )` + `Service Running...`）、
提示符、命令结果与异常；服务的当前状态、数据根体检的「建了什么 / 完不完整」都只进日志。
这就是本工程的老原则「控制台负责用户交互与重要异常，日志文件负责完整运行记录」的落位。

`temp/` 与 `log/` 都不是业务数据，但性质不同：`log/` 要长期保留，`temp/` **随时可以清空**。
`temp/` 里放提权结果文件（固定名 `fmt-elev-<父进程 pid>.json`，父进程读完立刻删除）与
上传时的暂存文件（**提交 `188e85d` 起是 `fmt-upload-<随机>-<序号>.tmp`**，边下载边算 MD5、
边判大小上限，失败即删）；`temp/` 跟着 exe 走，用户一眼能找到、随时可清。数据根不可写（例如 exe 放在
只读位置）时退回系统临时目录 `%TEMP%` 并写一行 WARN。**Service 启动时**删除 `temp/` 下以
`fmt-` 开头的遗留文件（提权结果与上传暂存都以 `fmt-` 开头，所以两类都能被收走），
用户手放进去的其它文件一律不动，删除数量记一行 INFO。

---

## 9. 错误码与退出码

退出码沿用旧表：`0` 成功、`1` 通用、`2` 参数、`3` 对象不存在、`4` 冲突、`5` 权限/访问、`6` 数据一致性、`7` 配置、`8` Service。`FMT-NNN` 定位原因，退出码给脚本分类，两层不混用。

| 场景 | 错误码 | 退出码 |
|---|---|---|
| 服务已存在，重复 install | `FMT-600 ServiceAlreadyInstalled` | 8 |
| 服务不存在 | `FMT-601 ServiceNotInstalled` | 8 |
| `service status` 发现服务未安装（打印 `服务状态：未安装`，但**不提权**） | `FMT-601 ServiceNotInstalled` | 8 |
| `service status` 成功（含「已安装但已停止」） | — | 0 |
| SCM 操作失败 / 提权等待超时（60 秒） | `FMT-602 ServiceOperationFailed` | 8 |
| `ShellExecuteExW` 其它失败 / 结果文件不存在（提权副本崩了） | `FMT-602 ServiceOperationFailed` | 8 |
| 需要管理员权限（打印「需要管理员权限」，或 `ShellExecuteExW` 失败且 `ERROR_ACCESS_DENIED` 5） | `FMT-603 AdminRequired` | 5 |
| 用户在 UAC 点「否」（`ERROR_CANCELLED` 1223） | `FMT-004 PermissionDenied` | 5 |
| 永久删除回收站条目（`trash delete`）**没有显式确认** | `FMT-001 InvalidArgument`（「永久删除不可恢复，需要确认（force = true）」） | 2 |
| 回收站条目不存在（`trash get` / `trash delete` 也走它，不只是回退） | `FMT-400 TrashEntryNotFound` | 3 |
| 上传时来源协议不是 http/https（如 `ftp://`，提交 `a2b6cd1`；原文此处写的是 `https://`，已作废） | `FMT-300 UrlInvalid`（「只支持 http:// 与 https:// 的来源：<来源>」；**不是 `FMT-002`**） | 2 |
| 上传时 URL 带用户名密码 / 端口非法 / 缺主机名（提交 `a2b6cd1`） | `FMT-300 UrlInvalid` | 2 |
| 下载超时（`ERROR_WINHTTP_TIMEOUT`，提交 `a2b6cd1`） | `FMT-302 DownloadTimeout` | 1 |
| 下载因域名/连接/TLS/响应异常失败，或服务器返回非 identity 的压缩内容（提交 `a2b6cd1`） | `FMT-301 DownloadFailed`（证书类失败附「根证书不受信任 / 证书主机名不符 / 证书已过期」） | 1 |
| 上传时文件名推不出来（如 `http://example.com/`，提交 `188e85d`） | `FMT-100 FileNameEmpty`（「无法从来源推断文件名，请显式给出文件名」） | 2 |
| 上传时同用户（**跨 Bucket**）已有相同 MD5 的正常文件 | `FMT-304 Md5Duplicate`（「该文件已经存在：<名字>（<file_id>）」） | 4 |
| 上传时同用户（**跨 Bucket**）已有同名正常文件（不自动改名） | `FMT-105 FileNameConflict`（「同名文件已存在：<file_name>（换一个文件名再上传）」） | 4 |
| 上传时文件名与 `file_id` 同形（`fmt-YYYYMMDD-N`，提交 `9c3d2cb`；前缀按 ASCII 折叠比较，「最短 14 个字符」「日期段恰好 8 位数字」「序号段全数字且非空」） | `FMT-106 FileNameLikeFileId`（「文件名不能与文件标识同形（fmt-YYYYMMDD-N）：<名字>（会与 file_id 混淆，请换一个名字）」；属 `FMT-1xx` 文件名校验一组，与 Windows 保留设备名同类，**显式名与从来源推断的名字走同一道校验**） | 2 |
| `file delete` 的 key 同时命中一条 `file_id` 与**另一条**记录的文件名（旧数据里已经存在这种形状） | `FMT-001 InvalidArgument`（「有歧义：<key> 既是 <file_id> 的文件标识，又是另一个文件的文件名（file_id <file_id>）。这种名字现在不允许上传；请直接用 file_id 指定要删哪一个」；**只给 `file delete`**，`file get` 不加） | 2 |
| `file delete` 的文件已经在回收站里 | `FMT-001 InvalidArgument`（「该文件已经在回收站里：<file_id>」；提交 `0ad9efc` 起按**名字**命中的那一路报「该文件已经在回收站里：<名字>（file_id <file_id>）」，**不是** `FMT-002`） | 2 |
| `file get` / `file delete` 按 `file_id` 或文件名都查不到 | `FMT-002 FileNotFound`（提交 `0ad9efc` 起 `file delete` 的定位与 `file get` 一致：先当 `file_id`、再当文件名（当前用户 + 正常文件），两步都没有才是 `FMT-002`） | 3 |
| `file get` 命中的记录在回收站里（提交 `9c3d2cb`） | 不是错误：照常回 `is_trash` / `trash_reason`，并**增加** `trash_path`（相对数据根、正斜杠，形如 `trash/user/.files/工作/2026/10/08/test.txt`）；仓库里找不到该文件时**不返回** `path`——这是设计，不是缺失。CLI 多打一行「回收站路径：…」 | 0 |
| 文件的落地路径推不出来且在日期树里有多个同名文件（有歧义） | `FMT-015 ConsistencyError` | 6 |

编号一旦发布不复用、不修改语义，新增只能追加。

---

## 10. 阶段拆分

| 阶段 | 内容 | 验收 | 状态 |
|---|---|---|---|
| 2 | `common`（Error / Result / Time / String / Path / Logger）、`config`、`storage`、`core` 初始化 | 能在指定数据根建出六个目录（`repository/` `trash/` `config/` `data/` `log/` `temp/`）+ 默认 JSON（**`ensure_root`/`check_root` 由 Service 与 CLI 共用**，第二次运行日志里只多一行「数据根完整」、控制台无输出）；JSON 损坏报 7 且不动原文件 | ✅ 完成 |
| 3 | `service`（SCM 五命令 `install`/`uninstall`/`start`/`stop`/`status` + 统一查询接口 `query_status()`/`query_state()`/`installed_binary_path()`/`last_start_failure()` + 按 `dwWaitHint` 自适应的 `settle_state()` + ServiceMain + 失败编号上报 `dwServiceSpecificExitCode` + Recovery + 服务状态文件）、`ipc`（管道 + 安全描述符）、`cli`（循环、横幅 `File Manager Tool  v1.0  ( build  <日期> )`、`help` 总览与分组详情、单实例、提权、双击幂等体检与补齐、落定判定与 reinstall 兜底） | **全新环境双击 → 一次 UAC → 服务装好且开机自启 → 控制台干净（无「建了什么」）→ `service stop/start` 各弹一次 UAC、`service status` 不弹 UAC、输出与样例一致；`help` / `help service` 输出与样例一致；复制 exe 到新目录双击 → 在新目录建出数据，日志里有「数据根切换」** | ✅ 完成 |
| 4 | `bucket` + `common/validation` 名称校验 | create / list / get / use / delete + `current_bucket`；Bucket 无独立 ID；删除移入回收站且**目录名一律带删除时间戳**（同秒冲突加 `_2`）；**Bucket 删除/回退的回收站形状**：`trash/<user>/.original` 是桶级记录唯一权威、回退**整单判定**（目标已存在 → `FMT-401`，不覆盖/不改名/不部分恢复）、`file.json` 增加 `trash_reason`、桶级 `trash list` / `get` / `restore` / `delete` 四条入口都在（`get` / `delete` 在提交 `4fee290` 补齐；永久删除要显式确认，顺序为「目录 → `trash_reason="bucket"` 的 file.json 记录 → `.original`」，文件级记录绝不动）；文件级条目落点定为 `trash/<user>/.files/<bucket>/YYYY/MM/DD/`（`PathManager::build` 的 `inner` 参数），桶级扫描跳点开头 + 只认 `<名字>_<14 位时间戳>`（可带 `_<1-3 位序号>`）形状；管道与 HTTP 两条入口行为一致；`current_bucket` 失效校验在**服务启动**与**数据根切换**时由运行体自动执行（失效置空、有效不动） | ✅ 完成（commit 32249ea；回收站形状 c2d545d；`trash get` / `trash delete` 与 `.files` 落点 4fee290；收尾 8a5e554、acc90a3，见 `FMT 技术文档.md` 第 18.15、18.17 节） |
| 5 | `file` / `upload` / `trash` / `share` | 需求里的文件、分享、回收站。**桶级回收站的四条命令已在阶段 4 全部落地**（`trash list` / `get` / `restore` / `delete`，commit c2d545d + `4fee290`），阶段 5/7 补**文件级**条目：`trash list`/`get`/`restore`/`delete`（文件级口径）、文件级永久删除与文件级（逐个文件判定的）恢复、文件级条目落在 `trash/<user>/.files/<bucket>/YYYY/MM/DD/` 下。**开工前先定锁粒度**：阶段 4 是「两条入口共用一把互斥锁」，上传/下载持锁会挡住 `bucket list` 与浏览器请求（见第 12 节） | 🟡 **`file` 部分已完成，且 https 可用**（提交 `188e85d` + `a2b6cd1` + `0ad9efc` + `9c3d2cb`）：`file upload <来源> [文件名]` / `file list` / `file get <file_id\|文件名>` / `file delete <file_id\|文件名>` 四条都可用（上传两段式、MD5 去重、重名拒绝、软删除写文件级 trash 条目、存储日期由 `file_id` 推出；`file delete` 的参数在 `0ad9efc` 扩成两种、定位与 `file get` 一致——先 `file_id` 再文件名，名字在回收站 → `FMT-001` 带 `file_id`；**`9c3d2cb` 再补三处**：文件名与 `file_id` 同形 → 上传拒绝 `FMT-106`、`file delete` 两个索引命中不同记录 → `FMT-001` 报歧义、`file get` 对回收站记录回 `trash_path`；**来源 `http://` 与 `https://` 都支持**，走 WinHTTP + Schannel，不用 OpenSSL、不分发 DLL，见第 2 节第 23 项）；**锁粒度已定并落地**——「把长任务移出锁」的简化形态（第 12 节）。**剩下的**：`share` 整组（阶段 5）、**文件级 trash 的读取侧**（list/get/restore/delete 与永久删除，阶段 7）。**「https 不支持」不再是限制** |

| 6 | `server`（HTTP + Preview） | 浏览器可用（`/api/bucket` 五条 + `/api/trash` 四条 + `/api/file` 四条路由已在阶段 4、5 落地；下载/预览路由与页面仍未开始） | ⏳ 未开始 |
| 7 | 文件级 Trash 收尾：文件级 `trash get` / 永久删除与文件级恢复；**永久删除时清理相关 Share**（`share.json`，第 60 节口径） | 文件级条目端到端可用；永久删除不留 share 残留 | ⏳ 未开始（**当前缺口**：share 模块属阶段 6，现在永久删除**没有**清理 `share.json` 的动作，只清 `file.json` 里 `trash_reason="bucket"` 的记录；**另一个缺口**：`file delete` 已在写文件级条目，但 `trash list` / `get` / `restore` 只认桶级条目，软删除的文件暂时看不到也恢复不了） |

阶段 2 + 3 完成后，「双击即用的服务 + CLI」闭环成立。

---

## 11. 与旧设计的差异清单

| # | 旧设计 | 新设计 | 原因 |
|---|---|---|---|
| 1 | Service 排在阶段 9 | 提到阶段 2 / 3 | 用户要求「最基本功能」先落地 |
| 2 | CLI 是 HTTP 客户端，全部命令走 4122 | CLI 走命名管道；HTTP 只给浏览器 | HTTP 可关闭、端口可被占，CLI 不应随之失效 |
| 3 | `--service install/start/stop/delete` | `service install/uninstall/start/stop`（后补第五条查询命令 `status`） | 用户命令集；`delete` 更名 `uninstall` |
| 4 | 无暂停 | 明确不做 pause | 用户确认不需要 |
| 5 | 提权只写在「检查管理员权限」一句里 | 明确的 `runas` 提权 + 结果回传 + 固定输出 | 用户要求可回显的提权过程 |
| 6 | `FMT_ROOT` = 服务 exe 所在目录 | 由 CLI 声明，服务维护当前数据根 | 用户要求数据随 exe 走 |
| 7 | 初始化归属含糊（CLI 只读 vs 首次运行建目录） | Service 与 CLI 共用同一套幂等规则（`ensure_root`/`check_root`），两边都只补缺失 | 消除文档自相矛盾：不再区分「谁有权建目录」，只约束「不碰业务数据内容」 |
| 8 | 单实例只写了一句要求 | 互斥体 + 窗口激活 + UIPI 处理 | 用户要求只留一个窗口 |
| 9 | 不引第三方库（架构 §2），但旧实现 vendor 了两个 | vendor `nlohmann/json` + `cpp-httplib`，静态链接。**提交 `a2b6cd1` 起 URL 下载不再用 `cpp-httplib` 的 `Client`**（它走 https 要 OpenSSL，会破坏离线可构建），改用系统的 WinHTTP + Schannel——仍然只有两个 vendored 单头文件，也没有多出任何 DLL | 用户确认；避免自研 HTTP 服务端风险 |
| 10 | 服务只有 `install` / `uninstall` / `start` / `stop`；想知道状态只能靠猜 | 新增 `service status`（查询，不提权）：打印状态 / 宿主 / 数据根 | 用户需要看服务当前状态，且不应为一次查询弹 UAC |
| 11 | 初始化只由服务做，CLI 只读 | CLI 双击时也对自己数据根做体检与补齐；已有 JSON 读一遍确认，损坏只报告不重置 | 双击时应立刻得到可用的数据根，而不是等一个可能启动失败的服务 |
| 12 | 服务启动失败只会说「启动失败」 | `ServiceMain` 把 FMT 编号写进 `dwServiceSpecificExitCode`，CLI 读回并打印 `服务启动失败：FMT-008 配置错误` | 让失败可诊断，并据此决定「要不要重装」 |
| 13 | 数据根切换对 CLI 不可见 | `hello` 响应加 `switched` / `previous_root`，切换时 CLI 记一行日志 `[Service] 数据根切换：旧 -> 新` | 双击新目录能在日志里看见服务跟过来了，又不往控制台刷例行状态 |
| 14 | 等待服务起来写死一个固定时长 | 改为按 SCM 的 `dwWaitHint` 自适应（100 ms – 2000 ms 夹取，兜底 30 秒，不是等待类就立即结束） | 慢机器上「还在启动」会被误判成「启动失败」，白弹一次解决不了问题的 UAC 重装 |
| 15 | 各命令各自调 `QueryServiceStatus` 再自己 `map_state` | 统一走 `service::query_status()` / `query_state()` / `installed_binary_path()` / `last_start_failure()`；「未安装」是正常结果而非错误 | 三处调用方（status / 落定判定 / 失败编号）判断一致，状态名只映射一次 |
| 16 | 结构化的 `--json` 输出被当作「将来加」 | **V1 明确不做，也不预留参数名** | 机器可读通道已由命令退出码覆盖，现在加只会提前冻结一个没想清楚的参数形状 |
| 17 | 横幅是 `FMT v1.0.0`，用法标题写 `FMT 1.0.0 - Windows 文件管理系统` | 横幅与 `--version` 共用 `File Manager Tool  v1.0  ( build  <配置日期> )`；用法标题改 `用法：fmt.exe [命令]` | 程序对外名字固定、构建日期由 CMake 生成，避免源码里再硬编码一个日期 |
| 18 | 双击时把「数据根：… / 新建目录：… / 新建文件：… / 数据根完整 / 服务状态：…」都打到控制台 | 这些行**只进日志**（模块 `Cli` / `Service`），控制台只留横幅、提示符、命令结果与异常 | 太杂乱，把提示符淹掉；按「控制台负责交互与异常，日志负责完整记录」归位 |
| 19 | 只有 `--help`（一次性、带完整用法） | 新增 `help` 命令：`help` 列命令总览（只列命令、不加描述）、`help <组>` 看该组详情；交互与一次性都支持；`help <未知组>` → `FMT-001` / 退出码 2 | 交互式里需要一个轻量的「我有哪些命令」，而 `--help` 太重 |
| 20 | Bucket 有独立标识、删除即丢弃数据（旧文档的口气） | **Bucket 就是目录**：`repository/<user>/<bucket>/`，无独立 ID；删除 = 整个目录移到 `trash/<user>/<bucket>_<时间戳>/`，**数据不丢**；回收站目录名一律带时间戳（同秒再加 `_2`），桶级身份记在 `trash/<user>/.original`，回退是整单判定（目标已存在拒绝，不做部分恢复）；桶级 `trash list` / `get` / `restore` / `delete` 四条命令都已落地（`get` / `delete` 提交 `4fee290`，永久删除要显式确认，且只清 `trash_reason="bucket"` 的 file.json 记录） | 目录本身就是最好的 ID；删除必须可恢复（第 1 节的范围里就写了「回收站」）。**时间戳后缀从「重名才加」改成「一律带」**：否则同一个桶删两次会有两条条目抢同一个回退位置，回退必然撞车 |
| 21 | 用户必须先设置 `current_user`（旧文档第 93 节的口气） | 初始化自动补占位名 `user` 并保存，用户第一条命令就能成功；`FMT-604` 只留给「被显式清空」 | V1 没有用户系统、也没有设置用户的命令，让首启先撞一次错误是白费一步 |
| 22 | 管道与 HTTP 各写一套参数解析 | 统一成 `op = "<组>.<动作>"` + `args.argv`；HTTP 请求体接受 `{"name":…}` 或 `{"argv":[…]}`，路径参数由服务端 `url_decode` 解码 | 两条入口一份业务实现，参数就不该有第二种形状 |
| 23 | 各入口自己决定 HTTP 状态码 | 冻结一张「错误码 → 状态码」映射表（400 / 403 / 404 / 409 / 500），实现里一个 `switch` + `default: 500` | 浏览器与调试工具需要稳定的状态码；新错误码未登记就落 500，绝不猜一个不匹配的 4xx |
| 24 | 上传是「一个 `FileService::upload()` 从头做到尾」，隐含地在业务锁内下载（旧设计没写锁粒度） | **两段式**：`prepare_upload()`（锁外，下载/复制到 `temp/`、边写边算 MD5、边判大小上限）+ `FileService::commit_upload()`（锁内，去重 → 重名 → `file_id` → 搬文件 → 写 `file.json`）；运行体 `ServerRuntime::run_upload()` 编排、**管道与 HTTP 共用这一份**（提交 `188e85d`） | 下载可能几十秒到几分钟；持着运行体那把业务锁下载会把 `bucket list`、`trash *` 与浏览器请求一起卡住。**长任务不持锁，锁只保护元数据提交那一下**——这条同时兑现了原来的「阶段 5 必须定锁粒度」 |
| 25 | `file delete` 只收 `file_id`，而 `file get` 两种参数都收（开发文档没写理由） | **两者定成同一套定位规则**：先当 `file_id`（全局唯一、形状 `fmt-YYYYMMDD-N` 固定，先查不误伤名字），再当文件名（当前用户 + 正常文件，同用户跨 Bucket）；**名字在、但已在回收站** → `FMT-001` 并给出 `file_id`（**不是** `FMT-002`），都没有才 `FMT-002`。`remove()` 侧由私有 `locate_record(records, key)` 实现（get 侧仍是 `get_by_id()` + `get_by_name()`，规则同、代码两套）；签名 `remove(std::string_view file_id_or_name)`；回收站落点用**记录自己的 `bucket`**（提交 `0ad9efc`） | 不对称本身没有理由；只按 `file_id` 单向查找时，`file delete <文件名>` 必然落空并报「文件不存在」——可文件明明存在，只是用名字称呼它，这是**诊断错误**。对齐后用户也不必先 `file get` 换 id（`FMT 技术文档.md` 第 10.2.4、18.21 节）。**提交 `9c3d2cb` 补充**：规则仍同一套，但 `file delete` 的 `locate_record()` 多一步——两个索引同时命中**不同**记录时报 `FMT-001` 并点名两条记录；这是**有意的差异**（get 是只读查询，最坏是把 id 命中的那条给用户看；delete 是破坏性操作，不能猜），`file get` 的两种查询范围保持不变 |
| 26 | 名字 / 标识的比较是 `==` 精确比较（旧实现没意识到 Windows 的路径不区分大小写） | **一律改 `iequals()`（ASCII 折叠）**：`commit_upload()` 的重名判定、`get_by_name()`、`locate_record()` 的三处比较；桶侧 `list()` 的 `is_current`、`remove()` 的 `was_current`、`find_trashed()` 的两轮定位（提交 `5bf2c1f`） | **这是数据损坏修复，不是体验优化**：`doc.txt` 之后再传 `DOC.TXT` 时精确比较会放过，而两者在 Windows 上落到**同一个磁盘路径**——第二次上传**覆盖**第一个文件的字节，`file.json` 里第一条记录还写着旧的 `size`/`md5`，即**同一个磁盘文件被两条记录指向 + 字节被覆盖 + 元数据失真**。回归用例 `File.大小写不同的同名必须被当成重名` 先复现、后修复（`FMT 技术文档.md` 第 18.22 节、`FMT 开发文档.md` 第 38 节） |
| 27 | `current_bucket` 存**用户敲的拼写**（`bucket use WORK` 存 `WORK`，而目录叫 `work`），文件级记录里的 `bucket` 与 `.original` 的 `original` 也就可能各留一份拼写；同形的文件名（`fmt-YYYYMMDD-N`）被「先查 id、再查名字」的第一步遮住，只能靠运气 | **提交 `9c3d2cb` 收口**：① Bucket 名称 `create` 时先按 ASCII 折叠转小写再建目录（`WORK` → `work`，`renamed` 为真时回 `note` 提示用户），`use` / `get` / `delete` 用 `canonical_name()` 规范化到**磁盘上的实际名字**——`current_bucket`、`file.json` 的 `bucket`、`.original` 的 `original` 只留一份拼写，restore 拼出来的目录名才一致；② 文件名与 `file_id` 同形 → 上传时按**保留形状**拒绝（`FMT-106`，与 Windows 保留设备名同类），旧数据里已经存在的这种名字在 `file delete` 报歧义（`FMT-001`，点名两条记录让用户用 `file_id`） | 目录不区分大小写，`WORK` 与 `work` 本来就是同一个目录；不统一拼写，记录之间会各留一份、日后比对与恢复都踩坑。名字与 `file_id` 同形则是定位规则本身的结构性歧义：一个文件叫 `fmt-20261008-0`、另一个文件的 `file_id` 恰好是它，按名字提交的删除就会删错对象 |
| 28 | `iequals()` 把 `>= 0x80` 的字节交给 `std::tolower`（C locale 下虽是恒等，但一旦有人调 `setlocale` 就会把 UTF-8 名字改坏）；`file get` 对回收站记录只说「在回收站」不说在哪 | **提交 `9c3d2cb`**：`iequals()` 改成**只折叠 ASCII**（`>= 0x80` 原样比较）——这是「比较一律不区分大小写」那条口径的**实现约束**，中文名字不受影响；`file get` 命中的记录在回收站时**增加** `trash_path`（相对数据根、正斜杠），仓库里找不到该文件时**不返回** `path`（设计如此，不是缺失），CLI 多打一行「回收站路径：…」 | `setlocale` 一旦被调用，`std::tolower` 就会按当前 locale 改字节，UTF-8 多字节序列会被拆坏；「在回收站」配上「在回收站哪儿」才算可用信息 |

---

## 12. 未决事项

| 事项 | 说明 |
|---|---|
| 日志轮转 | 单文件大小上限与轮转规则仍未定 |
| HTTP 鉴权 | 目前仅监听 `127.0.0.1`，局域网访问的安全控制后续再做 |
| 提权副本的结果通道细节 | **已定稿，从未决清单移出**：结果经结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 回传（第 4.4 节），数据根不可写时退回 `%TEMP%` 同名文件并记一行 WARN；命名管道方案作废 |
| 数据根切换的并发保护 | 目前依赖「只有一个 CLI 窗口」，多窗口场景不在本次范围 |
| 名字长得像 `file_id` 时会被「先查 id」遮住 | **已定稿，从未决清单移出（提交 `9c3d2cb`，第 2 节第 25 项、第 11 节差异 #27）**：两条可选做法**都做了**，不是二选一——① 上传时拒绝形如 `fmt-YYYYMMDD-N` 的**保留形状**（`FMT-106 FileNameLikeFileId`，退出码 2，`looks_like_file_id()`；显式名与从来源推断的名字都拦）；② 旧数据里已经存在的这种名字，`file delete` 在两个索引命中**不同**记录时报 `FMT-001`「有歧义：…」并点名两条记录、让用户直接用 `file_id`。**只给 `file delete` 加**（`locate_record()` 只被 `remove()` 用）；`file get` 的两种查询范围保持不变（按 `file_id` 全局含回收站，按文件名只查当前用户的正常文件）——这是**有意的差异**：get 只读，最坏是把 id 命中的那条给用户看；delete 破坏性，不能猜 |
| 按名字删除可能删到别的 Bucket 的文件（**待决，提交 `5bf2c1f` 之后记入**） | 名字的作用域是「同用户跨 Bucket」（第 4.2 节、`FMT 开发文档.md` 第 9.2、43 节），所以人在「生活」桶里敲 `file delete a.txt` 可能把「工作」桶里的 `a.txt` 删掉，而成功消息里只有文件名。可选做法（未定）：`message` 带上 Bucket，形如 `文件已移入回收站：a.txt（Bucket：工作）` |
| `bucket use` 的拼写要不要规范化 | **已定稿，从未决清单移出（提交 `9c3d2cb`，第 2 节第 24 项、第 11 节差异 #27）**：**落盘时规范化成磁盘上的实际名字**。新增私有 `BucketService::canonical_name()`（不区分大小写地扫桶目录，返回磁盘上的实际拼写；找不到才退回 `to_lower()`），`use` 存进 `current_bucket` 的就是它（`use WORK` 存 `work`），`get` 返回的 `name`、`delete` 的回收站目录名与 `.original` 里的 `original` 同样用它——restore 拼出来的目录名才与原来一致。显示层原有的 `iequals()` 照旧（第 11 节差异 #26） |
| **阶段 5 的锁粒度（开工前必须定）** | **已定，从未决清单移出（提交 `188e85d`）**：阶段 4 的并发形态是**管道连接级严格串行**、**HTTP 线程池并发**、**业务命令共用运行体的一把互斥锁**（互斥不是队列）。原来的代价是「一个慢命令卡住两条入口的所有业务命令」，上传/下载尤其明显。**实现选的是「把长任务移出锁」的简化形态——长任务不持锁**：`prepare_upload()`（下载/复制到 `temp/`、边写边算 MD5、边判大小上限）在**锁外**，`commit_upload()`（去重 → 重名 → `file_id` → 搬文件 → 写 `file.json`）在**锁内**，运行体 `ServerRuntime::run_upload()` 把两段串起来、管道与 HTTP 共用这一份；`file.upload` 因此是唯一不落在 `execute_business()` 里的 op。**没有引入细粒度锁**（`file_mutex` / `id_mutex` 仍是目标形态）。**换根也不会插进下载期间**：换根只由 `hello` 触发，而管道 accept/serve 串行，上传期间不会再处理第二个请求（`FMT 技术文档.md` 第 15.1 ④、15.3、18.16、18.19 节） |
| `file.upload` 的命令超时值 | **已定并落地（提交 `a2b6cd1`，从未决清单移出，保留残余风险）**：`include/fmt/ipc/protocol.hpp` 新增 `kUploadTimeoutMs = 30 * 60 * 1000`（30 分钟），CLI 对 `file.upload` 用它、**其余命令仍是 `kCommandTimeoutMs = 30000`**（`src/cli/cli.cpp:651` 按 operation 选值）。CLI 在等待超时时额外打印「提示：等待服务响应超时。服务端可能仍在处理，稍后用 file list 确认；也可以查看 log/fmt.log。」**这是「已知边界 + 现有缓解」，不是彻底解决**：30 分钟上限到了仍可能出现「用户看到失败、服务端已经入库」；原文要求的「进度语义」仍未做。`FMT 技术文档.md` 第 19.1 节保留此条 |
| 下载走哪个 HTTP 客户端 | **已定（提交 `a2b6cd1`）**：用 **WinHTTP + Schannel**（`include/fmt/common/http_client.hpp` + `src/common/http_client.cpp`，`src/common/CMakeLists.txt` 链系统库 `winhttp`），**不再用 `cpp-httplib` 的 `Client`**（那个 Client 走 https 要 OpenSSL，自己编需要 Perl + NASM，破坏离线可构建；静态链接 OpenSSL 又会在 `/MT` 与常见 `/MD` 静态包之间混 CRT）。WinHTTP 是系统组件，TLS 用系统证书库，**不分发任何 DLL**，并自动使用系统代理（`AUTOMATIC_PROXY`，失败退回 `DEFAULT_PROXY`）。`src/file/CMakeLists.txt` 因此**不再链 cpp-httplib**；`httplib::Client` 在业务代码里没有调用点（测试仍在用：`tests/server_test.cpp` 用它打同进程的 `httplib::Server`）。完整行为见 `FMT 开发文档.md` 第 33 节与 `FMT 技术文档.md` 第 10.2.2、18.20 节 |
| 双击引导的重装判定 | **已定稿，从待决清单移出**：等待时长**不写死**，按 SCM 的 `dwWaitHint` 自适应（夹在 100 ms – 2000 ms，兜底 30 秒，不是等待类就立即结束，第 4.1 节）；仍没起来先读 `dwServiceSpecificExitCode` 还原 FMT 编号（第 4.1、6 节）：命中数据根/配置类错误码集合（第 5.4 节表）就**不重装**、只打印原因，其余才提权 `reinstall` 一次 |
| `service status` 是否要机器可读输出 | **已定稿，从待决清单移出**：**V1 不做 `--json`、也不预留参数名**。机器可读通道是**命令退出码**（`0` 成功 / 未安装 `FMT-601` → `8`），人类可读通道是固定顺序的那几行文本（第 6 节） |

