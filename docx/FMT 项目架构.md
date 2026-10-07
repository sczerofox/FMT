# FMT 项目架构

> FMT：Windows 平台的文件管理系统。**单一 `fmt.exe`、三种形态**：CLI 形态、被 SCM 启动的
> Service 形态、提权短命副本。CLI 与浏览器都只是客户端，直连同一个常驻 Service 进程。
>
> **文档状态：重构版（分支 `arch-restart`，基线 `bdbe33d`）。** 本版按已冻结的重构决策重写了
> 架构描述：`fmt.exe` 多形态与 `asInvoker` manifest、CLI↔服务的命名管道、浏览器↔服务的 HTTP、
> `service` 四命令与提权语义、数据根由 CLI 声明、阶段顺序前移（Windows Service 提到阶段 2/3）。
> 旧版《项目架构》中「`FMT_ROOT` = exe 所在目录」「`fmt.exe --service install`」「CLI 走
> HTTP」「`service` 有 `pause` / `delete`」等描述**已作废**，与本文冲突时以本文为准。
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
| 18 | Service 是**全局单例**，也是唯一写 `data/*.json` 的进程；CLI 只是客户端，不碰 core、不建目录、不写 JSON |
| 19 | CLI 单实例：命名互斥体 `Local\FMT.CLI.v1`，已有窗口则激活、不新建——因此**同时只有一个 CLI 窗口，也就只有一个数据根** |
| 20 | **数据根由 CLI 声明**：CLI 用 hello 帧上报自身 exe 所在目录，服务维护「当前数据根」，切换**不删**旧根数据 |
| 21 | 初始化由**服务**执行、CLI 只读：五个目录 + 默认 JSON，`.tmp` 原子替换，已存在不改、不删、不覆盖 |
| 22 | 双入口直连服务：CLI 走命名管道 `\\.\pipe\fmt.control`，浏览器走 `127.0.0.1:4122`（cpp-httplib），两者进同一个 service 层 |
| 23 | `service` 命令只有 `install` / `uninstall` / `start` / `stop` 四条，**无 `pause`、无 `delete`**（`delete` 更名为 `uninstall`），命令**不带 `--` 前缀** |
| 24 | 每条 `service` 命令都走 UAC 提权，不做「目标状态已满足就免提权」的优化 |
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
  → 连上服务后首帧就是 hello：{"id":1,"op":"hello","root":"D:\\FMT2","pid":1234}
  → 服务比对：
       root != 当前数据根 ?
         ├─ 是 → 切换数据根（旧根数据原样保留），对新根做幂等初始化，
         │        记 INFO「数据根切换: D:\FMT → D:\FMT2」
         └─ 否 → 直接进入命令循环
  → 该连接上的所有业务命令都在该数据根下执行
```

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
└── log/                  运行日志（正式约定，与业务数据分离）
    ├── fmt.log           全部日志，只有 Service 写
    └── error.log         仅 ERROR 级
```

初始化是**幂等**的，且**由服务执行、CLI 只读**：

- 五个目录（`repository/`、`trash/`、`config/`、`data/`、`log/`）不存在则创建，已存在则保持原样，
  **绝不删除、清空或覆盖**已有内容。
- 默认 JSON 只在缺失时写入；关键 JSON 一律先写 `.tmp` 再原子替换，避免异常退出写出半文件。
- CLI **不建目录、不写 JSON**：它只解析命令、声明数据根、走管道（第 4.3 节）。

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
| CLI 形态 | 用户双击或命令行启动 | 用户关窗口为止 | 交互界面、声明数据根、经命名管道发请求；**不碰 core、不建目录、不写 JSON** |
| Service 形态 | SCM 按注册的 `binPath` 启动 | 常驻，开机自启 | 服务入口、初始化数据根、承载业务核心与 HTTP 入口；**唯一写 `data/*.json` 的进程** |
| 提权短命副本 | CLI 用 `ShellExecuteExW(lpVerb=L"runas", ...)` 拉起自己，带内部参数 `--elevated` | 秒级，做完一次操作即退出 | 只执行一次 `service install/uninstall/start/stop`，把结果回传给父进程 |

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
  ┌────────────────┐  管道 \\.\pipe\fmt.elev.<pid>    │                   ▼           │
  │ 提权短命副本    │ ──────────────────────────────→ │              core 层          │
  │ 回传 {ok, code, │ ←────────────────────────────── │                   │           │
  │ message}        │  （或临时文件）                 │              storage 层        │
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

超时：连接 **3 秒**、普通命令 **30 秒**。连不上服务（`ERROR_FILE_NOT_FOUND`）时输出
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
                              （不得删除 repository / trash / data / config / log）
fmt> service start       提权 → 存在则 StartServiceW；不存在 → FMT-601
fmt> service stop        提权 → ControlService(SERVICE_CONTROL_STOP) → 停止接受新请求
                              → 等在途事务 → 停 HTTP → 退出
```

- **只有这四条**：没有 `pause` / `continue`，也没有 `delete`（旧文档的 `delete` 已更名为
  `uninstall`）。命令**不带 `--` 前缀**：写 `fmt.exe service install`，旧写法
  `fmt.exe --service install` 作废。
- 服务**不声明** `SERVICE_ACCEPT_PAUSE_CONTINUE`；`HandlerEx` 只处理 `SERVICE_CONTROL_STOP`、
  `SERVICE_CONTROL_SHUTDOWN`、`SERVICE_CONTROL_INTERROGATE`。
- **每条命令都提权**，即使目标状态已经满足也照常提权，不做免提权优化。已满足状态由命令自身
  返回业务错误码（例如重复 `install` → `FMT-600`），而不是靠跳过提权绕过。

提权流程（提权副本不是独立入口分支，而是 CLI 形态收到内部参数 `--elevated` 后的短命分支）：

```text
CLI 形态（未提权）
  │ 1. 判断是否已提权：OpenProcessToken + GetTokenInformation(TokenElevation)
  │ 2. 未提权 → 打印「需要管理员权限」（FMT-603 AdminRequired）→ 打印「正在提权...」
  │ 3. ShellExecuteExW(lpVerb=L"runas",
  │                    fMask=SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
  │                    nShow=SW_HIDE)
  ▼
提权短命副本（--elevated，无窗口，不进命令循环）
  │ 4. 执行一次 service 子命令
  │ 5. 结果 {ok, code, message} → 命名管道 \\.\pipe\fmt.elev.<pid>（或临时文件）
  ▼
CLI 形态
  │ 6. WaitForSingleObject(hProcess, 60000) + GetExitCodeProcess
  │ 7. 打印第 5 步回传的结果，并按退出码退出
```

- `runas` 启动的控制台程序**会另开一个控制台窗口**，所以提权副本必须 `SW_HIDE` 无窗口，结果经
  管道（或临时文件）回传、由父进程打印——这是「只留一个窗口」的前提。
- 提权相关失败**不新增错误码**，全部复用附录 A 的已有编号：

| 情况 | 错误码 | 退出码 |
|---|---|---|
| 用户在 UAC 点「否」（`ShellExecuteExW` 失败且 `GetLastError() == ERROR_CANCELLED`，1223） | `FMT-004` | 5 |
| 提权副本等待超时（60 秒）、SCM 操作失败 | `FMT-602` | 8 |
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
```

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
  → 初始化存储与业务服务（幂等建目录 + 默认 JSON）
  → 起 HTTP 线程
  → SetServiceStatus(SERVICE_RUNNING, ACCEPT_STOP | ACCEPT_SHUTDOWN)
  → 等停止事件
  → 停 HTTP → 等在途操作 → SetServiceStatus(SERVICE_STOPPED)
```

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
| **不同**且该文件**已丢失** | 提示重新安装服务（`service uninstall` + `service install`，一次 UAC 完成） |

双击时的完整引导流程：

```text
双击 D:\FMT2\fmt.exe
  1. 单实例互斥体：旧窗口还开着 → 激活旧窗口，不开第二个（换目录前必须先关掉旧窗口）
  2. 查 SCM：已运行 → 不重装、不弹 UAC；未运行 → 提权 start；未安装 → 提权 install + start
  3. 比较服务 binPath 与自身路径（见上表）
  4. 连管道声明 root=D:\FMT2 → 服务在 D:\FMT2 下建目录与默认 JSON
  5. 打印横幅与 Service Running... → fmt> 提示符
```

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
| `storage` | 读 JSON、写 JSON、建目录、移动/删除/检查文件 | 业务规则（如「文件能否删除」由 Service 决定） |
| `core` | 数据根（`FMT_ROOT`）解析与切换、运行目录、程序生命周期 | 业务对象 |
| `ipc` | 命名管道服务端/客户端、帧编解码、请求/响应信封、管道 DACL 与 MIC、超时 | 业务规则（只把 `op` 交给 Service 层）；决定数据根的内容 |
| `cli` | 参数解析、控制台输出、交互循环、CLI 单实例窗口、提权引导 | 直接操作文件系统业务；建目录、写 JSON |
| `bucket`/`file`/`share`/`trash` | 各自的业务规则 | 越过 `storage` 直接操作文件 |
| `service` | Windows Service 生命周期（SCM 注册、状态机、停止流程）、服务模式主循环、数据根切换、`service` 四命令 | 业务逻辑 |
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
├── bucket  create <name> | list | get <name> | use <name> | delete <name>
├── file    upload <url> | list | get <file_id> | get <filename> | delete <file_id>
├── share   create <file_id> | get <share_id> | list <file_id> | delete <share_id>
├── trash   list | get <id> | restore <id> | delete <id>
└── service install | uninstall | start | stop
```

各子命令支持 `--help`（如 `fmt.exe file --help`），帮助内容必须与实际命令一致。

三类命令的走向完全不同，实现时不能混：

| 命令 | 走向 | 说明 |
|---|---|---|
| `bucket` / `file` / `share` / `trash` | **命名管道 → 服务** | CLI 不碰文件系统；服务未运行则 `FMT-601`、退出码 8 |
| `service install` / `uninstall` / `start` / `stop` | **直连 SCM + UAC 提权** | 不经管道、不经 HTTP，见第 4.5 节 |
| `--help` / `--version` | 本地处理 | 不连服务、不弹 UAC |

`service` 命令的硬性约束（第 2 节第 23 项、第 4.5 节）：

- **四条命令，不带 `--` 前缀**：写 `fmt.exe service install`。旧写法 `fmt.exe --service install`
  已作废，`fmt.exe --service --help` 同样作废，改用 `fmt.exe service --help`。
- **没有 `pause`**（服务不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`），**没有 `delete`**
  （旧文档的 `delete` 已更名为 `uninstall`）。
- 每条命令都走 UAC 提权，提权与否不改变命令名；用户取消 UAC → `FMT-004` / 退出码 5。

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

每行格式：

```text
时间 [级别] [模块] 消息
2026-10-05 23:34:21 [INFO] [FileService] 文件上传开始: example.txt
2026-10-05 23:36:12 [ERROR] [FileService] 文件删除失败: example.txt
```

| 级别 | 写入日志文件 | 输出到控制台 |
|---|---|---|
| INFO | ✅ | 视 CLI 场景 |
| WARN | ✅ | ✅ |
| ERROR | ✅ | ✅ |

V1 **不设 DEBUG 级别**，也不实现异步日志、日志线程、日志队列、压缩或轮转。

**只有 Service 写日志文件**；CLI 不写 `fmt.log`，只输出到控制台。这样不存在两个进程
争抢同一日志文件的问题。

原则：**控制台负责用户交互与重要异常，日志文件负责完整运行记录。**

### 7.3 路径管理（已冻结）

第二阶段引入 `PathManager`，持有 `FMT_ROOT`，提供 `repository()`、`trash()`、`config()`、
`data()` 以及 `file_data()`、`share_data()`、`trash_data()` 等具体路径。

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
FMT v1.0.0
Service Running...
fmt>
```

```text
服务已在运行  → 第二行 Service Running...
服务未运行    → 第二行 Service Stopped...
提示符        → fmt> （fmt 后紧跟 > 和一个空格）
输入          → 空行忽略；exit / quit 退出；命令失败后继续循环，不退出
输出去向      → 正常结果 → stdout；错误 → stderr（不与结果混流）
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

## 8. 并发与事务

V1 即使主要单机使用也必须考虑并发，重点保护：`file_id` 分配、JSON 写入、File metadata、
Share `download_count`、`current_bucket`、文件移动。

**重构后并发模型变简单了**：Service 是唯一写 `data/*.json` 的进程，CLI 只读、只发命令，所以
不存在「CLI 与服务同时改同一个 JSON」的跨进程竞争。但服务**进程内部**依然并发——命名管道连接
线程与 cpp-httplib 的每连接一个线程会同时进 Service 层，所以下面的锁策略一条都不能省。

- 用 Mutex/Lock 保护同一 JSON 数据的「读取—修改—写入」全过程，避免互相覆盖。
- 保护同一文件、同一 `file_id`、同一 Share 的操作，避免「删除+下载」「恢复+永久删除」
  「两个上传生成同一 ID」「两个下载突破 Share 次数」。
- 正式文件必须经「临时文件 → 完整完成 → 安全移动」才成为正式文件，避免下载读到半成品。
- 优先同文件系统内移动；跨盘时用「复制 → 校验 → 删除原文件」并做一致性保护。
- 数据一致性检查覆盖四种异常：metadata 有而文件无、文件有而 metadata 无、size 不符、
  md5 不符。发现异常只记录并停止危险操作，**不自动删除或重建**。
- **数据根切换**（hello 帧触发，第 3.1 节）必须与在途业务操作互斥，否则会出现一半命令落在旧根、
  一半落在新根的撕裂状态。切换只改根路径，不动旧根数据。

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

| 阶段 | 状态 |
|---|---|
| 1 骨架（CMake / Ninja / MSVC / `fmt.exe` / `--help`） | ✅ 完成并验收 |
| 2 基础设施（`common` / `config` / `storage` / `core` 初始化） | 进行中，**重新开始** |
| 3 服务与通道（`service` / `ipc` / `cli`） | 待开始，**重新开始** |
| 4 及以后 | 待开始 |

当前代码里**已经存在**的部分：

| 已实现 | 位置 |
|---|---|
| 程序入口 `wmain`、控制台 UTF-8、`--help` / `--version` / 未知参数退出码 2 | `src/main.cpp` |
| 统一错误码基础设施：`ErrorCode`、`code_string` / `code_from_string`、`exit_code`、`make_error` | `include/fmt/common/error.hpp`、`src/common/error.cpp` |
| 测试运行器与错误码用例 7 项 | `tests/fmt_test.cpp`、`tests/error_test.cpp` |
| vendored 第三方库 | `third_party/nlohmann/json.hpp`、`third_party/cpp-httplib/httplib.h` |
| 版本号的单一来源（CMake 生成头） | `cmake/version.hpp.in` |
| 构建辅助脚本 | `tools/build.ps1` |

**尚不存在**（阶段 2 / 3 要补）：`config`、`storage`、`core` 的数据根解析与幂等初始化、
`ipc` 的命名管道、`cli` 的交互循环与单实例、`service` 的 SCM 生命周期、`server` 的 HTTP 入口、
`resources/fmt.manifest`，以及全部业务模块。`src/main.cpp` 目前只有 CLI 形态的 `--help` /
`--version` 分支，**还没有** `StartServiceCtrlDispatcherW` 入口分发，也**还没有** `fmt> ` 交互循环。

> 旧的 `FMT_ROOT` 实现（`src/core/path.cpp`、`src/core/app.cpp`、`src/cli/cli.cpp`、
> `include/fmt/core.hpp`、`src/app/`、`tests/core_test.cpp`）已从工作区移除：它们实现的是
> 「`FMT_ROOT` = exe 所在目录 + CLI 自己建四个目录」的旧口径，与第 3 节的「数据根由 CLI 声明、
> 初始化由服务执行」直接冲突，不值得改造。

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
| 2 | 基础设施：`common`（Error / Result / Time / String / Path / Logger）、`config`、`storage`、`core` 初始化（数据根解析、幂等建五个目录 + 默认 JSON） | 待开始 |
| 3 | 服务与通道：`service`（SCM 四命令 + `ServiceMain` + Recovery + `%ProgramData%` 状态文件）、`ipc`（命名管道 + 安全描述符 + 帧）、`cli`（交互循环、横幅、单实例、提权引导） | 待开始 |
| 4 | Bucket：create / list / get / use / delete，`current_bucket` 逻辑 | 待开始 |
| 5 | File / Upload / Trash / Share：`file.json`、`file_id`、文件名、extension、file_type、size、md5、`is_trash`、list / get / get by name / delete；HTTP(S) 下载、临时文件、大小限制、MD5、文件名冲突；Trash list / get / restore / delete（含 Bucket 部分恢复）；Share create / get / list / delete、20 次限制、过期、并发安全 | 待开始 |
| 6 | HTTP Server + Preview：文件查询、下载、Share 访问（浏览器入口）；图片预览 | 待开始 |

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
| `FilenameValidator` | 文档第 19/25/26/118 节要求：拒绝空文件名、非法字符、路径分隔符、路径穿越、Windows 保留名称（`CON`/`PRN`/`AUX`/`NUL`/`COM1..`/`LPT1..`）、超长文件名。属于 `common/validation`，须在上传功能前完成。错误码已预留（附录 A 的 FMT-100～105） |
| 编译器矩阵 | 仅验证 MSVC；Clang 未验证 |
| 日志轮转策略 | 日志分级与去向已定（第 7.4 节），单文件大小上限与轮转规则未定 |
| 提权副本的结果通道细节 | 命名管道 `\\.\pipe\fmt.elev.<pid>` 与临时文件二选一，编码时定稿（两者都已冻结为候选，只是还没定用哪个） |
| 数据根切换的并发保护 | 目前依赖「只有一个 CLI 窗口」（第 4.7 节）；多窗口场景不在本次范围 |

> **已冻结、不再是待解决问题**：Service 与 CLI 的通信方式（CLI 走命名管道
> `\\.\pipe\fmt.control`，第 4.3 节；浏览器走 HTTP `127.0.0.1:4122`，第 4.4 节）、
> 控制台编码（UTF-8：`SetConsoleOutputCP(CP_UTF8)` + `SetConsoleCP(CP_UTF8)`，第 7.5 节）、
> `service` 四命令与提权语义（第 4.5 节）、服务注册参数与恢复策略（第 4.6 节）、
> CLI 单实例与窗口激活（第 4.7 节）、服务宿主规则（第 4.8 节）、数据根由 CLI 声明
> 与初始化归属（第 3 节）。

## 13. 暂定内容

以下属于 V1 暂定，编码前可继续确认，不影响总体架构：`file.json`、`share.json`、
`trash.json` 单条记录的最终字段；HTTP API 路由；Share ID 生成方式；Bucket Trash
元数据结构；最大上传大小默认值；日志轮转规则；HTTP 鉴权（目前只监听 `127.0.0.1`，
面向局域网访问的安全控制后续再做）。

> 已冻结、不再属于暂定：JSON 文件组织形式（第 5.1 节）、错误码编号（附录 A）、
> 日志分级与去向（第 7.2 节）、`PathManager` 形式（第 7.3 节）、`fmt.exe` 三形态与 manifest
> `asInvoker`（第 4.1 节）、双通道与「业务只有一份实现」（第 4.2 节）、`service` 四命令与
> 提权语义（第 4.5 节）、服务注册参数与恢复策略（第 4.6 节）、CLI 单实例（第 4.7 节）、
> 服务宿主规则（第 4.8 节）、CLI 界面与输出格式（第 7.5 节）、数据根由 CLI 声明（第 3.1 节）。

## 14. 不在 V1 范围

分块上传、断点续传、上传取消、复杂用户认证、权限系统、文件夹系统、文件逻辑对象、
物理对象引用计数、复杂搜索引擎、数据库、HTTPS、复杂 API 鉴权。

本次重构另外明确不做：`service pause` / `continue`（服务不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`，
也没有对应命令）、多个 CLI 窗口（因此也不支持同时挂多个数据根）、业务命令的 HTTP 客户端实现
（CLI 只走命名管道，HTTP 只留给浏览器）、服务宿主 exe 的自动搬迁（移动请用复制，第 4.8 节）。

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
| FMT-601 | `ServiceNotInstalled` | Service 不存在 | 8 |
| FMT-602 | `ServiceOperationFailed` | Service 操作失败 | 8 |
| FMT-603 | `AdminRequired` | 需要管理员权限 | 5 |
| FMT-604 | `NoCurrentUser` | 未设置当前用户 | 7 |
| FMT-700 | `HttpRequestInvalid` | HTTP 请求参数错误 | 2 |
| FMT-701 | `PreviewUnsupported` | 该文件类型不支持预览 | 2 |

> **提权相关失败不新增编号**，全部复用上表已有语义：用户取消 UAC → `FMT-004`；提权副本等待
> 超时、SCM 操作失败 → `FMT-602`；未提权提示（打印「需要管理员权限」）→ `FMT-603`（只有最终
> 无法提权时它才是退出码，正常提权流程中它只是提示行，见第 4.5 节）；CLI 连不上服务管道 →
> `FMT-601`；服务宿主 exe 丢失需重装 → 仍用 `FMT-601`。
> 因此 `FMT-605` 起的 Service 段编号**仍然空闲**，留待出现真正的新语义时再追加。

**未映射错误码的退出码默认为 1（通用错误）。** 上表退出码一项即代码中 `ErrorCode` →
`ExitCode` 的映射依据。

> 编号分段预留：`FMT-0xx` 通用、`FMT-1xx` 文件名、`FMT-2xx` Bucket、`FMT-3xx` 上传下载、
> `FMT-4xx` Trash、`FMT-5xx` Share、`FMT-6xx`/`FMT-7xx` Service 与 HTTP、`FMT-9xx`
> 保留给后续扩展。
