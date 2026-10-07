# FMT 项目架构

> FMT：Windows 平台的文件管理系统。**单一 `fmt.exe`、三种形态**：CLI 形态、被 SCM 启动的
> Service 形态、提权短命副本。CLI 与浏览器都只是客户端，直连同一个常驻 Service 进程。
>
> **文档状态：重构版（分支 `arch-restart`，基线 `bdbe33d`）。** 本版按已冻结的重构决策重写了
> 架构描述：`fmt.exe` 多形态与 `asInvoker` manifest、CLI↔服务的命名管道、浏览器↔服务的 HTTP、
> `service` 五命令（四条动作命令提权、`status` 查询不提权）与提权语义、数据根由 CLI 声明、
> 阶段顺序前移（Windows Service 提到阶段 2/3）；CLI 双击时对自己数据根做幂等体检与补齐。
> 旧版《项目架构》中「`FMT_ROOT` = exe 所在目录」「`fmt.exe --service install`」「CLI 走
> HTTP」「`service` 有 `pause` / `delete`」「`service` 只有四条命令」「初始化只由服务执行、
> CLI 只读不建目录」等描述**已作废**，与本文冲突时以本文为准。
>
> 文档关系：
>
> | 文档 | 定位 |
> |---|---|
> | `FMT 开发文档.md` | **规范主体**：模块职责、数据结构、命令、操作流程、测试要求 |
> | `FMT 技术文档.md` | 技术实现：技术栈、接口设计、实现方式，依据开发文档编写 |
> | `FMT 项目架构.md`（本文档） | 总体架构：对象关系、分层、设计决策、错误码清单 |
> | `FMT 重构设计.md` | 本次重构的差异说明与冻结决策索引，**架构改动的源头文档** |
> | `项目基本提示词.txt` | 项目背景说明（项目类型、架构方向），**不是规范依据** |
>
> 冲突时以 `FMT 开发文档.md` 为准；**进程形态、进程间通道与本文第 2 节的冻结决策以本文为准**。
> 本文档描述**当前代码的真实状态**。

---

## 1. 系统定位

FMT 的核心数据对象逐层归属：

```text
User
 │
 ├── Bucket           逻辑存储空间，用名称标识
 │     │
 │     └── File       用 file_id 标识，MD5 去重
 │           │
 │           └── Share 一次分享，独立管理有效期与下载次数
 │
 └── Trash            回收站，管理被删除的 File 与 Bucket
```

V1 用 **JSON + 文件系统**满足需求，不使用数据库、Redis、MQ、微服务或复杂任务调度器。**常驻进程
只有一个**：由 SCM 托管的 Windows Service（`fmt.exe` 的 Service 形态，见第 4 节）。CLI 不是常驻
进程，只是连上它的一次性客户端。

采用分层架构，CLI 与 HTTP Server **不直接操作底层文件**，都经同一套业务核心，避免出现两套文件处理逻辑。
两条入口在进程上落在**同一个 Service 进程**里：通道不同，业务实现只有一份（第 4 节）。

```text
              用户操作层
     ┌────────────┴────────────┐
    CLI                   HTTP Server
     │                         │
     └────────────┬────────────┘
                  ▼
             Service 层
     Bucket / File / Share / Trash / Config
                  │
                  ▼
               Core 层
     文件操作 / MD5 / file_id / 路径管理 / 数据校验 / 事务
                  │
                  ▼
              Storage 层
     repository / trash / data/*.json / config/*.json
```

## 2. 关键设计决策

V1 开发期间以下规则视为核心规则（`FMT 开发文档.md` 第 122 节）。其中第 **16～26** 项是
`arch-restart` 分支新增的**重构冻结项**，逐条对应 `FMT 重构设计.md` 第 2 节的决策索引：

| # | 规则 |
|---|---|
| 1 | Bucket **没有**独立 `bucket_id`，用 `bucket_name` 标识 |
| 2 | File 用 `file_id` 标识，格式 `fmt-YYYYMMDD-N` |
| 3 | Share 用 `share_id` 标识，必须唯一且不可预测 |
| 4 | File 用 MD5 去重，重复不上传物理文件、不生成新 ID |
| 5 | 同一用户的**正常**文件名唯一；Bucket 不构成命名空间 |
| 6 | Trash 不占用正常文件名空间 |
| 7 | `file_id` 不因删除、恢复而改变 |
| 8 | Bucket 删除不创建 `file_id`，其下文件只改 `is_trash` |
| 9 | 冲突恢复**不覆盖、不改名**，文件留在 Trash |
| 10 | `current_bucket` 无效时**不自动**选择其他 Bucket |
| 11 | `max_download_count` 属于 Share，File 本身不保存下载限制 |
| 12 | Share 默认最大下载次数 20，与过期时间相互独立 |
| 13 | JSON 损坏**不能静默重置** |
| 14 | 关键文件操作必须具备事务式处理 |
| 15 | CLI 与 HTTP 使用统一业务核心 |
| 16 | 单一 `fmt.exe` 承载三种形态：CLI 形态、Service 形态（SCM 启动）、提权短命副本（内部参数 `--elevated`） |
| 17 | manifest 固定 `asInvoker`，**绝不** `requireAdministrator`；提权只发生在提权副本里 |
| 18 | Service 是**全局单例**，也是唯一写 `data/*.json` 的进程；CLI 只是客户端，不碰 core 业务操作、不写任何业务 JSON。唯一例外是**双击时对自己所在数据根做一次体检与补齐**（只补缺失的目录与默认 JSON，见第 3.2 节），以及建写日志用的 `log/` 与提权前要用的 `temp/`（第 7.2 节） |
| 19 | CLI 单实例：命名互斥体 `Local\FMT.CLI.v1`，已有窗口则激活、不新建——因此**同时只有一个 CLI 窗口，也就只有一个数据根** |
| 20 | **数据根由 CLI 声明**：CLI 用 hello 帧上报自身 exe 所在目录，服务维护「当前数据根」，切换**不删**旧根数据；响应用 `switched` / `previous_root` 回执本次是否发生切换（第 3.1 节） |
| 21 | 初始化规则**两边共用同一套幂等实现**（`ensure_root` / `check_root`）：六个目录（`repository/`、`trash/`、`config/`、`data/`、`log/`、`temp/`）+ 默认 JSON，`.tmp` 原子替换，**只补缺失、已存在不改、不删、不覆盖**；服务在启动/换根时执行，CLI 在双击时对自己的数据根执行 |
| 22 | 双入口直连服务：CLI 走命名管道 `\\.\pipe\fmt.control`，浏览器走 `127.0.0.1:4122`（cpp-httplib），两者进同一个 service 层 |
| 23 | `service` 命令有 `install` / `uninstall` / `start` / `stop` / `status` **五条**，**无 `pause`、无 `delete`**（`delete` 更名为 `uninstall`），命令**不带 `--` 前缀**；其中 `status` 是**只读查询**，见第 24 项 |
| 24 | **四条动作命令**（`install` / `uninstall` / `start` / `stop`）都走 UAC 提权，不做「目标状态已满足就免提权」的优化；`status` **不提权、不弹 UAC**，它是查询命令 |
| 25 | 服务自身状态写 `%ProgramData%\FMT\service.json`（当前数据根 + 安装信息），**不属于业务数据** |
| 26 | vendor `nlohmann/json` + `cpp-httplib` 到 `third_party/`（头文件入库），`/MT` 静态链接 CRT，产物仍然只有一个 `fmt.exe` |

系统明确**禁止自动**执行：覆盖文件、修改用户文件名、选择其他 Bucket、创建恢复目标
Bucket、绕过下载限制、删除文件、清空损坏 JSON、**删除旧数据根的数据**、**把服务宿主的
`fmt.exe` 复制到别处**（宿主就是首次安装时注册的那个路径）。

优先级：**数据安全 > 数据一致性 > 功能正确 > 性能 > 代码简洁**。

依赖约束（**重构后已放宽**）：V1 允许 vendor **两个**第三方库——`nlohmann/json`（JSON 读写，
业务数据、IPC 帧与响应信封共用）与 `cpp-httplib`（HTTP 服务端与 URL 下载客户端），以头文件形式
放进 `third_party/`，不用包管理器、不用 FetchContent、不产生额外 DLL。CRT 用 `/MT` 静态链接，
产物仍然只有一个 `fmt.exe`，可完全离线构建。除此之外只使用 C++17 标准库与 Windows API
（单元测试用仓库内自带的极简运行器，不引入任何测试框架）。第三方库只在 `storage` 与 `server`
等少数模块内可见，业务模块不直接包含它们的头文件。命名空间 `fmt` 仍不存在与外部库重名的
问题，保持不变，后续也不再讨论改名。

## 3. 运行目录

### 3.1 数据根由 CLI 声明

`FMT_ROOT`（数据根）**不再固定为 exe 所在目录**，而是**由 CLI 声明**：

```text
CLI 启动
  → GetModuleFileNameW 取自身路径 → 取父目录 = 自己声明的 root（如 D:\FMT2）
  → 先对自己这个 root 做一次体检与补齐（第 3.2 节，只补缺失、不碰业务数据）
  → 连上服务后首帧就是 hello：{"id":1,"op":"hello","root":"D:\\FMT2","pid":1234}
  → 服务比对：
       root != 当前数据根 ?
         ├─ 是 → 切换数据根（旧根数据原样保留），对新根做幂等初始化，
         │        记 INFO「数据根切换: D:\FMT → D:\FMT2」，
         │        在 hello 响应里置 switched=true 并回填 previous_root
         └─ 否 → 直接进入命令循环，响应不含 switched/previous_root
  → 该连接上的所有业务命令都在该数据根下执行
```

`hello` 的响应体（复用第 4.3 节的信封，`data` 部分）：

```json
{ "id":1, "ok":true, "data":{ "root":"D:/FMT2", "switched":true, "previous_root":"D:/FMT" } }
```

| 字段 | 含义 |
|---|---|
| `root` | 服务处理后确认的当前数据根（等于 CLI 声明的 root） |
| `switched` | 这次声明是否**导致服务切换了数据根**；未切换时该字段不出现（或缺省为 false） |
| `previous_root` | 切换前的旧数据根；**只在 `switched` 为真时出现** |

`switched` 为真时，CLI 额外记一行日志 `[Service] 数据根切换：旧 -> 新`（只进日志），所以双击一个新目录就能看见
服务跟过来了；未切换时保持原来的静默。

数据根的取值优先级：

| 场景 | 数据根 |
|---|---|
| CLI 已连接（收到 hello 帧） | CLI 声明的目录，即 **CLI 自己 exe 所在目录** |
| 服务开机自启、无 CLI 连接 | `%ProgramData%\FMT\service.json` 里记录的上一次数据根 |
| 从未记录过 | 服务宿主 exe 所在目录 |

因为**同时只有一个 CLI 窗口**（第 2 节第 19 项、第 4.7 节），所以**同时只有一个数据根**，不会出现
两个根互相打架。换数据根必须先关闭旧 CLI 窗口，再启动新目录下的 `fmt.exe`。

### 3.2 数据根目录布局

```text
<数据根>/                   ← 由 CLI 声明，通常等于 CLI 的 fmt.exe 所在目录
├── fmt.exe
├── repository/           正式文件存储
│   └── <user>/<bucket>/YYYY/MM/DD/<file_name>
├── trash/                回收站，保持原有层级便于恢复
│   └── <user>/<bucket>/YYYY/MM/DD/<file_name>
├── config/
│   ├── config.json       version、current_user、current_bucket、max_upload_size、size_unit、language
│   └── server.json       version、enabled、host、port（默认 127.0.0.1:4122）
├── data/
│   ├── user.json         {"version":1,"users":[]}
│   ├── file.json         {"version":1,"files":[]} 文件元数据
│   ├── share.json        {"version":1,"shares":[]} 分享记录
│   └── trash.json        {"version":1,"trash":[]} 回收站管理信息
├── log/                  运行日志（正式约定，与业务数据分离）
│   ├── fmt.log           全部日志，Service 与 CLI 追加同一个文件（第 7.2 节）
│   └── error.log         仅 ERROR 级
└── temp/                临时文件（既不是业务数据、也不是日志，随时可以清空）
    └── fmt-elev-<父进程 pid>.json   提权结果文件，父进程读完立刻删除（第 4.5 节）
```

初始化是**幂等**的，且**两边共用同一套规则**（同一份 `ensure_root` / `check_root` 实现）：
**服务在启动/换根时对自己的数据根执行，CLI 在双击时对自己的数据根执行**；两边都只补不缺、
都不碰业务数据内容。规则细节：

- 六个目录（`repository/`、`trash/`、`config/`、`data/`、`log/`、`temp/`）不存在则创建，已存在则保持原样，
  **绝不删除、清空或覆盖**已有内容。
- **`current_user` 由初始化补上占位名 `user`**（阶段 4 起）：这一步在**服务侧**的
  `initialize_root()`（启动或 hello 换根时）里做——写出默认 `config.json` 之后读一次配置，
  发现 `current_user` 为空就置为 `user` 并保存。CLI 双击时调的是共用的
  `ensure_root` / `check_root`（只补缺失的目录与默认 JSON），**不写配置内容**。
  因此磁盘上的是 `repository/user/<bucket>/…`，用户不需要先「设置当前用户」；
  `FMT-604 NoCurrentUser` 只在用户被显式清空时出现（`FMT 开发文档.md` 第 93 节、
  `FMT 技术文档.md` 第 4.4.1 节）。
- 默认 JSON（`config/config.json`、`config/server.json`、`data/file.json`、`data/share.json`、
  `data/trash.json`、`data/user.json`）只在缺失时写入；关键 JSON 一律先写 `.tmp` 再原子替换，
  避免异常退出写出半文件。
- **已存在的 JSON 会被真正读一遍**（解析 + 版本检查）来确认完整性；读不出来或版本不受支持的，
  **只报告、绝不重置**——沿用第 2 节第 13 项「JSON 损坏不能静默重置」。日志记一行
  `[Cli] 数据根损坏（未自动修复）：data/file.json`，同时把异常送到 stderr。
- CLI 侧在**打开日志器之前**执行这一步，所以 `log/` 也由它创建、并出现在同一条「新建目录」清单里。
  CLI 仍然**不改任何业务数据**（不写 `data/*.json` 的内容、不删文件），它只是把「缺的目录和默认文件」补齐。

CLI 双击时这一步的结果**只进日志**（`log/fmt.log`，模块 `Cli`），**控制台一行都不打**：

```text
[Cli] 数据根检查：D:/FMT2
[Cli] 数据根新建目录：repository, trash, config, data, log, temp
[Cli] 数据根新建文件：D:/FMT2/config/config.json, ...
```

第二次及以后双击（什么都没缺）只是日志里换成一行：

```text
[Cli] 数据根检查：D:/FMT2
[Cli] 数据根完整
```

**只有异常才走 stderr**：数据根无法补齐（如 `FMT-013`）、以及
`数据根文件损坏（未自动修复）：data/file.json`。理由见第 7.5 节：
控制台负责用户交互与重要异常，日志负责完整运行记录。

CLI 曾经的两处「例外」（写日志用的 `log/` 与执行提权命令前要用的 `temp/`）现在都已经被这一步
统一覆盖：CLI 建目录这件事不再有特例，只有**补缺失**这一条规则（第 4.3 节、第 7.2 节）。

**`temp/` 的定位与规则**：

```text
定位      临时文件目录：不是业务数据、也不是日志，内容随时可以清空
放什么    ① 提权结果文件 temp/fmt-elev-<父进程 pid>.json（父进程读完立刻删除，第 4.5 节）
          ② 以后上传时的暂存文件（旧文写「temp/ 或者系统临时目录」，现在明确为 temp/）
谁创建    CLI 在执行提权类命令前会尝试创建 <数据根>/temp；创建不出来（例如 exe 放在只读位置）
          就退回系统临时目录 %TEMP%，并写一行 WARN 说明原因与改用后的路径。
          提权副本在写入结果文件前也会确保目录存在——它自己有权限，
          所以调用方数据根只读时它仍然能建出来
清理      Service 启动时删除 temp/ 下以 fmt- 开头的遗留文件（上次异常退出留下的提权结果等），
          用户手放进去的其它文件一律不动；删除数量记一行 INFO（第 4.6 节 ServiceMain 序列）
```

`temp/` 跟着 exe 走：它在数据根下，用户一眼能找到、随时可清。它按定义就是**可清空**的目录，
与 `repository/`、`data/`、`config/` 分属不同职责，因此把短暂的提权结果或上传暂存放进去，
不会污染业务数据。

### 3.3 服务自身状态文件（不属于业务数据）

```text
%ProgramData%\FMT\service.json     ← 只存「当前数据根路径」与安装信息
```

它描述「服务怎么跑」，不是任何数据根里的业务数据，因此**不放进数据根，也不放进 `log/`**。
数据根被删除或移动后，服务仍能读到这里记录的路径并报出明确错误，而不是自己猜一个目录。
服务开机自启且没有 CLI 连接时，数据根取这里的记录值；从未记录过则取服务宿主 exe 所在目录
（见第 3.1 节优先级表）。

## 4. 进程与通道

### 4.1 `fmt.exe` 的三种形态

同一个 `fmt.exe`（**只有一个可执行 target**，绝不拆成 `fmt_cli` / `fmt_svc`），靠**启动方式与内部
参数**区分三种形态：

| 形态 | 触发方式 | 生命周期 | 职责 |
|---|---|---|---|
| CLI 形态 | 用户双击或命令行启动 | 用户关窗口为止 | 交互界面、声明数据根、经命名管道发请求；双击时对自己所在数据根做一次幂等体检与补齐（只补缺失，见第 3.2 节），此外**不碰 core 业务操作、不写业务 JSON**（写日志用的 `log/` 与提权前要用的 `temp/` 见第 7.2 节） |
| Service 形态 | SCM 按注册的 `binPath` 启动 | 常驻，开机自启 | 服务入口、初始化数据根、承载业务核心与 HTTP 入口；**唯一写 `data/*.json` 的进程** |
| 提权短命副本 | CLI 用 `ShellExecuteExW(lpVerb=L"runas", ...)` 拉起自己，带内部参数 `--elevated` | 秒级，做完一次操作即退出 | 只执行一次 `service install/uninstall/start/stop/reinstall`，把结果写进结果临时文件回传给父进程。`service status` **不在这里**——它是查询命令，不提权、不进提权副本 |

入口分发靠一个 API 完成，不需要额外的模式参数：

```text
wmain
 ├─ StartServiceCtrlDispatcherW({ L"FMT", ServiceMain })
 │     ├─ 成功                                            → Service 形态
 │     ├─ 失败且 ERROR_FAILED_SERVICE_CONTROLLER_CONNECT  → 用户启动的 → CLI 形态
 │     └─ 其它错误                                        → FMT-602 / 退出码 8
 └─ CLI 形态若带内部参数 --elevated → 提权短命副本分支（不进命令循环）
```

- SCM 只给 **30 秒**连接窗口，所以 `wmain` 必须**尽早**调用 `StartServiceCtrlDispatcherW`。
- `ServiceMain` 里初始化慢时先 `SetServiceStatus(SERVICE_START_PENDING)` 并上报 `dwCheckPoint`，
  否则 SCM 报 1053。
- `ServiceMain` 里初始化**失败**时，把 FMT 编号（`FMT-NNN` 解析出的数字部分，**不是退出码**）
  写进 `SERVICE_STATUS::dwServiceSpecificExitCode`，并置 `dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR`、
  `dwCurrentState = SERVICE_STOPPED`。CLI 用 `QueryServiceStatusEx` 读回这个编号、映射成错误码，
  于是能打印 `服务启动失败：FMT-008 配置错误`，而不是只会说「启动失败」（第 4.8 节引导流程第 3 步）。
- manifest **固定 `asInvoker`**（`resources/fmt.manifest`）：CLI 形态与 Service 形态都不需要管理员
  权限，只有提权副本需要，而它由 UAC 临时授权，所以**绝不**写 `requireAdministrator`。

```text
                        fmt.exe（唯一产物，asInvoker）
        ┌───────────────────────┼───────────────────────┐
   双击 / 命令行              SCM 启动              ShellExecuteExW "runas"
        │                       │                        │
   CLI 形态（唯一窗口）     Service 形态（常驻）      提权短命副本（SW_HIDE，无窗口）
```

### 4.2 通道总图

```text
  ┌────────────────┐  命名管道 \\.\pipe\fmt.control   ┌──────────────────────────────┐
  │  CLI 窗口       │ ──────────────────────────────→ │  fmt.exe（Service 形态）      │
  │  互斥体单例     │ ←────────────────────────────── │  全局唯一，开机自启           │
  └────────┬───────┘  [4B 长度][UTF-8 JSON]          │                              │
           │                                         │   ipc ──┐                    │
           │ ShellExecuteExW "runas"                 │         ├─→ service 层        │
           ▼                                         │ server ─┘         │           │
  ┌────────────────┐  结果临时文件                    │                   ▼           │
  │ 提权短命副本    │ ──────────────────────────────→ │              core 层          │
  │ 写入 {ok, code, │ ←────────────────────────────── │                   │           │
  │ message, exit}  │  temp\fmt-elev-<pid>.json       │              storage 层        │
  └────────┬───────┘                                 └─────────── 当前数据根 ──────────┘
           │ Advapi32（OpenSCManagerW / CreateServiceW / ControlService / DeleteService）
           ▼
  ┌────────────────┐
  │ 服务控制管理器  │
  └────────────────┘

  浏览器 ── HTTP 127.0.0.1:4122（cpp-httplib，进程内线程）──→ Service 形态的 server 模块
```

三条路径互相独立，不能混用：

| 路径 | 谁走 | 通道 | 为什么不能混 |
|---|---|---|---|
| 业务命令 | CLI → 服务 | 命名管道 | HTTP 可关闭、端口可被占，CLI 不应随之失效 |
| 业务命令 | 浏览器 → 服务 | HTTP `127.0.0.1:4122` | 浏览器只会 HTTP |
| 服务生命周期 | CLI → SCM | **直连 SCM + UAC 提权** | 服务尚未安装时走任何进程间通道都是死锁 |
| 提权结果回传 | 提权副本 → CLI 父进程 | **结果文件** `<数据根>\temp\fmt-elev-<父进程 pid>.json`（数据根不可写时退回 `%TEMP%`） | 提权副本是高完整性进程，它创建的命名管道会被 MIC「禁止向上写」挡住（4.5 节） |

### 4.3 CLI ↔ 服务：命名管道（已冻结）

```text
管道名   \\.\pipe\fmt.control
类型     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT
实例数   PIPE_UNLIMITED_INSTANCES
帧       [4 字节小端长度][UTF-8 JSON]，一请求一响应，用 id 配对
```

请求：

```json
{"id": 1, "op": "hello", "root": "D:\\FMT2", "pid": 1234}
{"id": 2, "op": "bucket.list"}
```

响应**复用 HTTP 信封**——同一份信封在两个通道上复用，错误码用同一个函数还原：

```json
{"ok": true,  "data": { }}
{"ok": false, "error": {"code": "FMT-305", "message": "未设置当前 Bucket"}}
```

`id` 由 CLI 生成、服务原样回填；`root` 是 CLI 声明的数据根；`pid` 供服务诊断。

两个必须处理的 Windows 坑：

1. **DACL**：服务以 `LocalSystem` 运行，管道默认只有 SYSTEM / Administrators 能连。必须用
   `ConvertStringSecurityDescriptorToSecurityDescriptorW` 显式授权交互用户（IU）：
   `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`
2. **强制完整性级别（MIC）**：高完整性进程创建的对象带高完整性标签，中完整性的普通 CLI 会被
   「禁止向上写」挡住。必须再给管道加 MIC 标签 `S:(ML;;NW;;;ME)`，否则非提权 CLI 连接报
   `ERROR_ACCESS_DENIED`。

超时：连接时管道不存在最多重试 **5 秒**（权限类错误不重试）、普通命令 **30 秒**。
连不上服务（`ERROR_FILE_NOT_FOUND`）时输出
「无法连接 FMT Service，请先执行 service install」，错误码 `FMT-601`、退出码 8。

### 4.4 浏览器 ↔ 服务：HTTP

```text
http://127.0.0.1:4122        cpp-httplib，编译进 fmt.exe，只在 Service 形态里监听
```

- 开关与地址由**数据根**下的 `config/server.json` 控制（`enabled`、`host`、`port`，默认
  `127.0.0.1:4122`），默认只绑本机回环，不监听外部网卡。
- HTTP 请求作用于**当前数据根**。
- 浏览器入口与 CLI 入口**进入同一个 service 层**（第 1 节分层图），业务行为天然一致，不存在
  第二套文件处理逻辑。
- 响应体与第 4.3 节的信封完全一致。V1 只实现 HTTP；HTTPS 仍列在 V1 范围之外（第 14 节）。

### 4.5 `service` 命令：直连 SCM + UAC 提权（已冻结）

```text
fmt> service install     提权 → OpenSCManagerW → CreateServiceW（AUTO_START、LocalSystem）
                              → 配 Recovery → StartServiceW；已存在 → FMT-600，不重复创建
fmt> service uninstall   提权 → ControlService(STOP) → DeleteService
                              （不得删除 repository / trash / data / config / log / temp）
fmt> service start       提权 → 存在则 StartServiceW；不存在 → FMT-601
fmt> service stop        提权 → ControlService(SERVICE_CONTROL_STOP) → 停止接受新请求
                              → 等在途事务 → 停 HTTP → 退出
fmt> service status      **不提权** → service::query_status()（第 4.6.1 节；「未安装」是正常结果）
                              → 打印状态 / 宿主 / 数据根 / 错误码；未安装 → FMT-601、退出码 8
```

`service status` 的输出样例（服务已安装且运行中）：

```text
服务状态：运行中
服务宿主：D:/FMT2/fmt.exe
服务数据根：D:/FMT2
错误码：0
```

- 状态名由 `service::StatusInfo::state` 映射（`service::state_name()` 一处提供）：
  `NotInstalled` → `未安装`（`OpenServiceW` 报 `ERROR_SERVICE_DOES_NOT_EXIST`）/
  `Stopped` → `已停止` / `StartPending` → `正在启动` / `StopPending` → `正在停止` /
  `Running` → `运行中` / `ContinuePending` → `正在继续` / `PausePending` → `正在暂停` /
  `Paused` → `已暂停` / `Unknown` → `未知`（拿到服务但状态码不认识，不猜测）。
- `服务宿主` 取 `service::installed_binary_path()`（`QueryServiceConfigW` 的 `lpBinaryPathName`，
  去掉引号）；`服务数据根` 取 `%ProgramData%\FMT\service.json` 里记录的当前数据根
  （`service::load_state()`，第 3.3 节）。
- 未安装时 stdout 打印 `服务状态：未安装`，退出码 **8**（`FMT-601 ServiceNotInstalled`）；
  已安装但未运行时照常打印（如 `已停止`），退出码 **0**——「服务没在跑」不是错误，
  `status` 只回答「它现在什么样」。
- `query_status()` 自身**只报「查询失败」**（打不开服务控制管理器等 → `FMT-602` / 8）；
  「未安装要返回 `FMT-601` / 8」是命令层的判断（第 4.6.1 节）。

- **只有这五条**：`install` / `uninstall` / `start` / `stop` / `status`。没有 `pause` / `continue`，
  也没有 `delete`（旧文档的 `delete` 已更名为 `uninstall`）。命令**不带 `--` 前缀**：写
  `fmt.exe service install`，旧写法 `fmt.exe --service install` 作废。
- 服务**不声明** `SERVICE_ACCEPT_PAUSE_CONTINUE`；`HandlerEx` 只处理 `SERVICE_CONTROL_STOP`、
  `SERVICE_CONTROL_SHUTDOWN`、`SERVICE_CONTROL_INTERROGATE`。
- **四条动作命令都提权**：`install` / `uninstall` / `start` / `stop` 即使目标状态已经满足也照常提权，
  不做免提权优化。已满足状态由命令自身返回业务错误码（例如重复 `install` → `FMT-600`），
  而不是靠跳过提权绕过。
- **`status` 是查询，不提权**：它不走提权副本、不弹 UAC，也不需要管理员权限。这是它与其他
  四条命令的唯一区别；它同样不经命名管道，直连 SCM。

提权流程（提权副本不是独立入口分支，而是 CLI 形态收到内部参数 `--elevated` 后的短命分支）：

命令行形状（**已定稿**）：

```text
结果文件：<数据根>\temp\fmt-elev-<父进程 pid>.json
          固定名、跟着 exe 走，用户一眼能找到、随时可清（第 3.2 节）
          数据根不可写时退回 %TEMP%\fmt-elev-<父进程 pid>.json，并记一行 WARN
命令行  ：fmt.exe --elevated <operation> --result "<结果文件的绝对路径>"
operation ∈ install | uninstall | start | stop | reinstall
            （五种，全部是「动作」命令；`service status` 是查询、不提权，因此**不在**这个清单里）
```

`reinstall` = 先卸载（服务未安装时忽略该错误）再安装并启动，用于 4.8 节「服务宿主 exe 已丢失」
那一行，**一次 UAC 做完**。

```text
CLI 形态（未提权）
  │ 1. 判断是否已提权：OpenProcessToken + GetTokenInformation(TokenElevation)
  │ 2. 未提权 → 打印「需要管理员权限」（FMT-603 AdminRequired）→ 打印「正在提权...」
  │ 3. 先确保 <数据根>\temp 存在（建不出来 → 退回 %TEMP% + 一行 WARN），
  │    再删除可能残留的结果文件 <数据根>\temp\fmt-elev-<父进程 pid>.json
  │ 4. ShellExecuteExW(lpVerb=L"runas",
  │                    lpFile=<具名 self 变量>.c_str(),   ← 不能直接用临时对象，见下
  │                    fMask=SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI,
  │                    nShow=SW_HIDE)
  ▼
提权短命副本（--elevated，无窗口，不进命令循环）
  │ 5. 执行一次 service operation（五种之一）
  │ 6. 先确保 temp/ 存在，再把结果 {ok, code, message, exit}
  │    写进结果文件 <数据根>\temp\fmt-elev-<父进程 pid>.json
  ▼
CLI 形态
  │ 7. WaitForSingleObject(hProcess, 60000) + GetExitCodeProcess
  │ 8. 读结果文件（读完即删除），打印第 6 步写下的结果，并按退出码退出
```

- `runas` 启动的控制台程序**会另开一个控制台窗口**，所以提权副本必须 `SW_HIDE` 无窗口，结果写
  结果文件、由父进程读回打印——这是「只留一个窗口」的前提。
- **为什么回传不用命名管道**：提权副本是**高完整性进程**，它创建的命名管道带高完整性标签，
  中完整性的父进程受 MIC「禁止向上写」限制，**连接和读取都会被拒**（与技术文档 13.9.2 的
  「坑 2」同一机制）。结果文件写在数据根下的 `temp/` 里，父子是同一个用户、只是令牌不同，
  两边都能正常读写，不需要放宽任何安全描述符；只有数据根不可写时才退回 `%TEMP%`。
- 提权副本在写入结果文件前会**自己确保 `temp/` 存在**（它有权限，所以调用方数据根只读时仍能建出来）；
  **Service 启动时**会清理 `temp/` 下遗留的 `fmt-*` 文件（用户手放的其它文件不动），删除数量记一行 INFO。
- **结果文件是唯一通道**：命名管道方案作废，不再保留任何候选方案。
- `SHELLEXECUTEINFOW::lpFile` 必须指向具名变量：`info.lpFile = path_from_utf8(self).c_str();`
  会让指针指向语句结束即析构的临时 `std::wstring`，实测表现为 Win32 1155
  `ERROR_NO_ASSOCIATION`，看不出是提权的问题。
- 提权相关失败**不新增错误码**，全部复用附录 A 的已有编号：

| 情况 | 错误码 | 退出码 |
|---|---|---|
| 用户在 UAC 点「否」（`ShellExecuteExW` 失败且 `GetLastError() == ERROR_CANCELLED`，1223） | `FMT-004` | 5 |
| 非管理员账户 / 策略禁止提权（`ShellExecuteExW` 失败且 `ERROR_ACCESS_DENIED`，5） | `FMT-603` | 5 |
| `ShellExecuteExW` 其它失败 | `FMT-602` | 8 |
| 提权副本等待超时（60 秒）、SCM 操作失败 | `FMT-602` | 8 |
| 结果文件不存在（提权副本崩了） | `FMT-602` | 8 |
| 未提权提示（打印「需要管理员权限」） | `FMT-603` | 5 |

`FMT-603` 只在**最终无法提权、命令以失败告终**时作为退出码；正常提权流程里它只是提示行，
最终退出码以提权副本回传的结果为准（样例中就是 `0`）。

CLI 侧输出样例：

```text
fmt> service stop
需要管理员权限
正在提权...
执行成功...
错误码：0

fmt>
```

（提示符 `fmt> ` 之前先输出一个空行：横幅之后一次、每条命令之后一次；空命令不重复空行，见第 7.5 节。）

### 4.6 服务注册参数与恢复策略（已冻结）

| 项 | 值 |
|---|---|
| 服务名 | `FMT` |
| 显示名 | `FMT File Management Service` |
| 启动类型 | `SERVICE_AUTO_START`（开机自启） |
| 运行账户 | `LocalSystem` |
| 接受的控件 | `SERVICE_ACCEPT_STOP` + `SERVICE_ACCEPT_SHUTDOWN`（**不含** `SERVICE_ACCEPT_PAUSE_CONTINUE`） |
| 宿主路径 | 首次安装时注册的那个 `fmt.exe` 绝对路径（第 4.8 节） |
| Recovery | `ChangeServiceConfig2W(SERVICE_CONFIG_FAILURE_ACTIONS)`：第一次失败 5 秒、第二次 10 秒、后续 30 秒重启；失败计数 **1 天**重置 |
| watchdog | **不自建**，交给 SCM 的 Recovery |

`ServiceMain` 关键序列：

```text
RegisterServiceCtrlHandlerExW
  → SetServiceStatus(SERVICE_START_PENDING[, dwCheckPoint])
  → 读 %ProgramData%\FMT\service.json，确定数据根
  → 初始化存储与业务服务（幂等建目录 + 默认 JSON，含 temp/；与服务/CLI 共用的 ensure_root）
  → 清理 <数据根>/temp 下以 fmt- 开头的遗留文件（用户手放的其它文件不动），删除数量记一行 INFO
  → 起 HTTP 线程
  → SetServiceStatus(SERVICE_RUNNING, ACCEPT_STOP | ACCEPT_SHUTDOWN)
  → 等停止事件
  → 停 HTTP → 等在途操作 → SetServiceStatus(SERVICE_STOPPED)
```

**初始化失败时的上报**（第 4.8 节引导流程第 3 步依赖它）：任一步失败都**不把异常抛给 SCM**，
而是走到 `SetServiceStatus`，把 FMT 编号写进 `dwServiceSpecificExitCode`、置
`dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR`、`dwCurrentState = SERVICE_STOPPED`，
让 CLI 能读回「到底哪儿错了」：

```text
失败（如 FMT-008 ConfigError）
  → SERVICE_STATUS {
        dwServiceType             = SERVICE_WIN32_OWN_PROCESS,
        dwCurrentState            = SERVICE_STOPPED,
        dwWin32ExitCode           = ERROR_SERVICE_SPECIFIC_ERROR,
        dwServiceSpecificExitCode = 8,        ← FMT-008 的数字部分
        dwCheckPoint = 0, dwWaitHint = 0 }
  → 记一行 ERROR 日志（含完整 FMT-NNN 与消息）
```

CLI 侧用 `QueryServiceStatusEx(..., SC_STATUS_PROCESS_INFO, ...)` 读回 `dwServiceSpecificExitCode`，
再用 `code_from_number` 还原成 `FMT-008`，打印 `服务启动失败：FMT-008 配置错误`。

### 4.6.1 查询接口与「等它落定」（已冻结）

**所有对 SCM 状态与失败编号的读取都走一组统一接口**（实现在 `service` 模块，
详见 `FMT 技术文档.md` 13.4.2）：

```text
service::State { NotInstalled, Stopped, StartPending, StopPending, Running,
                 ContinuePending, PausePending, Paused, Unknown }
service::state_name(State)    -> 中文状态名（唯一出处）
service::StatusInfo { state, wait_hint_ms, win32_exit_code, service_exit_code }
service::query_status()        -> Result<StatusInfo>
       「未安装」是**正常结果**（state = State::NotInstalled），不是错误；
       只有查询本身失败（打不开服务控制管理器等）才返回错误（FMT-602 / 退出码 8）
service::query_state()         只是 query_status() 的薄封装（取 state，失败给 Unknown）
service::installed_binary_path() -> Result<std::string>（服务宿主 binPath，去掉引号）
service::last_start_failure()   -> Result<ErrorCode>
       也基于 query_status()，读 win32_exit_code / service_exit_code，
       把 service_exit_code 还原成 FMT 编号；没有失败信息时返回错误（FMT-602）
```

> 名字别混：`State` 是**运行状态枚举**，`ServiceState` 是 `%ProgramData%\FMT\service.json`
> 的结构体（`current_root` / `host_path` / `installed_at`，由 `load_state()` 读取）。

旧的「`QueryServiceStatus` + `map_state`」写法已被取代；`service status`、`service start`
的落定判定、双击引导读失败编号，三处都只走这一个入口。

**等待落定不写死时长，按 SCM 的 `dwWaitHint` 自适应**（实现是 CLI 内的 `settle_state()`）：

```text
间隔来源   服务处于 State::StartPending / State::StopPending 时，每轮查询读 SCM 给的 dwWaitHint
夹取区间   100 ms – 2000 ms（kMinStepMs / kMaxStepMs）
兜底上限   整体 30 秒（kSettleCapMs）；最后一步不越过上限
提前结束   状态一旦不是等待类（Running / Stopped / Paused / NotInstalled …）就立即结束
返回值     落定后的 State；回来仍是等待类就说明「没起来」
```

理由：`ServiceMain` 启动时要建目录、读 JSON、扫 `data/file.json`、起 HTTP、等管道就绪，
慢机器、大目录、杀毒软件介入时完全可能超过 8 秒——此时服务**并没有失败**，只是还在
`StartPending`。写死 8 秒会把「还在启动」误判成「启动失败」，接着白弹一次 UAC 去做根本
解决不了问题的重装（而且重装之后同样慢，用户会被反复弹 UAC）。**等多久，由 SCM 自己说。**

### 4.7 CLI 单实例窗口（已冻结）

- 互斥体名 `Local\FMT.CLI.v1`；`GetLastError() == ERROR_ALREADY_EXISTS` 表示已有实例。
- 已有实例时**不新建窗口**，转去激活它：`EnumWindows` 找 `ConsoleWindowClass` 的窗口 →
  `SetForegroundWindow`；受前台锁定限制失败时退化为 `FlashWindowEx` 提醒用户。
- **UIPI**：高完整性进程的窗口无法被中完整性进程置前，所以提权副本必须无窗口（第 4.5 节）。
- 推论：**同时只有一个 CLI 窗口，同时只有一个数据根**；换数据根必须先关掉旧窗口。

### 4.8 服务宿主 exe：移动请用复制（已冻结）

Service 形态的宿主 = **首次 `service install` 时注册的那个 exe 的绝对路径**（`CreateServiceW` 的
`lpBinaryPathName`），**不复制到别处**：注册的就是用户手上那份 `fmt.exe`。

CLI 双击时会比较服务的 `binPath` 与自身路径：

| 比较结果 | 行为 |
|---|---|
| 与自身路径**相同** | 正常作为 CLI 客户端运行 |
| **不同**但该文件**存在** | 仍然作为 CLI 客户端运行（连服务并声明自己的数据根） |
| **不同**且该文件**已丢失** | 提示重新安装服务（提权副本的 `reinstall` operation：先卸载、再安装并启动，服务未安装时忽略卸载阶段的 `FMT-601`，**一次 UAC 完成**，见第 4.5 节） |

双击时的完整引导流程：

```text
双击 D:\FMT2\fmt.exe
  1. 单实例互斥体：旧窗口还开着 → 激活旧窗口，不开第二个（换目录前必须先关掉旧窗口）
  2. 数据根体检与补齐：对自己所在数据根执行幂等 ensure_root/check_root
       （六个目录 + 六个默认 JSON，只补缺失；已有 JSON 读一遍确认，损坏只报告不重置，第 3.2 节）
       这一步在打开日志器之前完成，所以 log/ 也在这条「新建目录」清单里
       结果**只进日志**（[Cli] 数据根检查 / 数据根新建目录 / 数据根新建文件 / 数据根完整），
       **控制台一行都不打**；只有异常走 stderr（无法补齐、文件损坏）
  3. 查 SCM 状态（走统一的 `service::query_status()`，见技术文档 13.4.2；「未安装」是正常结果）
       ├─ 未安装        → 提权 install（装 + 启动，一次 UAC）
       ├─ 运行中        → 不动、不弹 UAC
       └─ 已安装未运行  → 提权 start → **等它落定**（第 4.6.1 节：按 SCM 的 `dwWaitHint` 自适应）
             ├─ 起来了       → 完成
             └─ 仍然没起     → 读服务留下的失败编号 dwServiceSpecificExitCode（第 4.6 节）
                   ├─ 数据根/配置类问题 → **不重装**（重装也解决不了），打印原因让用户先处理
                   └─ 其它             → 提权 reinstall 一次（卸载 + 安装，一次 UAC），仍失败则报错
  4. 比较服务 binPath 与自身路径（见上表）；不同且该宿主 exe 已丢失 → 询问是否 reinstall 指向当前目录
  5. 服务在运行 → 连管道发 hello 声明 root=D:\FMT2（第 3.1 节）；
       switched=true 时记一行日志「数据根切换：旧 -> 新」（只进日志）
  6. 打印横幅（`File Manager Tool  v1.0  ( build  <日期> )` + Service Running.../Service Stopped...）
       → 空一行 → fmt> 提示符
```

第 3 步判定「数据根/配置类问题」的错误码集合（命中就**跳过重装**、直接打印原因让用户先处理）：

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

这些都不是「服务注册坏了」，重装服务解决不了；报给用户、由用户先处理数据根或配置。
集合之外的失败（例如 `FMT-602 ServiceOperationFailed`）才走「提权 reinstall 一次」。

**移动请用复制**：复制出一份新的 `fmt.exe` 不影响已有服务；而**剪切或删除**已注册的宿主
`fmt.exe` 会让 SCM 无法按 `binPath` 启动服务（报 1053），必须重新 `service install`。

## 5. 数据模型

### 5.1 JSON 文件格式（已冻结）

分两类，**都带 `version` 字段**：

| 类型 | 格式 | 文件 |
|---|---|---|
| 单例数据 | `{ "version": 1, ... }` | `config.json`、`server.json` |
| 集合数据 | `{ "version": 1, "<集合名>": [ ... ] }` | `file.json`、`share.json`、`trash.json`、`user.json` |

集合字段名固定：`file.json` → `files`，`share.json` → `shares`，`trash.json` → `trash`，
`user.json` → `users`。

**版本策略（已冻结）：只接受明确支持的版本，未知版本直接拒绝。** 例如程序只支持
`version: 1`，遇到 `version: 2` 时返回版本错误，**不降级、不猜测、不尝试兼容、不自动
修改文件**。以后支持 v2 时由代码明确实现 v2 读取逻辑。

JSON 解析采用「最小但正确」：支持 Object、Array、String、`\uXXXX` 与常规转义、
Number、`true`/`false`/`null`；**不支持**注释、尾随逗号、任何非标准 JSON，也不做
「尽量解析」的容错——格式错误直接报错，不猜、不修、不吞。解析器（`JsonValue`、
`JsonParser`、`JsonWriter`）与业务结构（`FileRecord` 等）分离，字段变化不影响底层。

### 5.2 file.json

字段定义以 `FMT 开发文档.md` 第 14 节为准，实现细节见 `FMT 技术文档.md` 第 7.1 节：

```json
{
  "version": 1,
  "files": [
    {
      "file_id": "fmt-20261005-0",
      "user": "小谷",
      "bucket": "工作",
      "file_name": "test.txt",
      "extension": ".txt",
      "file_type": "text",
      "size": 1024,
      "md5": "d41d8cd98f00b204e9800998ecf8427e",
      "is_trash": false
    }
  ]
}
```

`file.json` **禁止**保存 `max_download_count`、`download_count`、`expire_time`
——这些属于 Share（`FMT 开发文档.md` 第 15 节）。

### 5.3 share.json

字段定义以 `FMT 开发文档.md` 第 16 节为准，实现细节见 `FMT 技术文档.md` 第 7.2 节：

```json
{
  "version": 1,
  "shares": [
    {
      "share_id": "a1b2c3d4e5f6",
      "file_id": "fmt-20261005-0",
      "max_download_count": 20,
      "download_count": 0,
      "expire_time": null,
      "is_valid": true
    }
  ]
}
```

一个 File 可以有多个 Share，各自独立管理有效期与次数。`share_id` 会作为外部访问
凭证，必须唯一且不可预测。默认 `max_download_count = 20`。

### 5.4 trash.json（文件级记录）

字段定义以 `FMT 开发文档.md` 第 17 节为准，实现细节见 `FMT 技术文档.md` 第 7.3 节：

```json
{
  "version": 1,
  "trash": [
    {
      "file_id": "fmt-20261005-0",
      "file_name": "test.txt",
      "original_path": "repository/小谷/工作/2026/10/05/test.txt",
      "trash_path": "trash/小谷/工作/2026/10/05/test.txt",
      "deleted_at": "2026-10-05T20:00:00",
      "type": "file"
    }
  ]
}
```

Bucket 级记录不使用 `file_id`，用 Bucket 信息管理。

**Bucket 级记录（阶段 4 已落地并冻结，形状见 `FMT 技术文档.md` 第 7.3 节）**：

```json
{
  "version": 1,
  "trash": [
    {
      "type": "bucket",
      "user": "user",
      "bucket": "工作",
      "original_path": "repository/user/工作",
      "trash_path": "trash/user/工作",
      "deleted_at": "2026-10-08T01:23:45"
    }
  ]
}
```

| 字段 | 说明 |
|---|---|
| `type` | `file` 或 `bucket`；Bucket 级记录固定 `"bucket"`，**没有 `file_id`** |
| `user` / `bucket` | 归属与名称；Bucket 用名称标识（第 5.5 节「无独立 ID」） |
| `original_path` / `trash_path` | **相对数据根、正斜杠**；`trash_path` 是**实际**落点 |
| `deleted_at` | 本地时间 ISO 8601（无时区） |

删除 Bucket 是**移入回收站**，数据不丢：`repository/<user>/<bucket>/` 整体移到
`trash/<user>/<bucket>/`。回收站里已有同名目录时**不覆盖**，新目录名加时间戳后缀
（`工作_20261008012345`），因此同一名字删两次会得到两条记录、两份数据。

### 5.5 对象生命周期

```text
File:   上传 → 正常 → 删除 → Trash → 恢复 → 正常
                                 └→ 永久删除 → 结束        file_id 全程不变

Bucket: 创建 → 正常 → 删除 → Trash → 恢复 → 正常
                                 └→ 永久删除 → 结束        无独立 ID

Share:  创建 → 有效 → 访问（计数 +1）→ 过期 / 次数耗尽 / 文件永久删除 → 失效
```

## 6. 源码模块

```text
include/fmt/<模块>/     公共头文件，按模块分子目录
src/
├── CMakeLists.txt      汇总各模块
├── main.cpp            wmain：入口分发（Service / CLI / 异常兜底）
├── common/             公共类型与工具：Error、Result、Time、String、Path、Hash、Validation、Logger
├── config/             config.json / server.json 的加载、校验、保存
├── storage/            底层数据访问：JSON Storage、File Storage、Trash Storage
├── core/               数据根（FMT_ROOT）解析与切换、运行目录、程序生命周期
├── ipc/                进程间通道：命名管道服务端/客户端、帧编解码、管道 DACL 与 MIC、超时
├── cli/                命令行解析、控制台输出、交互循环、CLI 单实例窗口、提权引导
├── bucket/             Bucket 逻辑
├── file/               文件逻辑
├── share/              Share 逻辑
├── trash/              Trash 逻辑
├── service/            Windows Service：SCM 生命周期、服务模式主循环、数据根切换
└── server/             HTTP Server（cpp-httplib，浏览器侧）

third_party/            vendored 单头文件库：nlohmann/json、cpp-httplib
resources/              图标、resources/fmt.manifest（requestedExecutionLevel = asInvoker）
```

> `ipc` 与 `cli` 分属通道两端，但**共用同一份帧格式定义**（`include/fmt/ipc/protocol.hpp`），
> 保证「写帧」与「读帧」不会各自漂移。该头文件必须放在 `include/fmt/ipc/` 而不是 `src/` 下的
> 私有头，否则 CLI 侧只能重复实现一遍。
>
> 目录细分以 `FMT 技术文档.md` 第 2 节为准：Share 的存储与规则并入 `file`（共用同一把锁与同一份
> 元数据读取路径），`trash` 落地时才建目录。上表是**目标布局**，当前实际骨架见第 10 节。

依赖方向：`CLI → IPC → Service/Business → Core → Storage → 文件系统`。
`server` 走同一条业务路径（`server → Service/Business`），不绕过 Service 层。
`common` 不依赖任何业务模块；业务模块之间不得直接调用对方的底层实现。
第三方库只在 `storage`（`nlohmann/json`）与 `server`（`cpp-httplib`）等少数模块内可见，
业务模块不直接包含它们的头文件。

```text
CLI (管道客户端)              HTTP (浏览器)
       │                            │
       ▼                            ▼
   IPC \\.\pipe\fmt.control      server
       │                            │
       └──────────┬─────────────────┘
                  ▼
            Service 层            bucket, file, share, trash, config
                  │
                  ▼
            Core 层               数据根解析与切换、生命周期
                  │
                  ▼
            Storage 层            JSON、文件操作、目录初始化
                  │
                  ▼
              文件系统（当前数据根）
```

### 6.1 模块职责边界

| 模块 | 负责 | 不负责 |
|---|---|---|
| `common` | 公共类型与工具 | 任何业务规则 |
| `config` | 配置读写与校验 | 业务数据 |
| `storage` | 读 JSON、写 JSON、建目录、移动/删除/检查文件、幂等 `ensure_root`/`check_root`（补缺失目录与默认 JSON） | 业务规则（如「文件能否删除」由 Service 决定）；决定何时调用幂等检查 |
| `core` | 数据根（`FMT_ROOT`）解析与切换、运行目录、程序生命周期 | 业务对象 |
| `ipc` | 命名管道服务端/客户端、帧编解码、请求/响应信封、管道 DACL 与 MIC、超时 | 业务规则（只把 `op` 交给 Service 层）；决定数据根的内容 |
| `cli` | 参数解析、控制台输出、`help` 总览与分组详情、交互循环、CLI 单实例窗口、提权引导、双击时对自身数据根调用幂等体检与补齐 | 直接操作文件系统业务；写业务 JSON、改业务数据内容 |
| `bucket`/`file`/`share`/`trash` | 各自的业务规则 | 越过 `storage` 直接操作文件 |
| `service` | Windows Service 生命周期（SCM 注册、状态机、停止流程、失败编号上报）、服务模式主循环、数据根切换、`service` 五命令（`install`/`uninstall`/`start`/`stop`/`status`） | 业务逻辑 |
| `server` | HTTP 请求解析、参数校验、调用 Service、返回响应 | 直接改 `file.json`、`repository`、`trash` |

### 6.2 头文件引用

公共头文件一律用带 `fmt/` 前缀的形式引用：

```cpp
#include "fmt/core/path_manager.hpp"
```

新增模块只需三步：建 `src/<模块>/` 与 `include/fmt/<模块>/`、在 `src/CMakeLists.txt`
加 `add_subdirectory`、在本模块 `CMakeLists.txt` 用 `target_sources(fmt_core PRIVATE ...)`
登记源文件。顶层构建脚本不需要改动。细节见 `src/README.md`。

## 7. CLI 命令结构

命令集以 `FMT 开发文档.md` 第 95 节为准，实现细节见 `FMT 技术文档.md` 第 11 节：

```text
fmt.exe
├── --help
├── --version
├── help    [组]       命令总览（不带参数）/ 某一组详情（help service）
├── exit | quit        交互循环内退出
├── bucket  create <name> | list | get <name> | use <name> | delete <name>
├── file    upload <url> | list | get <file_id> | get <filename> | delete <file_id>
├── share   create <file_id> | get <share_id> | list <file_id> | delete <share_id>
├── trash   list | get <id> | restore <id> | delete <id>
└── service install | uninstall | start | stop | status
```

帮助有两个入口，内容一致：`--help` 一次性打印带横幅与退出码表的完整用法（内含命令总览）；
`help` 不带参数只列命令、`help <组>` 打印该组详情（`service` / `bucket` / `file` /
`share` / `trash` / `help` / `exit`），交互式与一次性都支持。`help <未知组>` → stderr
一行 + 退出码 2（`FMT-001`）。**帮助文案已随阶段 4 同步（提交 `8a5e554`）**：
`bucket` 五条命令已经可用，因此 `(bucket)` 列在「可用命令」组；「服务端尚未实现，现在返回
FMT-602」这句只对 `file` / `share` / `trash` 成立，`help bucket` 写明五条子命令的真实行为
（第一个 Bucket 自动成为当前 / `use` 只改 `current_bucket` / `delete` 移入回收站且当前置空
不自动切换）。帮助内容必须与实际命令一致（见第 4.5 节与本表）；五条命令的输出样例见
`FMT 开发文档.md` 第 95、127.6 节与 `FMT 技术文档.md` 第 11.14 节。

`help` 的命令总览（**只列命令、不加描述**）：

```text
可用命令：
  (service)  install  uninstall  start  stop  status
  (bucket)   create  list  get  use  delete
  (help)     help [命令]
  (exit)     exit  quit

业务命令（服务端尚未实现，现在会返回 FMT-602）：
  (file)     upload  list  get  delete
  (share)    create  get  list  delete
  (trash)    list  get  restore  delete
```

三类命令的走向完全不同，实现时不能混：

| 命令 | 走向 | 说明 |
|---|---|---|
| `bucket` / `file` / `share` / `trash` | **命名管道 → 服务** | CLI 不碰文件系统；服务未运行则 `FMT-601`、退出码 8。**`bucket` 已在阶段 4 落地**（`bucket.*` 五种 op + 两条入口），`file` / `share` / `trash` 仍返回 `FMT-602` |
| `service install` / `uninstall` / `start` / `stop`（提权副本另有 `reinstall`） | **直连 SCM + UAC 提权** | 不经管道、不经 HTTP，见第 4.5 节 |
| `service status` | **直连 SCM，不提权** | 只读查询：不需要管理员权限、不弹 UAC、不走提权副本；未安装时 `FMT-601`、退出码 8 |
| `--help` / `--version` / `help` / `exit` | 本地处理 | 不连服务、不弹 UAC、不写日志 |

`service` 命令的硬性约束（第 2 节第 23 项、第 4.5 节）：

- **五条命令，不带 `--` 前缀**：写 `fmt.exe service install`。旧写法 `fmt.exe --service install`
  已作废，`fmt.exe --service --help` 同样作废，改用 `fmt.exe service --help`。
- **没有 `pause`**（服务不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`），**没有 `delete`**
  （旧文档的 `delete` 已更名为 `uninstall`）。
- **四条动作命令**（`install` / `uninstall` / `start` / `stop`）都走 UAC 提权，提权与否不改变命令名；
  用户取消 UAC → `FMT-004` / 退出码 5。
- **`service status` 是查询，不提权**：不弹 UAC、不要管理员权限，直接读 SCM 状态并打印
  （第 4.5 节）。它出现在帮助文本与 `fmt.exe service --help` 里。

退出码（`FMT 开发文档.md` 第 67 节）：

| 码 | 含义 | 码 | 含义 |
|---|---|---|---|
| 0 | 成功 | 5 | 权限/访问错误 |
| 1 | 通用错误 | 6 | 数据一致性错误 |
| 2 | 参数错误 | 7 | 配置错误 |
| 3 | 对象不存在 | 8 | Service 错误 |
| 4 | 冲突 | | |

脚本应依赖退出码，而不是中文提示文本。错误码（`FMT-NNN`）与退出码是两个层次：错误码
定位具体原因，退出码给脚本分类判断。两者通过统一映射关联，见附录 A。

### 7.1 错误与返回值（已冻结）

```cpp
template <typename T>
using Result = std::variant<T, Error>;          // 有返回值的操作

using Status = std::variant<std::monostate, Error>;  // delete / save / move 等无返回值操作

struct Error {
    ErrorCode code;
    std::string message;
};
```

**成功值与错误值互斥**：不使用 `bool success; T value; Error error;` 这种允许误读的结构。
`ErrorCode` 统一定义在 `common/error` 中，业务代码不直接写字符串错误码，由统一转换层生成。

两条规则：

- **错误码稳定，错误消息可修改。** `FMT-007` 的文案从「文件不存在」改成
  `File does not exist` 仍是同一个错误。CLI、日志、测试、将来的网络 API 都依赖错误码。
- **编号一旦发布不复用、不修改语义。** 新增错误只能追加新编号。

错误清单见附录 A。

### 7.2 日志（已冻结）

日志目录为**当前数据根**下的 `log/`（第 3.2 节），两个文件：

```text
log/
├── fmt.log        全部日志
└── error.log      仅 ERROR 级（快速排查）
```

与它平行的还有**临时文件目录 `temp/`**（第 3.2 节）：两者都不是业务数据，但性质不同——
`log/` 要长期保留、用于回答「谁在什么时候做了什么」，`temp/` 按定义**随时可以清空**。

每行格式：

```text
时间 [级别] [模块] 消息
2026-10-05 23:34:21 [INFO] [File] 文件上传开始: example.txt
2026-10-05 23:36:12 [ERROR] [File] 文件删除失败: example.txt
```

| 级别 | 写入日志文件 | 输出到控制台 |
|---|---|---|
| INFO | ✅ | 视 CLI 场景 |
| WARN | ✅ | ✅ |
| ERROR | ✅ | ✅ |

V1 **不设 DEBUG 级别**，也不实现异步日志、日志线程、日志队列、压缩或轮转。

**Service 与 CLI 都写日志文件，两个进程追加同一个文件**：两个进程都以「追加」方式打开
同一份 `<数据根>/log/fmt.log`（`error.log` 仅 ERROR 级，同样追加），每行一次写入；
MSVC 文件流是共享模式，因此不存在两个进程争抢同一日志文件的问题。

旧口径是「只有 Service 写日志文件，CLI 只输出控制台」，已被推翻：用户在 CLI 里敲
`service stop`，服务随即被停掉，旧口径下这次操作在日志里**一个字都没有**——日志跟不上
用户做过什么。日志要能回答「谁在什么时候对服务做了什么、结果如何」，因此两个进程都写。

CLI 侧的三条边界（详见 `FMT 技术文档.md` 第 11.12 节）：

```text
1. CLI 双击启动时，先对自己所在数据根执行一次与服务共用的幂等体检与补齐
   （六个目录 + 六个默认 JSON，只补缺失、已存在不动、绝不重置损坏 JSON，见第 3.2 节）；
   这一步在打开日志器之前完成，因此 log/ 也在同一条「新建目录」清单里；
   它的结果攒成若干行、等日志器开好后写进 log/fmt.log（模块 Cli），**控制台一行都不打**，
   只有异常（无法补齐、文件损坏）走 stderr。见第 3.2、7.5 节。
   除此之外 CLI 只允许创建 <数据根>/log/ 与执行提权命令前要用的 <数据根>/temp/ 这两个目录
   （都不属于业务数据：日志不是业务数据，temp/ 按定义随时可以清空）；
   CLI 仍然不改任何业务数据——不写 data/*.json 的内容、不删文件、不改名。
2. `--help` / `--version` / `help` / `exit` 不写日志、不创建任何目录。
3. 两个进程的数据根可能不同（服务可能被别人启动在另一个目录）：各写各自数据根下的
   log/fmt.log；这种情况下 CLI 会额外写一行 WARN，指明服务当前数据根与服务侧日志的位置。
4. 控制台只留横幅、提示符、命令结果与异常；服务的当前状态、数据根体检结果都只进日志
   （「控制台负责用户交互与重要异常，日志负责完整运行记录」）。
```

CLI 侧的模块短名为 `Cli`（CLI 本体）与 `Elevated`（提权副本）。

原则：**控制台负责用户交互与重要异常，日志文件负责完整运行记录。**

### 7.3 路径管理（已冻结）

第二阶段引入 `PathManager`，持有 `FMT_ROOT`，提供 `repository()`、`trash()`、`config()`、
`data()`、`log()`、`temp()` 以及 `file_data()`、`share_data()`、`trash_data()` 等具体路径。

**不做全局单例**，而是通过上下文对象传递：

```text
AppContext
 ├── PathManager
 ├── Config
 ├── Logger
 └── ...
```

模块从 `AppContext` 取所需依赖。这样测试可以用 `test_root` 构造独立上下文，不会被
全局单例绑死。

数据根由 CLI 在 hello 帧里声明（第 3.1 节），因此 `PathManager` 必须支持**切换数据根**：
切换只替换持有的根路径，旧根的数据原样留在磁盘上，新根重新做一次幂等初始化（第 3.2 节）。

### 7.4 日志轮转

单文件大小上限与轮转规则**尚未确定**（见第 12 节）。

### 7.5 CLI 界面与输出（已冻结）

启动横幅要告诉用户「服务在不在」：

```text
File Manager Tool  v1.0  ( build  2026.10.08 )
Service Running...

fmt>
```

```text
服务已在运行  → 第二行 Service Running...
服务未运行    → 第二行 Service Stopped...
横幅名称版本  → File Manager Tool  v1.0  ( build  <配置时的日期> )，与 --version 同一串
提示符        → fmt> （fmt 后紧跟 > 和一个空格）
提示符前的空行 → 打印 fmt> 之前先输出一个空行：横幅之后一次、每条命令执行完之后一次，
                 这样输出不会和提示符挤在一起；用户只敲回车（空命令）时不再重复空行
输入          → 空行忽略；exit / quit 退出；help 看命令；命令失败后继续循环，不退出
输出去向      → 正常结果 → stdout；错误 → stderr（不与结果混流）
```

**控制台只留交互，体检与状态的常规结果全部进日志。** 双击时控制台就这几行：

```text
需要管理员权限        ← 仅当需要装/启服务时才有这两行
正在提权...
执行成功...
错误码：0

File Manager Tool  v1.0  ( build  2026.10.08 )
Service Running...

fmt> 
```

数据根体检的「建了什么 / 完不完整」、以及服务的当前状态，**都不打印**，只写进
`log/fmt.log`（模块 `Cli` / `Service`，第 7.2 节）：

```text
[Cli] 数据根检查：D:/FMT2
[Cli] 数据根新建目录：repository, trash, config, data, log, temp
[Cli] 数据根新建文件：D:/FMT2/config/config.json, ...
[Cli] 数据根完整                      ← 第二次双击
[Service] 当前状态：运行中
```

**只有异常才走 stderr**：`数据根无法补齐：FMT-013 …`、
`数据根文件损坏（未自动修复）：data/file.json`。

**换根这种例行状态变化也只进日志、不刷控制台**（要看就问 `service status`，它会打印
「服务数据根」）：

```text
[Service] 数据根切换：D:/FMT -> D:/FMT2        ← 仅当 hello 回执 switched=true
[Service] 服务数据根已经是：D:/FMT2            ← 未切换时
```

这样归位正是本工程的老原则：**控制台负责用户交互与重要异常，日志文件负责完整运行记录**
（第 7.2 节）。双击时用户只想知道「能不能开始敲命令」，不该被一串「建了什么」淹掉。

`service status` 的输出也走 stdout（它是查询命令，成功与「未安装」两种结果都是它的正常输出）：

```text
fmt> service status
服务状态：运行中
服务宿主：D:/FMT2/fmt.exe
服务数据根：D:/FMT2
错误码：0

fmt>
```

输出格式固定（`FMT-NNN` 与退出码分属两层，见第 7 节退出码表与附录 A）：

```text
成功：                          失败：
执行成功...                     执行失败：FMT-601 服务未安装
错误码：0                       错误码：8
```

- 错误行的 `FMT-NNN` 之后跟**错误消息**；同一错误码的文案可以改（第 7.1 节）。
- **控制台编码已冻结为 UTF-8**：`wmain` 最开始调用 `SetConsoleOutputCP(CP_UTF8)`，同时置
  `SetConsoleCP(CP_UTF8)`，保证中文不乱码。旧的「控制台编码待定」描述作废。
- **V1 不做结构化输出（`--json` 等），也不预留参数名**（第 14 节）：机器可读通道是
  **命令退出码**（`0` 成功；未安装 `FMT-601` → `8`；查询失败 → `8`），人类可读通道就是上面
  那几行固定顺序的文本。脚本判断「服务在不在」读退出码即可；需要 `wait_hint_ms` 这类
  结构化字段时再加。

## 8. 并发与事务

V1 即使主要单机使用也必须考虑并发，重点保护：`file_id` 分配、JSON 写入、File metadata、
Share `download_count`、`current_bucket`、文件移动。

**重构后并发模型变简单了**：Service 是唯一写 `data/*.json` 的进程，CLI 只发命令、不改业务数据内容
（它只在自己双击时对自身数据根做一次幂等补齐，见第 3.2 节），所以不存在「CLI 与服务同时改同一个
JSON」的跨进程竞争。但服务**进程内部**依然有并发，两条入口都会进 Service 层，所以下面的锁策略
一条都不能省。

**阶段 4 实现的准确形态**（旧文写的「命名管道连接线程 + 每连接一个线程」不成立——
实现里没有为连接起线程）：

```text
① 命名管道：连接级严格串行
   服务端只有一条 accept 循环（ServerRuntime::run()）：accept 一次只建立一条连接，
   然后在这条连接上「读一个请求 → 处理 → 写一个响应」，客户端断开才回到 accept。
   所以同一时刻只有一条 CLI 连接（配套「只留一个 CLI 窗口」，第 4.7 节）；
   第二个客户端拿到 ERROR_PIPE_BUSY，客户端等 3 秒重试，最终归为 FMT-601。

② HTTP：线程池并发
   cpp-httplib 默认 max(8, hardware_concurrency - 1) 个线程，请求并发进入；
   /api/ping、/api/status 不碰业务锁，是真并发。

③ 业务命令：共用运行体的同一把互斥锁
   管道请求与 HTTP 业务处理器都在业务调用之前 lock 同一把 mutex，
   所以两条入口的 bucket 命令**互斥串行**——同一时刻只有服务在写数据根。
   这是**互斥（mutex），不是队列（queue）**：不保证先来先服务、没有优先级、
   没有排队长度上限、没有排队超时。

④ 代价（阶段 5 必须处理）：一个慢业务命令会卡住两条入口的所有业务命令。
   上传/下载可能持续几十秒到几分钟，阶段 5 必须二选一——**收细锁粒度**
   （按 JSON 文件 / 按 file_id 分锁）或**把长任务移出锁**（登记任务 + 后台线程 + 轮询状态）。
   在选完之前，「上传期间其他命令一起等」是既定限制，不是 bug。
```

- 用 Mutex/Lock 保护同一 JSON 数据的「读取—修改—写入」全过程，避免互相覆盖。
- 保护同一文件、同一 `file_id`、同一 Share 的操作，避免「删除+下载」「恢复+永久删除」
  「两个上传生成同一 ID」「两个下载突破 Share 次数」。
- 正式文件必须经「临时文件 → 完整完成 → 安全移动」才成为正式文件，避免下载读到半成品。
- 优先同文件系统内移动；跨盘时用「复制 → 校验 → 删除原文件」并做一致性保护。
- 数据一致性检查覆盖四种异常：metadata 有而文件无、文件有而 metadata 无、size 不符、
  md5 不符。发现异常只记录并停止危险操作，**不自动删除或重建**。
- **数据根切换**（hello 帧触发并回填 `switched` / `previous_root`，第 3.1 节）必须与在途业务操作
  互斥，否则会出现一半命令落在旧根、一半落在新根的撕裂状态。切换只改根路径，不动旧根数据。

## 9. 编译与构建

环境：Windows x64、MSVC（Visual Studio Build Tools）、CMake ≥ 3.20、Ninja、C++17。

构建目录只有 **`cmake-build-debug`** 一个，与 CLion 默认配置一致。

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

产物：`cmake-build-debug/bin/fmt.exe`（**唯一产物**，三种形态是同一个 target 的运行期分支，
不拆成 `fmt_cli` / `fmt_svc` 两个 exe）。

Ninja 需要 MSVC 环境变量，请在 *Visual Studio 开发者命令行* 中执行。

`fmt.exe` 的第三方依赖只有 vendor 进 `third_party/` 的 `nlohmann/json` 与 `cpp-httplib`
（**头文件入库，不用包管理器、不用 FetchContent**），CRT 用 `/MT` 静态链接
（`CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded`），因此可完全离线构建、不产生额外 DLL。
单元测试用**仓库内自带的极简运行器**（`tests/fmt_test.cpp`），不引入 Catch2、不需要联网，
需要跳过测试时：

```powershell
cmake -S . -B cmake-build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF
cmake --build cmake-build-debug --parallel
```

发布 Release 时用同一目录，但先删除它——CMake 缓存中的构建类型不会自动切换：

```powershell
Remove-Item cmake-build-debug -Recurse -Force
cmake --preset release
cmake --build --preset release
```

CI 见 `.github/workflows/ci.yml`，**只在 `windows-latest` 上**构建并测试：Windows Service
（`advapi32`）、`ShellExecuteEx` 提权与命名管道安全描述符都无法在 Linux 上构建。
分支：`main` 为稳定分支只接受 PR，`dev` 为集成分支，本次重构在 `arch-restart` 上进行。

## 10. 当前实现状态

**本分支（`arch-restart`）回到架构初始骨架**：只保留一个可编译、可运行的最小 `fmt.exe`，
阶段 2 / 阶段 3 按本文档的新架构**重新开始**。旧分支（`dev`）上按「CLI 直接操作本地目录 +
`--service` 参数 + CLI 走 HTTP」写出的实现**不迁移、不复用**。

**阶段状态（截至 commit 32249ea「feat(bucket): create, list, get, use and delete over both
entries」）**：

| 阶段 | 状态 |
|---|---|
| 1 骨架（CMake / Ninja / MSVC / `fmt.exe` / `--help`） | ✅ 完成并验收 |
| 2 基础设施（`common` / `config` / `storage` / `core` 初始化） | ✅ 完成 |
| 3 服务与通道（`service` / `ipc` / `cli`） | ✅ 完成 |
| 4 Bucket（create / list / get / use / delete + `current_bucket` + 名称校验 + 两条入口） | ✅ 完成（明细见 `FMT 技术文档.md` 第 18.15 节） |
| 5 File / Upload / Trash / Share | ⏳ **下一步**，未开始 |
| 6 HTTP Server + Preview | ⏳ 未开始（阶段 4 已落地 `/api/bucket` 五条路由，作为「两条入口一份实现」的验证） |

当前代码里**已经存在**的部分（阶段 2～4）：

| 已实现 | 位置 |
|---|---|
| 程序入口 `wmain`、控制台 UTF-8、`--help` / `--version` / 未知参数退出码 2 | `src/main.cpp` |
| 统一错误码基础设施：`ErrorCode`、`code_string` / `code_from_string`、`exit_code`、`make_error` | `include/fmt/common/error.hpp`、`src/common/error.cpp` |
| `common`：`string`（UTF-8 转换、`url_encode` / `url_decode`）、`time`、`logger`、`validation` | `include/fmt/common/`、`src/common/` |
| `config` / `storage` / `core`：数据根解析、幂等初始化、`PathManager`、JSON 原子读写 | `src/config/`、`src/storage/`、`src/core/` |
| `ipc`：命名管道帧、安全描述符 + MIC、`hello` 的 `switched` / `previous_root` | `src/ipc/` |
| `service`：SCM 五命令、`ServiceMain`、统一查询接口、按 `dwWaitHint` 落定、`Runtime`（锁 + 业务分发 + **启动/换根时校验 `current_bucket`**，失效置空） | `src/service/` |
| `cli`：交互循环、横幅、`help`、单实例、UAC 提权引导、双击体检与补齐 | `src/cli/` |
| `server`：cpp-httplib 监听、`/api/ping` / `/api/status`、`/api/bucket` 五条路由、错误码 → 状态码映射 | `src/server/` |
| `bucket`：`BucketService`（无独立 ID） + 业务分发 `bucket.*` | `src/bucket/`、`src/service/commands.cpp` |
| 单元测试（错误码、校验、配置、路径、存储、管道、服务、CLI、Bucket、集群…） | `tests/` |
| vendored 第三方库 | `third_party/nlohmann/json.hpp`、`third_party/cpp-httplib/httplib.h` |
| 版本号的单一来源（CMake 生成头） | `cmake/version.hpp.in` |
| 构建辅助脚本 | `tools/build.ps1` |

**尚不存在**：`file` / `share` / `trash` 三个业务模块（`file.*` / `share.*` / `trash.*`
返回 `FMT-602`）、上传下载与 MD5、Trash 的恢复与永久删除、HTTP 的下载/预览路由与浏览器页面、
`resources/fmt.manifest`。

> 旧的 `FMT_ROOT` 实现（`src/core/path.cpp`、`src/core/app.cpp`、`src/cli/cli.cpp`、
> `include/fmt/core.hpp`、`src/app/`、`tests/core_test.cpp`）已从工作区移除：它们实现的是
> 「`FMT_ROOT` = exe 所在目录 + CLI 自己建四个目录」的旧口径，与第 3 节「数据根由 CLI 声明、
> 初始化走与 Service 共用的同一套幂等规则」冲突，不值得改造。注意新口径下 CLI **确实会建目录**，
> 但方式是调用共用的 `ensure_root`/`check_root`（只补缺失），而不是旧实现里那份独立的、
> 只认四个目录的 CLI 私有逻辑。
>
> **路径提醒**：`src/core/app.cpp`、`src/cli/cli.cpp`、`include/fmt/core/path.hpp` 这些文件名
> **后来按新架构重新出现了**（阶段 2～4 的实现），与上面被移除的那批旧实现**不是同一份代码**；
> 上表列的是当前存在的文件。旧实现里「CLI 独立建四个目录」的私有逻辑没有回来——
> 现在的 CLI 只调共用的 `ensure_root`/`check_root`。

上一分支阶段 1 的验收结果（**历史记录**，保留备查，不代表当前骨架的能力；其中 `FMT_ROOT` 与
「四个运行目录」属于旧口径，新口径见第 3 节）：

| 项目 | 结果 |
|---|---|
| Debug / Release 构建 | 通过，无警告（MSVC 14.51） |
| `--help` / `--version` | 退出码 0，输出到 stdout |
| 未知参数 | `Unknown option: --abc` 到 stderr，退出码 2 |
| `FMT_ROOT` | 从 `C:\`、`D:\`、`%TEMP%` 启动均得到同一路径 |
| 四个运行目录 | 自动创建 |
| 已有数据 | 不破坏（`data/test.txt` 内容与 SHA256 前后一致） |
| Unicode / 空格路径 | 通过（`D:\FMT测试目录\FMT`、`D:\FMT Space Test\FMT`） |
| 无权限目录 | 退出码 1，输出失败目录与原因，不崩溃、不静默 |
| 单元测试 | 16/16 通过 |

## 11. 开发顺序

阶段划分以 `FMT 开发文档.md` 第 96 节为准，实现细节见 `FMT 技术文档.md` 第 18 节；重构后的
阶段顺序见 `FMT 重构设计.md` 第 10 节。**Windows Service 从原来的阶段 9 提前到阶段 2 / 3**，
业务功能整体后移。

**每完成一个模块就走「开发 → 单元测试 → 集成测试 → 异常测试」，不允许所有模块做完
才开始测试。**

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 项目骨架：CMake、Ninja、MSVC、`fmt.exe`、`--help` | ✅ 完成 |
| 2 | 基础设施：`common`（Error / Result / Time / String / Path / Logger）、`config`、`storage`、`core` 初始化（数据根解析、幂等建六个目录 + 默认 JSON；**`ensure_root`/`check_root` 由 Service 与 CLI 共用**，损坏 JSON 只报告不重置） | ✅ 完成 |
| 3 | 服务与通道：`service`（SCM 五命令 `install`/`uninstall`/`start`/`stop`/`status` + 统一查询接口 `query_status()`/`query_state()`/`last_start_failure()`（第 4.6.1 节）+ 按 `dwWaitHint` 自适应的落定等待 + `ServiceMain` + 失败编号上报 `dwServiceSpecificExitCode` + Recovery + `%ProgramData%` 状态文件）、`ipc`（命名管道 + 安全描述符 + 帧）、`cli`（双击幂等体检与补齐、交互循环、横幅、单实例、提权引导、落定判定与 reinstall 兜底） | ✅ 完成 |
| 4 | Bucket：create / list / get / use / delete，`current_bucket` 逻辑；`common/validation` 名称校验；管道与 HTTP 两条入口打通 | ✅ 完成（commit 32249ea；明细见 `FMT 技术文档.md` 第 18.15 节） |
| 5 | File / Upload / Trash / Share：`file.json`、`file_id`、文件名、extension、file_type、size、md5、`is_trash`、list / get / get by name / delete；HTTP(S) 下载、临时文件、大小限制、MD5、文件名冲突；Trash list / get / restore / delete（含 Bucket 部分恢复）；Share create / get / list / delete、20 次限制、过期、并发安全 | ⏳ **下一步**，未开始。开工前必须先定「上传/下载期间怎么不挡住其他命令」（第 8 节 ④） |
| 6 | HTTP Server + Preview：文件查询、下载、Share 访问（浏览器入口）；图片预览 | ⏳ 未开始（`/api/bucket` 五条路由已在阶段 4 落地） |

阶段 2 + 3 完成后，「双击即用的服务 + CLI」闭环成立：全新环境双击 → 一次 UAC → 服务装好且
开机自启 → `service stop` / `service start` 各弹一次 UAC 且输出与样例一致 → 把 exe 复制到
新目录双击，即在新目录建出数据。

与旧顺序的对应关系（便于对照旧文档）：原阶段 3（Bucket）→ 新阶段 4；原阶段 4（File 基础）、
5（Upload）、6（File 操作）、7（Trash）、8（Share）→ 合并进新阶段 5；原阶段 9（Windows
Service）→ **提前**到新阶段 3；原阶段 10（HTTP Server）、11（Preview）→ 新阶段 6。
新阶段 2 基本对应原阶段 2，并新增 `core` 的数据根初始化。

## 12. 后续阶段需要先解决的问题

| 事项 | 说明 |
|---|---|
| `FilenameValidator` | **已完成，从待决清单移出**：阶段 4 以 `common/validation` 落地（`include/fmt/common/validation.hpp` / `src/common/validation.cpp`）——`is_windows_reserved_name`（`CON`/`PRN`/`AUX`/`NUL`/`COM1..`/`LPT1..`，含带扩展名形式）、`kMaxNameBytes = 255`、`validate_bucket_name`（失败一律 `FMT-202`）、`validate_file_name`（`FMT-100`～`FMT-104` 按原因分工）。文档第 19/25/26/118 节的要求全部覆盖，用例见 `tests/validation_test.cpp`；错误码分工表见 `FMT 开发文档.md` 第 25 节、`FMT 技术文档.md` 第 9.3 节 |
| 编译器矩阵 | 仅验证 MSVC；Clang 未验证 |
| 日志轮转策略 | 日志分级与去向已定（第 7.4 节），单文件大小上限与轮转规则未定 |
| 提权副本的结果通道细节 | **已定稿，从待决清单移出**：结果经结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 回传（第 4.5 节），数据根不可写时退回 `%TEMP%` 同名文件并记一行 WARN；命令行 `--elevated <operation> --result "<路径>"`，operation 五种（`install` / `uninstall` / `start` / `stop` / `reinstall`）。命名管道方案作废——提权副本是高完整性进程，它创建的管道会被 MIC「禁止向上写」挡住（与技术文档 13.9.2「坑 2」同一机制） |
| 数据根切换的并发保护 | 目前依赖「只有一个 CLI 窗口」（第 4.7 节）；多窗口场景不在本次范围 |
| **阶段 5 的锁粒度** | **开工前必须定**：阶段 4 是「两条入口的业务命令共用运行体的一把互斥锁」（第 8 节 ③），互斥不是队列、没有排队上限。上传/下载可能持续几十秒到几分钟，持锁期间 `bucket list` 与浏览器请求都会一起等。二选一：**收细锁粒度**（按 JSON 文件 / 按 `file_id` 分锁）或**把长任务移出锁**（登记任务 + 后台线程 + 轮询状态）。在选完之前，「上传期间其他命令一起等」是既定限制，不是 bug（`FMT 技术文档.md` 第 18.16 节、`FMT 开发文档.md` 第 84、101 节） |
| 双击引导的重装判定 | **已定稿，从待决清单移出**：等待时长**不写死**，按 SCM 的 `dwWaitHint` 自适应（夹在 100 ms – 2000 ms，兜底 30 秒，不是等待类就立即结束，第 4.6.1 节）；仍没起来时先读 `dwServiceSpecificExitCode` 还原 FMT 编号（第 4.6 节）：命中数据根/配置类错误码集合（第 4.8 节表）就**不重装**、只打印原因，其余才提权 `reinstall` 一次 |
| `service status` 是否要机器可读输出 | **已定稿，从待决清单移出**：**V1 不做 `--json`、也不预留参数名**。机器可读通道是**命令退出码**（`0` 成功 / 未安装 `FMT-601` → `8`），人类可读通道是固定顺序的那几行文本（第 4.5、7.5 节）；需要结构化输出时再加 |

> **已冻结、不再是待解决问题**：Service 与 CLI 的通信方式（CLI 走命名管道
> `\\.\pipe\fmt.control`，第 4.3 节；浏览器走 HTTP `127.0.0.1:4122`，第 4.4 节）、
> 提权副本的结果通道（**`<数据根>\temp\` 下的结果文件**，第 4.5 节；命名管道方案作废）、
> 控制台编码（UTF-8：`SetConsoleOutputCP(CP_UTF8)` + `SetConsoleCP(CP_UTF8)`，第 7.5 节）、
> `service` 五命令与提权语义（第 4.5 节；`status` 是查询、不提权）、服务注册参数与恢复策略
> （第 4.6 节）、服务启动失败的编号上报（`dwServiceSpecificExitCode`，第 4.6 节）、
> 统一查询接口与按 `dwWaitHint` 自适应的落定等待（第 4.6.1 节；`--json` 不在 V1 范围）、
> CLI 单实例与窗口激活（第 4.7 节）、服务宿主规则（第 4.8 节）、双击引导六步流程（第 4.8 节）、
> 数据根由 CLI 声明与 `hello` 的 `switched`/`previous_root` 回执（第 3.1 节）、
> 初始化规则由 Service 与 CLI 共用（第 3.2 节）、程序横幅与构建日期（第 7、7.5 节）、
> `help` 命令（第 7.5 节）、控制台只留交互与异常（第 7.2、7.5 节）。

## 13. 暂定内容

以下属于 V1 暂定，编码前可继续确认，不影响总体架构：`file.json`、`share.json`、
`trash.json` 单条记录的最终字段；HTTP API 路由；Share ID 生成方式；Bucket Trash
元数据结构；最大上传大小默认值；日志轮转规则；HTTP 鉴权（目前只监听 `127.0.0.1`，
面向局域网访问的安全控制后续再做）。

**阶段 4 之后从暂定转为已冻结的两条**：

- **Bucket 级 Trash 元数据结构**：`{type:"bucket", user, bucket, original_path, trash_path,
  deleted_at}`，无 `file_id`，两个路径相对数据根、正斜杠（第 5.4 节）。
- **`/api/bucket` 五条路由与参数形状**：`GET/POST /api/bucket`、
  `GET/POST /api/bucket/<name>[/use]`、`DELETE /api/bucket/<name>`；请求体接受
  `{"name":"工作"}` 或 `{"argv":["工作"]}`；路径参数里的中文由服务端 `url_decode` 解码；
  错误码 → HTTP 状态码的映射表也一并冻结（`FMT 技术文档.md` 第 12.3.2.1、12.5 节）。
  其余 `/api/*` 路由（file / share / trash / config）仍按阶段 5、6 落地时细化。

> 已冻结、不再属于暂定：JSON 文件组织形式（第 5.1 节）、错误码编号（附录 A）、
> 日志分级与去向（第 7.2 节）、`PathManager` 形式（第 7.3 节）、`fmt.exe` 三形态与 manifest
> `asInvoker`（第 4.1 节）、双通道与「业务只有一份实现」（第 4.2 节）、`service` 五命令与
> 提权语义（第 4.5 节）、服务注册参数与恢复策略（第 4.6 节）、CLI 单实例（第 4.7 节）、
> 服务宿主规则（第 4.8 节）、CLI 界面与输出格式（第 7.5 节）、数据根由 CLI 声明（第 3.1 节）、
> `hello` 响应新增 `switched` / `previous_root`（第 3.1 节）、CLI 双击时的幂等体检与补齐
> （第 3.2 节）、`service status` 不提权（第 4.5 节）、统一查询接口与按 `dwWaitHint` 自适应的
> 落定等待（第 4.6.1 节）、程序横幅 `File Manager Tool  v1.0  ( build  <日期> )`
> 与 `help` 命令（第 7、7.5 节）、控制台只留交互与异常（第 7.2、7.5 节）。

## 14. 不在 V1 范围

分块上传、断点续传、上传取消、复杂用户认证、权限系统、文件夹系统、文件逻辑对象、
物理对象引用计数、复杂搜索引擎、数据库、HTTPS、复杂 API 鉴权。

本次重构另外明确不做：`service pause` / `continue`（服务不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`，
也没有对应命令）、多个 CLI 窗口（因此也不支持同时挂多个数据根）、业务命令的 HTTP 客户端实现
（CLI 只走命名管道，HTTP 只留给浏览器）、服务宿主 exe 的自动搬迁（移动请用复制，第 4.8 节）、
**命令的结构化输出（`--json` 等，也不预留参数名）**——机器可读通道是**命令退出码**
（`0` 成功 / 未安装 `FMT-601` → `8`），人类可读通道是固定顺序的那几行文本（第 4.5、7.5 节）。

---

## 附录 A. 错误码清单

**规则：编号一旦发布，不复用、不修改语义。新增错误只能追加新编号。错误码稳定，错误**
**消息可修改。**

前 10 项为已确认的核心错误，其余为 V1 各阶段所需，在对应模块实现前即可使用该编号。

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-001 | `InvalidArgument` | 参数错误 | 2 |
| FMT-002 | `FileNotFound` | 文件不存在 | 3 |
| FMT-003 | `FileAlreadyExists` | 文件已存在 | 4 |
| FMT-004 | `PermissionDenied` | 权限不足 | 5 |
| FMT-005 | `IoError` | 磁盘/IO 错误 | 1 |
| FMT-006 | `JsonParseError` | JSON 格式错误 | 6 |
| FMT-007 | `JsonWriteError` | JSON 写入失败 | 1 |
| FMT-008 | `ConfigError` | 配置错误 | 7 |
| FMT-009 | `StorageError` | 存储操作失败 | 1 |
| FMT-010 | `Md5Error` | MD5 计算失败 | 1 |
| FMT-011 | `JsonUnsupportedVersion` | JSON 数据版本不受支持 | 6 |
| FMT-012 | `PathTooLong` | 路径或文件名超长 | 2 |

**通用与路径（阶段 2）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-013 | `DirectoryCreateFailed` | 目录创建失败 | 1 |
| FMT-014 | `PathEscape` | 路径穿越 | 2 |
| FMT-015 | `ConsistencyError` | 数据一致性异常 | 6 |

**文件名校验（阶段 5 前，`common/validation`）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-100 | `FileNameEmpty` | 文件名为空 | 2 |
| FMT-101 | `FileNameInvalidChar` | 含 Windows 非法字符 | 2 |
| FMT-102 | `FileNameSeparator` | 含路径分隔符 | 2 |
| FMT-103 | `FileNameReserved` | Windows 保留设备名 | 2 |
| FMT-104 | `FileNameTooLong` | 文件名超长 | 2 |
| FMT-105 | `FileNameConflict` | 同用户正常文件重名 | 4 |

**Bucket（阶段 4）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-200 | `BucketNotFound` | Bucket 不存在 | 3 |
| FMT-201 | `BucketAlreadyExists` | Bucket 已存在 | 4 |
| FMT-202 | `BucketNameInvalid` | Bucket 名称非法 | 2 |
| FMT-203 | `BucketInUse` | Bucket 仍被引用，不能删除 | 4 |

> **阶段 4 的实现口径**：`bucket delete` 目前**不会**返回 `FMT-203`——V1 允许删除仍有文件的
> Bucket（数据一并移入回收站，该桶的 `file.json` 记录置 `is_trash`）。编号保留给以后
> 「有 Share 引用等场景」，语义不变。`FMT-200` / `FMT-201` / `FMT-202` 三条已在使用
> （`FMT 开发文档.md` 第 27～30 节）。

**上传与下载（阶段 5、6）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-300 | `UrlInvalid` | URL 非法或协议不被支持 | 2 |
| FMT-301 | `DownloadFailed` | 下载失败 | 1 |
| FMT-302 | `DownloadTimeout` | 下载超时 | 1 |
| FMT-303 | `SizeLimitExceeded` | 超过 `max_upload_size` | 2 |
| FMT-304 | `Md5Duplicate` | MD5 已存在，文件重复 | 4 |
| FMT-305 | `NoCurrentBucket` | 未设置当前 Bucket | 3 |

**Trash（阶段 5）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-400 | `TrashEntryNotFound` | Trash 记录不存在 | 3 |
| FMT-401 | `RestoreConflict` | 恢复目标已存在同名文件 | 4 |
| FMT-402 | `RestoreBucketMissing` | 原 Bucket 已永久删除 | 3 |

**Share（阶段 5）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-500 | `ShareNotFound` | Share 不存在 | 3 |
| FMT-501 | `ShareExpired` | Share 已过期 | 5 |
| FMT-502 | `ShareDownloadLimitReached` | 下载次数耗尽 | 5 |
| FMT-503 | `ShareFileUnavailable` | 关联文件不可用（处于 Trash 或已删除） | 5 |

**Service 与 HTTP（阶段 3、6）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-600 | `ServiceAlreadyInstalled` | Service 已存在 | 8 |
| FMT-601 | `ServiceNotInstalled` | Service 不存在（`service status` 在未安装时也用它） | 8 |
| FMT-602 | `ServiceOperationFailed` | Service 操作失败 | 8 |
| FMT-603 | `AdminRequired` | 需要管理员权限 | 5 |
| FMT-604 | `NoCurrentUser` | 未设置当前用户 | 7 |
| FMT-700 | `HttpRequestInvalid` | HTTP 请求参数错误 | 2 |
| FMT-701 | `PreviewUnsupported` | 该文件类型不支持预览 | 2 |

> **`FMT-604` 的阶段 4 口径**：数据根初始化会把空的 `current_user` 补成占位名 `user`
> （第 3.2 节），所以**正常首启不会**遇到它；它保留给「用户被显式清空」这种情况。
> 与 `FMT-305 NoCurrentBucket` 别混：前者是「没有用户」，后者是「没选当前 Bucket」，
> 后者由文件类命令（阶段 5）返回。

> **提权相关失败不新增编号**，全部复用上表已有语义：用户取消 UAC → `FMT-004`；提权副本等待
> 超时、`ShellExecuteExW` 其它失败、结果文件不存在（提权副本崩了）、SCM 操作失败 → `FMT-602`；
> 未提权提示（打印「需要管理员权限」）→ `FMT-603`（只有最终无法提权时它才是退出码——包括
> `ShellExecuteExW` 失败且 `ERROR_ACCESS_DENIED`（5）的情况，正常提权流程中它只是提示行，
> 见第 4.5 节）；CLI 连不上服务管道 → `FMT-601`；`service status` 发现服务未安装 → `FMT-601`
> （退出码 8，但**不属于提权失败**，`status` 全程不提权）；服务宿主 exe 丢失需重装 → 仍用 `FMT-601`
> （但提权副本的 `reinstall` operation 会**忽略**卸载阶段的服务不存在错误，继续安装并启动，
> 不作为失败）。
> 服务启动失败时上报的 `dwServiceSpecificExitCode` 用的也是**上表已有编号的数字部分**（第 4.6 节），
> 不新增编号；CLI 读回后按同一张表还原 `FMT-NNN` 并打印。
> 因此 `FMT-605` 起的 Service 段编号**仍然空闲**，留待出现真正的新语义时再追加。

**未映射错误码的退出码默认为 1（通用错误）。** 上表退出码一项即代码中 `ErrorCode` →
`ExitCode` 的映射依据。

> 编号分段预留：`FMT-0xx` 通用、`FMT-1xx` 文件名、`FMT-2xx` Bucket、`FMT-3xx` 上传下载、
> `FMT-4xx` Trash、`FMT-5xx` Share、`FMT-6xx`/`FMT-7xx` Service 与 HTTP、`FMT-9xx`
> 保留给后续扩展。
