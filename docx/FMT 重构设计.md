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
- 服务生命周期命令**每条都通过 UAC 提权**，并在 CLI 里以固定格式回显。

范围外（本阶段不做）：`pause`（本次确认不需要）、业务命令实现、HTTP 客户端 CLI、多用户与权限系统。

---

## 2. 冻结决策一览

| # | 决策 |
|---|---|
| 1 | 单一 `fmt.exe`，三种形态：CLI 形态、Service 形态（被 SCM 启动）、提权短命副本。manifest 为 `asInvoker`，**不是** `requireAdministrator` |
| 2 | service 命令只有四条：`install` / `uninstall` / `start` / `stop`，**无 pause、无 delete**（`delete` 更名 `uninstall`），命令不带 `--` 前缀 |
| 3 | 每条 service 命令都走 UAC 提权（不做「已满足状态就免提权」的优化） |
| 4 | CLI ↔ 服务：命名管道 `\\.\pipe\fmt.control`；浏览器 ↔ 服务：HTTP `127.0.0.1:4122`；两者进同一个 service 层 |
| 5 | 数据根（`FMT_ROOT`）由 CLI 连接时声明，服务维护「当前数据根」；切换不删旧数据 |
| 6 | 初始化（建目录 + 默认 JSON）**由服务执行，CLI 只读不建目录** |
| 7 | 服务自身状态写在 `%ProgramData%\FMT\service.json`，不属于业务数据 |
| 8 | 服务：名 `FMT`，显示名 `FMT File Management Service`，`SERVICE_AUTO_START`，`LocalSystem`，Recovery 5s/10s/30s、失败计数 1 天重置 |
| 9 | 服务不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`；`HandlerEx` 只处理 `STOP` / `SHUTDOWN` / `INTERROGATE` |
| 10 | CLI 单实例：互斥体 `Local\FMT.CLI.v1`，已有实例则激活已有窗口，不新建窗口 |
| 11 | 服务宿主 = 首次安装时注册的 exe 绝对路径（不复制到别处）；**移动请用复制** |
| 12 | 依赖：vendor `nlohmann/json` + `cpp-httplib` 到 `third_party/`，`/MT` 静态链接 CRT，产物只有一个 `fmt.exe` |
| 13 | CLI 横幅 `FMT v1.0.0` + `Service Running...`，提示符 `fmt> `，正常→stdout / 错误→stderr |
| 14 | 输出固定为 `执行成功...` / `错误码：0`，失败为 `执行失败：FMT-601 ...` / `错误码：8` |

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

CLI 形态：不碰 core、不建目录、不写 JSON；只解析命令 + 走管道
service 命令：本机直连 SCM + UAC 提权（不走管道、不走 HTTP）
```

要点：

- **服务是全局单例**，CLI 只是客户端。任何时刻只有一个进程写 `data/*.json`，旧设计里「CLI 与服务同时改同一个 JSON」的问题从根上消失。
- 两条通道汇入同一个 service 层，所以浏览器与 CLI 的业务行为天然一致（旧文档冻结规则「CLI 与 HTTP 使用统一业务核心」继续有效）。
- `service` 四条命令**必须直连 SCM**：服务尚未安装时走任何进程间通道都是死锁。

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
- 提权副本不是独立的入口分支，而是 CLI 形态收到 `--elevated` 内部参数后的短命分支。

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

响应信封与 HTTP 完全一致，跨进程错误码用同一个字符串还原函数，**一份信封两处复用**。

两个必须处理的 Windows 坑：

1. **DACL**：服务以 `LocalSystem` 运行，管道默认只有 SYSTEM / Administrators 能连。要用 `ConvertStringSecurityDescriptorToSecurityDescriptorW` 显式授权交互用户：
   `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`
2. **强制完整性级别（MIC）**：高完整性进程创建的对象带高完整性标签，中完整性的普通 CLI 会被「禁止向上写」挡住。必须再给管道加 MIC 标签 `S:(ML;;NW;;;ME)`，否则非提权 CLI 连接报 `ERROR_ACCESS_DENIED`。

超时：连接 3 秒、普通命令 30 秒。连不上（`ERROR_FILE_NOT_FOUND`）→「无法连接 FMT Service，请先执行 service install」+ 退出码 8（`FMT-601`）。

### 4.3 浏览器 ↔ 服务：HTTP

保持旧设计：`server.json` 控制 `enabled` / `host` / `port`（默认 `127.0.0.1:4122`），由 cpp-httplib 提供。HTTP 请求作用于**当前数据根**。

### 4.4 service 命令：直连 SCM + UAC

```text
fmt >service stop
需要管理员权限            ← FMT-603 AdminRequired
正在提权...               ← ShellExecuteExW(lpVerb=L"runas")
执行成功...
错误码：0                 ← 提权子进程的退出码
```

- 是否已提权：`OpenProcessToken` + `GetTokenInformation(TokenElevation)`。
- 提权：`ShellExecuteExW(lpVerb=L"runas", fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC, nShow=SW_HIDE)`，
  父进程 `WaitForSingleObject(hProcess, 60000)` + `GetExitCodeProcess`。
- **`runas` 启动的控制台程序会另开一个控制台窗口**，所以提权副本必须 `SW_HIDE` 无窗口，结果经 `\\.\pipe\fmt.elev.<pid>`（或临时文件）回传，由父进程打印——这是「只留一个窗口」的前提。
- 用户取消 UAC（`ERROR_CANCELLED` 1223）→ `FMT-004` / 退出码 5；等待超时 → `FMT-602` / 退出码 8。
- 提权副本只做 SCM 操作，短命、不进循环。

---

## 5. 数据根

### 5.1 由 CLI 声明

```text
CLI 启动
  → GetModuleFileNameW 取自身路径 → 父目录 = root（如 D:\FMT2）
  → 连管道，首帧 {"op":"hello","root":"D:\\FMT2","pid":1234}
  → 服务：
       root != 当前数据根 ?
         ├─ 是 → 切换数据根（旧根数据原样保留），对 root 做幂等初始化，
         │        记 INFO「数据根切换: D:\FMT → D:\FMT2」
         └─ 否 → 直接进入命令循环
  → 该连接上的所有业务命令都在该根下执行
```

只有同时一个 CLI 窗口（决策 10），所以同时只有一个数据根，不会出现两个根互相打架。

### 5.2 初始化职责

`repository/`、`trash/`、`config/`、`data/`、`log/` 与默认 JSON 全部由**服务**创建；CLI 只读。
规则沿用旧文档冻结项：不存在则创建，已存在保持原样，**绝不删除、清空、覆盖**；关键 JSON 一律 `.tmp` 写完再替换。

这条同时修掉了旧文档的内部矛盾：旧 §4.4 说「CLI 只读不建目录」，旧 §18.2 实测却说首次运行创建了五个目录。现在的口径唯一：**服务建，CLI 只读**。

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
 2. 查 SCM：已运行 → 不重装、不弹 UAC；未运行 → 提权 start；未安装 → 提权 install + start
 3. 比较服务 binPath 与自身路径：
      不同但文件存在   → 作为客户端继续
      不同且文件已丢失 → 提示重新安装服务（uninstall + install，一次 UAC）
 4. 连管道声明 root=D:\FMT2 → 服务在 D:\FMT2 下建目录与默认 JSON
 5. 打印横幅与 Service Running... → fmt> 提示符
```

服务宿主就是**首次安装时那份 exe**：复制走没事，剪切或删除会让服务起不来（SCM 报 1053），需要重新 `service install`。

---

## 6. 服务生命周期

| 命令 | 行为 |
|---|---|
| `service install` | 提权 → `OpenSCManagerW` → `CreateServiceW`（`SERVICE_AUTO_START`、`LocalSystem`）→ 配 Recovery → `StartServiceW`。已存在 → `FMT-600`，不重复创建 |
| `service uninstall` | 提权 → 先 `ControlService(STOP)` → `DeleteService`。**不得删除** `repository` / `trash` / `data` / `config` / `log` |
| `service start` | 提权 → 存在则 `StartServiceW`；不存在 → `FMT-601` |
| `service stop` | 提权 → `ControlService(SERVICE_CONTROL_STOP)` → 停止接受新请求 → 等在途事务 → 停 HTTP → 退出 |

Recovery：`ChangeServiceConfig2W(SERVICE_CONFIG_FAILURE_ACTIONS)`，第一次失败 5 秒、第二次 10 秒、后续 30 秒重启，失败计数 1 天重置；不自建 watchdog。

`ServiceMain` 关键序列：

```text
RegisterServiceCtrlHandlerExW
  → SetServiceStatus(START_PENDING[, dwCheckPoint])
  → 读取 %ProgramData%\FMT\service.json，确定数据根
  → 初始化存储与业务服务
  → 起 HTTP 线程
  → SetServiceStatus(RUNNING, ACCEPT_STOP | ACCEPT_SHUTDOWN)
  → 等停止事件
  → 停 HTTP → 等在途操作 → SetServiceStatus(STOPPED)
```

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
└── log/
    ├── fmt.log              只有 Service 写
    └── error.log            仅 ERROR 级

%ProgramData%\FMT\service.json  ← 服务自身状态，不属于业务数据
```

---

## 9. 错误码与退出码

退出码沿用旧表：`0` 成功、`1` 通用、`2` 参数、`3` 对象不存在、`4` 冲突、`5` 权限/访问、`6` 数据一致性、`7` 配置、`8` Service。`FMT-NNN` 定位原因，退出码给脚本分类，两层不混用。

| 场景 | 错误码 | 退出码 |
|---|---|---|
| 服务已存在，重复 install | `FMT-600 ServiceAlreadyInstalled` | 8 |
| 服务不存在 | `FMT-601 ServiceNotInstalled` | 8 |
| SCM 操作失败 / 提权等待超时 | `FMT-602 ServiceOperationFailed` | 8 |
| 需要管理员权限（打印「需要管理员权限」） | `FMT-603 AdminRequired` | 5 |
| 用户在 UAC 点「否」 | `FMT-004 PermissionDenied` | 5 |

编号一旦发布不复用、不修改语义，新增只能追加。

---

## 10. 阶段拆分

| 阶段 | 内容 | 验收 |
|---|---|---|
| 2 | `common`（Error / Result / Time / String / Path / Logger）、`config`、`storage`、`core` 初始化 | 能在指定数据根建出五个目录 + 默认 JSON；JSON 损坏报 7 且不动原文件 |
| 3 | `service`（SCM 四命令 + ServiceMain + Recovery + 服务状态文件）、`ipc`（管道 + 安全描述符）、`cli`（循环、横幅、单实例、提权） | **全新环境双击 → 一次 UAC → 服务装好且开机自启 → `service stop/start` 各弹一次 UAC、输出与样例一致；复制 exe 到新目录双击 → 在新目录建出数据** |
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
| 3 | `--service install/start/stop/delete` | `service install/uninstall/start/stop` | 用户命令集；`delete` 更名 `uninstall` |
| 4 | 无暂停 | 明确不做 pause | 用户确认不需要 |
| 5 | 提权只写在「检查管理员权限」一句里 | 明确的 `runas` 提权 + 结果回传 + 固定输出 | 用户要求可回显的提权过程 |
| 6 | `FMT_ROOT` = 服务 exe 所在目录 | 由 CLI 声明，服务维护当前数据根 | 用户要求数据随 exe 走 |
| 7 | 初始化归属含糊（CLI 只读 vs 首次运行建目录） | 服务建、CLI 只读 | 消除文档自相矛盾 |
| 8 | 单实例只写了一句要求 | 互斥体 + 窗口激活 + UIPI 处理 | 用户要求只留一个窗口 |
| 9 | 不引第三方库（架构 §2），但旧实现 vendor 了两个 | vendor `nlohmann/json` + `cpp-httplib`，静态链接 | 用户确认；避免自研 HTTP 服务端风险 |

---

## 12. 未决事项

| 事项 | 说明 |
|---|---|
| 日志轮转 | 单文件大小上限与轮转规则仍未定 |
| HTTP 鉴权 | 目前仅监听 `127.0.0.1`，局域网访问的安全控制后续再做 |
| 提权副本的结果通道细节 | 命名管道与临时文件二选一，编码时定稿 |
| 数据根切换的并发保护 | 目前依赖「只有一个 CLI 窗口」，多窗口场景不在本次范围 |
