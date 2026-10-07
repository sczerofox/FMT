# FMT 重构设计

> 项目名称：FMT
> 项目类型：Windows 文件管理系统
> 文档定位：**本次重构（V1 重构版）的差异说明与冻结决策索引**
> 状态：冻结，作为阶段 2 / 阶段 3 的编码依据
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

范围外（本阶段不做）：`pause`（本次确认不需要）、业务命令实现、HTTP 客户端 CLI、多用户与权限系统。

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
| 12 | 依赖：vendor `nlohmann/json` + `cpp-httplib` 到 `third_party/`，`/MT` 静态链接 CRT，产物只有一个 `fmt.exe` |
| 13 | CLI 横幅 `File Manager Tool  v1.0  ( build  2026.10.08 )`（与 `--version` 同一串）+ `Service Running...`，提示符 `fmt> `（打印前先空一行，空命令不重复空行），正常→stdout / 错误→stderr |
| 14 | 输出固定为 `执行成功...` / `错误码：0`，失败为 `执行失败：FMT-601 ...` / `错误码：8` |
| 15 | 运行目录是**六个**：`repository/` `trash/` `config/` `data/` `log/` `temp/`。`temp/` 是临时文件目录（既不是业务数据、也不是日志，随时可清）；提权结果文件与上传暂存都放这里，数据根不可写时才退回 `%TEMP%` |
| 16 | 程序横幅名 `File Manager Tool`，版本部分 `v<MAJOR>.<MINOR>`（当前 `v1.0`），构建日期由 CMake 配置时生成（`FMT_BUILD_DATE`，`%Y.%m.%d` 本地时间）→ `banner_text()` 一处产出，横幅与 `--version` 共用；用法标题是 `用法：fmt.exe [命令]` |
| 17 | **控制台只留交互**：双击时数据根体检结果（新建目录 / 新建文件 / 完整）与「服务当前状态」**只进日志**；只有异常（无法补齐、文件损坏）走 stderr |
| 18 | 帮助有两个入口：`--help` 打印带横幅与退出码表的完整用法；`help` 不带参数只列命令总览、`help <组>` 打印该组详情（`service` / `bucket` / `file` / `share` / `trash` / `help` / `exit`），交互与一次性都支持；`help <未知组>` → stderr 一行 + `FMT-001` / 退出码 2。`exit` / `quit` 是正式命令 |

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
实例数   PIPE_UNLIMITED_INSTANCES
帧格式   [4 字节小端长度][UTF-8 JSON]，一请求一响应，用 id 配对
请求     {"id":7,"op":"hello","root":"D:\\FMT2","pid":1234}
         {"id":8,"op":"file.list"}
响应     {"ok":true,"data":{...}}
         {"ok":false,"error":{"code":"FMT-305","message":"未设置当前 Bucket"}}
```

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

超时：连接时管道不存在最多重试 **5 秒**（权限类错误不重试）、普通命令 **30 秒**。连不上（`ERROR_FILE_NOT_FOUND`）→「无法连接 FMT Service，请先执行 service install」+ 退出码 8（`FMT-601`）。

### 4.3 浏览器 ↔ 服务：HTTP

保持旧设计：`server.json` 控制 `enabled` / `host` / `port`（默认 `127.0.0.1:4122`），由 cpp-httplib 提供。HTTP 请求作用于**当前数据根**。

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
├── trash/                   保持原层级，便于恢复
├── config/
│   ├── config.json          version / current_user / current_bucket /
│   │                        max_upload_size / size_unit / language
│   └── server.json          version / enabled / host / port(4122)
├── data/
│   ├── user.json            {"version":1,"users":[]}
│   ├── file.json            {"version":1,"files":[]}
│   ├── share.json           {"version":1,"shares":[]}
│   └── trash.json           {"version":1,"trash":[]}
├── log/
│   ├── fmt.log              Service 与 CLI 追加同一个文件
│   └── error.log            仅 ERROR 级
└── temp/                    临时文件，随时可以清空（既不是业务数据、也不是日志）
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
`temp/` 里放提权结果文件（固定名 `fmt-elev-<父进程 pid>.json`，父进程读完立刻删除）与以后
上传时的暂存文件；`temp/` 跟着 exe 走，用户一眼能找到、随时可清。数据根不可写（例如 exe 放在
只读位置）时退回系统临时目录 `%TEMP%` 并写一行 WARN。**Service 启动时**删除 `temp/` 下以
`fmt-` 开头的遗留文件，用户手放进去的其它文件一律不动，删除数量记一行 INFO。

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

编号一旦发布不复用、不修改语义，新增只能追加。

---

## 10. 阶段拆分

| 阶段 | 内容 | 验收 |
|---|---|---|
| 2 | `common`（Error / Result / Time / String / Path / Logger）、`config`、`storage`、`core` 初始化 | 能在指定数据根建出六个目录（`repository/` `trash/` `config/` `data/` `log/` `temp/`）+ 默认 JSON（**`ensure_root`/`check_root` 由 Service 与 CLI 共用**，第二次运行日志里只多一行「数据根完整」、控制台无输出）；JSON 损坏报 7 且不动原文件 |
| 3 | `service`（SCM 五命令 `install`/`uninstall`/`start`/`stop`/`status` + 统一查询接口 `query_status()`/`query_state()`/`installed_binary_path()`/`last_start_failure()` + 按 `dwWaitHint` 自适应的 `settle_state()` + ServiceMain + 失败编号上报 `dwServiceSpecificExitCode` + Recovery + 服务状态文件）、`ipc`（管道 + 安全描述符）、`cli`（循环、横幅 `File Manager Tool  v1.0  ( build  <日期> )`、`help` 总览与分组详情、单实例、提权、双击幂等体检与补齐、落定判定与 reinstall 兜底） | **全新环境双击 → 一次 UAC → 服务装好且开机自启 → 控制台干净（无「建了什么」）→ `service stop/start` 各弹一次 UAC、`service status` 不弹 UAC、输出与样例一致；`help` / `help service` 输出与样例一致；复制 exe 到新目录双击 → 在新目录建出数据，日志里有「数据根切换」** |
| 4 | `bucket` | create / list / get / use / delete + `current_bucket` |
| 5 | `file` / `upload` / `trash` / `share` | 需求里的文件、分享、回收站 |
| 6 | `server`（HTTP + Preview） | 浏览器可用 |

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
| 9 | 不引第三方库（架构 §2），但旧实现 vendor 了两个 | vendor `nlohmann/json` + `cpp-httplib`，静态链接 | 用户确认；避免自研 HTTP 服务端风险 |
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

---

## 12. 未决事项

| 事项 | 说明 |
|---|---|
| 日志轮转 | 单文件大小上限与轮转规则仍未定 |
| HTTP 鉴权 | 目前仅监听 `127.0.0.1`，局域网访问的安全控制后续再做 |
| 提权副本的结果通道细节 | **已定稿，从未决清单移出**：结果经结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 回传（第 4.4 节），数据根不可写时退回 `%TEMP%` 同名文件并记一行 WARN；命名管道方案作废 |
| 数据根切换的并发保护 | 目前依赖「只有一个 CLI 窗口」，多窗口场景不在本次范围 |
| 双击引导的重装判定 | **已定稿，从待决清单移出**：等待时长**不写死**，按 SCM 的 `dwWaitHint` 自适应（夹在 100 ms – 2000 ms，兜底 30 秒，不是等待类就立即结束，第 4.1 节）；仍没起来先读 `dwServiceSpecificExitCode` 还原 FMT 编号（第 4.1、6 节）：命中数据根/配置类错误码集合（第 5.4 节表）就**不重装**、只打印原因，其余才提权 `reinstall` 一次 |
| `service status` 是否要机器可读输出 | **已定稿，从待决清单移出**：**V1 不做 `--json`、也不预留参数名**。机器可读通道是**命令退出码**（`0` 成功 / 未安装 `FMT-601` → `8`），人类可读通道是固定顺序的那几行文本（第 6 节） |

