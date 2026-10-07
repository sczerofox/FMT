# FMT 技术文档

> 项目：FMT（Windows 文件管理系统）
> 版本：V1
> 平台：Windows x64｜语言：C++17｜构建：CMake + Ninja + MSVC
> 产物：`fmt.exe`
>
> **本文档的编写依据是 `FMT 开发文档.md`**，逐条落实其规范并补充实现细节。
> 与开发文档冲突时以开发文档为准。
>
> `项目基本提示词.txt` 是项目背景说明（项目类型、架构方向、技术层补充），
> 不作为规范依据。

---

## 1. 技术栈与实现约束

### 1.1 技术栈

```text
Windows x64
C++17（CMAKE_CXX_STANDARD 17，required ON，extensions OFF）
CMake ≥ 3.20
Ninja
MSVC（Visual Studio Build Tools，cl.exe / link.exe）
```

构建链：`CMake → Ninja → MSVC → fmt.exe`

不使用 `.sln` / `.vcxproj` 作为核心构建入口。

### 1.2 第三方依赖

**约束：最终产物只能是单个 `fmt.exe`。** 因此只引入**单头文件（header-only）库**——
它们编译进可执行文件，不产生 DLL。需要额外 DLL 的库不引入，除非确认无替代方案。

引入方式：**vendored，直接放进 `third_party/`**，不用 FetchContent。
理由是可重复构建、离线构建、不受 GitHub 网络影响。

| 库 | 版本 | 用途 | 位置 |
|---|---|---|---|
| nlohmann/json | 3.11.3 | JSON 解析与序列化 | `third_party/nlohmann/json.hpp` |
| cpp-httplib | 0.18.3 | HTTP 服务端**与**客户端 | `third_party/cpp-httplib/httplib.h` |

引用形式（包含路径以 `third_party/` 为根）：

```cpp
#include <nlohmann/json.hpp>
#include <cpp-httplib/httplib.h>
```

`cpp-httplib` 同时提供 `httplib::Server` 与 `httplib::Client`，正好覆盖
Service 的服务端与 CLI 的客户端两个需求。Windows 下需链接 `ws2_32`（已在
`third_party/CMakeLists.txt` 中处理）。

其余功能自行实现或使用系统组件：

| 功能 | 实现方式 |
|---|---|
| MD5 | `common/hash`，自研（RFC 1321，约 150 行；引入库不划算） |
| 文件名 / URL 校验 | `common/validation` |
| 时间格式化 | `common/time` |
| 日志 | `common/logger` |
| Windows Service | Windows SCM API（`advapi32` 系统库） |
| 随机数（Share ID） | `BCryptGenRandom`（`bcrypt` 系统库） |
| URL 下载 | cpp-httplib 的 `Client`（HTTP）；HTTPS 需要 OpenSSL，V1 暂不支持 |

**不引入任何测试框架。** 早期版本用 Catch2（`FetchContent` 从 github.com 拉取），
已移除，原因见 18.8。验证方式是真实 `fmt.exe` 跨进程实测。

### 1.3 单文件产物

```cmake
# 必须在 project() 之前设置，否则不生效
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
```

静态链接 MSVC 运行库。动态链接会引入 `VCRUNTIME140.dll` / `MSVCP140.dll`，
Debug 下更是 `ucrtbased.dll` 等调试版运行库——那些不会存在于用户机器上。

> **注意：不能用 `if(MSVC)` 包起来。** `MSVC` 变量由 `project()` 检测编译器后才定义，
> 在此之前恒为假，设置会被静默跳过（这是实际踩过的坑）。该变量只被 MSVC 生成器使用，
> 其他编译器忽略，因此无条件设置是安全的。

验证方式（依赖应只剩系统 DLL）：

```powershell
dumpbin /dependents cmake-build-debug\bin\fmt.exe
# 期望输出：WS2_32.dll、KERNEL32.dll —— 均为系统组件
```

实测结果：`fmt.exe` 仅依赖 `WS2_32.dll` 与 `KERNEL32.dll`，符合单 exe 要求。

### 1.4 编码要求

| 项 | 值 |
|---|---|
| 源文件编码 | UTF-8 |
| MSVC 选项 | `/utf-8`（源字符集与执行字符集均为 UTF-8） |
| 控制台 | `SetConsoleOutputCP(CP_UTF8)`，在 `App::initialize()` 中设置 |
| 路径类型 | `std::filesystem::path`，Windows 下用宽字符 API 获取 |

### 1.5 编译选项

```cmake
# MSVC
/W4 /permissive- /utf-8
# GCC / Clang
-Wall -Wextra -Wpedantic -Wshadow -Wconversion
```

目标：警告在开发阶段解决，不积累。不为追求「零警告」关闭合理检查。

---

## 2. 源码结构

```text
FMT/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md                     构建与使用说明
├── cmake/version.hpp.in          版本头模板
├── include/fmt/<模块>/           公共头文件
├── src/
│   ├── CMakeLists.txt            汇总各模块
│   ├── main.cpp                  程序入口
│   ├── common/                   公共类型与工具
│   ├── config/                   配置
│   ├── storage/                  底层数据访问
│   ├── core/                     路径与程序生命周期
│   ├── cli/                      命令行
│   ├── bucket/  file/            业务模块
│   ├── service/                  Windows Service
│   └── server/                   HTTP Server
├── third_party/                  vendored 单头文件库
├── resources/
└── docx/                         项目文档
```

> `share` 没有独立目录：Share 的存储与规则都在 `src/file/service.cpp` 里，
> 因为它与 File 共用同一把锁和同一份元数据读取路径，拆开只会引入跨模块加锁。
> `trash` 尚未实现，同样不建空目录。

### 2.1 各目录职责

| 目录 | 职责 |
|---|---|
| `cmake/` | CMake 辅助配置与模板 |
| `include/fmt/` | 公共头文件，按模块分子目录 |
| `src/` | 源代码，按模块分子目录 |
| `resources/` | 程序资源（图标等），**不得**存放 `repository`/`data`/`trash`/`config` |
| `docx/` | 项目文档 |

### 2.2 模块与依赖方向

```text
CLI
 │
 ▼
Service / Business      bucket, file, share, trash, config
 │
 ▼
Core                    path, app
 │
 ├── Storage            JSON、文件操作
 └── 文件系统
```

| 模块 | 负责 | 不负责 |
|---|---|---|
| `common` | Error、Result、Time、String、Path、Hash、Validation、Logger | 任何业务规则 |
| `config` | `config.json` / `server.json` 的加载、校验、保存 | 业务数据 |
| `storage` | 读 JSON、写 JSON、建目录、移动/删除/检查文件 | 业务规则（如「文件能否删除」由 Service 决定） |
| `core` | `FMT_ROOT`、运行目录、程序生命周期 | 业务对象 |
| `cli` | 参数解析、输出、交互循环 | 直接操作文件系统业务 |
| `bucket`/`file`/`share`/`trash` | 各自业务规则 | 越过 `storage` 直接操作文件 |
| `service` | Windows Service 生命周期 | 业务逻辑 |
| `server` | HTTP 请求解析、调用 Service、返回响应 | 直接改 `data/*.json`、`repository`、`trash` |

业务模块之间不得直接调用对方的底层实现。

### 2.3 公共头文件引用

```cpp
#include "fmt/core/path.hpp"     // 正确
#include "core/path.hpp"         // 错误
```

`include/` 是唯一公共头文件根，所有引用带 `fmt/` 前缀。

### 2.4 新增模块

1. 建 `src/<模块>/` 与 `include/fmt/<模块>/`
2. 在 `src/CMakeLists.txt` 加 `add_subdirectory(<模块>)`
3. 在该模块 `CMakeLists.txt` 用 `target_sources(fmt PRIVATE ...)` 登记源文件

顶层构建脚本不需要改动。

### 2.5 头文件守卫

统一使用 `#pragma once`（MSVC 支持良好，与项目单一编译器目标一致）。

---

## 3. 构建

### 3.1 标准流程

只有**一个构建配置**（Debug）和**一个构建目录**（`cmake-build-debug`）：

```powershell
cmake --preset debug
cmake --build --preset debug
```

产物：`cmake-build-debug/bin/fmt.exe`。

Ninja 需要 MSVC 环境变量，请在 *Visual Studio 开发者命令行* 中执行。

在 CLion 中直接打开工程即可：`CMakePresets.json` 里登记了 `debug` 预设，
CLion 的 CMake 配置与命令行**共用同一个** `cmake-build-debug`。

### 3.2 为什么只留一个构建目录

早期版本同时提供 `debug` 与 `release` 两个预设，各有独立目录。实践下来弊大于利：

- 两个预设共用目录时 CMake **不会**重设 `CMAKE_BUILD_TYPE`，结果是
  Release 静默覆盖 Debug 产物而缓存仍写着 Debug，排查极费时；
- 该工程是课程/交付用途，Release 并没有针对性的验证流程；
- 多一份 158 MB 的构建目录对本地磁盘是纯负担。

需要 Release 时临时手写一次即可，不必固化进预设：

```powershell
cmake -S . -B cmake-build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --parallel
```

### 3.3 不再使用 /RTC1

`cmake-build-debug` 里的 `CMAKE_CXX_FLAGS_DEBUG` 被显式设为 `/Zi /Ob0 /Od`，
**去掉了 MSVC 默认的 `/RTC1`**（= `/RTCs` + `/RTCu`）。

原因不是「RTC 不好」，而是它在本工程的组合下会把真实的悬垂引用/自锁死
渲染成「栈变量损坏」并直接以访问违例终止进程，崩溃点与真因无关。
拆除 `/RTC1` 之后，同一份代码暴露出的真因是
`Logger` 被移动赋值后 `Service` 仍持有旧对象引用（见 18.9）。

需要 RTC 时在 IDE 里对单个文件临时打开即可。

### 3.4 版本号

单一来源：`CMakeLists.txt` 的 `project(FMT VERSION 1.0.0)`。
经 `cmake/version.hpp.in` 生成 `fmt/version.hpp`：

```cpp
#define FMT_VERSION_MAJOR 1
#define FMT_VERSION_MINOR 0
#define FMT_VERSION_PATCH 0
#define FMT_VERSION      "1.0.0"
#define FMT_NAME_VERSION "FMT 1.0.0"
#define FMT_PROGRAM_NAME "fmt.exe"
```

**任何源码不得重复硬编码版本号。**

---

## 4. 运行目录

### 4.1 FMT_ROOT

```text
FMT_ROOT = fmt.exe 所在目录
```

**绝不是当前工作目录。** 用户位于 `C:\>` 执行 `D:\FMT\fmt.exe` 时，数据仍写在
`D:\FMT\`。

Windows 下用 `GetModuleFileNameW`（宽字符）获取自身路径，取其父目录。
缓冲区按需倍增，处理超长路径。

### 4.2 目录布局

```text
FMT_ROOT/
├── fmt.exe
├── repository/            正式文件
│   └── <user>/<bucket>/YYYY/MM/DD/<file_name>
├── trash/                 回收站，保持原层级
│   └── <user>/<bucket>/YYYY/MM/DD/<file_name>
├── config/
│   ├── config.json
│   └── server.json
├── data/
│   ├── user.json
│   ├── file.json
│   ├── share.json
│   └── trash.json
└── log/
    ├── fmt.log            全部日志
    └── error.log          仅 ERROR 级
```

### 4.3 PathManager

**所有路径的唯一出口**，任何模块不得自行拼接字符串。

```cpp
class PathManager {
public:
    explicit PathManager(std::filesystem::path root);

    const std::filesystem::path& root() const;
    std::filesystem::path repository() const;
    std::filesystem::path trash() const;
    std::filesystem::path config() const;
    std::filesystem::path data() const;
    std::filesystem::path logs() const;

    std::filesystem::path config_json() const;   // config/config.json
    std::filesystem::path server_json() const;   // config/server.json
    std::filesystem::path user_data() const;     // data/user.json
    std::filesystem::path file_data() const;     // data/file.json
    std::filesystem::path share_data() const;    // data/share.json
    std::filesystem::path trash_data() const;    // data/trash.json

    // repository/<user>/<bucket>/YYYY/MM/DD/<file_name>
    std::filesystem::path repository_file(const std::string& user,
                                          const std::string& bucket,
                                          const std::string& date,
                                          const std::string& file_name) const;
    // trash 同理，层级与 repository 一致，便于还原
    std::filesystem::path trash_file(const std::string& user,
                                     const std::string& bucket,
                                     const std::string& date,
                                     const std::string& file_name) const;
};
```

**不做全局单例**，通过上下文传递，便于测试用独立 root：

```cpp
struct AppContext {
    PathManager paths;
    Config      config;
    Logger      logger;
};
```

### 4.4 初始化

**Service 负责创建目录与数据；CLI 只读不写、不创建目录。**
因此初始化分为两条路径。

Service 启动时：

```text
1. 获取 exe 路径 → 确定 FMT_ROOT
2. 检查 FMT_ROOT 是否存在
3. 创建 repository/
4. 创建 trash/
5. 创建 config/
6. 创建 data/
7. 创建 log/
8. 创建默认 JSON（不存在时）
9. 加载配置
10. 初始化业务服务 → 启动 HTTP Server
```

CLI 启动时：

```text
1. 获取 exe 路径 → 确定 FMT_ROOT（仅用于提示信息）
2. 解析命令行
3. service 命令 → 直连 SCM，结束
4. 其他命令   → HTTP 请求 Service，格式化输出
```

规则：

```text
不存在 → 创建
已存在 → 保持原样，不删除、不清空、不覆盖
```

任一必要目录创建失败（仅 Service）：

```text
停止执行 → 输出失败目录与原因 → 返回非 0 退出码
```

绝不继续运行假装成功。示例输出：

```text
Failed to initialize FMT directories.
Directory: D:\FMT\repository
Reason: Access is denied.
```

不主动修改系统权限，不请求管理员权限（Service 安装除外）。

---

## 5. 配置

### 5.1 config/config.json

```json
{
  "current_user": "",
  "current_bucket": "",
  "max_upload_size": 52428800,
  "size_unit": "MB",
  "language": "zh-CN"
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `current_user` | string | 当前用户。用户系统未实现前为空，由 CLI 引导设置 |
| `current_bucket` | string | 当前默认 Bucket 的**名称**，不保存路径或 ID |
| `max_upload_size` | integer | 最大上传字节数，统一用字节 |
| `size_unit` | string | 显示单位 B/KB/MB/GB，只影响显示 |
| `language` | string | CLI 显示语言 |

### 5.2 config/server.json

```json
{
  "enabled": false,
  "host": "127.0.0.1",
  "port": 4122
}
```

| 字段 | 说明 |
|---|---|
| `enabled` | 是否启用 HTTP。默认 `false`，Service 场景下由安装流程置为 `true` |
| `host` | 监听地址，`0.0.0.0` 允许局域网访问 |
| `port` | 监听端口，默认 4122 |

V1 使用 HTTP，后续可扩展 HTTPS。

### 5.3 加载规则

```text
启动 → 检查 config/ → 加载 config.json → 校验 JSON
     → 加载 server.json → 校验配置 → 进入程序
```

| 情况 | 处理 |
|---|---|
| 配置不存在 | 允许创建默认配置 |
| JSON 损坏 | **停止初始化**，报告配置错误 |
| 字段缺失 | 用默认值补齐并回写（不改动其他字段） |
| 字段类型错误 | 报配置错误，不猜测 |

**不得删除原配置，不得重新生成空配置。**

### 5.4 保存规则

```text
读取 → 修改内存对象 → 写 config.json.tmp → 验证临时文件 → 替换正式文件
```

只有临时文件写入并验证通过后才替换。避免程序异常退出写出半文件。

配置节流：`current_bucket` 切换等高频改动可延迟写入，但**退出前必须落盘**。

---

## 6. JSON 实现

### 6.1 文件格式

分两类，**都带 `version` 字段**：

| 类型 | 格式 | 文件 |
|---|---|---|
| 单例 | `{ "version": 1, ... }` | `config.json`、`server.json` |
| 集合 | `{ "version": 1, "<集合名>": [ ... ] }` | `file.json`、`share.json`、`trash.json`、`user.json` |

集合字段名固定：`files`、`shares`、`trash`、`users`。

采用「带集合字段的对象」而非裸数组，便于给集合增加元数据、演进 Schema。

### 6.2 版本策略

> **只接受明确支持的版本，未知版本直接拒绝。**

程序只支持 `version: 1`。遇到 `version: 2` 返回错误，**不降级、不猜测、
不尝试兼容、不自动修改文件**。以后支持 v2 时由代码明确实现 v2 读取逻辑。

### 6.3 解析器边界

支持：

```text
Object { }
Array [ ]
String（含 \uXXXX 与常规转义）
Number（整数与浮点）
true / false / null
```

明确不支持：

```text
// 注释
/* */ 注释
尾随逗号
单引号字符串
NaN / Infinity
任何非标准 JSON
「尽量解析」的容错
```

遇到 `{ "file_id": "xxx", }` 直接报 JSON 格式错误，不自动修复。

> JSON 是程序数据，不是给用户写的配置语言。解析失败就失败，不猜、不修、不吞错误。

### 6.4 组件划分
```text
src/storage/
├── json_file.hpp/.cpp      JSON 文件读写（npjson 之上的薄封装）
└── json_io.hpp             统一解析/序列化入口与错误转换
```

JSON 值类型使用 **nlohmann/json 的 `nlohmann::json`**，不再自研：

```cpp
using Json = nlohmann::json;
```

自研的理由已消失——引入单头文件库符合"单 exe"约束，且更可靠。只需要在它之上包一层，
把解析失败转换为项目统一的 `Result`/`Error`（见 6.5）。

### 6.5 解析与业务结构分离

```text
file.json 文本
    ↓ nlohmann::json::parse
Json
    ↓ 字段映射（业务层）
FileRecord
```

业务层负责字段映射与校验，`file.json` 字段变化不影响底层库的用法。

```cpp
// 业务侧接口示意
Result<FileRecord> file_record_from_json(const Json&);
Json               file_record_to_json(const FileRecord&);
```

规则：

| 情况 | 处理 |
|---|---|
| 解析失败 | 返回 `JsonParseError`（`FMT-006`），**不尝试修复** |
| 缺少必填字段 | 报错，不猜测 |
| 出现未知字段 | 忽略（向前兼容），但**不写回** |
| 版本字段不支持 | 返回 `JsonUnsupportedVersion`（`FMT-011`） |

`nlohmann::json::parse` 使用 `allow_exceptions = true`（默认），由薄封装捕获
`nlohmann::json::parse_error` 并转换为 `Error`，避免异常穿透到业务层。

### 6.6 原子写入

```cpp
Status JsonFile::write_atomic(const std::filesystem::path& target,
                              const JsonValue& value);
```

步骤：

```text
1. 序列化到内存
2. 写入 target + ".tmp"
3. 关闭并 flush
4. 重新读取 .tmp 并解析，验证可读
5. 替换 target（同卷用 MoveFileEx 覆盖）
```

任一步失败：删除 `.tmp`，保留原文件，返回错误。

### 6.7 Unicode

JSON 文本按 UTF-8 读写。文件名可能含中文，因此：

- 解析 `\uXXXX` 时处理 UTF-16 代理对，正确合成非 BMP 字符
- 写字符串时**不转义非 ASCII 字符**，直接输出 UTF-8 字节（可读性优先）
- 必须转义的字符：`"`、`\`、控制字符（`\u0000`～`\u001F`）

### 6.8 并发

同一 JSON 文件的「读取—修改—写入」全过程必须持锁。见第 15 节。

---

## 7. 数据模型

### 7.1 file.json

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

| 字段 | 类型 | 说明 |
|---|---|---|
| `file_id` | string | 全局唯一 |
| `user` | string | 所属用户 |
| `bucket` | string | Bucket 名称 |
| `file_name` | string | 含扩展名的完整文件名 |
| `extension` | string | 小写扩展名，含点 |
| `file_type` | string | `image`/`text`/`video`/`archive`/`other` |
| `size` | integer | 字节数 |
| `md5` | string | 32 位小写十六进制 |
| `is_trash` | boolean | 是否在回收站 |

**禁止字段**（属于 Share，不得写入 `file.json`）：

```text
max_download_count
download_count
expire_time
```

C++ 侧对应结构：

```cpp
struct FileRecord {
    std::string file_id;
    std::string user;
    std::string bucket;
    std::string file_name;
    std::string extension;
    std::string file_type;
    std::uint64_t size = 0;
    std::string md5;
    bool is_trash = false;
};
```

### 7.2 share.json

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

| 字段 | 说明 |
|---|---|
| `share_id` | Share 唯一标识，**唯一且不可预测** |
| `file_id` | 对应文件 |
| `max_download_count` | 最大下载次数，默认 20 |
| `download_count` | 当前已完成下载次数 |
| `expire_time` | 过期时间，`null` 表示不过期 |
| `is_valid` | 是否有效 |

`expire_time` 与下载次数**相互独立**：20 次 + 7 天 = 7 天内最多下载 20 次。
一个文件可有多个 Share，各自独立。

### 7.3 trash.json

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

| 字段 | 说明 |
|---|---|
| `type` | `file` 或 `bucket` |
| `original_path` | 供恢复使用 |

Bucket 级记录**不使用 `file_id`**，用 Bucket 信息管理。Bucket 不生成 `file_id`。

### 7.4 user.json

```json
{
  "version": 1,
  "users": [
    { "username": "user" }
  ]
}
```

V1 不实现完整用户系统，保留此文件作为扩展入口。
当前用户由 `config.json` 的 `current_user` 确定。

### 7.5 时间格式

统一使用 ISO 8601：

```text
2026-10-05T20:00:00
```

本地时间，不带时区偏移。`deleted_at`、`expire_time` 等均用此格式。
序列化与解析集中在 `common/time`。

---

## 8. File ID 生成

### 8.1 格式

```text
fmt-YYYYMMDD-N
```

示例：`fmt-20261005-0`、`fmt-20261005-1`、`fmt-20261005-2`

### 8.2 规则

- 每天从 `0` 开始
- 日期变化后计数器重新从 `0` 开始
- **全局唯一**
- 程序重启后不得产生重复
- 文件进入回收站、恢复，`file_id` 不变
- Bucket 删除/恢复不改变文件 ID

### 8.3 持久化与并发

不能只依赖内存计数器。

```text
启动时：扫描 data/file.json 中当天（YYYYMMDD）已有 ID
       取最大 N → next = N + 1
分配时：mutex 保护 next++ → 格式化 file_id → 检查唯一性
```

`IDGenerator` 须保证：

- 分配是**原子**的，并发上传不产生相同 ID
- 即使程序异常退出，重启后也不会重复（因为从已有 metadata 重新计算）

跨天时重新扫描一次当天基数。

### 8.4 接口

```cpp
class FileIdGenerator {
public:
    FileIdGenerator(const PathManager& paths, Logger& logger);

    // 启动时调用：从已有 metadata 恢复当天基数
    Status initialize();

    // 原子分配下一个 ID
    Result<std::string> next();
};
```

---

## 9. 校验

### 9.1 文件名规则

必须支持：Unicode、中文、空格。

必须拒绝：

```text
空文件名
Windows 非法字符： \ / : * ? " < > |
路径分隔符：/ 与 \
路径穿越：.. 与 . 
Windows 保留名称：CON PRN AUX NUL COM1..COM9 LPT1..LPT9
   —— 含带扩展名的形式，如 CON.txt
Windows 保留字符结尾：文件名以空格或 . 结尾
超长文件名（超过 255 字符，或完整路径超过 260 字符）
```

**超长直接拒绝，不允许程序自动截断。**

### 9.2 文件名唯一性

同一用户的**正常**文件空间中 `file_name` 必须唯一。
**Bucket 不构成文件名命名空间**：

```text
Bucket A/test.txt
Bucket B/test.txt
```

对同一用户**不允许**。不同用户可以使用同名。

重名时：

```text
提示文件名冲突 → 要求用户修改文件名
```

绝不自动覆盖、自动改名、自动添加 `(1)`。

Trash **不占用**正常文件名空间：删除 `test.txt` 后可以重新上传 `test.txt`。

### 9.3 接口

```cpp
namespace fmt::common::validation {

// 文件名合法性：返回具体错误码（FMT-100 ~ FMT-104）
Status validate_file_name(const std::string& name);

// 是否为 Windows 保留设备名（含带扩展名形式）
bool is_reserved_device_name(const std::string& name);

// URL 合法性：仅允许 http / https
Status validate_url(const std::string& url);

}
```

---

## 10. 业务服务

各服务的流程规范见 `FMT 开发文档.md` 第 27～63 节，本节给出接口与实现要点。

### 10.1 BucketService

```cpp
class BucketService {
public:
    Status create(const std::string& name);
    Result<std::vector<std::string>> list();
    Result<BucketInfo> get(const std::string& name);
    Status use(const std::string& name);      // 设置 current_bucket 并保存配置
    Status remove(const std::string& name);   // 移入 Trash
};
```

要点：

| 操作 | 要点 |
|---|---|
| `create` | 名称校验 → 检查是否已存在 → 建目录 → **若是第一个 Bucket，自动设为 `current_bucket`** |
| `use` | 检查存在 → 改 `current_bucket` → 保存 `config.json`。**不修改 Bucket 本身** |
| `remove` | 移动数据到 Trash → 相关文件 `is_trash = true` → 写 `trash.json` → **若删除的是 `current_bucket`，置空，不自动切换** |

Bucket **不生成 `bucket_id`**，用名称标识。

### 10.2 FileService

```cpp
class FileService {
public:
    Result<FileRecord> upload(const std::string& source,   // 本机路径或 URL
                              const std::string& file_name);
    Result<std::vector<FileRecord>> list();
    Result<FileRecord> get_by_id(const std::string& file_id);
    Result<FileRecord> get_by_name(const std::string& file_name);
    Status remove(const std::string& file_id);             // 软删除进 Trash
    Result<std::filesystem::path> resolve_path(const FileRecord&);  // 供 HTTP 下载
};
```

**上传完整流程**（开发文档第 32 节）：

```text
1.  检查 current_user
2.  检查 current_bucket（为空则提示先创建或选择）
3.  校验来源：本机路径存在 / URL 为 http、https
4.  创建临时文件
5.  下载或复制数据（流式，不整文件入内存）
6.  检查下载/复制是否完整
7.  检查大小 ≤ max_upload_size（超限则删除临时文件）
8.  计算 MD5（32 位小写十六进制）
9.  MD5 去重检查（已存在 → 提示「该文件已经存在」，终止）
10. 文件名合法性检查
11. 文件名冲突检查（同用户 + 正常文件 + 同名 → 冲突）
12. 生成 file_id
13. 移动临时文件到 repository/<user>/<bucket>/YYYY/MM/DD/
14. 写入 file.json
15. 完成
```

**临时文件位置**：系统临时目录或 `FMT_ROOT/temp/`。
**绝不**直接下载到 `repository`，避免半成品进入正式空间。

**失败处理**：

| 情况 | 处理 |
|---|---|
| 404 / 连接失败 / 超时 / 中断 | 删除临时文件，不生成 ID，不改 metadata |
| 超过大小限制 | 删除临时文件，不生成 ID，不改 metadata |
| MD5 重复 | 删除临时文件，提示已存在 |
| 文件名冲突 | 删除临时文件，提示改名 |

**回滚**：若文件已移入 `repository` 但 `file.json` 写入失败，
必须删除刚提交的文件，或进入一致性恢复流程。**绝不报告上传成功。**

**list**：只显示 `current_user` + `current_bucket` + `is_trash == false` 的文件。
**get**：`file_id` 全局唯一；文件名查询限定 `current_user` + 正常文件。

### 10.3 ShareService

```cpp
class ShareService {
public:
    Result<ShareRecord> create(const std::string& file_id);
    Result<ShareRecord> get(const std::string& share_id);
    Result<std::vector<ShareRecord>> list(const std::string& file_id);
    Status remove(const std::string& share_id);

    // 访问前检查，通过后由调用方完成传输再计数
    Result<ShareRecord> begin_download(const std::string& share_id);
    Status finish_download(const std::string& share_id);   // 传输成功才调用
};
```

要点：

- 不存在的文件不能创建 Share；文件处于 Trash 时不能创建
- `share_id` 生成：用 `BCryptGenRandom` 取 16 字节随机数，转十六进制。
  **不得使用递增数字**
- 访问检查顺序：Share 有效 → 文件存在 → 文件状态（非 Trash）→ 下载次数 → 过期时间
- **Share 不得绕过 File 状态限制**
- 过期 Share 返回「存在但已过期」，不伪装成从未存在

**下载计数并发**：`begin_download` 在锁内完成「检查次数 → 确认资格 → 预占一个名额」，
`finish_download` 成功后才真正 `download_count + 1`；失败或中断则释放预占。
这样最大 20 次时不会出现 21 次。

### 10.4 TrashService

```cpp
class TrashService {
public:
    Result<std::vector<TrashEntry>> list();
    Result<TrashEntry> get(const std::string& id);
    Status restore(const std::string& id, const std::string& target_bucket = {});
    Status remove(const std::string& id);       // 永久删除
};
```

**恢复流程**：

```text
检查 Trash 记录 → 检查原 Bucket → 检查目标位置 → 检查文件名冲突
    ├─ 无冲突 → 移动文件回原位置 → is_trash = false → 更新 trash.json
    └─ 有冲突 → 不覆盖、不改名，文件留在 Trash，metadata 不变
               提示「存在文件名冲突，请处理冲突后再次恢复」
```

**原 Bucket 已永久删除**时：要求用户指定一个**现有** Bucket，
**不自动创建**。提示形式：

```text
请选择恢复目标 Bucket：
1. 工作
2. 学习
3. 项目
```

**Bucket 恢复**：逐个判断内部文件，无冲突的恢复，冲突的留在 Trash，
允许出现「部分恢复」状态。

**永久删除**：

```text
确认 → 删除实际文件 → 删除 file.json 记录 → 清理 trash.json → 清理相关 share.json
```

Bucket 永久删除时，其下所有文件与相关 metadata、Share 一并清理。

---

## 11. CLI

### 11.1 CLI 的定位（架构决策）

**CLI 不碰业务数据。** 它是 HTTP 客户端：解析命令、校验参数、构造请求、格式化输出。
真正的业务（文件是否存在、大小是否允许、算 MD5、生成 file_id、写 JSON）全部由
Service 完成。

```text
                 fmt.exe
                    │
          ┌─────────┴─────────┐
          │                   │
    service 命令          业务命令
          │                   │
          ↓                   ↓
      SCM API            HTTP Client
                              │
                              ↓ 127.0.0.1:4122
                        FMT Service
                              │
                              ↓
                        业务层 → Storage → 文件系统
```

**唯一写入者是 Service。** 这样不存在 CLI 与 Service 两个进程同时改 `data/*.json`
的并发问题。

### 11.2 命令通道

| 命令 | 通道 | 原因 |
|---|---|---|
| `service install/start/stop/delete` | **直连 SCM API** | Service 可能尚未安装/启动，走 HTTP 会引导死锁 |
| `--help` / `--version` | 本地 | 不依赖 Service |
| `bucket *` / `file *` / `share *` / `trash *` | **HTTP → Service** | 业务操作，必须由 Service 执行 |

### 11.3 命令结构

```text
fmt.exe
├── --help
├── --version
├── bucket  create <name> | list | get <name> | use <name> | delete <name>
├── file    upload <url> | list | get <file_id> | get <filename> | delete <file_id>
├── share   create <file_id> | get <share_id> | list <file_id> | delete <share_id>
├── trash   list | get <id> | restore <id> | delete <id>
└── --service  install | start | stop | delete
```

### 11.4 命令帮助

```text
fmt.exe --help
fmt.exe bucket --help
fmt.exe file --help
fmt.exe share --help
fmt.exe trash --help
fmt.exe --service --help
```

帮助内容必须与实际命令一致。

### 11.5 Service 未运行时的行为

业务命令连不上 Service 时：

```text
无法连接 FMT Service（127.0.0.1:4122）
请先执行：fmt.exe --service install
```

引导命令直连 SCM，因此用户始终有办法把 Service 起起来，不会死锁。

### 11.6 输出约定

```text
正常结果 → stdout
错误信息 → stderr
```

CLI 输出保持简洁、明确、用户可读。**CLI 不写日志文件**，
所有调试与运行记录由 Service 写入 `log/`。

未来可增加 `--json` 结构化输出。

### 11.7 错误原则

| 情况 | 处理 |
|---|---|
| 未知命令 | 报错 + 提示帮助 + 非 0 退出码 |
| 缺少参数 | 报错 + 提示正确用法 + 非 0 退出码 |
| 未知参数 | 拒绝执行 + 非 0 退出码 |
| Service 未运行 | 提示先安装/启动 Service + 非 0 退出码 |

**不忽略未知参数。**

### 11.8 退出码

| 码 | 含义 | 码 | 含义 |
|---|---|---|---|
| 0 | 成功 | 5 | 权限/访问错误 |
| 1 | 通用错误 | 6 | 数据一致性错误 |
| 2 | 参数错误 | 7 | 配置错误 |
| 3 | 对象不存在 | 8 | Service 错误 |
| 4 | 冲突 | | |

脚本应依赖退出码与稳定的错误码，而不是提示文本。

### 11.9 交互模式

双击 `fmt.exe` 时进入交互式循环，便于日常使用：

```text
Running...
fmt> file list
...
fmt> service stop
```

- 命令集与命令行参数形式一致
- 空行忽略
- `exit` / `quit` 退出
- 命令失败时输出错误并**继续**循环
- **同时只允许一个 CLI 窗口**

### 11.10 首次运行

```text
current_user 为空  → 引导用户设置当前用户
current_bucket 为空 → 执行 file upload / file list 时提示先创建或选择 Bucket
```

用户系统正式开发后，替换为正式登录机制。

---

## 12. HTTP Server

### 12.1 实现方式

使用 **cpp-httplib**（`third_party/cpp-httplib/httplib.h`），它同时提供
`httplib::Server` 与 `httplib::Client`，服务端与客户端共用一套代码，无需自研 HTTP 栈。

```text
Service 侧：httplib::Server    监听 127.0.0.1:4122，注册路由
CLI 侧    ：httplib::Client    连接 127.0.0.1:4122，发送请求
```

自己只写两薄层封装：

```text
src/common/net/
├── http_client.hpp/.cpp    对 httplib::Client 的封装（超时、JSON 收发）
└── api.hpp                 API 路径常量与请求/响应结构体
```

注意：cpp-httplib 处理请求时会为每个连接起线程，因此业务层的锁策略（第 15 节）
必须成立。Winsock 的初始化由 httplib 内部完成，无需手动 `WSAStartup`。

### 12.2 启动

```text
读取 server.json → enabled?
    ├─ false → 不启动 HTTP
    └─ true  → server.listen(host, port)
```

默认 `127.0.0.1:4122`。绑定失败（端口占用）→ 记录 ERROR 日志，
Service 模式下不中断其他功能。

Service 启动顺序：先初始化业务与存储，再启动 HTTP，避免请求到达时数据层未就绪。

### 12.3 路由

分两组：**浏览器直接访问的下载/预览路由**，和 **CLI 使用的业务 API**。

#### 12.3.1 浏览器路由

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/file/download/<文件名>` | 下载，流式传输 |
| GET | `/file/preview/<文件名>` | 浏览器内预览 |
| GET | `/share/download/<share_id>` | 通过分享下载，**成功后计数** |
| GET | `/share/preview/<share_id>` | 通过分享预览 |

#### 12.3.2 CLI 业务 API

CLI 的全部业务命令都通过这些接口执行。请求与响应体均为 JSON。

| 方法 | 路径 | 对应命令 |
|---|---|---|
| GET | `/api/bucket` | `bucket list` |
| POST | `/api/bucket` | `bucket create <name>` |
| GET | `/api/bucket/<name>` | `bucket get <name>` |
| POST | `/api/bucket/<name>/use` | `bucket use <name>` |
| DELETE | `/api/bucket/<name>` | `bucket delete <name>` |
| GET | `/api/file` | `file list` |
| POST | `/api/file` | `file upload <url>` |
| GET | `/api/file/<id_or_name>` | `file get <file_id>` / `file get <filename>` |
| DELETE | `/api/file/<file_id>` | `file delete <file_id>` |
| GET | `/api/config` | （供 CLI 读取 current_user / current_bucket） |
| PUT | `/api/config` | （设置 current_user / current_bucket） |
| GET | `/api/share` | `share list <file_id>` |
| POST | `/api/share` | `share create <file_id>` |
| GET | `/api/share/<share_id>` | `share get <share_id>` |
| DELETE | `/api/share/<share_id>` | `share delete <share_id>` |
| GET | `/api/trash` | `trash list` |
| GET | `/api/trash/<id>` | `trash get <id>` |
| POST | `/api/trash/<id>/restore` | `trash restore <id>` |
| DELETE | `/api/trash/<id>` | `trash delete <id>`（永久删除） |

**上传语义（已冻结）：CLI 传路径或 URL，不传文件内容。**

```json
POST /api/file
{ "path": "D:/test/a.txt", "file_name": "a.txt" }
```

```json
POST /api/file
{ "url": "https://example.com/a.zip", "file_name": "a.zip" }
```

Service 依据字段判断来源：`path` 为本地文件（Service 直接读盘），`url` 为网络地址
（Service 下载）。**V1 不让文件内容经过 HTTP**，避免无谓的数据搬运。

> 完整请求/响应字段在实现阶段随各命令一同确定。本表的路径形式为设计约定，
> 实现前可微调，但**「CLI 是 HTTP 客户端、Service 是唯一业务执行者」这一结构不变**。

Server **不直接修改** `file.json` / `repository` / `trash`，全部经 Service 层。

### 12.4 预览

预览只属于 HTTP，CLI 不实现。

| 扩展名 | Content-Type |
|---|---|
| `.png` | `image/png` |
| `.jpg` / `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |
| `.webp` | `image/webp` |
| `.bmp` | `image/bmp` |

流程：

```text
检查 File → 检查 file_type / extension → 是否支持
    ├─ 支持 → 返回内容
    └─ 不支持 → 返回明确错误，不返回乱码
```

V1 只实现图片，其他类型待后续扩展。

### 12.5 状态码

| 码 | 用途 |
|---|---|
| 200 | 成功并带响应体 |
| 204 | 成功但无响应体（**不得带 body**） |
| 400 | 参数错误 |
| 403 | 分享过期 / 次数耗尽 / 文件不可用 |
| 404 | 资源不存在 |
| 405 | 非 GET 方法 |
| 409 | 资源冲突 |
| 500 | 内部错误 |

删除类操作无返回内容时用 204，且必须保证 204 **不带响应体**。

### 12.6 下载与流式传输

```text
查询 File → 检查 is_trash → 检查实际文件 → 检查 metadata
   → 检查 size → 必要时校验 MD5 → 打开文件 → 缓冲区流式传输 → 完整完成
```

- 大文件**不得**一次性读入内存，用固定大小缓冲区（如 64 KB）循环发送
- 传输中断视为失败，不计数
- 只有**完整成功**才 `download_count + 1`

---

## 13. Windows Service

### 13.1 实现方式

Windows SCM API（`advapi32`）：

```text
OpenSCManager → CreateService / OpenService
   → StartService / ControlService / DeleteService
ServiceMain → RegisterServiceCtrlHandlerEx → SetServiceStatus
```

`fmt.exe` 承担两种角色，但**不是同时运行两个不同程序**：

```text
普通命令：fmt.exe file list      → CLI Client（HTTP 客户端）
后台服务：fmt.exe --service       → Service Server（HTTP 服务端）
```

由启动方式决定模式：被 SCM 启动时进入 Service 模式；用户直接运行时进入 CLI 模式。

**`service install/start/stop/delete` 直连 SCM API，不走 HTTP**——Service 可能尚未
安装，走 HTTP 会引导死锁。其余命令一律走 HTTP。

### 13.2 安装

```text
检查管理员权限 → 检查 Service 是否已存在
    ├─ 不存在 → 创建 → 设置自动启动 → 配置 Recovery → 启动
    └─ 已存在 → 不重复创建
```

Service 名称：`FMT`。显示名：`FMT File Management Service`。
启动类型：自动（`SERVICE_AUTO_START`）。
运行账户：`LocalSystem`（需读写 `FMT_ROOT` 下的数据目录）。

### 13.3 启动 / 停止 / 删除

| 命令 | 要求 |
|---|---|
| `service start` | 存在则启动；不存在报错 |
| `service stop` | 停止 HTTP → 等待关键操作完成 → 退出 Service |
| `service delete` | 要求 Service 已停止；**不得删除** `repository`、`trash`、`data`、`config`、`log` |

停止时使用 `SERVICE_CONTROL_STOP`，`ServiceMain` 收到后：
停止接受新请求 → 等待进行中的关键操作（上传/删除事务）完成 → `SetServiceStatus(STOPPED)`。

### 13.4 异常恢复

不实现自建 watchdog，使用 Windows Service Recovery：

```text
服务异常退出 → Windows 检测 → 重新启动 fmt.exe
```

配置：

```text
第一次失败：5 秒后重启
第二次失败：10 秒后重启
后续失败  ：30 秒后重启
失败计数重置：1 天后
```

### 13.5 双击行为

```text
第一次双击：检查 Service → 不存在 → 请求管理员权限 → 安装 → 启动
再次双击  ：检查 Service → 存在 → 不重复安装 → 检查状态 → 必要时启动
```

安装成功后进入交互式 CLI，不阻塞在服务进程里。

---

## 14. 日志

### 14.1 级别

```cpp
enum class LogLevel { Info, Warn, Error };   // V1 不设 DEBUG
```

| 级别 | 写入日志文件 | 控制台 |
|---|---|---|
| INFO | ✅ | 视 CLI 场景 |
| WARN | ✅ | ✅ |
| ERROR | ✅ | ✅ |

V1 **不实现**异步日志、日志线程、日志队列、日志压缩、日志服务器、ELK 或复杂配置。
将来若出现大量并发网络请求，再升级为「业务线程 → 日志队列 → 日志线程 → 文件」。

### 14.2 输出位置

```text
FMT_ROOT/log/
├── fmt.log        全部日志
└── error.log      仅 ERROR 级
```

日志**不是业务数据**，因此与 `data/` 分离。

**只有 Service 写日志文件。** CLI 不写 `fmt.log`，只输出到控制台——避免两个进程
争抢同一个日志文件。

### 14.3 格式

```text
时间 [级别] [模块] 消息
```

示例：

```text
2026-10-05 23:40:01 [INFO] [Main] FMT 启动
2026-10-05 23:40:01 [INFO] [Config] 加载 config.json
2026-10-05 23:40:02 [WARN] [Config] 配置文件不存在，使用默认配置
2026-10-05 23:40:03 [INFO] [File] 文件上传成功: file_id=fmt-20261005-0
2026-10-05 23:40:06 [ERROR] [File] 文件删除失败: example.txt
```

| 字段 | 说明 |
|---|---|
| 时间 | `YYYY-MM-DD HH:MM:SS`，本地时间 |
| 级别 | INFO / WARN / ERROR |
| 模块 | 短名：`Main`、`Config`、`File`、`Share`、`Trash`、`Bucket`、`Storage`、`Http` |
| 消息 | 内容，可含补充数据 |

ERROR 级同时写入 `fmt.log` 与 `error.log`。

### 14.4 接口

```cpp
class Logger {
public:
    static Result<Logger> open(const std::filesystem::path& log_dir);

    void info (std::string_view module, std::string_view message);
    void warn (std::string_view module, std::string_view message);
    void error(std::string_view module, std::string_view message);

    // 错误码形式，自动附带 FMT-NNN
    void error(std::string_view module, const Error& error);
};
```

要求：

- 线程安全（内部 mutex）——HTTP 请求可能来自多个线程
- 追加写入，open 时创建文件（不存在则建）
- 打开失败不得导致程序崩溃，降级为仅控制台输出
- **不得污染正常 CLI 输出**

### 14.5 记录内容

```text
服务启动 / 停止
上传（含 file_id、大小、来源）
删除 / 恢复 / 永久删除
异常与失败原因
HTTP 请求（方法、路径、状态码）
数据一致性问题
```

---

## 15. 并发与事务

### 15.1 线程模型

**只有 Service 一个进程写数据。** CLI 不写任何业务数据，因此不存在跨进程并发。

Service 内部：

```text
主线程              ServiceMain / SCM 回调
HTTP 处理线程        cpp-httplib 为每个连接起一个线程
```

因此并发全部来自 HTTP 请求，锁必须是**进程内**的。

### 15.2 必须保护的数据

```text
file_id 分配
JSON 写入（file / share / trash / user / config）
File metadata
Share download_count
current_bucket
文件移动
```

### 15.3 锁策略

```text
每个 JSON 文件一把 std::mutex：
    file.json  → file_mutex
    share.json → share_mutex
    trash.json → trash_mutex
    配置       → config_mutex
file_id 分配   → id_mutex
```

要求：

- 「读取 → 修改 → 写入」全过程持锁，不能只锁写入
- 不得同时持多把锁造成死锁；若必须，固定获取顺序（file → share → trash）
- 锁粒度以「一个操作」为单位，不跨用户交互持锁
- 锁只在 Service 进程内，**不需要跨进程文件锁**（CLI 是 HTTP 客户端，不碰文件）

### 15.4 一致性检查

```cpp
class ConsistencyChecker {
public:
    struct Issue {
        std::string file_id;
        enum class Kind {
            MetadataWithoutFile,
            FileWithoutMetadata,
            SizeMismatch,
            Md5Mismatch,
        } kind;
        std::string detail;
    };

    Result<std::vector<Issue>> check();
};
```

检查四种异常：

```text
metadata 存在，实际文件不存在
实际文件存在，metadata 不存在
metadata.size != 实际 size
metadata.md5  != 实际 md5
```

发现异常：

```text
记录错误 → 停止危险操作
```

**不自动删除文件、不自动覆盖 metadata、不自动重新生成 metadata。**

### 15.5 文件移动

- 优先**同文件系统内移动**（`MoveFileEx` 同卷），减少复制+删除的失败窗口
- 跨盘时：复制 → 校验（比对 size 与 MD5）→ 删除原文件，并做一致性保护

---

## 16. 错误处理

### 16.1 结构

```cpp
enum class ErrorCode { /* FMT-001 ... 见架构文档附录 A */ };

struct Error {
    ErrorCode   code;
    std::string message;
};

template <typename T>
using Result = std::variant<T, Error>;

using Status = std::variant<std::monostate, Error>;
```

**成功值与错误值互斥**，不使用 `bool success; T value; Error error;` 这种允许误读的结构。

### 16.2 规则

1. **错误码稳定，错误消息可修改**——`FMT-002` 的文案变化不改变其含义
2. **编号一旦发布不复用、不修改语义**，新增错误只能追加新编号
3. 业务代码不直接写字符串错误码，由统一转换层生成
4. CLI、日志、测试、HTTP 都依赖错误码，不依赖提示文本

错误码清单见 `FMT 项目架构.md` 附录 A。

### 16.3 异常兜底

`main.cpp` 捕获所有异常：

```cpp
int main(int argc, char* argv[]) {
    try {
        const fmt::core::App app;
        return app.run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        return static_cast<int>(fmt::core::ExitCode::GeneralError);
    } catch (...) {
        std::cerr << "Fatal: unknown error\n";
        return static_cast<int>(fmt::core::ExitCode::GeneralError);
    }
}
```

`std::filesystem::filesystem_error` 必须转换成可理解的错误信息，
并**指出具体路径**，而不是只输出 `error`。

---

## 17. 验证方式

### 17.1 流程

**每完成一段可运行的链路**即做一次真实运行验证，不允许所有模块做完才验证。

```text
开发 → 构建出 fmt.exe → 起 --server → 真实 CLI 跨进程跑一遍 → 看磁盘上的实际结果
```

### 17.2 为什么不是自动化单元测试

早期有过一套 Catch2 单元测试（291 个用例，全绿）。它被移除，理由不是「测试无用」，
而是这份工程的取舍变了：

- **它测的不是主线。** 用例覆盖的是各模块的孤立行为，而真正出问题的地方
  （悬垂引用、`res.status` 默认值、日志自锁死）全在**跨模块的接缝**上，
  恰恰是单元测试覆盖不到的部分。
- **它阻碍亲手构建。** Catch2 需要联网拉取，构建目录里的 `_deps` 一旦损坏
  就会阻塞配置，而工程的目标是「clone 下来就能构建」。
- **调试期反复被 `/RTC1` 弹窗挂住**，时间成本远超收益。

取代它的是**端到端实测**：直接起服务、用真实 `fmt.exe` 发命令、然后去看
`data/*.json` 与 `repository/` 里到底落了什么。见 18.10 的实测记录。

> 若后续要补自动化测试，**优先补「上传 → 列表 → 下载 → 分享」这一条链路**，
> 而不是回头做各模块的孤立单元测试。

### 17.3 手工验证清单

起服务：

```powershell
.\cmake-build-debug\bin\fmt.exe --server
```

另一个终端里按顺序执行（括号内是应当看到的结果）：

```text
fmt.exe config get                                  （user/bucket 为空）
fmt.exe file upload --file <本机文件>                （自动初始化 user + Bucket）
fmt.exe file list                                   （刚上传的文件，中文名不乱码）
fmt.exe file get --id <file_id>                     （MD5、类型、大小）
fmt.exe share create --file <file_id>               （share_id + 可点链接）
fmt.exe share list                                  （0/20 有效）
浏览器打开下载链接                                    （HTTP 200，字节数与源文件一致）
fmt.exe share list                                  （1/20，计数只在完整写出后 +1）
fmt.exe config set --user <名字>                     （写回 config.json）
```

再检查磁盘：

```text
repository/<user>/<bucket>/YYYY/MM/DD/<文件名>       物理文件在位、内容一致
data/file.json  data/share.json                     合法 UTF-8 JSON，含 version 字段
config/config.json                                  current_user / current_bucket 已落盘
log/fmt.log                                         有对应的上传记录
```

### 17.4 判断「真的对了」的硬标准

1. **退出码。** 0 才算成功；失败必须是明确的错误码，不是「什么都没发生」。
2. **磁盘结果。** 不只看 CLI 打印，必须去 `repository/` 与 `data/` 里核对。
3. **中文。** 文件名、Bucket 名、用户名用中文跑一遍，检查磁盘上的目录名是否
   仍是中文（乱码目录同样是「目录」，`is_directory` 检查不出来）。
4. **服务不退出。** 一轮命令跑完，`--server` 进程必须还活着。

---

## 18. 开发阶段与状态

### 18.1 阶段划分

规范见 `FMT 开发文档.md` 第 96～106 节。因 CLI 改为 HTTP 客户端，**HTTP 与 Service
骨架提前到阶段 3**——否则先做 Bucket 再回来补 HTTP 通道会返工。

阶段 5～9 的实际做法与规范顺序不同：不按「先 File 元数据 → 再 Upload → 再 File
操作 → 再 Share」逐层收口，而是**先把「上传 → 列表 → 下载 → 分享」这条链路一次打通**，
中间层粗糙一点也先让它跑起来。这是明确的项目决策，原因见 18.8。

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 项目骨架：CMake、Ninja、MSVC、`fmt.exe`、`--help`、目录初始化 | ✅ 完成 |
| 2 | 基础层：第三方库、`common`、`storage`、`config`、`PathManager`、`log/` | ✅ 完成 |
| 3 | HTTP + Service + CLI 客户端：cpp-httplib 封装、业务 API、SCM 生命周期 | ✅ 完成 |
| 4 | Bucket：create / list / get / use / delete，`current_bucket` 逻辑 | ✅ 完成 |
| 5 | File 基础：`file.json`、`file_id`、文件名、extension、file_type、size、md5、`is_trash` | ✅ 完成 |
| 6 | Upload：本机路径 + URL、临时文件、大小限制、MD5、文件名冲突 | ✅ 完成 |
| 7 | File 操作：list / get / download link / share link | ✅ 完成 |
| 8 | Share：create / get / list / delete，下载计数、过期、有效性 | ✅ 完成 |
| 9 | Trash：list / get / restore / delete，含 Bucket 部分恢复 | ⏳ 未实现（返回「本版本尚未实现」） |
| 10 | Service Recovery + 自动启动完善 | ⏳ 未实现 |
| 11 | Preview：图片预览 | ⏳ 返回 FMT-701 |
| 12 | `file delete`：标记 `is_trash` + 移入 Trash | ⏳ 返回「本版本尚未实现」 |

### 18.2 当前实现状态

**阶段 1 至阶段 8 的主链路已打通并端到端实测。**
不再维护 Catch2 单元测试套件（见 18.8），验证方式改为**真实 `fmt.exe` 跨进程实测**。

阶段 5～8（File + Upload + Share）实测记录见 18.10。


阶段 1：

| 已实现 | 位置 |
|---|---|
| 程序入口与异常兜底 | `src/main.cpp` |
| 运行目录计算（`FMT_ROOT`） | `src/core/path.cpp` |
| `--help` / `--version` / 未知参数 | `src/cli/cli.cpp` |

阶段 2：

| 模块 | 已实现 | 位置 |
|---|---|---|
| 第三方库 | nlohmann/json 3.11.3、cpp-httplib 0.18.3（vendored）；静态链接 MSVC 运行库 | `third_party/`、根 `CMakeLists.txt` |
| `common/error` | `ErrorCode`（44 条）、`Error`、`Result<T>`、`Status`、`ExitCode`；`error_code_from_string` 支持跨进程还原 | `include/fmt/common/error.hpp` |
| `common/time` | 日志/ISO/日期/路径/日键 五种格式，严格解析，`is_expired` | `include/fmt/common/time.hpp` |
| `common/string` | `trim`、大小写、前后缀、`split`/`split_trimmed`、`join`、`replace_all` | `include/fmt/common/string.hpp` |
| `common/size` | 自动/固定单位的大小显示，`size_unit` 解析 | `include/fmt/common/size.hpp` |
| `common/logger` | `log/fmt.log` + `error.log`，三级，`[模块]`，线程安全 | `include/fmt/common/logger.hpp` |
| `common/validation` | 文件名与 URL 校验（含保留设备名、路径穿越） | `include/fmt/common/validation.hpp` |
| `storage` | 目录/文件操作，JSON 原子读写（`MoveFileEx` 替换），版本校验 | `include/fmt/storage/storage.hpp` |
| `config` | `config.json` / `server.json` 加载、校验、保存 | `include/fmt/config/config.hpp` |
| `core/PathManager` | 所有路径的唯一出口 | `include/fmt/core/path_manager.hpp` |
| `core/App` | 启动链路：目录 → 默认配置 → `server.json` → 日志 | `src/core/app.cpp` |

阶段 3：

| 模块 | 已实现 | 位置 |
|---|---|---|
| `common/api` | 响应信封：`{"ok":true,"data":…}` / `{"ok":false,"error":{"code":"FMT-002",…}}` | `include/fmt/common/api.hpp` |
| `common/net` | HTTP 客户端（`api_get`/`api_post`/`api_delete`）、URL 编码 `url_encode`/`fill_path_param`、错误码映射 | `include/fmt/common/net.hpp` |
| `api` | CLI 与 Service 的共同契约：Bucket/File/Share/Trash/Config 结构 + 路由常量 + JSON 编解码 | `include/fmt/api/api.hpp` |
| `server` | HTTP 服务端：路由注册、信封编码、`status_for_error` 映射、`make_default_handlers` 业务装配点 | `include/fmt/server/server.hpp` |
| `service` | SCM 生命周期：install（自动启动 + Recovery）/ start / stop / delete / status，绕过 HTTP | `include/fmt/service/service.hpp` |
| `cli` | 命令解析（纯函数）+ HTTP 客户端执行；`service` 命令直连 SCM | `include/fmt/cli/cli.hpp` |

实测：`fmt.exe` 依赖为 `ADVAPI32.dll`、`WS2_32.dll`、`KERNEL32.dll`、`SHELL32.dll`（全部为系统组件），**无第三方 DLL**。

阶段 3 端到端实测（真实 `fmt.exe` ↔ 真实 HTTP 服务，跨进程）：

```text
bucket list                      → 列出两个 Bucket，中文正常，退出码 0
bucket get 工作                  → 名称/文件数/占用，中文路径参数编码正确，退出码 0
bucket get 不存在                → "Bucket 不存在"，退出码 3（NotFound）
bucket create --name 新桶        → 创建成功，退出码 0（中文参数经 UTF-8 传递）
bucket create --name 已存在      → "Bucket 已存在"，退出码 4（Conflict）
file download --name 报告.txt    → 链接中中文编码为 %E6%8A%A5%E5%91%8A.txt
Service 未运行时                  → "无法连接 FMT Service" + 启动指引，退出码 8
service status                   → "Service 状态：未安装"，退出码 0
service install（非管理员）       → 权限提示，退出码 5
```


端到端验证（真实运行 `fmt.exe`）：

```text
首次运行 → 创建 repository/ trash/ config/ data/ log/
         → 写出 config/config.json（缺失时）
         → 退出码 0
修改配置后再运行 → 用户修改保持不变
数据目录探针文件 → 多次运行后依然存在
配置损坏时运行   → 退出码 7，原文件字节未变
```

其余模块（`trash`、`preview`）**尚无实现**，handler 返回「本版本尚未实现」。

### 18.3 阶段 2 完成明细

```text
2.1  引入 nlohmann/json + cpp-httplib，验证单 exe        ✅
2.2  common/error      ErrorCode、Error、Result、Status  ✅
2.3  common/time       时间格式化与解析                   ✅
2.4  common/string + common/size                         ✅
2.5  common/logger     log/fmt.log + error.log           ✅
2.6  common/validation 文件名与 URL 校验                  ✅
2.7  storage           文件操作 + JSON 原子读写            ✅
2.8  config            配置加载、校验、保存                ✅
2.9  core/PathManager  路径唯一出口 + App::initialize()   ✅
```

### 18.4 阶段 3 完成明细

```text
3.1  common/net    HTTP 客户端 + 错误码往返（error_code_from_string）  ✅
3.2  common/api + api   响应信封 + CLI/Service 共同契约（结构 + 路由）  ✅
3.3  server        路由注册、信封编码、状态码映射、业务装配点          ✅
3.4  service       SCM 生命周期：install/start/stop/delete/status      ✅
3.5  cli           命令解析（纯函数）+ HTTP 客户端执行                  ✅
```

每步配套单元测试，测试名用 ASCII（见第 17.3 节）。

### 18.5 阶段 3 踩到的坑（已修，记录以免重犯）

| 现象 | 根因 | 处理 |
|---|---|---|
| `bucket create --name 新桶` 抛 `invalid UTF-8 byte 0xC2` | Windows 的 `argv` 是控制台代码页（GBK），而 JSON 要求 UTF-8 | `paths::utf8_arguments()` 用 `GetCommandLineW` 转换后交给命令层 |
| `bucket get 工作` 返回 HTTP 500 | 路径参数直接拼进 URL，中文原始字节让服务端路径匹配失败 | `net::fill_path_param` 做百分号编码，CLI 全部 10 处改用它 |
| 测试挂死无输出 | 夹具先 `join()` 再 `stop()`，而 `listen()` 阻塞到 `stop()` | 析构里先 `stop()` 再 `join()` |
| 参数化路由全部 404 | cpp-httplib 用 `std::regex_match` 全匹配，`/api/bucket/` 只能匹配自身 | 改用 `:name` 语法 + `req.path_params` |
| 先探测空闲端口再绑定后客户端连不上 | Windows 上释放与重绑之间端口可能被占用，而 `is_running()` 仍为 true | 改为一次 `bind_any_port()`，之后 `register_routes()` |
| 测试里 `AppContext` 析构 SIGSEGV | MSVC `/RTC1` 的 `_RTC_CheckStackVars` 与隐式生成析构函数组合下误报 | `AppContext` 显式声明并在 `.cpp` 定义构造/析构/移动 |
| 配置阶段 `Unknown CMake command FetchContent_Declare` | `include(FetchContent)` 被前一次编辑粘到注释行末尾 | 恢复为独立行 |

### 18.6 阶段 4 完成明细（Bucket）

```text
4.1  common/text   UTF-8 ↔ fs::path 转换（path_from_utf8 / path_to_utf8 / join）  ✅
4.2  bucket        名称校验、存在性、创建、列表、切换、删除（移入 Trash）        ✅
4.3  server        把 bucket handler 接入 make_default_handlers                   ✅
4.4  main          `--service run` 与 `--server` 进入 HTTP 服务主循环            ✅
4.5  端到端        真实 CLI ↔ 真实 Service ↔ 磁盘                                  ✅
```

Bucket 就是 `repository/<user>/<bucket>` 目录，**没有**独立的 `bucket.json`；
文件数与占用由扫描目录得出，因此不会与磁盘实际内容不一致。

新增错误码 `FMT-604 NoCurrentUser`（追加在末尾，已有编号语义不变）。

阶段 4 端到端实测（真实 `fmt.exe --server` + 真实 CLI，跨进程）：

```text
bucket list                     → 没有 Bucket
bucket create --name 工作       → 已创建；第一个 Bucket 自动成为当前（第 28 节）
bucket create --name 学习       → 已创建
bucket list                     → 学习 / 工作，工作标 [当前]
bucket get 工作                 → 名称/文件数/占用
bucket use --name 学习          → 切换成功，config.json 落盘
bucket delete 学习（当前）      → 数据移入 trash/小谷/学习，current_bucket 清空
                                  且不自动改选（第 30 节）
磁盘校验                        → trash/小谷/学习/笔记.txt 内容完好
```

### 18.7 阶段 4 踩到的坑

| 现象 | 根因 | 处理 |
|---|---|---|
| 中文用户名让 `path / "小谷"` 抛 `No mapping for the Unicode character` | MSVC 的 `fs::path` 从 `std::string` 构造按 **ANSI 代码页**解释，不是 UTF-8 | `common/text` 统一两个方向的转换；`PathManager` 所有用户输入片段都经 `path_from_utf8` |
| 删除 Bucket 后 Trash 里出现乱码目录 `宸ヤ綔`，看起来数据丢了 | `user_trash(user) / std::string{name}` 同一陷阱，漏改 | 新增 `text::join`；`bucket::remove` 改用它 |
| 上面这个 bug 单元测试**没抓到** | 测试只断言 `is_directory(目标)`，而乱码目录同样是目录 | 回归测试改为**读取磁盘上真实目录名**并比对；夹具的期望值也改走 `join` |
| 测试期望值与实现不一致（`[灏忚胺]` vs `[小谷]`） | 测试自己也用 `path / "小谷"` 构造期望值 | 测试期望值同样经 `path_from_utf8` |
| Release 下两个 server 测试失败、Debug 通过 | 插入新错误码后枚举值移位，而两套构建的产物来自不同版本头文件 | 重新完整构建；并记录「不要依赖枚举整数编号」 |
| 删除运行时 `--server` 无法验证 | 之前 `make_default_handlers` 从未被调用，服务主循环是空的 | 接入 `--service run` / `--server` 主循环 |

### 18.8 阶段 5～8 的做法调整（重要）

**改动：不再按「逐层收口」推进，改为先把一条完整链路打通。**

原来的打算是阶段 5 只做 File 元数据，阶段 6 只做 Upload，阶段 7 才做文件操作，
阶段 8 才做 Share——每层都写完整的单元测试再进下一层。实际执行下来的问题是：
做到阶段 6 时用户侧仍然无法完成任何一件完整的事（上传完看不到列表、能看到列表
但拿不到文件），而「上传 → 列表 → 下载 → 分享」这条链路才是需求主线。

因此阶段 5～8 合并为一次实现，验收标准改为：

> **能用 `fmt.exe` 完整走通「上传一个文件 → 列表 → 下载 → 分享给对方」，
> 中间层粗糙可以接受。**

同时做了三处范围缩减，都是明确决策而非遗漏：

| 缩减 | 内容 | 理由 |
|---|---|---|
| 删除测试套件 | 移除 `tests/`、`BUILD_TESTING`、CTest、Catch2 依赖、CI workflow | 项目要**方便自己亲手构建**；Catch2 需联网拉取，且调试期反复挂在 `/RTC1` 弹窗上，收益远小于时间成本。验证改为真实跨进程实测 |
| 删除空骨架模块 | 移除 `src/share/`、`src/trash/`（都只有 CMakeLists 没有源文件） | Share 实现在 `src/file/service.cpp` 内；Trash 未实现，不需要空目录占位 |
| 删除 CI | 移除 `.github/workflows/ci.yml` | 它唯一的作用是跑测试；测试已删，且工程约束是仅 Windows + 本机 MSVC |

**仍然保留的工程纪律**（这些不是过度设计，是血泪教训换来的）：

- `common/text` 的 UTF-8 ↔ `fs::path` 转换：不遵守会静默写出乱码目录名；
- `PathManager` 作为路径唯一出口：不遵守会绕过 `text` 转换；
- 只有 Service 写业务数据：CLI 全程走 HTTP。

### 18.9 阶段 5～8 完成明细

```text
5.1  common/md5      RFC 1321 紧凑实现，md5_hex / Md5Stream / md5_file       ✅
5.2  file::Service   Record / Share / ResolvedFile，file.json + share.json    ✅
                     file_id（fmt-YYYYMMDD-N）、share_id（16 位随机十六进制） ✅
                     extension / file_type 判定、MD5、大小上限               ✅
5.3  file 上传       本机路径（Service 直接读）+ URL（httplib client 下载）   ✅
                     同名冲突：同一用户内唯一，Bucket 不构成命名空间         ✅
5.4  file 列表/查询  list / find / resolve                                    ✅
5.5  file 下载       浏览器直连 `/file/download/:name`，RFC 5987 文件名      ✅
5.6  share            create / get / list / delete                            ✅
                     下载计数在完整写出后 +1，达上限置 is_valid=false        ✅
                     浏览器直连 `/share/download/:share_id`                   ✅
5.7  config          新增 `fmt config get` / `config set --user/--bucket`     ✅
5.8  server wiring   file_* / share_* / config_* handler 接入装配点           ✅
5.9  首次使用体验    `current_user` / `current_bucket` 为空时自动落到        ✅
                     `default` 与默认 Bucket「工作」并写回 config.json
```

### 18.10 阶段 5～8 端到端实测

真实 `fmt.exe --server` + 真实 CLI，跨进程；**从零开始**（先删掉
`config/config.json`、`data/*.json`、`repository/`）。

```text
1) config get                      → 当前用户/当前 Bucket 均为空
2) file upload --file .smoke-中文名.txt
                                   → 自动初始化：用户=default，Bucket=工作
                                   → 上传完成：.smoke-中文名.txt
                                     file_id=fmt-20261007-0
3) file list                       → fmt-20261007-0  .smoke-中文名.txt  text
4) share create --file fmt-20261007-0
                                   → share_id=64c17f38464b567e
                                     链接 http://127.0.0.1:4122/share/download/64c17f38464b567e
5) 浏览器直连分享下载               → HTTP 200，27 字节，内容与源文件逐字节一致
6) share list                      → 64c17f38464b567e  .smoke-中文名.txt  1/20  有效
                                     （下载计数只在完整写出后 +1，符合第 45 节）
7) file download --name .smoke-中文名.txt
                                   → /file/download/.smoke-%E4%B8%AD%E6%96%87%E5%90%8D.txt
8) config get                      → 当前用户=default，当前 Bucket=工作（已写回）
9) bucket list                     → 工作  1 个文件
磁盘校验                            → repository/default/工作/2026/10/07/.smoke-中文名.txt
                                     data/file.json、data/share.json 均为合法 UTF-8 JSON
```

### 18.11 阶段 5～8 踩到的坑

这一阶段的坑集中在**「崩溃点与真因无关」**，记录在此以免重犯。

| 现象 | 根因 | 处理 |
|---|---|---|
| 服务进程收到 `POST /api/file` 就静默退出，`/api/ping`、`/api/config` 正常 | `make_default_handlers` 里定义了局部 lambda `current_user`，handler **按引用**捕获它；函数返回后 lambda 已析构，调用处直接访问违例——**崩溃点落在函数体之前，连日志都打不出来** | 把「取当前用户/当前 Bucket」的规则上移到 `AppContext::resolve_user()` / `resolve_bucket()`，handler 只调用成员函数 |
| `file upload` 上传后写日志时崩溃，`logger.is_open()` 却是 true | `AppContext` 里 `Logger` 是**值成员**，`context.logger = std::move(logger).value()` 触发移动赋值；而 `file::Service` 保存的是 `Logger&`，指向被搬空的旧对象。旧对象的 `mutex` 正被移动赋值持有，于是写日志**自锁死**；Windows 上 `std::mutex` 自锁表现为访问违例而不是抛异常 | `Logger` 改为 `std::unique_ptr` 持有，地址稳定；`Service` 拿到的是堆上真实对象 |
| 同类问题：`AppContext` 被移动后 `Service` 行为异常 | `Service` 也保存 `const PathManager&`，`AppContext` 值成员被移动后引用悬垂 | `PathManager` 同样改为 `std::unique_ptr` 持有 |
| 分享下载成功但 `download_count` 永远是 0 | `send_file` 没给 `res.status` 赋值，httplib 的默认值是 **-1**，而计数判断写的是 `if (res.status == 200)` | `send_file` 里显式 `res.status = 200` |
| `share get` 打印的链接是 `.../share/download/:share_id64c1...` | 直接把路由模板和 id 字符串相加，没有替换 `:share_id` 占位符 | 改用 `net::fill_path_param`，并补上 `share create` 的输出链接 |
| 调试期所有崩溃都表现为「栈变量损坏」弹窗或 `0xC0000005` | MSVC Debug 默认的 `/RTC1` 会干扰上述悬垂引用类问题，且把真因盖住 | 显式设 `CMAKE_CXX_FLAGS_DEBUG=/Zi /Ob0 /Od`，去掉 `/RTC1` |
| `/api/bucket` 一请求服务就退出，`/api/config` 却正常 | 两个 handler 走的是不同代码路径，前者调用了按引用捕获的局部 lambda | 同上第 1 条 |
| `nlohmann::json` 编译报 `get<Record>` 找不到重载 | `to_json` / `from_json` 定义在**匿名命名空间**里，ADL 找不到，只有本翻译单元的普通查找能找到 | 移到 `fmt::file` 命名空间并在头文件声明 |
| `file::Service` 编译报 `recursive_directory_iterator != path` | 拿 `fs::path` 当 `end()` 哨兵用了 | 用 `fs::recursive_directory_iterator end;` 作哨兵，并保留 `increment(ec)` 的错误处理 |

### 18.12 阶段 5～8 之后仍未做的事

| 事项 | 现状 |
|---|---|
| `file delete` | CLI 与路由都在，handler 返回「本版本尚未实现」。落地方案已定：标记 `is_trash=true` + 把物理文件移入 `trash/<user>/<bucket>/YYYY/MM/DD/` |
| Bucket 删除的元数据标记 | `bucket::remove` 只做了目录搬迁。第 30 节要求同时把该 Bucket 下所有文件的 `is_trash` 置 true——等 `file delete` 落地时一并做，两处必须同一套逻辑 |
| Trash 全部命令 | `trash list/get/restore/delete` 均未实现 |
| Preview | `file preview` / `share preview` 返回 FMT-701 |
| `common/md5` 的测试覆盖 | 无自动化测试；实测与 `md5sum` 对比一致（上传后 `file.json` 里的 md5 与外部工具一致） |

---

## 19. 待决事项

| 事项 | 说明 |
|---|---|
| HTTP API 字段细节 | 路由已在第 12.3 节与 `include/fmt/api/api.hpp` 固化；请求体的完整字段随各命令实现时确定 |
| 业务处理函数装配 | 业务模块在 `server::make_default_handlers(AppContext&)` 注册；已接入 bucket / file / share / config，trash / preview 返回「本版本尚未实现」 |
| 首次使用的用户与 Bucket | 开发文档第 93/94 节要求「提示用户先设置」。当前实现改为**自动落到 `default` 与 Bucket「工作」**并写回 `config.json`，否则用户第一条命令就是报错。这是有意放宽，若需回到严格模式，改 `AppContext::resolve_user` / `resolve_bucket` 两处即可 |
| `file delete` 与 Bucket 删除的元数据标记 | 见 18.12；两处必须共用同一套「移入 Trash + 置 `is_trash`」逻辑 |
| 大小显示 | `size_unit` 默认 `MB`，于是 27 字节显示成 `0MB`。是否改成默认 `AUTO` 待定（`common/size` 已支持 `AUTO`） |
| 日志轮转 | V1 不做轮转；单文件上限与轮转规则留待后续 |
| Bucket Trash 元数据结构 | 开发文档标注为暂定，Bucket 级 Trash 记录的字段需细化 |
| 最大上传大小默认值 | 现取 50 MB，硬编码在 `file/service.cpp` 的 `kMaxUploadSize`，尚未接到 `config.max_upload_size` |
| HTTPS | 需要 OpenSSL，会引入 DLL，V1 不支持；`file upload <https://...>` 返回 `UrlInvalid` |
| 控制台交互输入 | 输出已用 UTF-8 + `SetConsoleOutputCP`，参数已转 UTF-8；交互式输入（未来若需要）方案待冻结 |
| Service 特权路径未实测 | install/start/stop/delete 需要管理员权限，当前环境非提权，只验证到权限检查与只读查询；需一次提权手工验证 |
| 旧 `paths::` 自由函数 | `src/core/path.cpp` 仍保留 `executable_path` / `root` 等自由函数供启动阶段使用，与 `PathManager` 并存；后续可考虑收敛，但 `root()` 必须保留（它要先于 PathManager 存在） |
| `text::join` 的使用纪律 | 凡是用用户输入拼接路径，必须经 `common/text`。这条纪律目前靠代码审查保证，没有编译期强制 |
| `AppContext` 的指针持有纪律 | `paths` 与 `logger` 都必须 `unique_ptr` 持有，因为 `file::Service` 保存它们的引用。这条约束靠注释说明，没有编译期强制——改动 `AppContext` 成员时容易踩回去 |
| 自动化回归测试 | 当前没有。若后续要补，优先补「上传 → 列表 → 下载 → 分享」这一条链路，而不是各模块的孤立单元测试 |

