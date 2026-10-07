# FMT 项目架构

> FMT：Windows 平台的文件管理系统，V1 以 CLI 为主，稳定后再加网络服务。
>
> 文档关系：
>
> | 文档 | 定位 |
> |---|---|
> | `FMT 开发文档.md` | **规范主体**：模块职责、数据结构、命令、操作流程、测试要求 |
> | `FMT 技术文档.md` | 技术实现：技术栈、接口设计、实现方式，依据开发文档编写 |
> | `FMT 项目架构.md`（本文档） | 总体架构：对象关系、分层、设计决策、错误码清单 |
> | `项目基本提示词.txt` | 项目背景说明（项目类型、架构方向），**不是规范依据** |
>
> 冲突时以 `FMT 开发文档.md` 为准。本文档描述**当前代码的真实状态**。

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

V1 用 **JSON + 文件系统**满足需求，不使用数据库、Redis、MQ、微服务、独立守护进程或复杂任务调度器。

采用分层架构，CLI 与 HTTP Server **不直接操作底层文件**，都经同一套业务核心，避免出现两套文件处理逻辑。

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

V1 开发期间以下规则视为核心规则（`FMT 开发文档.md` 第 122 节）：

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

系统明确**禁止自动**执行：覆盖文件、修改用户文件名、选择其他 Bucket、创建恢复目标
Bucket、绕过下载限制、删除文件、清空损坏 JSON。

优先级：**数据安全 > 数据一致性 > 功能正确 > 性能 > 代码简洁**。

依赖约束：V1 不引入第三方库，仅使用 C++17 标准库与 Windows API（测试用的 Catch2 除外）。
因此命名空间 `fmt` 不存在与外部库重名的问题，保持不变，后续也不再讨论改名。

## 3. 运行目录

`FMT_ROOT` = `fmt.exe` 所在目录，**不是**当前工作目录。

```text
FMT/
├── fmt.exe
├── repository/           正式文件存储
│   └── <user>/<bucket>/YYYY/MM/DD/<file_name>
├── trash/                回收站，保持原有层级便于恢复
│   └── <user>/<bucket>/YYYY/MM/DD/<file_name>
├── config/
│   ├── config.json       current_user、current_bucket、max_upload_size、size_unit、language
│   └── server.json       enabled、host、port（默认 127.0.0.1:4122）
└── data/
    ├── user.json         用户系统扩展入口
    ├── file.json         文件元数据
    ├── share.json        分享记录
    └── trash.json        回收站管理信息

log/                      运行日志（与业务数据分离）
```

初始化只做「目录检查」与「目录创建」：不存在则创建，已存在则保持原样，绝不删除、清空
或覆盖已有内容。关键 JSON 采用 `.tmp` 写入后替换，避免异常退出写出半文件。

> `log/` 已在文档中冻结，但**尚未实现创建**——当前 `App::initialize()` 只创建
> `repository/`、`trash/`、`config/`、`data/` 四个目录，日志模块落地时一并补上。

## 4. 数据模型

### 4.1 JSON 文件格式（已冻结）

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

### 4.2 file.json

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

### 4.3 share.json

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

### 4.4 trash.json（文件级记录）

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

### 4.5 对象生命周期

```text
File:   上传 → 正常 → 删除 → Trash → 恢复 → 正常
                                 └→ 永久删除 → 结束        file_id 全程不变

Bucket: 创建 → 正常 → 删除 → Trash → 恢复 → 正常
                                 └→ 永久删除 → 结束        无独立 ID

Share:  创建 → 有效 → 访问（计数 +1）→ 过期 / 次数耗尽 / 文件永久删除 → 失效
```

## 5. 源码模块

```text
include/fmt/<模块>/     公共头文件
src/
├── CMakeLists.txt      汇总各模块
├── main.cpp            程序入口
├── common/             公共类型与工具：Error、Result、Time、String、Path、Hash、Validation、Logger
├── config/             config.json / server.json 的加载、校验、保存
├── storage/            底层数据访问：JSON Storage、File Storage、Trash Storage
├── core/               路径计算与应用生命周期
├── cli/                命令行解析与输出
├── bucket/             Bucket 逻辑
├── file/               文件逻辑
├── share/              Share 逻辑
├── trash/              Trash 逻辑
├── service/            Windows Service
└── server/             HTTP Server
```

依赖方向：`CLI → Service/Business → Core → Storage → 文件系统`。
`common` 不依赖任何业务模块；业务模块之间不得直接调用对方的底层实现。

### 5.1 模块职责边界

| 模块 | 负责 | 不负责 |
|---|---|---|
| `common` | 公共类型与工具 | 任何业务规则 |
| `config` | 配置读写与校验 | 业务数据 |
| `storage` | 读 JSON、写 JSON、建目录、移动/删除/检查文件 | 业务规则（如「文件能否删除」由 Service 决定） |
| `core` | 路径计算、程序生命周期 | 业务对象 |
| `cli` | 参数解析与输出 | 直接操作文件系统业务 |
| `bucket`/`file`/`share`/`trash` | 各自的业务规则 | 越过 `storage` 直接操作文件 |
| `service` | Windows Service 生命周期 | 业务逻辑 |
| `server` | HTTP 请求解析、参数校验、调用 Service、返回响应 | 直接改 `file.json`、`repository`、`trash` |

### 5.2 头文件引用

公共头文件一律用带 `fmt/` 前缀的形式引用：

```cpp
#include "fmt/core/path.hpp"
```

新增模块只需三步：建 `src/<模块>/` 与 `include/fmt/<模块>/`、在 `src/CMakeLists.txt`
加 `add_subdirectory`、在本模块 `CMakeLists.txt` 用 `target_sources(fmt PRIVATE ...)`
登记源文件。顶层构建脚本不需要改动。细节见 `src/README.md`。

## 6. CLI 命令结构

命令集以 `FMT 开发文档.md` 第 95 节为准，实现细节见 `FMT 技术文档.md` 第 11 节：

```text
fmt.exe
├── --help
├── bucket  create <name> | list | get <name> | use <name> | delete <name>
├── file    upload <url> | list | get <file_id> | get <filename> | delete <file_id>
├── share   create <file_id> | get <share_id> | list <file_id> | delete <share_id>
├── trash   list | get <id> | restore <id> | delete <id>
└── --service  install | start | stop | delete
```

各子命令支持 `--help`（如 `fmt.exe file --help`），帮助内容必须与实际命令一致。

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

### 6.1 错误与返回值（已冻结）

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

### 6.2 日志（已冻结）

日志目录为 `FMT_ROOT/log/`，两个文件：

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

### 6.3 路径管理（已冻结）

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

### 6.4 日志轮转

单文件大小上限与轮转规则**尚未确定**（见第 11 节）。

## 7. 并发与事务

V1 即使主要单机使用也必须考虑并发，重点保护：`file_id` 分配、JSON 写入、File metadata、
Share `download_count`、`current_bucket`、文件移动。

- 用 Mutex/Lock 保护同一 JSON 数据的「读取—修改—写入」全过程，避免互相覆盖。
- 保护同一文件、同一 `file_id`、同一 Share 的操作，避免「删除+下载」「恢复+永久删除」
  「两个上传生成同一 ID」「两个下载突破 Share 次数」。
- 正式文件必须经「临时文件 → 完整完成 → 安全移动」才成为正式文件，避免下载读到半成品。
- 优先同文件系统内移动；跨盘时用「复制 → 校验 → 删除原文件」并做一致性保护。
- 数据一致性检查覆盖四种异常：metadata 有而文件无、文件有而 metadata 无、size 不符、
  md5 不符。发现异常只记录并停止危险操作，**不自动删除或重建**。

## 8. 编译与构建

环境：Windows x64、MSVC（Visual Studio Build Tools）、CMake ≥ 3.20、Ninja、C++17。

构建目录只有 **`cmake-build-debug`** 一个，与 CLion 默认配置一致。

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

产物：`cmake-build-debug/bin/fmt.exe`。

Ninja 需要 MSVC 环境变量，请在 *Visual Studio 开发者命令行* 中执行。

`fmt.exe` 不依赖任何第三方库，可完全离线构建。测试依赖 Catch2（从 github.com 拉取），
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

CI 见 `.github/workflows/ci.yml`，在 `windows-latest` 与 `ubuntu-latest` 上构建并测试。
分支：`main` 为稳定分支只接受 PR，`dev` 为集成分支。

## 9. 当前实现状态

**第一阶段（项目骨架）已完成并验收。**

| 已实现 | 位置 |
|---|---|
| 程序入口与异常兜底 | `src/main.cpp` |
| 运行目录计算（`FMT_ROOT` 与四个基础目录） | `src/core/path.cpp` |
| 启动初始化、退出码 | `src/core/app.cpp` |
| `--help` / `--version` / 未知参数 | `src/cli/cli.cpp` |
| 单元测试 16 项 | `tests/` |

其余模块（`common`、`config`、`storage`、`bucket`、`file`、`share`、`trash`、`service`、
`server`）只有目录与构建登记点，**尚无实现**。

验收结果（实测）：

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

## 10. 开发顺序

阶段划分以 `FMT 开发文档.md` 第 96 节为准，实现细节见 `FMT 技术文档.md` 第 18 节：

**每完成一个模块就走「开发 → 单元测试 → 集成测试 → 异常测试」，不允许所有模块做完
才开始测试。**

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 项目骨架：CMake、Ninja、MSVC、`fmt.exe`、`--help` | ✅ 完成 |
| 2 | 基础存储：目录初始化、Config、JSON Storage、Path、Logger、Error | 待开始 |
| 3 | Bucket：create / list / get / use / delete，`current_bucket` 逻辑 | 待开始 |
| 4 | File 基础：`file.json`、`file_id`、文件名、extension、file_type、size、md5、`is_trash` | 待开始 |
| 5 | Upload：HTTP/HTTPS 下载、临时文件、大小限制、MD5、文件名冲突 | 待开始 |
| 6 | File 操作：list / get / get by name / delete | 待开始 |
| 7 | Trash：list / get / restore / delete，含 Bucket 部分恢复 | 待开始 |
| 8 | Share：create / get / list / delete，20 次限制、过期、并发安全 | 待开始 |
| 9 | Windows Service：install / start / stop / delete + 自动恢复 | 待开始 |
| 10 | HTTP Server：文件查询、下载、Share 访问 | 待开始 |
| 11 | Preview：图片预览 | 待开始 |

## 11. 后续阶段需要先解决的问题

| 事项 | 说明 |
|---|---|
| `FilenameValidator` | 文档第 19/25/26/118 节要求：拒绝空文件名、非法字符、路径分隔符、路径穿越、Windows 保留名称（`CON`/`PRN`/`AUX`/`NUL`/`COM1..`/`LPT1..`）、超长文件名。属于 `common/validation`，须在上传功能前完成。错误码已预留（附录 A 的 FMT-100～106） |
| 控制台编码 | 现仅 `SetConsoleOutputCP(CP_UTF8)`，正式方案在 CLI 模块开发阶段冻结 |
| 编译器矩阵 | 仅验证 MSVC；Clang 未验证 |
| 日志轮转策略 | 日志分级与去向已定（第 6.4 节），单文件大小上限与轮转规则未定 |

## 12. 暂定内容

以下属于 V1 暂定，编码前可继续确认，不影响总体架构：`file.json`、`share.json`、
`trash.json` 单条记录的最终字段；HTTP API 路由；Share ID 生成方式；Bucket Trash
元数据结构；CLI 输出格式；最大上传大小默认值；Service 与 CLI 进程的最终通信方式；
日志轮转规则。

> 已冻结、不再属于暂定：JSON 文件组织形式（第 4.1 节）、错误码编号（附录 A）、
> 日志分级与去向（第 6.2 节）、`PathManager` 形式（第 6.3 节）。

## 13. 不在 V1 范围

分块上传、断点续传、上传取消、复杂用户认证、权限系统、文件夹系统、文件逻辑对象、
物理对象引用计数、复杂搜索引擎、数据库、HTTPS、复杂 API 鉴权。

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

**Bucket（阶段 3）**

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

**Trash（阶段 7）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-400 | `TrashEntryNotFound` | Trash 记录不存在 | 3 |
| FMT-401 | `RestoreConflict` | 恢复目标已存在同名文件 | 4 |
| FMT-402 | `RestoreBucketMissing` | 原 Bucket 已永久删除 | 3 |

**Share（阶段 8）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-500 | `ShareNotFound` | Share 不存在 | 3 |
| FMT-501 | `ShareExpired` | Share 已过期 | 5 |
| FMT-502 | `ShareDownloadLimitReached` | 下载次数耗尽 | 5 |
| FMT-503 | `ShareFileUnavailable` | 关联文件不可用（处于 Trash 或已删除） | 5 |

**Service 与 HTTP（阶段 9、10）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-600 | `ServiceAlreadyInstalled` | Service 已存在 | 8 |
| FMT-601 | `ServiceNotInstalled` | Service 不存在 | 8 |
| FMT-602 | `ServiceOperationFailed` | Service 操作失败 | 8 |
| FMT-603 | `AdminRequired` | 需要管理员权限 | 5 |
| FMT-604 | `NoCurrentUser` | 未设置当前用户 | 7 |
| FMT-700 | `HttpRequestInvalid` | HTTP 请求参数错误 | 2 |
| FMT-701 | `PreviewUnsupported` | 该文件类型不支持预览 | 2 |

**未映射错误码的退出码默认为 1（通用错误）。** 上表退出码一项即代码中 `ErrorCode` →
`ExitCode` 的映射依据。

> 编号分段预留：`FMT-0xx` 通用、`FMT-1xx` 文件名、`FMT-2xx` Bucket、`FMT-3xx` 上传下载、
> `FMT-4xx` Trash、`FMT-5xx` Share、`FMT-6xx`/`FMT-7xx` Service 与 HTTP、`FMT-9xx`
> 保留给后续扩展。
