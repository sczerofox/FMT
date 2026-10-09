# FMT — Windows 文件管理工具

一个**单文件**（`fmt.exe`）的本地文件管理工具：既是命令行，也是 Windows 服务，
还带一套给程序调用的 HTTP 接口。没有安装包、没有第三方 DLL、没有运行库依赖。

| | |
|---|---|
| 形态 | 单个 `fmt.exe` —— CLI / Windows 服务 / 提权副本 三合一，manifest `asInvoker` |
| 语言与构建 | C++17 · CMake ≥ 3.20 · Ninja · MSVC（`/MT` 静态链接运行库） |
| 依赖 | 只用 Windows 自带组件（WinHTTP + Schannel 下载、CNG/bcrypt 随机数与哈希、`cpp-httplib` 单头文件已 vendored） |
| 接口 | CLI 走命名管道 `\\.\pipe\fmt.control`；HTTP 走 `localhost:4122`（默认关闭） |
| 业务 | Bucket、File、Trash、Share、Config |
| 测试 | 157 个用例，全绿 |
| 设计文档 | [技术文档](<docx/FMT 技术文档.md>) · [开发文档](<docx/FMT 开发文档.md>) · [项目架构](<docx/FMT 项目架构.md>) |

---

## 1. 它是什么

**数据根跟着 exe 走。** 你把这个 `fmt.exe` 放到哪个目录，**那个目录就是数据根**
（`repository/`、`trash/`、`config/`、`data/`、`log/`、`temp/` 都在它下面）。
换一个位置放 exe，就等于换了一套数据，互不干扰；服务记录的数据根也会跟着你运行的
那个 exe 走，并在切换时给出提示。

**两条入口，一份业务实现。** CLI 通过命名管道发命令，HTTP 通过 JSON 路由发命令，
两者最终调用**同一个**业务层（`service::execute_business`），所以校验、错误码、
确认流程完全一致——不会出现「CLI 拦得住、HTTP 拦不住」这种事。

**服务不是必须的，但推荐。** 不带参数双击 `fmt.exe` 会：检查/创建数据根 →
确保服务已安装并运行（必要时弹一次 UAC）→ 进入交互式命令行。CLI 自己也会写日志，
服务不在时只剩本机文件操作可用。

---

## 2. 环境要求

- Windows 10/11（x64）
- **Visual Studio Build Tools**（含 C++ 工具集，`vcvars64.bat` 存在即可，不必装完整 IDE）
- CMake ≥ 3.20、Ninja（也可以直接用 Build Tools 自带的那两份）
- PowerShell 5.1+（构建脚本用）

## 3. 构建

**方式一：一行命令（推荐）**

```powershell
pwsh -File tools/build.ps1            # 配置 + 构建
pwsh -File tools/build.ps1 -Test      # 构建完顺手跑测试
pwsh -File tools/build.ps1 -Fresh     # 先删构建目录再配置（切换构建类型时必须）
```

脚本会自动用 `vswhere` 找到 Build Tools、导入 `vcvars64` 环境、再找 `cmake` 与 `ninja`。
产物：**`cmake-build-debug/bin/fmt.exe`**（还有一个 `fmt_tests.exe`）。

**方式二：CMake 预设**

```powershell
cmake --preset debug
cmake --build --preset debug
```

---

## 4. 快速开始

```powershell
# 第一次：安装并启动服务（弹一次 UAC，之后不再需要）
.\cmake-build-debug\bin\fmt.exe service install

# 不带参数运行会进入交互式命令行
.\cmake-build-debug\bin\fmt.exe

# 一次性命令
.\cmake-build-debug\bin\fmt.exe bucket create 工作
.\cmake-build-debug\bin\fmt.exe file upload D:\某处\报表.xlsx
.\cmake-build-debug\bin\fmt.exe file list
.\cmake-build-debug\bin\fmt.exe share create fmt-20261009-0
```

在窗口里 `help` 看全部命令，`help <命令>` 看某一组命令的详细说明
（例如 `help file`、`help trash`、`help share`、`help config`）。

---

## 5. 命令一览

| 组 | 子命令 |
|---|---|
| `service` | `install` `uninstall` `start` `stop` `reinstall` `status` |
| `bucket` | `create` `list` `get` `use` `delete` |
| `file` | `upload` `list` `get` `delete` |
| `trash` | `list` `get` `restore` `delete` `empty` |
| `share` | `create` `get` `list` `delete` |
| `config` | `list` `set` |
| 其他 | `help [命令]` `version` `exit` / `quit` |

几个值得知道的：

```text
file upload <来源> [文件名]     来源可以是 http(s):// 或本机路径；大小上限取 config.json
file list [--sort name|size|id] [--search 关键字] [--page N --page-size M]
file delete <file_id|文件名>    软删除进回收站；跨 Bucket 时会先说明再确认
trash restore <标识>            文件级按 file_id 回退；桶级整体回退（同名冲突会拒绝）
trash empty                     清空整个回收站（两级一起，不可恢复，要 --yes 或答 y）
share create <file_id>          默认 20 次、7 天过期；share_id 就是访问凭证
config list                     看配置，**也打印 HTTP 访问 token**
config set max_upload_size 10MB 支持 10485760 / 10MB / 512KB / 1GB
```

**破坏性操作**（`file delete`、`trash delete`、`trash empty`、`bucket delete`）都是三步：
先检查并说清要动什么 → 交互窗口里问一次 → 一次性命令必须加 `--yes`。
服务端**独立**再校验一次 `force`，客户端跳过确认也拦得住。

## 6. 退出码

```text
0 成功    1 通用错误    2 参数错误    3 对象不存在    4 冲突
5 权限/访问    6 数据一致性    7 配置错误    8 Service 错误
```

错误码按家族划分，报错时同时给出 `FMT-NNN` 与中文说明：

| 家族 | 含义 |
|---|---|
| `FMT-0xx` | 通用、JSON、路径、路由、认证 |
| `FMT-1xx` | 文件名校验（非法字符、保留设备名、与 `file_id` 同形等） |
| `FMT-2xx` | Bucket |
| `FMT-3xx` | 上传与下载 |
| `FMT-4xx` | 回收站 |
| `FMT-5xx` | 分享 |
| `FMT-6xx` | 服务与 IPC |
| `FMT-7xx` | HTTP 与预览 |

---

## 7. HTTP 接口

**默认关闭**，由数据根下的 `config/server.json` 打开：

```json
{ "version": 1, "enabled": true, "host": "localhost", "port": 4122 }
```

改完需要重启服务（`fmt.exe service stop` → `service start`）。它**只监听本机**。

### 认证

**除下面两个之外，所有 `/api/*` 都要带 token**：

- `GET /api/ping` —— 健康检查，不要 token
- `GET /api/share/<share_id>/download` —— 别人拿分享链接下载，**分享链接本身就是凭证**

请求头两种写法都认：

```http
X-FMT-Token: 1a2867378b112b8e7864218fd2fd693f
Authorization: Bearer 1a2867378b112b8e7864218fd2fd693f
```

**token 从哪来**：`fmt.exe config list` 会打印「用户 ID / 访问 token / 建议的请求头」。
它存在数据根的 `data/user.json` 里，**默认永久有效**；密码只存哈希（PBKDF2-SHA256 + 每用户随机盐），
明文从不落盘。缺失或无效的 token 返回 **401 + `FMT-018`**。

### 路由表

| 方法 | 路径 | 说明 |
|---|---|---|
| `POST` | `/api/file/upload` | **流式上传**：请求体就是文件内容；文件名用 `?name=` 或 `Content-Disposition` |
| `GET` | `/api/file` | 列出文件，支持 `?search=` `?sort=` `?page=` `?page_size=` |
| `GET` | `/api/file/<file_id或文件名>` | 单条文件信息（含相对数据根的 `path`） |
| `GET` | `/api/file/<…>/download` | 下载（流式，`attachment`） |
| `GET` | `/api/file/<…>/preview` | 预览（流式，`inline`；图片与文本类，其余 `FMT-701`） |
| `DELETE` | `/api/file/<…>` | 软删除（`?dry_run=1` 预检、`?force=1` 执行） |
| `GET` | `/api/trash` | 回收站条目（文件级与桶级都标出来） |
| `GET` | `/api/trash/<标识>` | 单条回收站条目 |
| `DELETE` | `/api/trash/<标识>` | **永久删除**（不可恢复，需 `?force=1`） |
| `POST` | `/api/trash/<标识>/restore` | 回退 |
| `POST` | `/api/share` | 创建分享，请求体 `{"file_id": "fmt-20261009-0"}` |
| `GET` | `/api/share/<share_id>` | 查看分享（如实报告 可用/已过期/次数用尽/已撤销/文件不可用） |
| `GET` | `/api/share?file_id=<file_id>` | 列出某个文件的分享 |
| `DELETE` | `/api/share/<share_id>` | 撤销分享 |
| `GET` | `/api/share/<share_id>/download` | **公开**：别人下载分享的文件，先记账再放行 |

响应统一是信封：成功 `{"ok":true,"data":{…}}`，失败 `{"ok":false,"error":{"code":"FMT-0xx","message":"…"}}`。
状态码：`400` 参数/校验、`401` 认证、`403` 权限、`404` 不存在（含「故意不提供的接口」）、
`409` 冲突、`501` 已登记但未实现的接口。

### 例子

```powershell
$token = '1a2867378b112b8e7864218fd2fd693f'
$h = @{ 'X-FMT-Token' = $token }

# 流式上传（请求体就是内容，适合 .jar 这类大文件）
Invoke-WebRequest 'http://localhost:4122/api/file/upload?name=lib.jar' -Method Post `
    -Headers $h -ContentType 'application/octet-stream' -InFile .\lib.jar

# 搜索 + 分页：total 是命中总数，不是本页条数
Invoke-WebRequest 'http://localhost:4122/api/file?search=report&page=1&page_size=20' -Headers $h

# 下载
Invoke-WebRequest 'http://localhost:4122/api/file/fmt-20261009-0/download' -Headers $h -OutFile .\x.jar

# 公开分享链接（**不带 token**）
Invoke-WebRequest 'http://localhost:4122/api/share/0d7b3cf4bc2e/download' -OutFile .\shared.bin
```

**分页与搜索**：`page_size` 缺省或 `0` 表示不分页（一次给全，只多回一个 `total`）；
`page` 从 1 开始；上限 1000。顺序固定为**过滤 → 排序 → 分页**，所以翻页不会漏也不会重；
超出末页返回空页而不是报错。

---

## 8. 数据根

把 exe 放在 `D:\Tools\fmt\` 就等于数据根是 `D:\Tools\fmt\`：

```text
D:\Tools\fmt\
├── fmt.exe
├── repository/              唯一的业务数据（按用户/Bucket/日期分层）
│   └── <用户>/<Bucket>/YYYY/MM/DD/<文件名>
├── trash/                   回收站数据
│   └── <用户>/.files/…      文件级条目（权威记录在 data/file.json）
│   └── <用户>/<桶名>_<时间戳>/  桶级条目（权威是 .original）
├── config/
│   ├── config.json          当前用户、当前 Bucket、上传上限、语言、大小单位
│   └── server.json          HTTP 开关、监听地址与端口
├── data/                    索引（JSON）
│   ├── file.json            文件索引（**文件状态的权威**，含 is_trash）
│   ├── user.json            账号与 token
│   ├── share.json           分享
│   └── trash.json           只读兼容（老数据），新数据不再写入
├── log/
│   ├── fmt.log              运行日志（CLI 与服务共用；超过 5MB 轮转出一代）
│   └── error.log            仅 ERROR 级
└── temp/                    临时文件，随时可以清空
```

**权威在哪里**（出问题时看这几句）：文件的正常/回收站状态看 `data/file.json`；
桶是否存在看 `repository/<用户>/<Bucket>/` 这个目录；桶级回收站的原名看
`trash/<用户>/.original`；当前用户与当前 Bucket 看 `config/config.json`。
`data/user.json` 里**没有**桶列表——桶列表以磁盘上的真实目录为准。

**备份建议**：备份整个数据根即可，其中 `repository/` + `data/` + `trash/` 是必须的，
`log/` 与 `temp/` 可以不要。

---

## 9. 测试

```powershell
# 构建并跑全部用例（脚本自己找 ctest）
pwsh -File tools/build.ps1 -Test

# 也可以直接跑，按套件名过滤
.\cmake-build-debug\bin\fmt_tests.exe
.\cmake-build-debug\bin\fmt_tests.exe File.

# 全部套件名（过滤时用「套件名 + 点」，例如 Service.）
#   App  Bucket  Cli  CliE2e  Config  ErrorCode  ErrorValue  File  Hash
#   HttpClient  Ipc  Logger  PathManager  Server  Service  Storage
#   String  Time  Trash  Validation
```

157 个用例，覆盖文件名与路径校验、Bucket/File/Trash/Share 全流程、错误码与退出码、
配置与存储的并发写、日志轮转、命名管道协议、HTTP 路由与状态码、认证、流式上传下载、
以及**用真实 exe + 真实管道**跑的端到端用例。

> 端到端测试会把管道名换成唯一的 `FMT_PIPE`，并用 `FMT_NO_SERVICE=1` 禁止子进程
> 触碰 Windows 服务管理——测试**不能**改动你机器上已安装的服务。

---

## 10. 目录结构

```text
include/fmt/     公共头文件（按模块分目录）
src/             各模块实现，源文件在各自 CMakeLists.txt 里登记
tests/           单元测试与端到端测试
tools/           build.ps1（配置 + 构建 + 可选测试）
third_party/     vendored 单头文件库（cpp-httplib 等）
cmake/           构建辅助（含版本头模板 version.hpp.in）
resources/       fmt.manifest（asInvoker）
docx/            设计文档
```

---

## 11. 设计文档

三份文档分工明确，改动代码时请同步：

- **[技术文档](<docx/FMT 技术文档.md>)** —— 数据结构、模块实现、HTTP 路由、服务与 IPC、
  错误码与状态码、以及历次提交留下的**陷阱与教训**。
- **[开发文档](<docx/FMT 开发文档.md>)** —— 需求口径、命令语义、校验规则、决策与其理由，
  以及「不在本次范围」的东西。
- **[项目架构](<docx/FMT 项目架构.md>)** —— 模块划分、数据根布局、阶段推进与决策索引。

---

## 12. 目前不做的

- **服务暂停/继续**（不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`，只处理 STOP/SHUTDOWN/INTERROGATE）
- **HTTP 客户端形态的 CLI**（CLI 走管道；HTTP 可关闭、端口可能被占）
- **多用户与权限系统**（`current_user` 目前是占位名 `user`；HTTP 侧是单账号 + token，不是多用户体系）
- **HTTP 的 Bucket 接口**（桶由 CLI 管理，HTTP 客户端只操作**当前** Bucket）

## 13. 已知边界

- HTTP 默认关闭，且只监听本机；打开或改地址要改 `config/server.json` 后重启服务。
- token 默认**永久有效**（尚无轮换/吊销命令），请按凭证对待。
- 分享默认 **20 次下载、7 天过期**，两者相互独立；文件进回收站后分享立即失效。
- 预览只支持图片（按扩展名）与文本类（`.txt/.md/.json/.csv/.log/.xml`），其余返回 `FMT-701`；
  **下载不受此限制**，任何类型都能下载。
- 日志超过 **5MB** 轮转一代（`fmt.log.1`），只保留一代。
