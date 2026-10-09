# FMT 技术文档

> 项目：FMT（Windows 文件管理系统）
> 版本：V1
> 文档状态：**已按 2026-10 架构重构同步**（单一 `fmt.exe` 三形态、CLI 走命名管道、
> service **六条子命令**（`install` / `uninstall` / `start` / `stop` / **`reinstall`** / `status`；
> 除 `status` 外都提权，`reinstall` 提交 `c573f14` 起正式化）、数据根由 CLI 声明、
> Service 与 CLI 共用同一套幂等初始化规则）
> 分支：`arch-restart`｜平台：Windows x64｜语言：C++17｜构建：CMake + Ninja + MSVC
> 产物：`fmt.exe`（单文件，三种形态）
>
> **三份文档的分工：**
>
> | 文档 | 负责内容 |
> |---|---|
> | `FMT 开发文档.md` | 规范主体：模块职责、数据结构、命令、流程、测试要求 |
> | `FMT 项目架构.md` | 总体架构、冻结决策、错误码清单（附录 A） |
> | ~~`FMT 重构设计.md`~~ | **该文档已删除**：内容并入本文与另外两份文档——冻结决策见 `FMT 开发文档.md` 第 122 / 128 节，通道与阶段见本文 12.3、13 节与 `FMT 项目架构.md` 第 11 节 |
> | **本文档** | **技术实现**：技术栈、接口设计、实现方式、类与函数、协议细节、落地步骤 |
>
> 与开发文档或架构文档冲突时以它们为准；本文档只补实现细节，不另立规范。
> 本次重构涉及的决策以 `FMT 开发文档.md` 第 122 节「开发中的冻结规则」（为什么这么定见第 128 节）与 `FMT 项目架构.md` 第 2 节「关键设计决策」为准。
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
| nlohmann/json | 3.11.3 | JSON 解析与序列化（业务数据、IPC 帧、响应信封都用它） | `third_party/nlohmann/json.hpp` |
| cpp-httplib | 0.18.3 | HTTP **服务端**（浏览器侧；`httplib::Server`） | `third_party/cpp-httplib/httplib.h` |

引用形式（包含路径以 `third_party/` 为根）：

```cpp
#include <nlohmann/json.hpp>
#include <cpp-httplib/httplib.h>
```

`cpp-httplib` 提供 `httplib::Server`（Service 侧监听 `localhost:4122`）。Windows 下需链接
`ws2_32`（已在 `third_party/CMakeLists.txt` 中处理）。

> **URL 下载已经从 cpp-httplib 移走（提交 `a2b6cd1`）**：原文的「用途」栏写的是
> 「HTTP 服务端（浏览器侧）**与** URL 下载客户端」。现在 `third_party/cpp-httplib` 只服务
> **浏览器入口的服务端**；`file upload <url>` 的下载改由
> `src/common/http_client.cpp`（**WinHTTP + Schannel**，系统组件，自动用系统代理，
> 不需要 OpenSSL、不分发 DLL）完成，`src/file/CMakeLists.txt` **不再链 cpp-httplib**。
> 因此 `httplib::Client` 在**业务代码里没有调用点**；它仍然出现在 `tests/server_test.cpp`
> （用 `httplib::Client` 打同进程里的 `httplib::Server`），下载桩则用 `httplib::Server`
> 起本地服务（`tests/http_client_test.cpp`）。

> **CLI 不走 HTTP。** CLI 与 Service 之间的命令通道是**命名管道** `\\.\pipe\fmt.control`
> （见 13.9 / 11.1）。早期设计的「CLI 是 HTTP 客户端」已作废。

其余功能自行实现或使用系统组件（全部是 Windows 系统库，不引入额外 DLL）：

| 功能 | 实现方式 |
|---|---|
| MD5 | `common/hash`，自研（RFC 1321，约 150 行；引入库不划算） |
| 文件名 / URL 校验 | `common/validation` |
| 时间格式化 | `common/time` |
| 日志 | `common/logger` |
| Windows Service（SCM） | SCM API：`advapi32`（`OpenSCManagerW` / `CreateServiceW` / `StartServiceW` / `ControlService` / `RegisterServiceCtrlHandlerExW` / `SetServiceStatus`） |
| UAC 提权 | `ShellExecuteExW`（`shell32`，`lpVerb = L"runas"`） |
| 命名管道 | `kernel32`：`CreateNamedPipeW` / `ConnectNamedPipe` / `CreateFileW` |
| 管道 DACL 与 MIC 标签 | `ConvertStringSecurityDescriptorToSecurityDescriptorW` + `SetSecurityInfo`（`advapi32`） |
| 单实例互斥体 / 窗口激活 | `CreateMutexW`、`EnumWindows`、`SetForegroundWindow`、`FlashWindowEx`（`kernel32` / `user32`） |
| 随机数（Share ID） | `BCryptGenRandom`（`bcrypt` 系统库） |
| URL 下载 | `common/http_client`（WinHTTP + Schannel）：`http://` 与 `https://` **都已支持** |

> **更正（提交 `a2b6cd1`）：`https://` 已经支持，而且不引 OpenSSL、不分发 DLL。**
>
> 原文写的是「cpp-httplib 的 `Client`（HTTP）；HTTPS 需要 OpenSSL，V1 暂不支持」——两处都要改：
>
> * **不是 cpp-httplib 的 `Client` 了**：`fmt_core` 里新增 `include/fmt/common/http_client.hpp` +
>   `src/common/http_client.cpp`，是一个基于 **WinHTTP + Schannel** 的流式 GET 客户端，
>   `src/common/CMakeLists.txt` 里链 `winhttp`；`src/file/CMakeLists.txt` **不再链 cpp-httplib**。
>   cpp-httplib 只剩一个**业务**用途：**浏览器入口的 HTTP 服务端**（`src/server/`）；
>   测试里还在用它的 `Server` / `Client` 起桩与打桩。
> * **「HTTPS 需要 OpenSSL」也不再成立**：TLS 由 WinHTTP 交给系统的 **Schannel** 完成，
>   证书走**系统证书库**，既不需要 OpenSSL，也不需要随产物带任何 DLL。
>
> **为什么换掉 cpp-httplib 的 `Client`**（本轮最重要的决策）：
>
> | 理由 | 说明 |
> |---|---|
> | cpp-httplib 的 `Client` 走 https 必须 OpenSSL | 编译 OpenSSL 需要 Perl + NASM，破坏本项目「只依赖 vendored 单头文件、离线可构建」的前提 |
> | 本项目用 `/MT` 静态 CRT | 静态链接 OpenSSL 虽然也能做到不引 DLL，但 `/MT` 与市面上常见的 `/MD` 静态包混用 CRT 会出问题，自己编又回到上一条 |
> | WinHTTP 是**系统组件** | 只要 Windows 在，它就在；`target_link_libraries(fmt_core PRIVATE winhttp)` 链的是系统导入库 |
> | TLS 走 **Schannel** | 用系统证书库，证书更新跟着系统走，不需要在仓库里塞 CA bundle |
> | **自动使用系统代理** | `WinHttpOpen` 首选 `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`（Win8.1+），失败退回 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY`；访问 https 站点基本都要走代理，这一点是刚需 |
>
> 接口与行为见 10.2.2；完整设计见 18.20 与 `FMT 开发文档.md` 第 33 节。

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
| 控制台 | 在 `wmain` 最开始调用 `SetConsoleOutputCP(CP_UTF8)`（同时置 `SetConsoleCP(CP_UTF8)`），保证中文不乱码 |
| 程序入口 | `wmain(int argc, wchar_t** argv)`——Windows 控制台程序的宽字符入口 |
| 参数获取 | **不用 `argv` 的窄字符形式**：用 `GetCommandLineW` 取原始命令行，再统一转 UTF-8（见 13.1）。中文参数在 GBK 系统上经窄 `argv` 会被破坏 |
| 路径类型 | `std::filesystem::path`，Windows 下用宽字符 API 获取 |

参数转换的落点是 `paths::utf8_arguments()`（`common/text`）：它接收 `GetCommandLineW`
的结果，按 `CommandLineToArgvW` 的规则切分，再逐段 `wide_to_utf8`。
**CLI 内部一律用 UTF-8 `std::string`**，只在调用 Windows API 时才转回宽字符。

### 1.5 编译选项

```cmake
# MSVC
/W4 /permissive- /utf-8
# GCC / Clang
-Wall -Wextra -Wpedantic -Wshadow -Wconversion
```

目标：警告在开发阶段解决，不积累。不为追求「零警告」关闭合理检查。

### 1.6 应用清单（manifest）

**冻结决策：manifest 的 `requestedExecutionLevel` 必须是 `asInvoker`。**

```xml
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security>
      <requestedPrivileges>
        <requestedExecutionLevel level="asInvoker" uiAccess="false" />
      </requestedPrivileges>
    </security>
  </trustInfo>
  <!-- 其余节点：兼容性、DPI 等，按需补充 -->
</assembly>
```

接入方式（`src/CMakeLists.txt`）：

```cmake
target_sources(fmt PRIVATE "${CMAKE_SOURCE_DIR}/resources/fmt.manifest")
set_property(TARGET fmt APPEND PROPERTY LINK_OPTIONS "/MANIFEST:EMBED")
target_link_options(fmt PRIVATE
    "/MANIFESTINPUT:${CMAKE_SOURCE_DIR}/resources/fmt.manifest")
```

**绝不能写 `requireAdministrator`。** 理由：

| 原因 | 说明 |
|---|---|
| 服务形态由 SCM 启动 | SCM 以 `LocalSystem` 拉起服务宿主进程，与 exe 清单无关；`requireAdministrator` 对它没有任何帮助 |
| CLI 必须能在普通用户下运行 | 普通命令（`file list` 等）本来就不需要管理员；`requireAdministrator` 会让**每次双击都弹 UAC**，连查询都跑不起来 |
| 提权是**按命令**发生的 | 只有 `service install/uninstall/start/stop` 这**四条动作命令**走 UAC 提权（`runas`），见 13.8；第五条 `service status` 是**查询命令，不提权**（13.4.1）。提权发生在**另一个短命副本**里，而不是主进程 |

因此 `fmt.exe` 的三种形态（13.1）与 manifest 必须是：

```text
CLI 形态               asInvoker 直接启动          普通用户权限
Service 形态           由 SCM 以 LocalSystem 启动   系统权限（与清单无关）
提权短命副本           ShellExecuteExW(L"runas")   高完整性，做完即退
```

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
│   ├── main.cpp                  wmain：入口分发（Service / CLI / 异常兜底）
│   ├── common/                   公共类型与工具
│   ├── config/                   配置
│   ├── storage/                  底层数据访问
│   ├── core/                     路径与程序生命周期
│   ├── ipc/                      命名管道协议（帧、DACL、超时）
│   ├── service/                  Windows Service（SCM 生命周期 + 服务模式主循环）
│   ├── cli/                      命令行（解析、管道客户端、交互循环、提权引导）
│   ├── bucket/  file/            业务模块
│   └── server/                   HTTP Server（浏览器侧）
├── third_party/                  vendored 单头文件库
├── resources/                    图标、`fmt.manifest`（asInvoker）
└── docx/                         项目文档
```

> `share` 没有独立目录：Share 的存储与规则都在 `src/file/service.cpp` 里，
> 因为它与 File 共用同一把锁和同一份元数据读取路径，拆开只会引入跨模块加锁。
> **`trash` 现在有独立目录了**（提交 `0fc242b`）：`include/fmt/trash/trash.hpp` +
> `src/trash/trash.cpp` 的 `TrashService` 把两级条目合成一份视图并做标识解析（10.4）；
> 桶级的数据操作仍写在 `src/bucket/`（10.1），文件级的仍写在 `src/file/`（10.2）。
> 原口径「`src/trash/` 要等文件级条目落地才需要」**已兑现**。
>
> `ipc` 与 `cli` 分属两端但共用同一份帧格式定义（`include/fmt/ipc/protocol.hpp`），
> 保证「写帧」与「读帧」不会各自漂移。

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
CLI (管道客户端)              HTTP (浏览器)
       │                            │
       ▼                            ▼
   IPC 管道 \\.\pipe\fmt.control   server
       │                            │
       └──────────┬─────────────────┘
                  ▼
            Service 层            bucket, file, share, trash, config
                  │
                  ▼
            Core                  path, app
                  │
                  ├── Storage     JSON、文件操作
                  └── 文件系统
```

**两条入口都直连同一个 service 层**：命令通道不同，业务实现只有一份。

| 模块 | 负责 | 不负责 |
|---|---|---|
| `common` | Error、Result、Time、String、Path、Hash、Validation、Logger | 任何业务规则 |
| `config` | `config.json` / `server.json` 的加载、校验、保存 | 业务数据 |
| `storage` | 读 JSON、写 JSON、建目录、移动/删除/检查文件 | 业务规则（如「文件能否删除」由 Service 决定） |
| `core` | 数据根（`FMT_ROOT`）解析与切换、运行目录、程序生命周期 | 业务对象 |
| `ipc` | 命名管道服务端/客户端、帧编解码、管道 DACL 与 MIC | 业务规则（只把 `op` 交给 Service 层） |
| `cli` | 参数解析、输出、交互循环、单实例、提权引导 | 直接操作文件系统业务 |
| `bucket`/`file`/`share`/`trash` | 各自业务规则 | 越过 `storage` 直接操作文件 |
| `service` | Windows Service 生命周期（SCM 注册、状态机、停止流程）、服务模式主循环、数据根切换 | 业务逻辑 |
| `server` | HTTP 请求解析、调用 Service、返回响应 | 直接改 `data/*.json`、`repository`、`trash` |

业务模块之间不得直接调用对方的底层实现。

### 2.3 公共头文件引用

```cpp
#include "fmt/core/path_manager.hpp"     // 正确
#include "core/path_manager.hpp"         // 错误
```

`include/` 是唯一公共头文件根，所有引用带 `fmt/` 前缀。

### 2.4 新增模块

1. 建 `src/<模块>/` 与 `include/fmt/<模块>/`
2. 在 `src/CMakeLists.txt` 加 `add_subdirectory(<模块>)`
3. 在该模块 `CMakeLists.txt` 用 `target_sources(fmt_core PRIVATE ...)` 登记源文件

顶层构建脚本不需要改动。细节与本次重构新增的模块（`service` / `ipc`）见 2.6。

### 2.5 头文件守卫

统一使用 `#pragma once`（MSVC 支持良好，与项目单一编译器目标一致）。

### 2.6 本次重构新增模块的接线

`service`、`ipc`（以及 `cli` 的管道客户端部分）是本次重构新增或重写的目录，
登记时要一并注意四点：

```text
1. src/CMakeLists.txt            add_subdirectory(service) / add_subdirectory(ipc)
2. src/<模块>/CMakeLists.txt     target_sources(fmt_core PRIVATE ...)
3. include/fmt/<模块>/           公共头文件（带 fmt/ 前缀引用）
4. 被 CLI 与 Service 共用的定义（ipc 的帧格式与 op 常量）
   必须放在 include/fmt/ipc/，不能放 src/ 下的私有头
```

另外：

| 事项 | 要求 |
|---|---|
| 链接库 | `advapi32`（SCM、DACL）、`shell32`（提权）、`bcrypt`（随机数）、`ws2_32`（httplib）、`winhttp`（URL 下载，提交 `a2b6cd1`）；全部系统库，不引入 DLL |
| manifest | `resources/fmt.manifest`（`asInvoker`）在 `src/CMakeLists.txt` 里嵌入（1.6） |
| target 数量 | **`fmt` 始终只有一个可执行 target。** 三种形态是同一个 target 的运行期分支，不拆成 `fmt_cli` / `fmt_svc`——拆开就违反「产物只有一个 `fmt.exe`」 |

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

### 3.4 版本号与构建日期

单一来源：`CMakeLists.txt` 的 `project(FMT VERSION 1.0.0)`。
经 `cmake/version.hpp.in` 生成 `fmt/version.hpp`：

```cpp
#define FMT_VERSION_MAJOR 1
#define FMT_VERSION_MINOR 0
#define FMT_VERSION_PATCH 0
#define FMT_VERSION      "1.0.0"
#define FMT_PROGRAM_NAME "fmt.exe"
```

**构建日期也由 CMake 在配置时生成**，格式 `%Y.%m.%d`（本地时间），进同一个头文件：

```cmake
# CMakeLists.txt
string(TIMESTAMP FMT_BUILD_DATE "%Y.%m.%d")
```

```cpp
// cmake/version.hpp.in → 生成的 fmt/version.hpp
namespace fmt::version {
inline constexpr std::string_view MAJOR      = "@PROJECT_VERSION_MAJOR@";
inline constexpr std::string_view MINOR      = "@PROJECT_VERSION_MINOR@";
inline constexpr std::string_view PATCH      = "@PROJECT_VERSION_PATCH@";
inline constexpr std::string_view STRING     = "@PROJECT_VERSION@";
inline constexpr std::string_view BUILD_DATE = "@FMT_BUILD_DATE@";   // 如 2026.10.08
}
```

**横幅与 `--version` 共用同一个字符串**（`banner_text()`，`src/cli/cli.cpp`）：

```text
File Manager Tool  v1.0  ( build  2026.10.08 )
```

```cpp
std::string banner_text() {
    return "File Manager Tool  v" + std::string(version::MAJOR) + "." +
           std::string(version::MINOR) + "  ( build  " + std::string(version::BUILD_DATE) + " )";
}
```

| 项 | 约定 |
|---|---|
| 程序名 | 固定 **`File Manager Tool`**（文档名/工程名仍叫 FMT，只有程序横幅用这个名字） |
| 版本部分 | `v<MAJOR>.<MINOR>`，当前 `v1.0`；工程版本仍是 `1.0.0`（`PATCH` 不出现在横幅里） |
| 构建日期 | `BUILD_DATE`，CMake 配置时按本地时间生成，**每次重新配置都会变** |
| 空格 | 「Tool」与「v1.0」之间、`(` 与 `build` 之间、`build` 与日期之间都是**两个空格**（照实现原样） |
| 单一来源 | `--version` 与交互横幅调同一个 `banner_text()`，不各写一份 |
| 用法标题 | `--help` 里是 `用法：fmt.exe [命令]`，**不再**用 `FMT 1.0.0 - Windows 文件管理系统` 这类旧标题 |

**任何源码不得重复硬编码版本号或构建日期。**

---

## 4. 运行目录

### 4.1 数据根（FMT_ROOT）

```text
FMT_ROOT = fmt.exe 所在目录
```

**绝不是当前工作目录。** 用户位于 `C:\>` 执行 `D:\FMT\fmt.exe` 时，数据仍写在
`D:\FMT\`。

Windows 下用 `GetModuleFileNameW`（宽字符）获取自身路径，取其父目录。
缓冲区按需倍增，处理超长路径。

**数据根是运行期概念，不是编译期常量。** 冻结后的分工：

| 角色 | 对数据根的作用 |
|---|---|
| CLI | **声明者 + 自检者**：用 `GetModuleFileNameW` 取自身路径的父目录作为 root；双击时先对自己这个 root 做一次幂等体检与补齐（建缺失目录与默认 JSON、读已有 JSON 确认完整性，见 11.13），连接时再用 hello 帧声明（见 13.10）。响应里的 `switched` / `previous_root` 告诉它服务是否跟着换了根 |
| Service | **持有者**：维护「当前数据根」；收到不同 root 时切换，并对新根做幂等初始化（与 CLI 共用同一份 `ensure_root`/`check_root`） |
| Service（无 CLI 连接时） | 读取 `%ProgramData%\FMT\service.json` 记录的数据根；从未记录过则取**服务宿主 exe**所在目录 |

**必须区分「服务宿主 exe」与「数据根」**，两者是不同概念、可以不同：

```text
服务宿主 exe   首次 service install 时通过 CreateServiceW 的 lpBinaryPathName
               登记为当时的绝对路径，SCM 据此启动服务
数据根         CLI 声明的目录，运行目录（repository/trash/config/data/log/temp）在这里
```

由此得到两条实际操作纪律：

```text
移动 fmt.exe 请用「复制」，不要用「移动」或删除宿主 exe
   → 宿主 exe 被移动/删除后 SCM 找不到映像，启动服务报 1053

把 fmt.exe 放到别处运行 = 声明了另一个数据根
   → Service 会切换根并初始化新根（hello 响应回填 switched=true / previous_root）；
     CLI **在 stderr 打印换根提示**（提交 `8f2fbc5`，`cli::root_switch_notice(previous, current)`）并记一行日志「数据根切换：旧 -> 新」（**原口径「只进日志，不刷控制台」已作废**，11.9 / 13.10）；旧根数据不删除，留在原地
```

数据根切换与初始化的完整流程见 13.10；服务自身状态文件见 13.6。

### 4.2 目录布局

```text
FMT_ROOT/                      CLI 声明的数据根
├── fmt.exe                    服务宿主 exe（安装时登记的路径，通常就在这里）
├── repository/                正式文件
│   └── <user>/<bucket>/YYYY/MM/DD/<file_name>
├── trash/                     回收站；**<user>/ 的顶层只放桶级条目**
│   └── <user>/
│       ├── .files/<bucket>/YYYY/MM/DD/<file_name>   文件级条目（提交 188e85d 起
│       │                                        file delete 真的会往这里写；
│       │                                        读取侧提交 0fc242b 已落地，10.4）
│       │                                        提交 4fee290：中间那层点开头的 .files
│       │                                        是刻意的，见下
│       ├── <bucket>_<YYYYMMDDHHMMSS>/           桶级条目：整个桶搬进来，
│       │                                        目录名一律带删除时间戳（同秒加 _2）
│       └── .original                            桶级身份记录的唯一权威（7.3.2）
├── config/
│   ├── config.json
│   └── server.json
├── data/
│   ├── user.json
│   ├── file.json
│   ├── share.json
│   └── trash.json
├── log/
│   ├── fmt.log                全部日志
│   └── error.log              仅 ERROR 级
└── temp/                      临时文件，随时可以清空；既不是业务数据、也不是日志
    ├── fmt-upload-<随机>-<序号>.tmp  上传暂存（提交 188e85d，10.2.3；fmt- 前缀让启动清理收走）
    └── fmt-elev-<父进程 pid>.json   提权结果文件（13.8.3），父进程读完立刻删除
```

**不在数据根内的位置：**

```text
%ProgramData%\FMT\service.json    服务自身状态（当前数据根 + 安装信息），不是业务数据
```

> **`trash/<user>/` 顶层归属与文件级落点（提交 `4fee290`）**：顶层只放**桶级条目**
> （`<名字>_<14 位时间戳>`，可带 `_<1-3 位序号>`），文件级条目收在点开头的
> `trash/<user>/.files/<bucket>/YYYY/MM/DD/` 下。原口径「文件级条目直接落在
> `trash/<user>/<bucket>/`」**已作废**：阶段 5 落地 `file delete` 之后，桶级扫描会把这个
> 目录误当成「孤儿桶条目」列出来（10.3.2 的 `list_trashed` 就是这么扫的）。
> 扫描跳过点开头的条目（`.files` / `.original`），另有一道形状检查兜底（10.3.2）。
>
> **这条布局在提交 `188e85d` 兑现了它要防的问题**：`file delete` 现在真的会往
> `trash/<user>/.files/…` 写文件，而 `list_trashed()` 因为「跳过点开头 + 形状检查」
> 确实不会把它列成桶级条目——代价则是**文件级条目也就一条都列不出来**（10.4、18.19）。
> 这是当初权衡时接受的代价，不是回归。

**`temp/` 的定位与规则**：

```text
定位      临时文件目录：不是业务数据、也不是日志，内容随时可以清空
放什么    ① 提权结果文件 temp/fmt-elev-<父进程 pid>.json（父进程读完立刻删除，13.8.3）
          ② 上传时的暂存文件（10.2 旧文写「temp/ 或者系统临时目录」，现在明确为 temp/；
             提交 188e85d 的实际名字是 fmt-upload-<随机>-<序号>.tmp，见 10.2.3）
谁创建    CLI 在执行提权类命令前会尝试创建 <数据根>/temp；创建不出来（例如 exe 放在只读位置）
          就退回系统临时目录 %TEMP%，并写一行 WARN 说明原因与改用后的路径。
          提权副本在写入结果文件前也会确保目录存在——它自己有权限，
          所以调用方数据根只读时它仍然能建出来
清理      Service 启动时删除 temp/ 下**十分钟以前**的、以 fmt- 开头的遗留文件
          （上次异常退出留下的提权结果等），用户手放进去的其它文件一律不动；
          删除数量记一行 INFO（时机见 13.7.1 第 4 步）。
          **为什么按年龄过滤（提交 5b316b3，`src/service/runtime.cpp` 的
          `clean_temp_directory()`）**：提权副本回传结果的临时文件也叫
          `fmt-elev-<pid>.json(.tmp)`，而 `service install` 会在同一次操作里
          **启动服务**——服务启动就来清 temp/。原来只看前缀 `fmt-`，于是把父进程
          **正在收**的结果文件一起删掉，父进程只好报「FMT-602 提权副本没有返回结果」：
          服务其实已经装好并启动，**用户看到的是假失败**（实测日志与完整分析见 13.8.3）。
          正在回传的结果文件寿命只有几十毫秒，所以按年龄放过新的、只清旧的；
          读不到时间戳的文件也不动
```

`temp/` 跟着 exe 走：它在数据根下，用户一眼能找到、随时可清。
它按定义就是**可清空**的目录，且与 `repository/`、`data/`、`config/` 分属不同职责，
所以把一个短暂的提权结果或上传暂存放进去，不会污染业务数据。

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
    std::filesystem::path temp() const;          // <root>/temp（临时文件，随时可清）

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
    // **文件级**条目（阶段 5 起）：提交 4fee290 起落点是
    // trash/<user>/.files/<bucket>/YYYY/MM/DD/<file_name>
    // （桶以下的层级与 repository 一致，便于还原）
    std::filesystem::path trash_file(const std::string& user,
                                     const std::string& bucket,
                                     const std::string& date,
                                     const std::string& file_name) const;

    // 桶级条目没有独立方法：落点是 trash/<user>/<bucket>_<时间戳>/，
    // 身份记录是 trash/<user>/.original，两者都由 BucketService 自己拼
    // （original_index_path()，常量 kOriginalIndexName，见 7.3.2 / 10.1）。

private:
    // 实际的私有实现（提交 4fee290 增加可选参数 inner，插在 user 与 bucket 之间）：
    // base/user[/inner]/bucket/YYYY/MM/DD/file_name
    Result<std::filesystem::path> build(std::filesystem::path base, std::string_view user,
                                        std::string_view bucket, const DateParts& date,
                                        std::string_view file_name,
                                        std::wstring_view inner = {}) const;
    // repository_file() 不传 inner（仓库路径不变）；trash_file() 传 L".files"
};
```

**不做全局单例**，通过上下文传递，便于测试用独立 root：

```cpp
struct AppContext {
    std::unique_ptr<PathManager> paths;   // 必须堆持有：Service 保存的是它的引用
    std::unique_ptr<Logger>      logger;  // 同上，值成员被移动会留下悬垂引用
    Config                       config;
};
```

> 这两处 `unique_ptr` 不是风格选择，是踩过坑的结论（见 18.11 第 2、3 条）：
> 值成员被移动赋值后，`file::Service` 持有的引用会悬垂。

**数据根在运行期可能变化**（13.10 的根切换）。切换时**整体重建** `AppContext` 中
与根相关的部分，而不是就地改 `PathManager` 的 root：

```cpp
// service 层：根切换的唯一入口
Status ServiceRuntime::switch_root(const std::filesystem::path& new_root);
//   1. 停止接受新请求（gate 关闭）
//   2. 等在途关键操作完成
//   3. 换掉 paths / logger / config / 业务服务 实例
//   4. 幂等初始化新根（见 4.4）
//   5. 开 gate，写 INFO 日志
```

**任何模块都不得缓存 `PathManager` 之外的路径字符串**，否则根切换后会指向旧根。

### 4.4 初始化

**冻结决策：初始化由 Service 与 CLI 共用同一套幂等规则**（同一份 `ensure_root` / `check_root`
实现，见 11.13）。两边都只补缺失、都不碰业务数据内容：

```text
谁执行  Service：启动时（4.4.1 时机 1）、以及 hello 触发换根时（时机 2）
        CLI    ：双击时，对自己 exe 所在的数据根执行（4.4.4 / 11.13）
                —— 在打开日志器之前执行，所以 log/ 也由它创建
共同规则 六个目录（repository/ trash/ config/ data/ log/ temp/）缺则建，已存在一律不动
        六个默认 JSON 缺则写（.tmp 原子替换，4.4.2）
        已有的 JSON 会被真正读一遍（解析 + 版本检查）确认完整性
        读不出来或版本不受支持 → 只报告、绝不重置（沿用「JSON 损坏不能静默重置」）
        都不删除、不覆盖、不改名，都不写 data/*.json 的内容
```

因此初始化有两条**共用同一实现**的调用路径，规则完全一致、都是幂等的。

#### 4.4.1 幂等初始化（bootstrap_root / ensure_root）

同一个函数被两类调用方使用：

```text
时机 1（Service 启动）：当前数据根来自 %ProgramData%\FMT\service.json 或服务宿主目录
时机 2（hello 换根）：CLI 用 hello 帧声明了一个与当前根不同的 root（见 13.10）
时机 3（CLI 双击）：CLI 对自己 exe 所在的 root 执行，**不经过服务**（见 11.13）
```

```text
bootstrap_root(root):          // CLI 侧同一函数也叫 check_root，语义完全相同
 1. 规范化 root（去尾部分隔符、取绝对路径）
 2. 检查 root 是否存在 → 不存在直接报错，不隐式创建盘符/父目录
 3. 依次确保存在：repository/  trash/  config/  data/  log/  temp/
 4. 依次确保存在默认 JSON（不存在才写）：
      config/config.json、config/server.json、
      data/user.json、data/file.json、data/share.json、data/trash.json
 5. 把**已存在**的 JSON 真正读一遍（解析 + 版本检查）确认完整性：
      读不出来或版本不受支持 → 记进「损坏清单」，只报告、**绝不重置**
 6. （Service 侧）加载配置（损坏则报错，不覆盖）
 7. （Service 侧）初始化业务服务（FileIdGenerator::initialize 等）
 8. 写 INFO 日志：数据根已就绪
 9. 返回本次「新建目录 / 新建文件 / 损坏文件」三个清单，供 CLI 记日志（见 11.13）
```

第 5 步是**只读**的：它保证「服务与 CLI 对同一个根的判断一致」，同时让 CLI 双击时能立刻
发现问题——记一行 `[Cli] 数据根损坏（未自动修复）：data/file.json` 到日志，并把
`数据根文件损坏（未自动修复）：data/file.json` 送到 stderr，而不是等业务命令失败才暴露。

**阶段 4 在 `initialize_root()` 上补的一步（占位用户名）**：

```text
5.5 加载 config.json（损坏 → FMT-008，不覆盖）
    current_user 为空？
      ├─ 是 → 置为占位名 "user" → save_config() 落盘 → 记日志
      └─ 否 → 原样保留（这一步幂等：第二次读到 "user" 什么都不做）
    最后加载 server.json
```

需求原文是「不做用户先用 user 代替」，所以这不是降级路径而是正常首启路径：用户双击后
直接敲 `bucket create 工作` 就能得到 `repository/user/工作/`。**`FMT-604 NoCurrentUser`
仍然保留**，但只在 `current_user` 被外部显式清空时才会由 Bucket 服务返回（18.15）。

实现位置：`src/core/app.cpp`（`initialize_root`）——注意这一步在 `ensure_root()` **之外**，
因为它是「配置内容」的修正，而 `ensure_root` 只负责目录与文件是否存在。

**`temp/` 的清理**：**Service 启动时**（时机 1）删除 `temp/` 下以 `fmt-` 开头的遗留文件——
上次异常退出留下的提权结果（`fmt-elev-<父进程 pid>.json`）等；**用户手放进去的其它文件一律不动**。
删除数量记一行 INFO（例如 `[Service] 清理 temp/ 中 2 个遗留临时文件`；为 0 时不记）。
清理**只认 `fmt-` 前缀**，不做「整个目录清空」——`temp/` 虽然是可清空的目录，但服务不替用户决定删什么。
> **一个残留窗口**：清理只发生在**服务启动**时。若服务**运行中**被切换了数据根，
> 旧根 `temp/` 里没做完的 `.tmp` 与遗留结果文件**不会立刻被清**——服务不替另一个根做决定，
> 要等下次服务带着那个根启动时才清掉。因此**换根必须等在途上传结束**（13.10.2 第 5 步），
> 这是阶段 5 实现上传时要真正接上的约束，别只关 gate 拿锁就换。

> **别和 `.tmp` 原子替换混淆**：4.4.2 的 `<目标>.tmp` 写在**目标文件旁边**
> （`config/config.json.tmp` 这种），是原子替换的中间态；`temp/` 目录只放上面两类临时文件，
> 两者不是一回事。

#### 4.4.2 默认 JSON 的写入方式

**每个默认文件都必须走 `.tmp` 原子替换**（第 6.6 节）：

```text
1. 判断目标文件是否存在
   ├─ 存在 → 什么都不做（不读、不写、不迁移、不补字段）
   └─ 不存在 → 继续
2. 序列化默认内容
3. 写入 <目标>.tmp
4. 重新读取 .tmp 并解析，验证可读（防止半文件）
5. MoveFileEx(.tmp → 目标, MOVEFILE_REPLACE_EXISTING)
6. 任一步失败 → 删除 .tmp，报错，保留现场
```

规则（与旧版一致，仍然有效）：

```text
不存在 → 创建
已存在 → 保持原样，不删除、不清空、不覆盖
```

**这条规则是根切换的安全前提，也是 CLI 双击体检的安全前提**：CLI 换目录运行时，新根里若已有
`data/file.json`（例如同一个 exe 目录被两个人共用），**谁都不会**把它重置成空；CLI 双击时只会
把它读一遍确认可解析、版本受支持。反过来说，也**不会**把旧根的数据迁到新根——切换只换指针，不做搬迁。
若该文件损坏或版本不受支持，两边都**只报告**：CLI 记一行
`[Cli] 数据根损坏（未自动修复）：data/file.json` 并把它送到 stderr，服务在加载配置时返回
`FMT-008` / 退出码 7，绝不静默重置。

#### 4.4.3 失败处理

任一必要目录或默认文件创建失败：

```text
停止绑定该根 → 通过管道把错误回给 CLI（FMT-6xx / 退出码 7 或 8）→ 不进入该根
```

绝不继续运行假装成功。服务日志里记录失败目录与原因，示例：

```text
2026-10-07 22:03:59 [ERROR] [Service] 初始化失败
Directory: D:\FMT\repository
Reason: Access is denied.
```

#### 4.4.4 CLI 侧

```text
1. 设置控制台输出编码（SetConsoleOutputCP(CP_UTF8)）
2. 获取 exe 路径 → 计算 root
3. **对 root 执行与 Service 共用的幂等体检与补齐**（check_root，见 11.13）：
     六个目录 + 六个默认 JSON，只补缺失；已有 JSON 读一遍确认，损坏只报告不重置
     —— 这一步在打开日志器之前，所以 log/ 也在这条「新建目录」清单里
     —— 结果**只进日志**（[Cli] 数据根检查：<root> / 数据根新建目录：… / 数据根新建文件：…，
        什么都没缺时是「数据根完整」），控制台一行都不打（见 11.13）
4. 单实例检查（命名互斥体 Local\FMT.CLI.v1，见 11.11）
5. 解析命令行
6. service 命令 → 走 SCM 路径（见 13.8），结束
                  —— install / uninstall / start / stop 提权前先确保 <root>/temp 存在
                     （见 4.2、13.8.2），建不出来就退回 %TEMP% 并写一行 WARN；
                     status 是查询命令，不提权，也不碰 temp/
7. 其他命令   → 连管道 + hello 声明 root → 进入命令循环（见 11.2）
                 —— switched=true 时**在 stderr 打印换根提示**（提交 `8f2fbc5`）并记一行日志「数据根切换：旧 -> 新」
8. 日志        → 以追加方式打开 root 下的 log/fmt.log（该目录已由第 3 步建好，见 11.12）
                 ——`--help` / `--version` 不执行第 3 步与这一步，也不创建任何目录
```

**CLI 不碰业务数据内容**：它不写 `data/*.json` 的内容、不删除文件、不改名，连业务文件是否存在
都不检查——那是服务的职责。第 3 步是它唯一会碰目录结构的动作，而且只补缺失、只读确认。
CLI 会碰磁盘的地方因此是：`log/fmt.log` 与 `log/error.log`（追加写）、
第 3 步补齐的六个目录与六个默认 JSON（只补缺失，见 11.13）、
`temp/`（提权前建目录、写完结果文件后读回并删除，见 4.2 与 13.8.3）、
`file upload` 的**源文件路径**（只读）与 `service install` 时的自身路径。

不主动修改系统权限，不请求管理员权限（`service` 四条动作命令的 UAC 提权除外）。

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

> **注意：这是「刚写出的默认文件」的样子，不是运行时的样子。** 阶段 4 起，
> 数据根初始化（`initialize_root`，4.4）读到 `current_user` 为空时会立刻置为**占位名
> `user`** 并保存（需求原文「不做用户先用 user 代替」）。因此实际运行中字段是这样的：
>
> ```json
> { "version": 1, "current_user": "user", "current_bucket": "工作", … }
> ```
>
> 好处是磁盘路径与用户身份解耦：没有用户系统也能得到 `repository/user/<bucket>/…` 这种
> 合法层级，`FMT-604 NoCurrentUser` 只在用户被**显式清空**时才出现（见 18.15）。

| 字段 | 类型 | 说明 |
|---|---|---|
| `current_user` | string | 当前用户。用户系统未实现前由初始化写成占位名 `user`；为空表示被显式清空 |
| `current_bucket` | string | 当前默认 Bucket 的**名称**，不保存路径或 ID |
| `max_upload_size` | integer | 最大上传字节数，统一用字节 |
| `size_unit` | string | 显示单位 B/KB/MB/GB，只影响显示 |
| `language` | string | CLI 显示语言 |

**配置文件属于数据根。** 每个数据根有自己独立的一份 `config/config.json`：
切换根（13.10）后 `current_user` / `current_bucket` 也随之切换。
服务**不会**把配置从一个根复制到另一个根。

配置文件的**内容**创建仍然是服务的行为：进入一个从未初始化过的根时，由服务写出这份默认
JSON（`.tmp` 原子替换，见 4.4.2）；CLI 不写入任何配置文件的内容，`config set` 也是通过管道
请服务去写。唯一的例外是**文件缺失时**：CLI 双击体检也会补出这份默认 JSON
（与服务同一份实现、同一份内容，见 11.13），已存在则一律不动。

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
| `enabled` | 是否启用 HTTP。**代码默认 `false`**（由 `config/server.json` 打开）；**线上已置为 `true`**（提交 `22c3c3e`，真机验收见 18.39）。**仍是缺口**：**安装流程不会自动打开**它——那句「Service 场景下由安装流程置为 `true`」一直没实现（`grep 'enabled = true'` 在 `src/` 里零命中，只有 `tests/config_test.cpp` 写过一次），所以全新数据根装完服务后 HTTP 仍是关的，要手改配置。见 12.1、12.2、19.1 |
| `host` | 监听地址，默认 **`localhost`**（提交 `22c3c3e`：**代码默认值从 `127.0.0.1` 改成 `localhost`**，用户明确要求写 `localhost`）；`0.0.0.0` 允许局域网访问 |
| `port` | 监听端口，默认 4122 |

V1 的**浏览器入口只提供 HTTP**，后续可扩展 HTTPS。

> **别把这一条与「上传下载」混起来**：`server.json` 说的是本地浏览器入口（`localhost:4122`）
> 的监听协议，它至今仍只有 HTTP。**下载来源**的 `http://` / `https://` 都支持
> （`common/http_client`，WinHTTP + Schannel，提交 `a2b6cd1`，见 1.2 与 10.2.2）。

> **不要与 `%ProgramData%\FMT\service.json` 混淆。** 两者都是 JSON，但归属不同：
>
> | 文件 | 位置 | 归属 |
> |---|---|---|
> | `server.json` | `<数据根>/config/` | 业务配置，随数据根切换 |
> | `service.json` | `%ProgramData%\FMT\` | 服务自身状态（当前数据根、安装信息），**不是业务数据**，不随根切换 |

`service.json` 的字段与读写时机见 13.6。

### 5.3 加载规则

```text
启动 → 检查 config/ → 加载 config.json → 校验 JSON
     → 加载 server.json → 校验配置 → 进入程序
```

| 情况 | 处理 |
|---|---|
| 配置不存在 | 允许创建默认配置（服务在启动/换根时、CLI 在双击体检时都会补；见 4.4.2、11.13） |
| JSON 损坏 | **停止初始化**，报告配置错误 |
| 字段缺失 | 用默认值补齐并回写（不改动其他字段） |
| 字段类型错误 | 用默认值补齐并回写（**阶段 2 的实现口径**：`read_field()` 发现类型不符时保留默认值并标记 `patched`，随即整份回写一次；旧文写「报配置错误，不猜测」与实现不符） |

**不得删除原配置，不得重新生成空配置。**

> 只有「文件不是合法 JSON / 版本不受支持 / 写不回去」才报 `FMT-008 ConfigError`；
> 单个字段写错类型不会让整个数据根不可用，代价是那一次启动会写回一份修正后的配置
> （字段缺失、类型错误的处理都走同一条 `patched → save_config()` 路径）。
> `current_user` 为空也走这条路径：由 `initialize_root` 补成占位名 `user`（5.1、4.4）。

加载发生在两个时机：服务启动时（当前根）与根切换时（CLI 声明的新根）。
两个时机走的是同一个 `bootstrap_root()`，规则完全一致。

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
| 集合（**在数据根之外的那一个**） | `{ "version": 1, "buckets": [ ... ] }` | `trash/<user>/.original`（桶级身份记录，**不是** `data/*.json`；见 7.3.2） |

集合字段名固定：`files`、`shares`、`trash`、`users`。**`trash.json` 只服务文件级条目**
（阶段 5 起）：它的 `trash` 数组里不再出现 Bucket 级记录（旧的 `type:"bucket"` 作废），
桶级身份改由 `trash/<user>/.original` 的 `buckets` 数组承载（提交 c2d545d）。

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
2. 写入 target + "." + <pid> + "." + <序号> + ".tmp"（提交 5b316b3，见下）
3. 关闭并 flush
4. 重新读取 .tmp 并解析，验证可读
5. 替换 target（同卷用 MoveFileEx 带 MOVEFILE_REPLACE_EXISTING；短暂竞争会重试）
```

任一步失败：删除 `.tmp`，保留原文件，返回错误。

**为什么临时名必须唯一（提交 `5b316b3`，`src/storage/storage.cpp`，实测踩出来的）**：

```text
原来     固定用 <目标>.tmp
问题     **同一个目标会有两个写者**：`service install` 在启动服务之后由安装器写
         service.json（记 host_path / installed_at），而服务启动时也写它。
         两者共用 service.json.tmp：先完成的一方把它 rename 成正式文件，另一方
         紧接着做**读回校验**时临时文件已经不在了 → 「无法打开文件 …service.json.tmp」。
         更糟的是安装器那处是 `(void)save_state(updated);`（**忽略了返回值**），
         于是 service.json 静默地一直不更新（实测 installed_at 停在 18:19）
现在     ① 临时名 = <目标>.<pid>.<序号>.tmp（pid 取 GetCurrentProcessId()，序号是
            进程内 static std::atomic<unsigned>）——两个写者各写各的临时文件，
            rename 是「后完成者胜」，正常结果就是最后那份内容
         ② **进程内**对原子写加互斥（std::mutex g_write_mutex）：
            同一进程里的多个线程不必去抢替换那一步
         ③ MoveFileExW 的替换（replace_file()）在遇到 ERROR_ACCESS_DENIED /
            ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION / ERROR_FILE_NOT_FOUND
            时**短暂重试**（最多 40 次 × 5 ms）——这类失败是短暂的，不该当成永久
            错误报给用户；其它错误码立即失败，并把 Win32 码写进消息
用例     Storage.两个写者同时写同一个文件不会互相踩：8 线程 × 40 轮写同一个文件，
         断言零失败、内容必须是某一次**完整**写入、且**不留 .tmp**
```

**同一文件的「读取—修改—写入」仍必须在业务锁内完成**（6.8、15.2）：临时名唯一解决的是
「两个写者互相踩」，不解决「读到的内容已经被别人改过」。

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
      "is_trash": false,
      "trash_reason": ""
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
| `trash_reason` | string | **为什么在回收站**：`"bucket"`（桶被删）或 `"file"`（文件自己删的，提交 `188e85d` 起已落地）；不在回收站时为空串 |

**`trash_reason` 的写入规则（阶段 4 已实现，commit c2d545d）**：`is_trash = true` 时**必须同时写**
它，由 `BucketService::set_bucket_files_trash_flag(user+bucket 匹配的记录, trashed, &affected)`
一处维护：

```text
删除桶（trashed = true）   跳过已经 is_trash 的记录；其余置 is_trash=true 且
                          trash_reason="bucket"，计数进 BucketRemoval::files_affected
回退桶（trashed = false）  只处理 is_trash=true 且 trash_reason=="bucket" 的记录：
                          置 is_trash=false、trash_reason=""；"file" 的记录一律不动
```

**为什么要这个字段**：桶删除与文件删除都只写 `is_trash` 一个布尔值时，回退桶无法区分
「这条记录该不该跟着桶一起回去」，会把用户单独删过的文件一起放出来。阶段 5 的
`file delete` 落地时写 `trash_reason = "file"`，与桶删除共用同一套判定。

**提交 `188e85d` 已落地这一半**：`FileService::remove()` 只动**自己那一条**记录
（置 `is_trash = true`、`trash_reason = "file"`），不碰桶级那套 `set_bucket_files_trash_flag()`；
桶回退时 `"file"` 的记录一律不动（10.2.4）。两条路径共用同一个字段语义，
但不动对方的记录——这就是当初把 `trash_reason` 分出来的目的。

**记录里不存路径也不存时间**：上表没有 `path`、没有 `created_at`；文件落在哪一天由
`file_id` 推出（8.5）。`size` 在 C++ 结构里是 `std::uintmax_t`（磁盘大小本来就是无符号），
JSON 里是整数。

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
    std::string trash_reason;   // "bucket" / "file"，不在回收站为空串
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

### 7.3 trash.json 与桶级记录 `trash/<user>/.original`

> **口径（提交 `0fc242b` 之后）**：**文件级条目的权威是 `file.json`**
> （`is_trash` / `trash_reason` / **`deleted_at`**，见 7.1 与 7.3.1），路径由 `file_id`
> 与记录推出；**桶级身份记录的唯一权威是 `trash/<user>/.original`**——它跟着数据走，
> `data/*.json` 丢了原名也还在。理由：**名字带时间戳是「不能反推原名」的**
> （`x_2026...` 也可能本来就叫这个），而「重名才加时间戳」的旧口径会让同一个桶的两条
> 回收站条目抢同一个回退位置。
>
> **`data/trash.json` 从此不再写入**（它以前只是文件级条目的第二份副本）：
> 保留为**只读兼容**——老数据缺 `deleted_at` 时从它补（`legacy_deleted_at()`），
> 回退/永久删除时顺手清掉那条老记录（`remove_trash_record()`，失败只记 WARN）。
> 原口径「`trash.json` 只服务文件级条目、写入侧已落地」（commit `c2d545d` / `188e85d`）
> **已被 `0fc242b` 取代**。

#### 7.3.1 trash.json（**只读兼容**；提交 `188e85d` 曾写入，`0fc242b` 起不再写）

**不再有写入动作**：`file delete` 现在只改 `file.json`（`is_trash` / `trash_reason` /
`deleted_at`，见 7.1 与 10.2.4），**不碰 `trash.json`**。这个文件保留是为了两件事：
读出老记录里的 `deleted_at`（老数据补字段），以及回退/永久删除时清掉遗留记录。
新装的数据根里它始终是空的 `{"version":1,"trash":[]}`。

```json
{
  "version": 1,
  "trash": [
    {
      "file_id": "fmt-20261005-0",
      "file_name": "test.txt",
      "original_path": "repository/小谷/工作/2026/10/05/test.txt",
      "trash_path": "trash/小谷/.files/工作/2026/10/05/test.txt",
      "deleted_at": "2026-10-05T20:00:00",
      "type": "file"
    }
  ]
}
```

（上面是**老数据**里的形状，仅供兼容读取对照。）

| 字段 | 说明 |
|---|---|
| `type` | 老记录里是 `"file"`（`"bucket"` 更早已作废，从未在 `0fc242b` 之后写入） |
| `original_path` | 老字段：原位置。新口径不存它——由 `file_id` + 记录推出（8.5） |
| `trash_path` | 老字段：物理落点 **`trash/<用户>/.files/<桶>/YYYY/MM/DD/<文件>`**（提交 `4fee290`）；原口径写成 `trash/小谷/工作/…` 已作废，理由见 4.2 与 10.2 |
| `deleted_at` | **唯一仍在用**的字段：`file.json` 里那条缺 `deleted_at` 时，从这里补上 |

文件级条目的 list / get / restore / delete 与永久删除**都已落地**（提交 `0fc242b`，
11.4、10.4、18.25）——原口径「属阶段 5/7」**已作废**；
**桶级**的 list / get / restore / delete 在阶段 4／`4fee290` 落地（10.1、10.4）。

#### 7.3.2 `trash/<user>/.original`（桶级条目，阶段 4 已落地，形状已冻结）

Bucket 级记录**不使用 `file_id`**，用 Bucket 名称标识。常量在
`include/fmt/bucket/bucket.hpp`：`inline constexpr const char* kOriginalIndexName = ".original";`

```json
{
  "version": 1,
  "buckets": [
    { "trashed": "lazy-fox_20261008012233",
      "original": "lazy-fox",
      "deleted_at": "2026-10-08T01:22:33" }
  ]
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `version` | integer | 固定 `1`；`check_version(1)` 不认就拒绝（7.x 版本策略同 `data/*.json`） |
| `buckets` | array | 桶级条目表，**唯一权威** |
| `trashed` | string | 回收站里的目录名，`<原桶名>_<YYYYMMDDHHMMSS>`（同秒冲突加 `_2`） |
| `original` | string | 原桶名；**空** = 没有身份记录，回退拒绝（`FMT-001`）。**提交 `9c3d2cb` 起写的是磁盘上的实际桶名**（`canonical_name()` 的结果，`delete WORK` 记 `work`），这样 `restore` 拼出来的目录名与原来完全一致（10.1、18.23） |
| `deleted_at` | string | 本地时间 ISO 8601（7.5），无时区 |

桶级条目的四条硬规则（都已实现）：

1. **目录名一律带删除时间戳**：`trash/<user>/<原桶名>_<YYYYMMDDHHMMSS>`，**无论有没有重名**
   （`unique_trashed_name()`）；同一秒内删两次、或目录恰好同名时再加序号
   `<原桶名>_<时间戳>_2`、`_3`……绝不覆盖已有条目。旧口径「用原名、重名才加时间戳」
   **作废**——那样同一个桶的两条条目会抢同一个回退位置。
2. **绝不靠剥离时间戳反推原名**，一律查这张表。
3. **索引是权威，先读后搬**：`remove()` 先 `load_original_index()`——读不出来（损坏 →
   `FMT-006 JsonParseError`；版本不认 → `FMT-011`）**直接拒绝，一个字都不搬**，
   否则会留下「没有身份记录、原名永久丢失」的条目；搬完写 `save_original_index()`，
   写失败（`FMT-007` / `FMT-009`）就 `std::filesystem::rename` **把目录搬回去**再报错。
   回退方向对称：**索引没减掉就把目录退回回收站**。磁盘与索引不允许不一致。
4. 桶级记录**只存名字**，不存 `original_path` / `trash_path`（路径可由名字推出来）。

写入时机：`BucketService::remove()` 里目录搬成功之后，往 `buckets` 数组**末尾追加**一条
（不是覆盖整个文件）；`BucketService::restore()` 成功后**删掉**对应那条并落盘。
相对路径的理由见 7.6。

### 7.4 user.json

**最终 schema（提交 `bfd89f7`）**：

```json
{ "version": 1, "users": [ {
  "user_id": "u-b1e7c28f", "username": "user",
  "password_hash": "<PBKDF2-SHA256，64 位十六进制>", "password_salt": "<16 字节随机盐>",
  "token": "<32 位十六进制，永久有效>", "current_bucket": "lazy",
  "created_at": "…", "updated_at": "…", "last_login_at": "" } ] }
```

```text
**没有 buckets 字段**（用户要求删除）：桶以磁盘上真实存在的
          repository/<user>/<bucket>/ 为准。读的时候**忽略**老字段，
          **下一次初始化会把老文件里的它清掉**（线上已验证清掉了）。
current_bucket 保留（用户说以后可能有用），但它只是**快照**——
          运行时的权威仍是 config.json 的 current_bucket。
密码      只存哈希：PBKDF2-SHA256 **10 万轮** + 每用户盐；初始密码生成后**不落地明文**。
          V1 没有登录接口，所以拿不到也不影响——**真正的凭证是 token**。
token     32 位十六进制、**永久有效**；`config list` 会打印它（机主唯一方便拿到的地方）。
默认账号  在数据根初始化时创建（initialize_root）：**新根会建**；
          **已经是 {"users":[],"version":1} 的老根也会补建**（线上就是这种情况）；
          **已存在则绝不覆盖**。
```

> **原口径「V1 不实现完整用户系统，只保留 `{"username":"user"}` 作为扩展入口」已作废**
> （提交 `bfd89f7`）：文件现在有账号、密码哈希与 token，HTTP 认证就靠它（12.3.2）。
> 当前用户仍由 `config.json` 的 `current_user` 确定。

### 7.5 时间格式

统一使用 ISO 8601：

```text
2026-10-05T20:00:00
```

本地时间，不带时区偏移。`deleted_at`、`expire_time` 等均用此格式。
序列化与解析集中在 `common/time`。

### 7.6 数据模型与数据根的关系

`data/*.json` 里的记录**不保存绝对路径**，只保存相对数据根的层级信息：

```text
file.json     只记 user / bucket / file_name / …（层级由规则推出）
              **既不存路径也不存时间**：文件落在 repository/<user>/<bucket>/YYYY/MM/DD/
              下的哪一天，由 file_id 的 fmt-YYYYMMDD-N 片段推出（8.5、10.2）
trash.json    original_path 与 trash_path 都是相对数据根的相对路径
              （形如 repository/小谷/工作/2026/10/05/test.txt）
```

这是数据根动态化（4.1）能成立的前提：切换根之后，同一个 `data/file.json`
放到另一个根下，相对路径仍然指向该根内部的正确位置。「日期也从记录里推出来」
是这条纪律的延伸——`file_id` 全程不变（8.2），所以日期片段永远等于入库日期。

由此得到三条纪律：

| 纪律 | 原因 |
|---|---|
| `data/*.json` 不写绝对路径 | 否则换根后全部失效 |
| 时间格式不带时区 | 与本机数据绑定，不跨机器迁移 |
| 数据根不做自动迁移 | 换根只是换指针；搬迁旧数据是用户的显式操作 |

**初始化规则由 Service 与 CLI 共用，幂等、只补缺失**：任何 `data/*.json` 的**内容**创建、补齐、
写回都发生在服务进程内；CLI 双击时只调用同一套规则补齐**缺失的**目录与默认 JSON、并把已有的
JSON 读一遍确认完整性（不写内容、不重置、不删除，见 4.4 与 11.13），其余业务改动一律通过管道
请求服务完成。因此「谁写 JSON 内容」仍然只有一个答案：**Service**。

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

**实现（提交 `188e85d`）：没有单独的 `IDGenerator` 类、也没有启动时的一次性扫描**，
上面那套语义落在 `FileService::next_file_id(const std::vector<FileRecord>&)` 里，
参数就是本次操作已经从 `data/file.json` 读出来的那份快照：

```text
前缀    "fmt-" + local_date_compact() + "-"        // local_date_compact() = YYYYMMDD
取最大  遍历快照，跳过前缀不符的、跳过尾巴不是纯数字的（手工改过的 id）、
        跳过 stoull 溢出的 → next = max(当天序号) + 1
跨天    不需要「重新扫描」：前缀里带的日期就是今天，昨天的记录前缀不符、自然被跳过
异常退出 下次分配照样从 file.json 现算，不存在内存计数器丢状态的问题
```

**为什么是「最大序号 + 1」而不是「条数」**：删除过的 id 不许复用（8.2 的「全局唯一」）。
若按条数算，删掉 `fmt-20261008-1` 之后下一条又会拿到 `-1`，历史日志与旧的
`trash.json` 记录就会指向两个不同的文件。

**并发安全靠业务锁，不靠独立 mutex**：`next_file_id()` 是 `commit_upload()`
（运行体在 `ServerRuntime::mutex_` 下调用，15.1）的一个内部步骤，
「读快照 → 算序号 → 追加 → 写回」整段在同一把锁内完成，所以两个上传不可能拿到同一个 N。
上传的下载阶段在锁外，但那一阶段不分配 id（10.2 的两段式）。

### 8.4 接口

设计草案的形状（仅供对照，**没有按这个建类**）：

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

**实际接口（提交 `188e85d`，`include/fmt/file/file.hpp`）**：

```cpp
class FileService {
public:
    // 上传第二阶段（锁内）：去重 → 重名 → next_file_id() → 搬到仓库 → 写 file.json
    Result<FileRecord> commit_upload(PreparedUpload& prepared);
    // …
private:
    Result<std::vector<FileRecord>> load_records() const;
    Status save_records(const std::vector<FileRecord>& records) const;
    std::string next_file_id(const std::vector<FileRecord>& records) const;   // 8.3
    std::filesystem::path bucket_path() const;
    // …
};
```

### 8.5 存储日期由 `file_id` 推出

`file.json` 里没有路径、也没有入库时间，日期就是 `file_id` 的日期片段：

```text
fmt-20261008-0   ->   repository/<user>/<bucket>/2026/10/08/<file_name>
                 ->   trash/<user>/.files/<bucket>/2026/10/08/<file_name>（软删除后）
```

两个入口都走同一个解析函数（`src/file/file.cpp`）：

```cpp
// fmt-YYYYMMDD-N 的形状检查：长度 ≥ 13、前 4 字符 "fmt-"、第 13 个字符 '-'、中间 8 位是数字
Result<DateParts> date_from_file_id(std::string_view file_id);

Result<std::filesystem::path> FileService::resolve_path(const FileRecord& record) const {
    // ① 先按 file_id 推：repository/<user>/<bucket>/YYYY/MM/DD/<file_name>
    // ② 推出来的路径不存在 → 在桶的日期树里（往下三层，find_in_date_tree()）
    //    找同名文件兜底
    // ③ 唯一命中才用；命中多个 → FMT-015 ConsistencyError（有歧义，交给人处理）；
    //    一个都没有 → FMT-002 FileNotFound「磁盘上找不到文件」
}
Result<std::filesystem::path> FileService::trash_path_of(const FileRecord& record) const {
    // 同样由 file_id 推：trash/<user>/.files/<bucket>/YYYY/MM/DD/<file_name>
    // PathManager::trash_file()，落点见 4.2 与 18.18
}
```

兜底这一段是**为「记录被手工改过、或文件被挪过」准备的**：日期树里同名文件有多份时
**不猜**，报 `FMT-015` 交给人——这与 9.2 的「绝不自动改名」是同一种态度。
`file_id` 形状不对同样是 `FMT-015`。

> **`trash_path_of()` 的消费方（提交 `9c3d2cb`）**：除了 `file.delete` 用它算
> `moved_to` / 落点，`file.get` 命中 `is_trash == true` 的记录时也用它给出
> `data.trash_path`（相对数据根、正斜杠）。回收站里的文件不在 `repository/` 下，
> 所以那种响应里**没有 `path`**——这是设计，不是缺失（10.2.4、12.3.2.1）。

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
与 file_id 同形的保留形状：fmt-YYYYMMDD-N（提交 9c3d2cb → FMT-106，见 9.3）
不可见格式字符：U+00A0 / U+00AD / U+200B–U+200F / U+202A–U+202E /
   U+2060–U+2064 / U+2066–U+2069 / U+FEFF（提交 a9af276 → 文件名 FMT-101、
   Bucket 名 FMT-202，见 9.3 与 9.5）
Windows 保留字符结尾：文件名以空格或 . 结尾
超长文件名（超过 255 字符，或完整路径超过 260 字符）
```

**超长直接拒绝，不允许程序自动截断。**

> **为什么连不可见字符也要拒（提交 `a9af276`）**：从聊天窗口、网页、终端复制一段文本
> 时常常夹进方向格式字符（`U+202A` 这类），屏幕上看不出来；如果进了名字，用户
> **没法把它重新敲一遍**，按名查找、排序、日志也全对不上。所以名字里出现它们一律拒绝
> （`validate_file_name()` → `FMT-101`、`validate_bucket_name()` → `FMT-202`），
> 消息里点名码位。**注意区分**：**路径参数**里的这类字符会被 `clean_user_path()`
> 清掉（9.5），照常能用；只有**名字**是不许带的。

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

**实现（提交 `188e85d`，`FileService::commit_upload()`）**：这条规则落在
「同用户 + **任何 Bucket** + **正常文件** + 同名」上，与 §37/§38 的措辞一致：

```text
判重名的唯一条件是 record.user == current_user && !record.is_trash
                  && iequals(record.file_name, prepared.file_name)   ← 提交 5bf2c1f
—— 不比较 record.bucket：Bucket 不构成命名空间，别的桶里的同名文件照样算冲突
—— is_trash == true 的记录不参与（8.2：Trash 不占正常文件名空间）
—— **比较不区分大小写**：doc.txt 与 DOC.TXT 必须判为同名。原来的 == 精确比较会放过它，
   而两者在 Windows 上落到同一个磁盘路径，第二次上传直接覆盖第一个文件的字节、
   第一条记录的 size/md5 还留在旧值上——**同一个磁盘文件被两条记录指向 + 字节被覆盖 +
   元数据失真**（提交 5bf2c1f「fix(file): compare names the way Windows does, or uploads
   overwrite data」；回归用例 File.大小写不同的同名必须被当成重名，18.22）

命中 → FMT-105 FileNameConflict（退出码 4）「同名文件已存在：<file_name>（换一个文件名再上传）」
      绝不自动覆盖、自动改名、自动加 (1)
```

MD5 去重（开发文档第 37 节）用的是**同一个作用域**，只是比较的字段换成 `md5`：
同用户 + 任何 Bucket + 正常文件命中 → `FMT-304 Md5Duplicate`。两条检查的顺序是
**去重在前、重名在后**（10.2），所以「同样的内容 + 同样的名字」报的是 `FMT-304`。

### 9.3 接口

**阶段 4 已落地**：`common/validation` 已在 `include/fmt/common/validation.hpp` /
`src/common/validation.cpp` 实现，**命名空间是 `fmt`**（不是下面那份草案里的
`fmt::common::validation`），函数名以实际为准：

```cpp
// Windows 保留设备名：CON / PRN / AUX / NUL / COM1-9 / LPT1-9（带扩展名同样算）
bool is_windows_reserved_name(std::string_view name);

// 名字是否与 file_id 同形（fmt-YYYYMMDD-N，前缀按 ASCII 折叠比较）——提交 9c3d2cb。
// 这种名字会让「先按 file_id 查、查不到再按名字查」的定位产生歧义，属保留形状。
bool looks_like_file_id(std::string_view name);

// 单个路径分量的字节上限
inline constexpr std::size_t kMaxNameBytes = 255;

// Bucket 名：失败一律 FMT-202 BucketNameInvalid
Status validate_bucket_name(std::string_view name);

// 文件名（上传用）：空 FMT-100 / 非法字符 FMT-101 / 分隔符或 . .. FMT-102 /
//                     保留名 FMT-103 / 超长 FMT-104 /
//                     与 file_id 同形 FMT-106（提交 9c3d2cb）
Status validate_file_name(std::string_view name);

// URL 合法性：**不再走单独的 validate_url()**。
// 提交 188e85d 的实现把它放在 file.cpp 的 prepare_upload() 里；
// 提交 a2b6cd1 起 http 与 https **都支持**，URL 的细分校验下沉到
// common/http_client.cpp 的 parse_url()：
//   http:// 或 https://  → 走网络下载（is_remote_url()）
//   含 "://" 但非 http/https → FMT-300「只支持 http:// 与 https:// 的来源：<来源>」
//   其余                 → 本地路径；存在性检查不过 → FMT-002 FileNotFound
//   parse_url() 内：缺主机名 / 端口非数字 / 端口越界 / 带 user:pass → FMT-300
//   path 为空            → 按 "/" 处理（例 http://example.com）
// Status validate_url(std::string_view url);
```

判定顺序（两类名称共用，差别只在错误码粒度）：

```text
1. 非空
2. ≤ kMaxNameBytes（255 字节；中文一个字 3 字节，所以名字不能靠「字数」估）
3. 不含路径分隔符 / 与 \，且不是 "." 或 ".."
4. 不含控制字符（< 0x20 与 0x7F）与 < > : " | ? *（文件名把两者都归 FMT-101）
4.5 **（提交 a9af276）不含不可见格式字符**（invisible_characters() 非空即拒）：
   文件名 → FMT-101 FileNameInvalidChar、Bucket 名 → FMT-202 BucketNameInvalid，
   消息点名码位（「请把名字重敲一遍」）；判定在非法字符之后、保留设备名之前
5. 不是 Windows 保留设备名（9.1 的名单，主干比对、大小写不敏感）
6. **（提交 9c3d2cb，只对文件名）不是与 file_id 同形的保留形状** fmt-YYYYMMDD-N
   —— 判定在保留设备名**之后**，命中 → FMT-106 FileNameLikeFileId
7. 不以点或空格结尾（Windows 会静默截断，宁可拒绝）
```

**第 6 条的判定（`looks_like_file_id()`，提交 `9c3d2cb`）**：

```text
形状      fmt-YYYYMMDD-N
最短      14 个字符（4 + 8 + 1 + 1）
前缀      "fmt-" 按 ASCII 折叠比较（file_id 的比对本身就不区分大小写）
日期段    恰好 8 位数字（name[4..11]）
分隔符    name[12] == '-'
序号段    name[13..] 全是数字且非空（不限制位数）
拒绝      fmt-20261008-0 / FMT-20261008-0 / fmt-20261008-123
放行      fmt-20261008-0.txt（带扩展名就不是 id）/ my-fmt-20261008-0 /
          fmt-20261008（没有序号）/ fmt-2026100-0（日期 7 位）
```

**为什么它是保留形状**：定位是「**先按 `file_id` 查、查不到再按名字查**」（10.2.4）。
若一个文件就叫 `fmt-20261008-0`，而另一个文件的 `file_id` 恰好是它，按名字提交的
**删除**就会先命中 id 那条记录——删错对象。所以这个形状与 9.1 的 Windows 保留设备名
**同类**：名字语法合法，但会被定位逻辑（或文件系统）吃掉，必须在上传时就拒绝。
`validate_file_name()` 是**显式名与从来源推断的名字共用的那道校验**，两条路都拦。
旧数据里已经存在的这种名字不静默处理，见 10.2.4 的歧义判定（只给 `file delete`）。

错误码对照：

| 失败原因 | `validate_file_name` | `validate_bucket_name` |
|---|---|---|
| 空 | `FMT-100` | `FMT-202` |
| 含 Windows 非法字符 / 以点或空格结尾 | `FMT-101` | `FMT-202` |
| 含路径分隔符 / 是 `.` 或 `..` | `FMT-102` | `FMT-202` |
| Windows 保留设备名 | `FMT-103` | `FMT-202` |
| 超过 255 字节 | `FMT-104` | `FMT-202` |
| **含不可见格式字符**（提交 `a9af276`） | `FMT-101`（消息点名码位） | `FMT-202`（消息点名码位） |
| 与 `file_id` 同形（`fmt-YYYYMMDD-N`，提交 `9c3d2cb`） | `FMT-106`（退出码 2） | —（Bucket 名不受限） |

`FMT-105`（重名）不在本表内——它回答的是「这个名字是否已存在」，属业务规则（9.2）。
`FMT-106` 属 `FMT-1xx` 文件名校验一组，编号排序在 `FMT-105` 之后。

**校验只回答「这个名字能不能用」，不回答「这个名字是否已存在」**——后者是业务规则
（Bucket 用 `FMT-201` / `FMT-200`，文件用 `FMT-105 FileNameConflict`）。中文、空格、
UTF-8 多字节名称都合法（`工作`、`小谷姐姐麻辣烫.jpg`、`a b.txt` 均通过）。
单元测试：`tests/validation_test.cpp`。

> **下面这份是最初的接口草案，已被上面的实现取代**（保留以便对照：命名空间不同、
> `validate_url` 尚未落地）：

```cpp
namespace fmt::common::validation {

// 文件名合法性：返回具体错误码（FMT-100 ~ FMT-104）
Status validate_file_name(const std::string& name);

// 是否为 Windows 保留设备名（含带扩展名形式）
bool is_reserved_device_name(const std::string& name);

// URL 合法性：仅允许 http / https
// **口径已改（提交 188e85d，提交 a2b6cd1 再改一次）**：没有这个 validate_url()——
//   来源分支判定在 src/file/file.cpp 的 prepare_upload() 里，URL 细分校验在
//   src/common/http_client.cpp 的 parse_url() 里，**http:// 与 https:// 都允许**
//   （见 10.2.2 与 9.3 末尾的说明）
Status validate_url(const std::string& url);

}
```

---

### 9.4 名称比较与大小写（`iequals()` / `to_lower()` / `canonical_name()`）

**口径：凡按名字或标识定位，一律不区分大小写**（文件名、`file_id`、桶名、回收站条目名；
提交 `5bf2c1f` 定下，`9c3d2cb` 收紧实现）。理由不是「体验」，而是 Windows 的文件系统与路径
本身不区分大小写：`doc.txt` 与 `DOC.TXT` 落到**同一个磁盘路径**，用 `==` 精确比较会
把它们当成两个名字放过，第二次上传直接覆盖第一个文件的字节（9.2 的覆盖事故）。

**实现约束（提交 `9c3d2cb`，`src/common/string.cpp`）：`iequals()` 只折叠 ASCII。**

```cpp
bool iequals(std::string_view left, std::string_view right);   // 长度不同直接 false
// 逐字节比较：value < 0x80 才走 std::tolower(value)，>= 0x80 的字节**原样比较**
// —— UTF-8 多字节序列的字节都 >= 0x80，交给 std::tolower 会随 locale 变化：
//    C locale 下虽然是恒等，但一旦有人调用 setlocale，中文名字就可能被改坏。
//    这是「比较一律不区分大小写」那条口径的实现约束，不是可选的优化。

std::string to_lower(std::string_view text);   // 同样只折叠 ASCII（>= 0x80 原样保留）
```

**谁用它**（与 10.1 / 10.2.4 / 18.22 一致）：

```text
file   commit_upload()  重名判定（9.2，命中 → FMT-105）
       get_by_name()    按名字查
       locate_record()  ① file_id ② 文件名 ③ 已在回收站的名字（10.2.4）
bucket list()           is_current = iequals(info.name, config_.current_bucket)
       remove()         was_current = iequals(config_.current_bucket, actual)
       find_trashed()   回收站名字与「原桶名」两轮定位
```

**标识比较一律不区分大小写（提交 `6a40742` 补齐）**：`FileService::get_by_id()` 现在也用
`iequals(record.file_id, file_id)`——所以 `file get FMT-20261008-0`（大写）也查得到，
与 `file delete` 的 `locate_record()` 完全一致；用例 `File.列表与查询` 里加了这条断言。

> **原口径「`get_by_id()` 用 `==` 精确比较、与 delete 不一致、属遗留差异」已作废**
> （那是提交 `5bf2c1f` 到 `6a40742` 之间的实况）：同一个标识不该因为大小写两种结果，
> 现在两处都是 `iequals`，`help file` 末行「名字与 file_id 的比较都不区分大小写
> （Windows 习惯）」与实现一致。

**`canonical_name()`（提交 `9c3d2cb`，`BucketService` 私有）**：

```text
作用    把用户敲的桶名规范化成**磁盘上的实际名字**：不区分大小写地扫
        repository/<user>/ 下的目录（用 iequals 比对 entry 名），返回磁盘上的拼写；
        扫不到（目录不存在、或没有同名项）才退回 to_lower(name)
为什么  Windows 目录不区分大小写，WORK 与 work 本来就是同一个目录。不统一拼写，
        current_bucket、file.json 的 bucket、.original 的 original 会各留一份，
        日后比对与恢复都会踩坑（10.1、开发文档第 27～30 节）
用在哪  use（落盘 current_bucket）、get（返回的 name）、remove（回收站目录名、
        .original 的 original、file.json 里按桶名匹配的记录）；create 不走它，
        而是在建目录前先 to_lower()
```

---

### 9.5 粘贴污染：`clean_user_path()` / `invisible_characters()`（提交 `a9af276`）

**用户报的现象**：`fmt> file upload ‪C:\...\Pictures\pet-food-store\头像\asdva.jpg`
→ `FMT-002 本地文件不存在`，可文件**确实存在**（130184 字节）。原因是路径首尾各夹了
一个**不可见的方向格式字符**：开头 `U+202A`（LEFT-TO-RIGHT EMBEDDING）、结尾
`U+202C`（POP DIRECTIONAL FORMATTING）——从聊天窗口、网页、终端复制路径时常见，
**屏幕上完全看不出来**。CLI 走 `wmain`，中文路径本身没问题；是这两个字符被当成了
路径的一部分，`exists()` 因此说不存在。

```cpp
// include/fmt/common/string.hpp / src/common/string.cpp（提交 a9af276）
// 清掉粘贴污染：不可见格式字符 + **成对**引号（Explorer「复制路径」给路径套一对引号）
std::string clean_user_path(std::string_view text);
// 文本里出现的不可见字符，按顺序去重，形如 {"U+202A", "U+202C"}——报错时点出码位用
std::vector<std::string> invisible_characters(std::string_view text);
```

```text
清掉的码位   U+00A0（不换行空格）、U+00AD、U+200B–U+200F、U+202A–U+202E、
             U+2060–U+2064、U+2066–U+2069、U+FEFF
另外         去掉首尾空白；`"` / `'` / `“”` **成对**才去掉
             （不成对不动，免得改掉名字里真的带引号的情况）
不动         中文等多字节序列（合法 UTF-8 原样保留）
```

**两处调用（一处收口 + 一处纵深防御）**：

```text
① src/service/commands.cpp 的 argument()  —— **所有**位置参数读取的唯一入口，
   CLI 与 HTTP 共用（12.3.2.1）：粘贴污染在这里就被清掉，这是「一处收口」
② src/file/file.cpp 的 prepare_upload()  —— 上传**来源**与**显式文件名**再清一次；
   将来有人绕过 argument() 直接调它也不受影响（纵深防御）
```

**名字里不允许**（9.1、9.3）：`validate_file_name()` → `FMT-101`、
`validate_bucket_name()` → `FMT-202`，消息点名码位。理由：名字要长期存下来、
还要被用户再敲一遍，屏幕上看不出来的字符没法重敲，按名查找 / 排序 / 日志也全对不上。

**清掉后仍找不到的报错口径**（10.2.3、11.14 也有样例）：`prepare_upload()` 在本地路径
存在性检查失败时，用 `invisible_characters(raw_source)` 把**原始输入**里的码位点名：

```text
有不可见字符    FMT-002：「本地文件不存在：<清理后的路径>（你粘贴的路径里有不可见字符
                U+202A、U+202C，它会让路径对不上；已自动清掉，请检查路径是否还有别的问题）」
只去了引号/空白 「本地文件不存在：<清理后的路径>（已去掉粘贴带进来的引号或空白）」
什么都没清      原来那句「本地文件不存在：<路径>」
```

用例：`String.清理粘贴带进来的路径污染`、`File.粘贴路径里的不可见字符会被清掉`
（后者**复刻用户场景**：中文目录 `头像/` + `U+202A`/`U+202C` 包裹 → 上传预检成功）。

---

## 10. 业务服务

各服务的流程规范见 `FMT 开发文档.md` 第 27～63 节，本节给出接口与实现要点。

### 10.1 BucketService

**阶段 4 已落地**（`include/fmt/bucket/bucket.hpp`、`src/bucket/bucket.cpp`）。
Bucket **没有独立 ID**：`repository/<user>/<bucket>/` 这个目录就是 Bucket 本身，
存在性 = 目录存在；`current_bucket` 存在 `config.json` 里。

```cpp
// 回收站里桶级记录的权威文件名（位于 trash/<user>/ 下）。
inline constexpr const char* kOriginalIndexName = ".original";

struct BucketInfo {
    std::string name;
    bool is_current = false;
};

// 创建结果（提交 9c3d2cb）：名称统一转小写后再建目录，调用方据此提示用户。
struct BucketCreation {
    std::string requested;         // 用户原本敲的（可能带大写）
    std::string name;              // 实际创建的名字（小写）
    bool renamed = false;          // requested != name
    bool became_current = false;   // 是不是顺手设成了当前 Bucket
};

// 删除 Bucket 前的预检（只读，提交 0fc242b）：桶里有没有东西、删掉之后要怎么才能拿回来。
struct BucketDeleteCheck {
    std::string bucket;
    bool is_current = false;
    std::size_t files = 0;
    std::uintmax_t bytes = 0;
    bool has_content = false;  // 有内容就要提醒 + 确认；空桶不打扰用户
    std::string message;
};

// 删除 Bucket 的结果：删到哪儿去了（回收站里的名字）、影响了几个文件、删的是不是当前 Bucket。
// 调用方（管道/HTTP 响应）要如实回报这些，而不是只说一句「成功」。
struct BucketRemoval {
    std::filesystem::path moved_to;
    std::string trashed_name;      // trash/<user>/<trashed_name>
    std::size_t files_affected = 0;
    bool was_current = false;
};

// 回收站里的一个 Bucket 条目；directory_present=false = 索引里有、目录没了（只报告）。
struct TrashBucket {
    std::string trashed_name;
    std::string original_name;   // 索引里没有则为空（回退时拒绝）
    std::string deleted_at;
    bool directory_present = true;
};

// 单个条目的详情（trash get 用，提交 4fee290）。
struct TrashBucketDetail {
    TrashBucket bucket;
    std::filesystem::path directory;
    std::size_t file_count = 0;      // 目录里的实际文件数
    std::uintmax_t byte_count = 0;   // 总字节数
};

// 永久删除之后的结果（trash delete 用，提交 4fee290）。
struct TrashPurge {
    std::string trashed_name;
    std::string original_name;
    std::size_t removed_files = 0;    // 真正删掉的磁盘文件数（删前统计）
    std::size_t removed_records = 0;  // file.json 里清掉的记录数
};

class BucketService {
public:
    BucketService(const PathManager& paths, Config& config, Logger* logger);

    Result<BucketCreation> create(std::string_view name);   // 9c3d2cb：原为 Status
    Result<std::vector<BucketInfo>> list();
    Result<BucketInfo> get(std::string_view name);
    Status use(std::string_view name);
    Result<BucketRemoval> remove(std::string_view name);   // 注意：不是 Status
    // 删除前的预检：只读。桶里有东西就提醒「之后只能整体恢复这个桶」（提交 0fc242b）。
    Result<BucketDeleteCheck> check_remove(std::string_view name) const;

    // 回收站里的 Bucket 列表（索引 + 目录扫描；异常状态如实报告，不擅自修）。
    Result<std::vector<TrashBucket>> list_trashed();

    // 回退一个被删除的 Bucket：整目录搬回 repository/<user>/<原名>。
    // identifier 可以是回收站里的名字，也可以是原桶名（同名多条时必须用前者）。
    Result<TrashBucket> restore(std::string_view identifier);

    // 单个条目的详情：目录、文件数、占用字节数；索引里有、目录没了如实报 present=false。
    Result<TrashBucketDetail> get_trashed(std::string_view identifier);   // 4fee290

    // **永久删除，不可恢复**：删目录 → 清该桶的 file.json 记录 → 摘索引。
    // 调用方必须先取得用户确认（服务端另外要求请求里带 force）。
    Result<TrashPurge> purge(std::string_view identifier);                // 4fee290

    // Bucket 目录：repository/<user>/<bucket>。
    std::filesystem::path directory_of(std::string_view name) const;

    // 当前 Bucket 不存在时置空并落盘；不做任何自动切换。
    Status refresh_current_bucket();
};
```

要点：

| 操作 | 要点 |
|---|---|
| `create` | **提交 `9c3d2cb`：先 `to_lower()`（只折叠 ASCII，中文不受影响）→ 再校验 → 再建目录**（`create WORK` 建成 `work`，`renamed = true`，返回值是 `BucketCreation`）。`current_user` 空 → `FMT-604`；名称校验失败 → `FMT-202`；目录已存在 → `FMT-201`（所以再敲一次 `create WORK` 会被当成同一个桶）；建 `repository/<user>/<小写名>/`；**`current_bucket` 为空则设为它并保存配置**（第二个及以后的桶不抢「当前」，此时 `became_current = true`）。日志里名称被转换时注明「（名称统一小写，由 WORK 转换）」 |
| `list` | 扫 `repository/<user>/` 下的**目录**（文件忽略），按名称排序，标出 `is_current`；目录不存在时返回空列表，不报错。**`is_current` 的比较不区分大小写（提交 `5bf2c1f`）**：目录名按磁盘实际拼写，`config_.current_bucket` 也已被 `use` 规范化，所以用 `iequals(info.name, config_.current_bucket)`（9.4、18.22） |
| `get` | 名称空 → `FMT-001`；目录不存在 → `FMT-200`；返回 `{name, is_current, path}`。**提交 `9c3d2cb`：`name` 回的是 `canonical_name()` 的结果——磁盘上的实际名字**（`get WORK` 显示 `work`），`is_current` 同样用 `iequals`。**提交 `6a40742`：`path` 也用 `found.name`（规范化后的名字）拼**，所以 `bucket get WORK` 的 `bucket` 与 `path` 都是 `work`，不会自相矛盾 |
| `use` | 名称空 → `FMT-001`；目录不存在 → `FMT-200`；只改 `current_bucket` 并保存 `config.json`。**不修改 Bucket 本身**（不移动文件、不改 file.json）。**提交 `9c3d2cb`：落盘时规范化成磁盘上的实际名字**（`current_bucket = canonical_name(name)`，`use WORK` 存的是 `work`）——否则 `WORK` / `work` 会在 `current_bucket`、`file.json` 的 `bucket`、`.original` 的 `original` 里各留一份（19.1 那条待决项由此定稿） |
| `remove` | 目录不存在 → `FMT-200`；**提交 `9c3d2cb`：先 `actual = canonical_name(name)`**，之后搬目录、记 `.original` 的 `original`、匹配 `file.json` 的记录、算 `was_current` 一律用 `actual`；接着按第 30 节：**先读 `.original`**（读不出来 → `FMT-006`，不搬）→ 搬目录（名字一律带时间戳）→ 写 `.original` 一条（写不进去就搬回）→ 标记文件 → 处理当前桶；**若删除的是 `current_bucket`，置空，不自动切换**（`was_current` 的比较**不区分大小写**，提交 `5bf2c1f`）；返回 `BucketRemoval` |
| `list_trashed` | 读 `.original` + 扫 `trash/<user>/` 下的目录：索引有目录没了 → `directory_present=false`；目录有索引没有 → `original_name` 留空照样列出。**扫描带形状检查（提交 `4fee290`）**：跳过点开头的条目（`.files` / `.original`），且**只认 `<名字>_<14 位时间戳>` 或 `<名字>_<14 位时间戳>_<1-3 位序号>` 形状的目录**，别的东西不会被误当成桶级条目。按 `trashed_name` 排序；**异常只报告，不清理** |
| `restore` | 先用 `trashed_name` 精确匹配，再按 `original_name` 且**必须唯一**（多条 → `FMT-001` 并列候选）；`original_name` 空 → `FMT-001`（缺 `.original` 记录）；回收站目录不在 → `FMT-400`；目标 `repository/<user>/<原名>` 已存在 → `FMT-401`（**整单拒绝**）；否则整个目录一次 `rename` 搬回，然后**先减索引**（减不掉就把目录退回去）再翻 `trash_reason=="bucket"` 的记录 |
| `get_trashed` | **提交 `4fee290`**。定位与 `restore` 完全一致（共用私有 `find_trashed()`）；索引里有、目录没了**不报错**，返回 `present=false`（不擅自清理）；孤儿目录（索引里没有）按目录名也能查到（`original` 空、`present=true`）；两者都没有 → `FMT-400`。返回 `{trashed, original, deleted_at, present, path, files, bytes}`：`path` 相对数据根、正斜杠，`files` 递归数普通文件，`bytes` 累加 `file_size`。**只有单条查询遍历目录**，`list_trashed` 不做 |
| `purge` | **提交 `4fee290`，永久删除、不可恢复**。三步顺序见下。幽灵条目（索引有、目录没了）也能删；孤儿目录（目录在、索引没有）按目录名删。返回 `TrashPurge`（`TrashService` 再包成统一 `TrashEntry`，10.4）。**服务端另有一道确认**：`trash.delete` 的 `args` 里没有 `force == true` 就返回 **`FMT-016 ConfirmRequired`**（退出码 2；提交 `711da4c` 起不再复用 `FMT-001`，「永久删除不可恢复，需要确认（force = true）」） |
| `check_remove` | **提交 `0fc242b`，只读预检**：`canonical_name` 定位（不存在 → `FMT-200`）→ 递归数普通文件与字节数 → `has_content = files > 0`；有内容时消息写明「删除后整个桶移入回收站，之后只能整体恢复这个桶，无法只恢复其中某个文件」，是当前桶再补「当前 Bucket 会被置空」。服务端 `bucket.delete` 在没带 `force` 且 `has_content` 时返回 **`FMT-016`** |
| `directory_of` | `repository/<user>/<bucket>`；**它按传入的名字拼**（`repo/<user>/<name>`），所以命令层要传 `canonical_name()` 的结果，`bucket get` 的 `path` 才不会与 `bucket` 字段打架（提交 `6a40742`，`src/service/commands.cpp` 传 `found.name`） |
| `refresh_current_bucket` | `current_bucket` 非空且目录不存在 → 置空 + 保存，记一行 WARN；**绝不自动选择别的 Bucket**（开发文档第 61 节） |

**`remove` 的步骤（与开发文档第 30 节一致；顺序有意为之）**：

```text
0. canonical_name(name)       提交 9c3d2cb：actual = 不区分大小写地在 repository/<user>/
                              下找到的实际桶名（找不到才退回 to_lower(name)）；
                              下面第 2 步的基名、第 5 步匹配 file.json 的桶名都用 actual
1. load_original_index        先读 trash/<user>/.original：
                              文件不存在 → 空表；解析失败/版本不认 → FMT-006 / FMT-011，
                              **到此为止，一个字都不搬**
2. unique_trashed_name        基名 = <actual> + "_" + compact_stamp()
                              （local_timestamp() 去掉 - : 空格 → YYYYMMDDHHMMSS）；
                              trash/<user>/ 下已存在就依次试 _2、_3……，绝不覆盖
3. move_bucket_to_trash       repository/<user>/<actual>/ → trash/<user>/<trashed_name>/
                              （ensure_directory(trash_user_root()) 之后 rename，失败 → FMT-009）
4. save_original_index        追加 {trashed, original = actual, deleted_at}（7.3.2）并整文件落盘；
                              **写失败就把目录 rename 回原位**再报错——磁盘与索引不许不一致
5. set_bucket_files_trash_flag(user, actual, trashed=true, &files_affected)
                              只改 file.json 里 user + bucket（用 actual 匹配）都匹配、
                              且 is_trash 原本为 false 的记录：置 is_trash=true、
                              trash_reason="bucket"；
                              file.json 缺失 → 视为 0 个文件；别的桶与 "file" 的记录一律不动
6. 当前桶处理                 was_current = iequals(current_bucket, actual) → 置空 + 保存配置
```

**`purge()` 的步骤（提交 `4fee290`；顺序是刻意的）**：

```text
0. find_trashed(identifier)   回收站名字优先 → 原桶名（必须唯一，多条 → FMT-001 并列候选）；
                              **两轮比较都不区分大小写**（提交 5bf2c1f：回收站名里的桶名
                              那一段可能是用户按不同大小写敲的，18.22）；
                              **找不到不算错**：孤儿目录走「按目录名处理」那条路
1. 删磁盘数据                 directory_exists 时先递归数普通文件（removed_files），
                              再 std::filesystem::remove_all(directory)；
                              失败 → FMT-008 StorageError，**什么都没变**（索引还在，可以重来）
2. remove_bucket_file_records 清 file.json 里 user + bucket 匹配、is_trash == true
                              **且 trash_reason == "bucket"** 的记录（removed_records）；
                              "file" 的记录**绝不动**——那些文件的数据不在这个目录里
3. 摘索引                     found 时从 .original 去掉这一条并整文件落盘；
                              **顺序刻意如此**：先删数据，中途失败不会留下
                              「索引没了、目录还在」的幽灵条目；反过来先摘索引再删目录，
                              失败就会留下谁也认不出的孤儿目录
```

幽灵条目（索引有、目录没了）第 1 步跳过（`removed_files = 0`），照样能删掉——
否则它永远清不掉，只能一直被 `trash list` 标成 `present: false`。

> **已知缺口（阶段 6/7 待办）**：`purge()` **没有**清理 `share.json`。开发文档第 60 节要求
> 永久删除时「清理相关 Share」，但 share 模块属阶段 6，目前没有任何相关动作；
> 一个被永久删除的桶如果曾有过分享记录，那些记录会留在 `share.json` 里。

Bucket **不生成 `bucket_id`**，用名称标识。日志模块名统一 `Bucket`（桶级永久删除那一条记在
`Trash` 模块下：条目名 + 文件数 + 记录数，提交 `4fee290`）。

**`refresh_current_bucket()` 已接线（运行时生效）**：

```text
接线点   ServerRuntime::start()      服务启动、上下文建好后
         ServerRuntime::apply_root() 数据根切换（hello 触发的那条路径）换根后
调用位置 两者都在业务锁（mutex_）内，经由 ServerRuntime::refresh_current_bucket_locked()
行为     current_bucket 指向的目录不存在 → 置空并 save_config()；存在 → 什么都不做
失败处理 返回错误只记一条 WARN（模块 Bucket），不影响启动/换根
```

放在锁内是必要的：置空动作与并发的 `bucket.use` / `bucket.create` 走同一把锁，
不会出现「刚把 current_bucket 设成 A，启动校验又把它清掉」的覆盖。
单测：`tests/bucket_test.cpp` 的 `当前Bucket失效时置空`、
`tests/service_test.cpp` 的 `启动时把失效的当前Bucket置空`（同时断言「存在的当前 Bucket
不能被误清」）。

### 10.2 FileService

**设计草案（仅供对照）**：

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

**实际接口（提交 `188e85d`，`include/fmt/file/file.hpp`）——`upload()` 被拆成两段**：

```cpp
// 第一阶段：**锁外**的长任务。不依赖 FileService，是自由函数。
Result<PreparedUpload> prepare_upload(const PathManager& paths, const std::string& source,
                                      const std::string& name, std::uintmax_t size_limit,
                                      Logger* logger);

class FileService {
public:
    FileService(const PathManager& paths, Config& config, Logger* logger);

    // 第二阶段：**锁内**的快速登记。接手之后临时文件的生命周期归它管：
    // 任何失败路径都会删掉它（开发文档第 35 节）。
    Result<FileRecord> commit_upload(PreparedUpload& prepared);

    Result<std::vector<FileRecord>> list();                       // 10.2.4
    Result<FileRecord> get_by_id(std::string_view file_id);        // 10.2.4（提交 6a40742 起 iequals）
    Result<FileRecord> get_by_name(std::string_view file_name);    // 10.2.4
    Result<FileRecord> remove(std::string_view file_id_or_name);   // 10.2.4（软删除）
    // ↑ 提交 0ad9efc：参数名由 file_id 改成 file_id_or_name，
    //   定位规则与 file get 完全一致（先当 file_id、再当文件名）

    // 删除前的预检：只读，不改任何东西（提交 711da4c）。
    Result<FileDeleteCheck> check_remove(std::string_view file_id_or_name) const;

    // 回收站读侧（提交 0fc242b）：
    //   只列**文件级**条目（is_trash + trash_reason == "file"），随桶删除的不列；
    //   deleted_at 缺了就从事务性的 trash.json 老记录里补（只读兼容）。
    Result<std::vector<FileRecord>> list_trashed();
    struct FileRestoreCheck {
        FileRecord record;
        bool bucket_deleted = false;   // 随桶删除 → 只能整体恢复桶（FMT-402）
        bool conflict = false;         // 目标位置已有同名正常文件（FMT-401）
        FileRecord conflicting;        // 冲突的那一条（file_id 要报给用户）
        bool missing = false;          // 回收站里找不到数据（FMT-002）
        std::filesystem::path source;
        std::filesystem::path target;
        std::string message;
    };
    Result<FileRestoreCheck> check_restore(std::string_view file_id) const;   // 只读
    Result<FileRecord> restore(std::string_view file_id);   // 搬回仓库 + 复位三个字段
    Result<FileRecord> purge(std::string_view file_id);     // 删数据 → 删记录 → 清老 trash.json

    Result<std::filesystem::path> resolve_path(const FileRecord&) const;      // 8.5
    Result<std::filesystem::path> trash_path_of(const FileRecord&) const;     // 8.5

private:
    Result<std::vector<FileRecord>> load_records() const;
    Status save_records(const std::vector<FileRecord>& records) const;
    std::string next_file_id(const std::vector<FileRecord>& records) const;   // 8.3
    // 提交 0ad9efc 新增：按 file_id 或文件名定位一条记录，并给出**准确**的失败原因
    // （「名字存在但已在回收站」不能报成「文件不存在」）。**remove() 的定位走它**；
    // get 的「命令层」仍走 get_by_id() + get_by_name()（第 42 节的规则、两套代码）。
    // 提交 5bf2c1f：三处比较一律用 iequals()（ASCII 折叠，18.22）。
    // 提交 9c3d2cb：分两步各自记下命中（by_id / by_name），两者命中**不同**记录时
    //   返回 FMT-001 报歧义（消息点名两条记录，让用户直接用 file_id）——**只服务删除**；
    //   iequals() 同时收紧成只折叠 ASCII（9.4）。
    Result<std::size_t> locate_record(const std::vector<FileRecord>& records,
                                      std::string_view key) const;
    // 提交 711da4c：预检与定位共用一份判定（两维各自命中哪一条），
    // locate_record() 与 check_remove() 都基于它，避免两处规则漂移。
    struct RecordMatch { bool has_id; bool has_name; std::size_t by_id; std::size_t by_name; };
    RecordMatch match_record(const std::vector<FileRecord>& records, std::string_view key) const;
    // 提交 0fc242b：记录**按规则推**出来的仓库目标路径（file_id 定日期，记录定用户/桶/文件名），
    // 回退时往这里搬回去；与 resolve_path() 的区别是**不做兜底搜索**。
    Result<std::filesystem::path> repository_path_of(const FileRecord& record) const;
    std::filesystem::path bucket_path() const;
    const PathManager& paths_;
    Config& config_;
    Logger* logger_ = nullptr;
};

// file.json 的一条记录（7.1）
struct FileRecord {
    std::string file_id;  std::string user;  std::string bucket;  std::string file_name;
    std::string extension;     // 小写、含点；没有扩展名时为空
    std::string file_type;     // image / text / video / archive / other
    std::uintmax_t size = 0;
    std::string md5;           // 32 位小写十六进制
    bool is_trash = false;
    std::string trash_reason;  // "bucket" / "file" / 空
    std::string deleted_at;    // 进回收站的时间（提交 0fc242b；file.json 是唯一权威）
};
```

**为什么分两段（本轮最重要的设计决定）**：下载可能几十秒到几分钟。持着运行体那把业务锁
去下载，会把 `bucket list`、`trash *` 和浏览器的业务请求一起卡住（15.1 ④、18.16 的
「阶段 5 必须定锁粒度」）。**长任务不持锁，锁只保护元数据提交那一下**：

```text
ServerRuntime::run_upload(args)        // 管道与 HTTP 共用这一份（12.3.2）
  ① 锁下取快照    paths / logger / size_limit，随即放锁
  ② 锁外 prepare  prepare_upload()：下载或复制到 temp/、边写边算 MD5、边判大小上限
  ③ 锁内 commit   commit_upload()：MD5 去重 → 文件名冲突 → 分配 file_id → 移动到仓库
                  → 写 file.json
```

`file.upload` 是**唯一**走这条路径的 op：`ServerRuntime::handle()`（管道）与
`BusinessHandler`（HTTP，13.10 注册）都在取锁**之前**把它拦下来交给 `run_upload()`，
其余业务命令仍是「取锁 → `execute_business()`」。`commands.cpp` 的 `file_command()`
里**没有** `file.upload` 分支——它只处理 `file.list` / `file.get` / `file.delete`。

> **提交 `d3aeb3d` 之后多了一条近亲：`file.upload_stream`**（HTTP 流式上传用）。
> 它的**第一阶段不同**：HTTP 处理器用 `ContentReader` 把请求体**边收边写**到
> `temp/`（边判上限，超了立刻中止并删暂存文件），落盘后再交给业务层——
> op 的 `args.argv = [暂存路径, 文件名]`，第二阶段与 `file.upload` 同一套
> （补算大小与 MD5 后入库，**只有一次移动**）。所以「上传必须先有完整文件」这条
> 只对 `file.upload` 成立：流式那条连「本地路径」都不需要（12.3.2）。
> `file.upload` 仍是**管道侧唯一**走 `run_upload()` 的 op。

**换根不会插进第 ② 段**：换根只由 `hello` 触发，而管道的 accept/serve 是串行的
（15.1 ①）——上传期间不会再处理第二个请求，所以第 ① 段拿到的快照在整段下载期间都成立。
HTTP 侧每个请求各自取当前 `context_`，真正的写只在第 ③ 段的锁内发生。

**上传完整流程**（开发文档第 32 节）：

```text
第一段（锁外）
1.  检查来源非空；缺来源 → FMT-001（run_upload 层）/ FMT-300（prepare_upload 层）
2.  文件名：用户给的，或从来源推断（10.2.1）；推断为空 → FMT-100
3.  文件名合法性检查（9.1；**提交 9c3d2cb 起含第 6 条「与 file_id 同形」→ FMT-106**，
    显式名与从来源推断的名字走同一道校验）
4.  校验来源分支：http:// 与 https:// 都走 `common/http_client` 下载；含 "://" 但非
    http/https → FMT-300；本地路径不存在 → FMT-002（10.2.2）
5.  创建 <数据根>/temp/fmt-upload-<随机>-<序号>.tmp
6.  流式写入（64 KiB 一块），边写边算 MD5、边判大小 ≤ max_upload_size
7.  完整性检查（Content-Length 与实收字节必须一致）→ 不符 FMT-301
8.  临时文件落盘，拿到 size 与 32 位小写 MD5

第二段（锁内）
9.  检查 current_user（FMT-604）/ current_bucket（FMT-305）/ 桶目录存在（FMT-200）
10. MD5 去重检查（同用户 + 任何 Bucket + 正常文件 → FMT-304）
11. 文件名冲突检查（同用户 + 任何 Bucket + 正常文件 + 同名 → FMT-105）
12. 生成 file_id（8.3）→ 由它推出日期目录（8.5）
13. 移动临时文件到 repository/<user>/<bucket>/YYYY/MM/DD/（同卷 rename，跨卷复制+校验+删源，15.5）
14. 写入 file.json；写不进去就删掉刚提交的仓库文件（开发文档第 40 节）
15. 完成；记 INFO 日志「入库：<file_id> <file_name> -> <仓库路径>」
```

顺序上**去重在前、重名在后**；两条失败都不写 metadata、不生成 id、临时文件被删掉。

### 10.2.1 文件名推断（`file_name_from_source()`）

省略文件名时按顺序处理，**任何一步都不拿主机名当文件名**：

```text
① 砍掉 scheme://host，只看路径部分（"://" 后第一个 "/" 起；没有 "/" 则路径为空）
② 砍掉查询串与锚点（第一个 '?' 或 '#' 起全部截掉）
③ 去掉结尾的 '/' 与 '\'（可能不止一个）
④ 取最后一段
⑤ 百分号解码（common/string 的 url_decode，URL 里的中文是 %XX 编码的）
```

对照（`tests/file_test.cpp` 的 `File.从来源推断文件名` 逐条断言）：

```text
https://example.com/a/b/test.zip           -> test.zip
http://example.com/a.bin?token=1#x         -> a.bin
http://example.com/dir/                    -> dir
http://example.com/%E5%B7%A5%E4%BD%9C.txt  -> 工作.txt
D:\Data\test\a.txt                         -> a.txt
/tmp/x/y.log                               -> y.log
http://example.com/                        -> （空串）→ prepare_upload 报 FMT-100
```

推断为空**不猜**：报 `FMT-100 FileNameEmpty`「无法从来源推断文件名，请显式给出文件名」，
要用户显式给名字，而不是存一个叫 `example.com` 的文件。

### 10.2.2 URL 支持范围与下载客户端（**口径已改两次：`188e85d` → `a2b6cd1`**）

**判定顺序（`prepare_upload()`，提交 `a2b6cd1` 的实况）**：

```text
http:// 或 https://        -> 走网络下载（is_remote_url()）
含 "://" 但不是 http/https  -> FMT-300 UrlInvalid
                              「只支持 http:// 与 https:// 的来源：<来源>」
其余                       -> 本地路径，存在性检查不过报 FMT-002 FileNotFound
```

> **关键更正**：`ftp://` 之类的**错误码是 `FMT-300`，不是 `FMT-002`**。
> 实现里专门先判 `source.find("://") != std::string::npos`，就是为了不让用户
> 把「协议不支持」误当成「本地文件不存在」去排查磁盘。
> 用例 `File.暂存失败会清理临时文件` 断言的正是 `ftp://example.com/a.bin` → `FMT-300`
> （**这里原先是 `https://`，提交 `a2b6cd1` 换成了 `ftp://`**——因为 https 现在是合法来源了）。

**来源类型**：

```text
http://        ✅ 支持（WinHTTP：连接超时 10 秒、发送 30 秒、接收 300 秒、跟随重定向）
https://       ✅ 支持（TLS 走系统 Schannel；不需要 OpenSSL，不分发 DLL）
本地文件路径    ✅ 支持
ftp:// 等其它  ❌ 不支持 → FMT-300（「只支持 http:// 与 https:// 的来源：<来源>」）
URL 带用户名密码 ❌ FMT-300（「URL 不支持带用户名密码的形式」）
```

**原口径「V1 不支持 https」「HTTPS 需要 OpenSSL，会引入 DLL」已作废。**
https 现在支持，而且**不引 OpenSSL、不带任何 DLL**：
TLS 由 WinHTTP 交给系统 **Schannel**，证书走系统证书库。理由与对照见 1.2。

#### 10.2.2.1 接口（`include/fmt/common/http_client.hpp`，提交 `a2b6cd1`）

```cpp
// 是不是需要走网络的来源（http:// 或 https://）。
bool is_remote_url(std::string_view source);

struct HttpDownloadRequest {
    std::string url;
    std::string user_agent = "FileManagerTool/1.0";
    int connect_timeout_ms = 10000;
    int send_timeout_ms = 30000;
    int receive_timeout_ms = 300000;  // 单次读取的空闲超时，不是总时长
    bool follow_redirects = true;
};

struct HttpDownloadResult {
    int status = 0;
    std::uintmax_t bytes = 0;                 // 实际交给 sink 的字节数
    std::uintmax_t content_length = 0;        // 服务器声明的长度
    bool has_content_length = false;
    std::string content_type;
    bool aborted = false;                     // sink 主动中止（例如超过大小上限）
};

// 流式下载。只有 2xx 的响应体会交给 sink；非 2xx 的响应体直接丢弃，
// 状态码照实返回（由调用方决定报什么错）。
Result<HttpDownloadResult> http_download(
    const HttpDownloadRequest& request,
    const std::function<bool(const char* data, std::size_t size)>& sink);
```

**请求与行为**：

| 项 | 实况 |
|---|---|
| 方法 | `GET` |
| 重定向 | `WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS`（设置失败按系统默认，不致命） |
| 超时 | `WinHttpSetTimeouts(session, 0, connect, send, receive)`；默认 10s / 30s / 300s |
| 接收超时的语义 | **单次读取的空闲超时，不是总时长**——读得慢但一直在读，就不会超时 |
| 代理 | `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY` 优先，失败退回 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` |
| 读取块 | 64 KiB（`kReadChunk`，首次分配 4 KiB 后按 `WinHttpQueryDataAvailable` 调整） |
| 压缩 | **显式要求 `Accept-Encoding: identity`**（见下） |
| 协议 | 只支持 `http://` 与 `https://`；**URL 里的用户名密码不接受** |

**只有 2xx 的响应体交给 sink**：

```text
status 在 [200, 300)  -> 逐块 sink(data, size)
status 不在该区间    -> 继续把响应体读干净但**不交给 sink**（丢弃），
                        status 照实返回，由调用方决定报什么错
```

非 2xx 的响应体**直接丢弃**，理由很直白：错误页（无论 HTML 还是 JSON）不该落进用户的文件。
`file.cpp` 看到 `status != 200` 才报 `FMT-301 DownloadFailed`。

**sink 返回 false = 调用方要求中止**：

```text
sink 返回 false
  → http_download 返回**成功**，且 aborted = true
  → 被拒绝的那一块**不算收到**（result.bytes 不含它）
```

`prepare_upload()` 的 sink 在「已写 + 本次 > max_upload_size」时返回 false：
边下边判，不把整个大文件拉完才发现超限；随后按 `SizeLimitExceeded`（FMT-303）报错。

**为什么明确要求 `Accept-Encoding: identity`**：

| 风险 | 说明 |
|---|---|
| 长度对不上 | 服务器压缩后 `Content-Length` 是**压缩后**的长度，与实际落盘字节对不上，10.2 第 7 步的完整性检查会误报 |
| 存下压缩内容 | 更糟的情况是把 gzip/deflate 的字节当成文件原样存进仓库，用户拿到一个打不开的文件 |

若服务器**无视要求**仍返回非 identity 的 `Content-Encoding`，直接报 `FMT-301 DownloadFailed`，
消息为「服务器返回了 xxx 压缩内容，暂不支持：<URL>」——宁可失败，也不存一份坏文件。

**错误映射（`common/http_client.cpp`）**：

| 情况 | 错误码 | 消息要点 |
|---|---|---|
| 超时（`ERROR_WINHTTP_TIMEOUT`） | `FMT-302 DownloadTimeout` | 「发送请求超时」/「等待响应超时」/「读取数据超时（已收 N 字节）」 |
| 域名解析 / 连接 / TLS / 响应异常 | `FMT-301 DownloadFailed` | 带 Win32 原因（域名解析失败、无法连接、连接被中断、服务器响应异常、`Win32 <code> <系统文案>`） |
| **证书类失败** | `FMT-301 DownloadFailed` | 额外读 `WINHTTP_OPTION_SECURITY_FLAGS`，给出「根证书不受信任」/「证书主机名不符」/「证书已过期或尚未生效」 |
| URL 非法 / 协议不支持 / 带凭据 / 端口非法 | `FMT-300 UrlInvalid` | 「只支持 http:// 与 https:// 的 URL」等 |

`WinHttpQueryHeaders` 另取 `Content-Length`（解析失败则 `has_content_length = false`）、
`Content-Type`、`Content-Encoding` 三项填进结果。

`parse_url()` 另做几项检查：主机名为空 → `FMT-300`「URL 缺少主机名」；
端口非数字 / 越界 → `FMT-300`；带 `user:pass@` → `FMT-300`；`path` 为空按 `/` 处理。
查询串保留（下载要用），只有文件名推断会把它砍掉。

#### 10.2.2.2 构建改动（提交 `a2b6cd1`）

```cmake
# src/common/CMakeLists.txt
target_sources(fmt_core PRIVATE ... http_client.cpp)
if(WIN32)
    target_link_libraries(fmt_core PRIVATE bcrypt)
    # 下载走 WinHTTP + Schannel：系统组件，不用分发 OpenSSL 的 DLL
    target_link_libraries(fmt_core PRIVATE winhttp)
endif()

# src/file/CMakeLists.txt：**不再链 cpp-httplib**
# 下载走 common/http_client（WinHTTP + Schannel，https 不需要 OpenSSL），
# 服务端的浏览器入口才用 cpp-httplib。
```

### 10.2.3 临时文件、失败与回滚

**进函数先清粘贴污染（提交 `a9af276`）**：`prepare_upload(paths, raw_source, raw_name, …)`
开头就把两个入参过 `clean_user_path()`（9.5），再往下用——

```cpp
const std::string source = clean_user_path(raw_source);
const std::string name = clean_user_path(raw_name);
```

位置参数在 `argument()` 那层已经清过一次（12.3.2.1），这里是**纵深防御**：
将来有人绕过 `argument()` 直接调 `prepare_upload()` 也不会踩到 `U+202A` 这类不可见字符。
**清掉后本地路径仍然不存在**时，用 `invisible_characters(raw_source)` 把原始输入里的
码位点名进 `FMT-002` 的消息（9.5 有三种消息形态；11.14 有 CLI 样例）。

**临时文件位置**（**已改口径**）：

```text
<数据根>\temp\fmt-upload-<随机>-<序号>.tmp  上传暂存文件（提交 188e85d 的实际名字；
                                            fmt- 前缀让服务启动时的 temp 清理顺手收走）
<数据根>\temp\fmt-elev-<父进程 pid>.json   提权结果文件（固定名字，13.8.3）
%TEMP%\fmt\<pid>\<name>.tmp                唯一退路：数据根不可写（例如 exe 放在只读位置）时改用，
                                           并写一行 WARN 说明原因与改用后的路径
```

**改口径的理由**：`temp/` 跟着 exe 走，用户一眼能找到、随时可清；
它按定义就是**可清空**的目录，与 `repository/`、`data/`、`config/` 分属不同职责，
把一个短暂的提权结果或上传暂存放进 `temp/`，不会污染业务数据。

**旧口径作废**：本节旧文写「系统临时目录或 `FMT_ROOT/temp/`」，4.2 曾一度改成
`%TEMP%\fmt\<pid>\`，理由是「临时文件不放在数据根，避免污染被切换的目录」。
这条理由**不再成立**——`temp/` 本身就是可清空的目录，换根带来的也只是另一个根里的
可清空目录，不会被当成未知目录（它在数据根布局里是**已登记的第六个目录**，见 4.2）。

**绝不**直接下载到 `repository`，避免半成品进入正式空间。

**失败处理**：

| 情况 | 处理 |
|---|---|
| 404 / 连接失败 / 超时 / 中断 | 删除临时文件，不生成 ID，不改 metadata |
| 超过大小限制 | 删除临时文件，不生成 ID，不改 metadata |
| MD5 重复 | 删除临时文件，提示已存在 |
| 文件名冲突 | 删除临时文件，提示改名 |

**临时文件的清理责任人（提交 `188e85d`）**：

```text
prepare_upload()   TempGuard（RAII）守着：函数正常返回才 guard.keep = true 交出所有权；
                   任何提前 return（404、连接失败、超时、中断、超大小上限、写盘失败、
                   Content-Length 对不上、MD5 算不出来）都由析构删掉它
commit_upload()    从接手那一刻起（开头就 TempGuard guard{prepared.temp_path}）临时文件归它管：
                   参数/状态类失败、去重命中、重名命中、移动失败……每条失败路径都会删掉它
                   移动成功后 guard.keep = true，改由下面的回滚负责
```

用例 `File.暂存失败会清理临时文件` 直接断言：三种失败之后 `temp/` 里常规文件数 == 0。

**回滚**（提交 `188e85d` 已实现，开发文档第 40 节）：若文件已移入 `repository`
但 `file.json` 写入失败，**删掉刚提交的仓库文件**（同时记 ERROR 日志
「写 file.json 失败，已回滚仓库文件：<路径>」）。**绝不报告上传成功**——
用户拿到的是错误，磁盘上最多留一个「文件有、metadata 没有」的孤儿，
属 15.4 一致性检查能报出来的异常，不静默吞掉。

### 10.2.4 `list` / `get` / `remove`（软删除）

**list**：只显示 `current_user` + `current_bucket` + `is_trash == false` 的文件，
按 `file_id` 升序（= 入库顺序；同一天 N 递增，跨天按日期字符串也天然有序）。
`current_user` 为空 → `FMT-604`；`current_bucket` 为空 → `FMT-305`。
响应 `data` 见 12.3.2.1（`{files:[{file_id,file_name,extension,file_type,size,md5}], count,
current_bucket}`——列表本来就限定在当前用户 + 当前桶 + 正常文件，所以不回 `user` / `bucket` /
`is_trash`）。

**get**：`file_id` 全局唯一，**不限用户、不限 Bucket、不限 `is_trash`**（软删除后仍能按 id 查到，
此时 `is_trash = true`、`trash_reason = "file"`，并**多回一个 `trash_path`**，提交 `9c3d2cb`）；
文件名查询限定 `current_user` +
正常文件（**同用户跨 Bucket**，与 9.2 同一套命名空间）。命令层的顺序是
**先当 `file_id` 查（`get_by_id`）、查不到再当文件名查（`get_by_name`）**，
这是刻意的：`file_id` 形状固定（`fmt-YYYYMMDD-N`），先查 id 不会误伤名字。
**`get_by_name()` 的同名判定不区分大小写**（提交 `5bf2c1f`：`file get report.txt`
找得到 `Report.txt`，Windows 的文件名本来就不区分大小写，18.22）。
查不到 → `FMT-002 FileNotFound`（退出码 3）。响应里的 `path` 由 `resolve_path()` 现推
（8.5）；路径推不出来或磁盘上没有时**不写 `path` 字段**，查询本身仍然成功——
「记录在、文件不在」是 15.4 要报的一致性异常，不该让一次查询直接失败。

**回收站记录多回一个 `trash_path`（提交 `9c3d2cb`，`file.get` 的 `data`）**：命中记录的
`is_trash == true` 时，在 `is_trash` / `trash_reason` 之外**增加** `trash_path`，值是
`trash_path_of(record)` 相对数据根、正斜杠的文本（形如
`trash/user/.files/工作/2026/10/08/test.txt`，12.3.2.1）。**回收站里的文件不在
`repository/` 下，所以这时 `path` 本来就不返回**——这是**设计**，不是字段缺失；
`trash_path` 才是回答「它现在在哪」。CLI 据此多打一行「回收站路径：…」（11.14）。

**两种查询范围是有意不同的（提交 `9c3d2cb` 明确，并保持现状）**：

```text
按 file_id   全局：不限用户、不限 Bucket、**连回收站里的也查得到**
按文件名     只查当前用户的**正常**文件（is_trash == false，同用户跨 Bucket）
```

`file get` 是**只读查询**：最坏结果是把 id 命中的那条给用户看，看错了还能再看一次，
所以不加歧义判定。`file delete` 是**破坏性操作**（本节下面），多一步歧义判定。两处范围
不一致**不是漏实现**。

**remove（软删除）**：`file_id` **不变**。

**定位（提交 `0ad9efc`，`locate_record(records, key)`；比较在 `5bf2c1f` 改为不区分大小写）**：
`remove(std::string_view file_id_or_name)` 与 `get` 收**同一套参数**，定位也是
**同一套规则**（第 42 节的 `get` 两步 + 这里多两步：「两个索引命中不同记录」的歧义判定、
「名字在、但已在回收站」）：

```text
① 先当 file_id     iequals(record.file_id, key) 即命中（**比较不区分大小写**，见 9.4、18.22）。
                   全局唯一；file_id 形状固定（fmt-YYYYMMDD-N），先查不会误伤名字
                   ——与 10.2.4 上面 get 那段同一套理由
② 再当文件名       record.user == current_user && !record.is_trash &&
                   iequals(record.file_name, key)（**不区分大小写**，提交 5bf2c1f）
                   （同用户**跨 Bucket**；名字在同用户范围内唯一——不区分大小写也算同名，
                    重名上传会被 FMT-105 拒绝，所以不会出现「一个名字对应两条正常记录」
                    的歧义，也不会出现「两条记录指向同一个磁盘文件」的损坏，18.22）
②.5 两个索引命中**不同**记录（提交 9c3d2cb）
                    ① 与 ② 各自记下命中下标（by_id / by_name），两者都命中且不是同一条
                    → FMT-001 InvalidArgument（退出码 2），「有歧义：<key> 既是 <file_id>
                      的文件标识，又是另一个文件的文件名（file_id <file_id>）。
                      这种名字现在不允许上传；请直接用 file_id 指定要删哪一个」
                    ——**不猜**。这种数据只可能来自旧版本或手工改过的 file.json：
                      新数据上传时已被 FMT-106 拦住（9.1 第 6 条、9.3）。
                      命中同一条记录、或只命中一路，都照常继续
③ 名字在、但 is_trash
                   → FMT-001 InvalidArgument（退出码 2），「该文件已经在回收站里：<key>
                     （file_id <file_id>）」——**不能报成 FMT-002**：文件还在，只是不在正常区
                   （这一步的同名判定同样不区分大小写）
都没有             → FMT-002 FileNotFound（退出码 3），「文件不存在：<key>」
```

> **②.5 只服务 `file delete`**：`locate_record()` 只被 `remove()` 用；`file get` 的
> 「命令层」走 `get_by_id()` + `get_by_name()`（两种范围见上），**不加**歧义判定。
> 这是**有意的差异**：get 只读，最坏是把 id 命中的那条给你看；delete 破坏性，不能猜。

**接着的两道校验（提交 `188e85d`，未变）**：记录不属于当前用户 → `FMT-004
PermissionDenied`（退出码 5）；记录 `is_trash`（**按 file_id 命中时**）→ `FMT-001
InvalidArgument`「该文件已经在回收站里：<file_id>」——这与上面定位第 ③ 步是
**两个入口、同一个错误码**（按名字命中的在定位时报、按 id 命中的在这里报，消息都带
`file_id`）。

流程与顺序（提交 `188e85d`；**提交 `0fc242b` 去掉 trash.json 那一步**）：

```text
① 定位记录      见上（提交 0ad9efc）；找不到 → FMT-002；不属于当前用户 → FMT-004；
                已在回收站 → FMT-001（「该文件已经在回收站里：<file_id 或 名字+file_id>」）
② 定位路径      resolve_path() 拿仓库里的实际位置；trash_path_of() 由 file_id 的日期
                推出 trash/<user>/.files/<bucket>/YYYY/MM/DD/<file_name>（8.5），
                先 ensure_directory() 建好父目录
③ 搬文件        同卷 rename，跨卷复制 → 校验大小 → 删源
④ 改 metadata   同一份快照上置 is_trash = true、trash_reason = "file"、
                **deleted_at = local_datetime_iso()**，写回 file.json
⑤ INFO 日志     「软删除：<file_id> <file_name> -> <回收站路径>」
```

> **提交 `0fc242b` 改了什么**：原第 ④ 步「写 trash.json 追加一条文件级记录」**没有了**
> ——**`file.json` 是唯一权威**（`deleted_at` 就记在那条记录里，7.3.1），
> `trash.json` 只在读取时兼容老记录。回滚因此从「撤 trash.json + 搬回」简化为
> 「搬回 file.json 没写成的那一步」。

**预检 `dry_run` 与 `FMT-016`（提交 `711da4c`，`file.delete` 的第一道闸）**：

```text
预检      args.dry_run = true（HTTP：?dry_run=1）→ check_remove()，**只读**，返回
            ambiguous / other_bucket / blocked / needs_confirm / current_bucket /
            file_id / file_name / bucket / path / candidates[] / message
          blocked = ambiguous（**y/N 解决不了**：要用户改用 file_id，候选两条都摊开）；
          needs_confirm = other_bucket（跨 Bucket 才要确认，同桶不打扰用户）
执行      用户同意后**才**带 force 发真实请求；服务端兜底：跨 Bucket 且没带 force
          → **FMT-016 ConfirmRequired**（消息 = 预检消息 + 「；确认删除请加 force
          （CLI：--yes）」）。同桶删除不需要 force。
命中之后  成功响应多一个 bucket 字段；跨 Bucket 时 message 变成
          「文件已移入回收站：a.txt（Bucket：工作）」
```

**顺序与回滚**：顺序是「先搬文件 → 再写 `file.json`」，每一步失败都能退回去：

```text
搬文件失败        直接返回，仓库文件没动、JSON 也没动
file.json 失败    把文件**搬回仓库**，返回错误并记 ERROR 日志
                  「写 file.json 失败，已回滚软删除」
```

响应 `data` 是 `{file_id, file_name, bucket, moved_to, message}`（提交 `711da4c` 增加
`bucket`），`message` 为 `文件已移入回收站：<文件名>`（跨 Bucket 时带
`（Bucket：<桶>）`）；`moved_to` 由 `trash_path_of()` 算出来、相对数据根。

> ~~**重要缺口（阶段 7）**~~ **已在提交 `0fc242b` 关闭**：文件级条目现在能列出、回退、
> 永久删除（10.4 的 `TrashService`）。原口径「`trash list` / `get` / `restore` 只处理
> 桶级条目、软删除的文件看不到也恢复不了」**已作废**。

> **落点里的 `<bucket>` 是记录自己的 `bucket`，不是当前 Bucket（提交 `0ad9efc` 明确）**：
> `trash_path_of(const FileRecord&)` 只读传进来的记录，从不看 `config_.current_bucket`。
> 所以「在桶 B 里按名字删掉桶 A 的文件」时，文件进
> `trash/<user>/.files/<A 的桶名>/YYYY/MM/DD/`。用例
> `File.按名字删除用的是记录自己的Bucket` 钉这一条。

**顺序与回滚（提交 `188e85d`；`0fc242b` 简化为两步）**：顺序是「先搬文件 →
再写 `file.json`」，每一步失败都能退回去：

```text
搬文件失败        直接返回，仓库文件没动、JSON 也没动
file.json 失败    把文件**搬回仓库**，返回错误并记 ERROR 日志「写 file.json 失败，已回滚软删除」
```

响应 `data` 是 `{file_id, file_name, bucket, moved_to, message}`（提交 `711da4c` 增加
`bucket` 字段），`message` 为 `文件已移入回收站：<文件名>`（跨 Bucket 时带
`（Bucket：<桶>）`）；`moved_to` 由 `trash_path_of()` 算出来、相对数据根。

> ~~**重要缺口（阶段 7，如实记录）**~~ **已在提交 `0fc242b` 关闭**：文件级条目现在能
> 列出、回退、永久删除（10.4 的 `TrashService`）。原口径「`trash list` / `get` /
> `restore` 只处理桶级条目、软删除的文件看不到也恢复不了」**已作废**。

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

**实现状态（提交 `d5779db`：数据面已落地，HTTP 下载端点未做）**

```text
模块      include/fmt/share/share.hpp + src/share/share.cpp + CMake（**真实存在**）
存储      data/share.json = {version:1, shares:[{share_id, file_id,
          max_download_count, download_count, expire_time, is_valid}]}
          expire_time 为空 = JSON 里写 null = **不过期**
share_id  **12 位随机十六进制**，来自 **BCryptGenRandom**（不是 std::mt19937 ✗——
          伪随机可预测，当访问凭证不合格；3.x 的库表本来就指定了这个系统调用）
          创建时撞号就重摇（最多 16 次）；**生成失败当错误返回**，绝不退化成可预测 id
默认值    max_download_count = 20、expire_time = 现在 + 7 天
          （开发文档第 16 节原来只写了 20；7 天来自本文档 7.2
            「20 次 + 7 天 = 7 天内最多下载 20 次」。**两个条件相互独立**，
            谁先到谁生效。一个文件可以有多个 share，各自独立计数）
share get **如实报状态**（开发文档第 49 节）：未知 id 才是真错误 FMT-500（退出码 3）；
          「已过期 / 次数用尽 / 已撤销 / 关联文件在回收站」都是成功返回 + 状态
          （state 字段 + available 布尔 + message），不伪装成「从未存在」
          检查顺序：有效性 → 文件存在 → 不在回收站 → **过期 → 次数**
          （开发文档 §50 的流程图把次数写在过期前面，**以 §49 为准**）
          expire_time 解析失败（被手工改坏）按**已过期**处理（安全侧默认可拒）
          撤销（is_valid=false）映射到 FMT-500；**撤销是删记录**，
          所以 delete 之后再 get 就是 FMT-500
中心约束  create 要求文件属于当前用户且**不在回收站**，否则 FMT-503；
          文件进回收站后它的 share **立刻不可用**（FMT-503），**也阻断新的 create**
          （开发文档第 50 节）
新增 op   **share.download**（管道可用，将来的 HTTP 下载端点也走它）：
          register_download() 在**业务锁内**一次完成「全部检查 + 计数 +1 + 落盘」，
          所以开发文档第 51 节的「19 + 两次 = 21」不可能发生；
          **计数写不进去就拒绝这次下载**（不放行，否则会超发）
CLI       四条命令都能用；打印分支**必须排在「单条文件信息」之前**——分享数据里
          也带 file_id 与 size，否则会被当成文件详情打出来（11.4）
HTTP      **一个路由都没加**（用户决定）：`/api/share` 四条仍是设计约定，
          等用户定开放哪几个接口（12.3.2、18.38）
```

> **原口径「整组尚未实现、`src/share/` 不存在、打过去是 `FMT-602`」已作废**（提交 `d5779db`）。
> 现在 `FMT-602` 只对应两件事：`server.*` 这个空壳，以及**未实现的 HTTP 路由**（12.5 的 501）。
> `begin_download` / `finish_download` 那对接口**没有照原样实现**：改成单个
> `register_download()` 在业务锁内一次做完（上面的「新增 op」），
> 原因是「预占名额 + 事后 +1」会把状态拆到两个时刻，不如一次落盘干净。

### 10.4 TrashService（**提交 `0fc242b` 起真实存在**）

`include/fmt/trash/trash.hpp` + `src/trash/trash.cpp`（**新模块**）：
`TrashService` **组合** `FileService`（文件级）与 `BucketService`（桶级），
把两级合成一份视图，并负责**跨命名空间的标识解析**。它自己不存状态。

```cpp
// 回收站里的一项：type 决定归谁管。
struct TrashEntry {
    std::string type;        // "file" / "bucket"
    std::string id;          // 文件：file_id；桶：回收站目录名
    std::string name;        // 展示名：文件=文件名；桶=原桶名（未记录时给目录名）
    std::string bucket;      // 文件所属 Bucket；桶级为空
    std::string deleted_at;
    std::uintmax_t bytes = 0;
    std::size_t files = 0;   // 桶级：树里的文件数；文件级：1
    bool present = true;     // **数据在不在磁盘上**（提交 5b316b3 写死语义，见下）
    bool restorable = true;  // 能不能单独回退（随桶删除的文件为 false）
    std::string trash_path;  // 相对数据根、正斜杠
    std::string message;     // 不能回退时的原因
};

// 预检结论：说清楚 + 要不要确认 + 能不能靠确认解决。
struct TrashCheck {
    TrashEntry entry;
    bool needs_confirm = false;  // 永久删除：要 y/N
    bool blocked = false;        // 同名冲突 / 随桶删除 / 数据缺失：确认也解决不了
    std::string message;
};
```

**`present` 的语义写死为「数据在不在磁盘上」（提交 `5b316b3` + `8f0fd5c`）**：

```text
含义      文件：.files/ 下那个文件在不在；桶：那个回收站目录在不在
结果条目  restore / purge **不翻转它**（**两级四种结果都是**：5b316b3 改文件级两处、
          8f0fd5c 再删掉桶级两处）：恢复成功后数据在仓库里，它就是 true；
          永久删除后数据确实没了，但结果同样不由它表达——两者都由 message 说明
为什么    原来 restore 把它置成 false（本意是「已经不在回收站里了」），
          于是 trash restore 成功后 CLI 打印「状态：数据已不存在」——而数据刚刚
          被搬回仓库、明明在。这是实测报上来的误导信息
断言      Service.管道能执行回收站命令（桶级 restore / purge 各一条 present 断言）、
          Trash.文件级条目能列出并回退 / Trash.永久删除文件级条目（文件级各一条）；
          8f0fd5c 只补断言、没有新增用例
真机      8f0fd5c 之后对着已安装的服务跑过：建桶 → 删桶 → trash list → trash restore
          （打印里没有「状态：数据已不存在」）→ trash delete → trash list 归零，
          完整输出见 18.30
```

```cpp
class TrashService {
public:
    TrashService(const PathManager& paths, Config& config, Logger* logger);

    Result<std::vector<TrashEntry>> list();                    // 文件级 + 桶级，按删除时间倒序
    Result<TrashEntry> get(std::string_view identifier);

    Result<TrashCheck> check_restore(std::string_view identifier) const;  // 只读
    Result<TrashEntry> restore(std::string_view identifier);

    Result<TrashCheck> check_purge(std::string_view identifier) const;    // 只读，needs_confirm 恒真
    Result<TrashEntry> purge(std::string_view identifier);

private:
    enum class Kind { None, File, Bucket, Ambiguous };
    // ① 桶的回收站目录名（精确）→ ② file_id → ③ 桶的原名 → ④ 文件名。
    // ③④ 命中多条就是歧义：报候选（Kind::Ambiguous），让用户用 file_id 或完整回收站名指定。
    Result<Resolution> resolve(std::string_view identifier) const;
    Result<TrashEntry> entry_of_file(std::string_view file_id) const;
    Result<TrashEntry> entry_of_bucket(std::string_view trashed_name) const;
};
```

> **权威来源（提交 `0fc242b`）**：**文件级 = `file.json`**（`is_trash` / `trash_reason` /
> **`deleted_at`**，路径由 `file_id` 与记录推出）；**桶级 = `trash/<用户>/.original`**（不变）。
> **`data/trash.json` 不再写入**——它保留为**只读兼容**：老数据缺 `deleted_at` 时从它补
> （`legacy_deleted_at()`），回退/永久删除时顺手清掉那条老记录（`remove_trash_record()`，
> 失败只记 WARN）。原口径「`trash.json` 记文件级条目」（commit `188e85d`）**已作废**。

**标识解析（`resolve()`，不猜）**：

```text
① 桶的回收站目录名（精确）   lazy-fox_20261008012233
② file_id                    **查所有回收站记录**（含随桶删除的），否则按 id 找那个文件
                             会得到「没有这个条目」，而真相是「它在那个桶里」
③ 桶的原名                   可能多条
④ 文件名                     可能多条
③④ 多条 → Kind::Ambiguous：check_* 返回 blocked = true + message（含每条候选的
        type / name / id），get / restore / purge 返回 FMT-001 + 同一条消息
都没有   → FMT-400 TrashEntryNotFound，消息补一句「如果是随 Bucket 删除的文件，
        它的整棵树挂在桶级条目下，用 trash list 找到那个桶再整体恢复」
```

**文件级条目的视图（`entry_of_file()`）**：

```text
trash_reason == "file"    restorable = true；trash_path 走 trash_path_of()；
                          present = .files/ 下那个文件还在不在；不在时带 reason
trash_reason == "bucket"  restorable = false；数据在**桶的**回收站目录里
                          （trash/<用户>/<桶>_<时间戳>/），present 以那个桶级条目的
                          directory_present 为准；message 让用户整体恢复那个桶
```

**两级条目的列表**：`list()` = `BucketService::list_trashed()` + `FileService::list_trashed()`，
按 `deleted_at` 倒序（相同时先桶后文件、再按 `id`）；`trash list` 的响应形状见 12.3.2.1。
**桶级条目的 `files` / `bytes` 在列表里就算出来（提交 `18f16ca`）**：对每个
`present == true` 的桶级条目调一次 `get_trashed()`（遍历那个回收站目录）填上文件数与占用。
**代价（如实记录）**：每个桶条目多遍历一次目录，`trash list` 因此**不是纯索引查询**
——它是用户显式敲的命令，可以接受；`BucketService::list_trashed()` 那层索引仍不遍历目录。
原口径「桶级的 `files`/`bytes` 在列表里是 0、精确值只在 `trash get`/预检给」**已作废**
（那是 `0fc242b` 到 `18f16ca` 之间的实况）。

**恢复流程（提交 `0fc242b`，`check_restore()` + `restore()`）**：

```text
检查 Trash 记录 → 随桶删除？ → 数据还在？ → 检查目标位置冲突
    ├─ 无冲突 → 移动文件回原位置（repository_path_of()）→ is_trash = false、
    │           trash_reason / deleted_at 清空 → 写回 file.json → 清老 trash.json 记录
    └─ 三种「确认解决不了」的情况一律 blocked：
       ① 同名正常文件占着目标位置 → FMT-401 RestoreConflict（消息点明冲突那条的 file_id）
       ② trash_reason == "bucket"（随桶删除）→ FMT-402 RestoreBucketMissing
       ③ .files/ 下的数据不在 → FMT-002（消息给出路径）
       都不覆盖、不改名；预检里 blocked = true、needs_confirm = false
```

> **目标位置被文件系统里别的东西占着**（`file.json` 里没记录、磁盘上有同名文件）也算
> 冲突（`FMT-401`，消息给路径）——否则 `move_file` 会把它覆盖掉。

**Bucket 回退（阶段 4 已实现，口径已改，commit c2d545d）**：**整单判定**——先按回收站里的名字
（或唯一的原桶名）在 `trash/<user>/.original` 里定位，目标 `repository/<user>/<原名>` 已存在就
**整单拒绝**（`FMT-401 RestoreConflict`，退出码 4，提示「回退失败：Bucket 已存在：<原名>」），
不覆盖、不改名、不把不冲突的文件先塞进去；目标不存在就整个目录一次
`std::filesystem::rename` 搬回（整棵树一次搬走，**不存在文件级冲突**）。
**桶级不再有「部分恢复」状态**——「逐个判断内部文件、无冲突的恢复、冲突的留在 Trash」
属于**文件级恢复**（`trash restore <file_id>`，**提交 `0fc242b` 已落地**）。
实现要点见 10.1 与 18.17。

**永久删除（提交 `0fc242b` 两级都实现）**：

```text
确认 → 删除实际数据 → 删除/清理 File metadata → 删除 Trash 记录 → 清理相关 share.json
```

- **确认（提交 `711da4c` 改错误码）**：服务端要求请求带 `force == true`，否则
  **`FMT-016 ConfirmRequired`**（退出码 2，不再复用 `FMT-001`，
  「永久删除不可恢复，需要确认（force = true）」）；CLI 先发只读预检（`args.dry_run`）、
  把条目详情与「不可恢复」打出来，再问「确认执行？(y/N)」（答 n → 「已取消」+ 退出码 0 +
  不发请求）；一次性命令必须 `--yes` / `-y`（否则本地拒绝、退出码 2）；
  HTTP 用 `?force=1` 或请求体 `{"force":true}`，预检用 `?dry_run=1`。
- **文件级（提交 `0fc242b`，`FileService::purge()`）**：① 删 `.files/` 下的数据
  （不在也继续）→ ② 从 `file.json` 删掉那条记录 → ③ 清老 `trash.json` 记录（失败只记 WARN）。
  `check_purge()` 的 `needs_confirm` **恒为 true**，消息形如
  「永久删除后不可恢复：a.txt（Bucket 工作，1.2KB）」。
- **桶级三步顺序**：① `remove_all` 回收站目录 → ② 清 `file.json` 里
  `trash_reason=="bucket"` 的记录（**"file" 的记录绝不动**）→ ③ 摘 `.original` 那条。
  预检消息形如「永久删除后不可恢复：<trashed>（原桶 <original>，N 个文件，X）」。
- **桶级还要清身份记录**：永久删除回收站里的桶时，必须从 `trash/<user>/.original` 里
  按 `trashed` 删掉对应那条，否则会留下「索引里有、目录没了」的幽灵条目
  （`trash list` 会标 `present: false`，但那是**报告**，不是清理）。
- **缺口**：最后一步「清理相关 share.json」**尚未实现**——share 模块属阶段 6，
  现在没有任何清理 `share.json` 的动作（阶段 6 待办）。
  原缺口「文件级的 `删除 trash.json` 一步属阶段 5/7」**已随 `0fc242b` 关闭**
  （文件级永久删除实现，并顺手清掉老记录）。
- 桶级永久删除在 `4fee290` 落地（10.1 的 `purge()`）；原口径「桶级永久删除属阶段 7
  （返回 `FMT-602`）」**已作废**。

Bucket 永久删除时，其下所有文件与相关 metadata 一并清理；
**相关 Share 的清理尚未实现**（见上一条缺口）。

**命令层的接线（提交 `0fc242b`）**：`src/service/commands.cpp` 的 `trash_command()`
只做参数与 JSON 形状——`trash.list` → `{entries, count, files, buckets}`；
`trash.get` / `trash.restore` / `trash.delete` → `{entry, message?}`；
`trash.restore` / `trash.delete` 的 `dry_run` → `{needs_confirm, blocked, entry, message?}`
（12.3.2.1）。`bucket.delete` 的预检 → `bucket_command()` 里的 `check_remove` 分支
（`{bucket, is_current, files, bytes, has_content, needs_confirm, blocked, message?}`）。

---

## 11. CLI

### 11.1 CLI 的定位（架构决策）

**CLI 不碰业务数据内容。** 它解析命令、校验参数、构造请求帧、格式化输出。
真正的业务（文件是否存在、大小是否允许、算 MD5、生成 file_id、写 JSON）全部由
Service 完成。唯一的例外是它**双击时对自己所在数据根做的幂等体检与补齐**
（只补缺失的目录与默认 JSON、只读确认已有 JSON，见 4.4.4 / 11.13）——那一步不碰任何业务数据内容。

**CLI 的通道是命名管道，不是 HTTP。**（旧文档的「其余命令一律走 HTTP」已作废。）

```text
                        fmt.exe（asInvoker）
                             │
             ┌───────────────┴───────────────┐
             │                               │
        service 命令                     业务命令
             │                               │
             ↓                               ↓
   UAC 提权（短命副本）              命名管道 \\.\pipe\fmt.control
             │                               │
             ↓                               ↓
        SCM API  ────────────────────→  FMT Service（LocalSystem）
             ▲                               │
             └── service status（不提权）      ├──→ 业务层 → Storage → 文件系统
                                              └──→ HTTP localhost:4122（仅浏览器）
```

| 通道 | 用于 | 说明 |
|---|---|---|
| 命名管道 `\\.\pipe\fmt.control` | 全部业务命令 | 一请求一响应，帧见 13.9 |
| SCM API（经 UAC 提权的短命副本） | `service install/uninstall/start/stop` | 见 13.8 |
| SCM API（**不提权**，本地直连） | `service status` | 只读查询，见 11.2 / 13.4.1 |
| 本地处理 | `--help` / `--version` / `-v` / **`version`** / `help` / `exit` | 不依赖 Service（`version` 是提交 `d108c80` 新增的命令，见 11.6） |
| HTTP `localhost:4122` | **仅浏览器** | CLI 不再使用 |

**唯一写入者是 Service。** 这样不存在 CLI 与 Service 两个进程同时改 `data/*.json`
的并发问题（CLI 双击时的幂等补齐只**新增缺失文件**，不修改任何已有内容）。
CLI 与 HTTP 两条入口**共用同一套响应信封**和同一个错误码字符串还原函数
`error_code_from_string`（见 12.3.2 / 16.1）：

```json
{ "ok": true,  "data": { } }
{ "ok": false, "error": { "code": "FMT-305", "message": "Bucket 不存在" } }
```

CLI 拿到响应后只做两件事：`ok == true` 时格式化 `data`；`ok == false` 时把
`code` 还原成 `ErrorCode`，据此决定退出码（11.8）。

### 11.2 命令通道

| 命令 | 通道 | 原因 |
|---|---|---|
| `service install` / `service uninstall` / `service start` / `service stop` | **UAC 提权 + SCM API** | Service 可能尚未安装/启动，走管道会引导死锁；且 SCM 操作需要管理员 |
| `service status` | **SCM API，不提权** | 只读查询：不需要管理员权限、不弹 UAC、不进提权副本；未安装时也要能回答「未安装」 |
| `--help` / `--version` / `-v` / **`version`** | 本地 | 不依赖 Service（**`version` 命令**提交 `d108c80` 新增：窗口里也能敲，见 11.6） |
| `bucket *` / `file *` / `share *` / `trash *` / `config *` | **命名管道 → Service** | 业务操作，必须由 Service 执行 |

服务命令**有这五条**，比早期设计少两条、多一条：

```text
没有 pause      服务不声明 SERVICE_ACCEPT_PAUSE_CONTINUE，SCM 也不会给出暂停入口
没有 delete     「卸载」统一叫 uninstall（旧名 delete 作废）
新增 status     查询当前状态，**不提权、不弹 UAC**
```

四条**动作**命令（`install` / `uninstall` / `start` / `stop`）都走 UAC 提权。
`status` 是**查询**命令，走的是「本地直连 SCM、不提权」这条路：

```text
fmt.exe service status
  → service::query_state()                                 ← 薄封装，见 13.4.2
       OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)
       OpenServiceW(hSCM, L"FMT", SERVICE_QUERY_STATUS)
         ├─ ERROR_SERVICE_DOES_NOT_EXIST → **正常结果** State::NotInstalled（不是错误）
         └─ 成功 → QueryServiceStatusEx(SC_STATUS_PROCESS_INFO) → 映射成 State
  → service::installed_binary_path() → 服务宿主（QueryServiceConfigW，去掉引号）
  → service::load_state() → ServiceState::current_root → 服务数据根
  → state == State::NotInstalled ? 打印「服务状态：未安装」+ FMT-601 / 退出码 8
                                  : 打印状态 / 宿主 / 数据根 / 错误码，退出码 0
```

输出（服务已安装且运行中）：

```text
服务状态：运行中
服务宿主：D:/FMT2/fmt.exe
服务数据根：D:/FMT2
错误码：0
```

状态名映射（`service::State` → 中文）由 `state_name()` 一处提供：
`Stopped` → `已停止`、`StartPending` → `正在启动`、`StopPending` → `正在停止`、
`Running` → `运行中`、`ContinuePending` → `正在继续`、`PausePending` → `正在暂停`、
`Paused` → `已暂停`、`NotInstalled` → `未安装`（`OpenServiceW` 报
`ERROR_SERVICE_DOES_NOT_EXIST`）、`Unknown` → `未知`（拿到服务但状态码不认识，不猜测）。
**已安装但未运行不是错误**：照常打印状态（如 `已停止`）、退出码 0。

命令**不带 `--` 前缀**：

```text
fmt.exe service install          ← 正确
fmt.exe service status           ← 正确
fmt.exe --service install        ← 作废，不再识别
```

### 11.3 命令结构

```text
fmt.exe
├── --help
├── --version | -v                   ← 一次性；与横幅、`version` 命令同一份文本
├── version                          ← **提交 d108c80 新增**：打印版本与构建日期；
│                                      一次性与窗口里都能用（也接受 --version / -v），
│                                      不需要服务、不写磁盘（11.6）
├── help    [组]                     ← 命令总览 / 某一组的详细说明
├── exit | quit                      ← 交互循环内退出
├── bucket  create <name> | list | get <name> | use <name> | delete <name>
├── file    upload <来源> [文件名] | list | get <file_id|文件名> | delete <file_id|文件名>
│           （**四条都已落地**：提交 188e85d；`delete` 的参数在提交 0ad9efc 从
│             只收 `file_id` 扩成两种，与 `get` 同一套定位规则。注意 `upload` 有第二个
│             **可选**的文件名参数，来源可以是 http:// URL 或本机路径，见 10.2）
├── share   create <file_id> | get <share_id> | list <file_id> | delete <share_id>
│           （**数据面已落地**：提交 d5779db；HTTP 下载端点未做，10.3、18.38）
├── trash   list | get <标识> | restore <标识> | delete <标识>
│           （**两级都已落地**：桶级 list / restore 阶段 4、get / delete 提交 4fee290；
│             文件级读取侧提交 0fc242b；标识可以是 file_id / 回收站目录名 / 原名，
│             list 会标出 [文件] 还是 [桶]；delete / restore 有预检与确认，10.4、11.15）
├── config  get | set --user <name> | set --bucket <name>
│           （尚未实现）
└── service install | uninstall | start | stop | reinstall | status
            （**六条子命令**：提交 c573f14 起 reinstall 从引导内部用法变成正式命令；
              除 status 外每条都提权，11.4 / 13.8.2）
```

`file delete` / `share delete` / `bucket delete` 是**业务命令**（走管道，删除语义是移入
Trash），与 `service uninstall` 完全不同层次，不要混用「删除」二字。
`service status` 与它们都不冲突：它是 `service` 子命令，读的是 SCM 状态。

**阶段 4 已落地的 `bucket` 五条命令 + 桶级 `trash` 四条命令如何变成管道 `op`**（CLI 侧实现为准）：

| CLI 输入 | 管道 `op` | `args` |
|---|---|---|
| `bucket create 工作` | `bucket.create` | `{"argv":["工作"]}` |
| `bucket list` | `bucket.list` | 不带 |
| `bucket get 工作` | `bucket.get` | `{"argv":["工作"]}` |
| `bucket use 工作` | `bucket.use` | `{"argv":["工作"]}` |
| `bucket delete 工作` | `bucket.delete` | `{"argv":["工作"]}` |
| `trash list` | `trash.list` | 不带 |
| `trash get lazy-fox_20261008012233` | `trash.get` | `{"argv":["lazy-fox_20261008012233"]}`（也可以是 `file_id` 或原名，10.4） |
| `trash restore lazy-fox_20261008012233` | `trash.restore` | `{"argv":["lazy-fox_20261008012233"]}`（`dry_run` 时额外带 `{"dry_run":true}`，11.15） |
| `trash delete lazy-fox_20261008012233 --yes` | `trash.delete` | `{"argv":["lazy-fox_20261008012233"],"force":true}`（提交 `4fee290`；CLI 先发一次 `dry_run` 预检，11.15） |
| `file upload D:/a.txt` | `file.upload` | `{"argv":["D:/a.txt"]}`（提交 `188e85d`；文件名省略，由服务端推断） |
| `file upload http://h/a.bin 改名.bin` | `file.upload` | `{"argv":["http://h/a.bin","改名.bin"]}`（第二个位置参数 = 文件名） |
| `file list` | `file.list` | 不带 |
| `file get fmt-20261008-0` | `file.get` | `{"argv":["fmt-20261008-0"]}`（`file get 报告.txt` 同一个 op，参数换成文件名） |
| `file delete fmt-20261008-0` | `file.delete` | `{"argv":["fmt-20261008-0"]}`（**不带 `force`**：软删除不需要确认） |
| `file delete 报告.txt` | `file.delete` | `{"argv":["报告.txt"]}`（提交 `0ad9efc`：同一个 op，参数换成文件名；服务端按「先 file_id、再文件名」定位，10.2.4） |

规则：`op = <组>.<动作>`，**位置参数从 `parts[2]` 起原样进 `argv`**（第 3 段开始，
不再有 `--name` 这类开关；旧写法 `bucket create --name 工作` 属于上一次实现，已作废）。
`trash delete` / `file delete` / `bucket delete` 的 `--yes` / `-y` 是
**CLI 本地开关，不进 `argv`**：CLI 先发一次只读预检（`args.dry_run = true`），
用户同意之后才把 `args.force` 置 `true` 发真实请求（**提交 `711da4c`，11.15**）。
`file` 组的四条**已经在提交 `188e85d` 落地**（原口径「`file` / `share` / `config` 同规则，
但尚未落地，现在返回 `FMT-602`」里的 `file` 部分**已作废**）；
**提交 `674d0b0` 起 `trash empty` 与 `config list/set` 也已落地**、
**提交 `d5779db` 起 `share` 四条落地**——于是原口径「`share` / `config` 仍尚未落地、
现在返回 `FMT-602`」**已作废**：`FMT-602` 现在只对应 `server.*` 这个空壳与
**未实现的 HTTP 路由**（12.5 的 501）。
另：`file list --sort <key>` 的 `--sort`、`trash delete/empty` 等的 `--yes`/`-y`
都是 **CLI 本地开关**，不进 `argv`（`--sort` 单独放进 `args.sort`，`674d0b0`）；
`trash` 的**桶级四条与文件级都在**——原口径「只有桶级的一半落地」**已作废**（提交 `4fee290`）。
参数形状的完整说明见 12.3.2.1 与 13.9.3。

### 11.4 命令帮助

帮助有**两个入口**，内容来源是同一份命令总览与同一份分组详情：

```text
fmt.exe --help        一次性：横幅 + 用法 + 命令总览 + 退出码表 + 「详细说明」一行
fmt.exe help [组]     一次性：不带参数 = 命令总览；带组名 = 该组详情
help [组]             交互循环内同上（交互里也接受 --help 这个别名）
```

**`help`（不带参数）只列命令、不加描述**（提交 `188e85d` 之后的逐字输出；
**提交 `d108c80` 起多了 `(version)` 一行**）：

```text
可用命令：
  (service)  install  uninstall  start  stop  reinstall  status
  (bucket)   create  list  get  use  delete
  (file)     upload  list  get  delete
  (trash)    list  get  restore  delete  empty
  (share)    create  get  list  delete
  (config)   list  set
  (help)     help [命令]
  (version)  version                打印版本与构建日期
  (exit)     exit  quit

业务命令（服务端尚未实现，现在会返回 FMT-602）：
  (server)   （`server.*` 是唯一还剩的空壳；原口径写 `(share)` 已作废）
```

`(file) upload list get delete` 在提交 `188e85d` 落地后移进了「可用命令」组；
**提交 `674d0b0` 起 `trash` 多了 `empty`、新增 `(config) list set`**，
**提交 `d5779db` 起 `(share)` 也移进「可用命令」组**——「尚未实现」组里
**只剩 `(server)` 这个空壳**（原口径「只剩 `(share)` 一行」已作废）。
顺序就是源码 `print_command_list()` 里的顺序：service → bucket → file → trash → share →
config → help → **version** → exit。

**`help version` 的正文（提交 `d108c80`，照源码抄）**：

```text
version —— 打印程序名、版本与构建日期
  与启动横幅、`--version` 共用同一份文本，不会各说一套。
  不需要服务在运行，也不写任何磁盘内容：
    fmt.exe version          一次性执行
    fmt> version             窗口里执行
```

**`help <组>` 打印该组详情。** 当前支持 `service` / `bucket` / `file` / `share` /
`trash` / `help` / `exit`（`quit` 等同 `exit`；**提交 `d108c80` 起多了 `version`**）。
`help service` 的输出（**提交 `c573f14` 起多出 `reinstall` 那一段**）：

```text
service —— Windows 服务管理
  install    安装并启动服务；需要管理员权限，弹一次 UAC
  uninstall  停止并删除服务；需要管理员权限
             不删除 repository / trash / config / data / log / temp
  start      启动服务；需要管理员权限
  stop       停止服务；需要管理员权限
  reinstall  一次 UAC 里做完「卸载 → 按**当前这个 exe** 重新安装并启动」；
             需要管理员权限。两个用途：
               ① 用新 exe 更新服务——直接运行**新 exe** 的这条命令即可，
                  它会先停掉旧宿主（解开文件占用）、再指向自己并启动；
               ② 修复宿主 exe 被移动或删除。
             **业务数据一个都不动**（repository / trash / config / data / log）。
             注意：宿主会变成「你运行的那一份 exe」；数据根仍由连上来的
             CLI 声明（见窗口横幅上的「数据根：…」）。
  status     查询服务状态；不需要管理员权限

说明：启动类型为自动启动，运行账户为 LocalSystem；异常退出由 Windows
      服务恢复策略自动重启（第一次 5 秒、第二次 10 秒、之后 30 秒）。
```

> **service 子命令是六条**（提交 `c573f14`）：`install` / `uninstall` / `start` / `stop` /
> **`reinstall`** / `status`，其中**除 `status` 外每条都提权**。用法提示行是
> `用法：service install | uninstall | start | stop | reinstall | status`（两处都是）。
> **原口径「文档冻结的是四条命令」「`reinstall` 只在引导流程内部使用」已作废**——
> 它本来就是提权副本的 operation（13.8.2），本轮把它接线给用户（13.8.4 有完整语义）。

四组业务命令的详情里原本都要注明「服务端尚未实现，现在返回 FMT-602」——因为管道 op
还没落地时，敲下去拿到的是 `FMT-602`，不注明会让人以为是参数写错了。

**帮助文案已随阶段 4 同步（提交 `8a5e554`，`c2d545d` 与收尾 `4fee290` 各再同步一次，
`188e85d` 又一次）**：
`bucket` 的五条 op 与
**桶级 `trash.list` / `trash.get` / `trash.restore` / `trash.delete`** 都已经落地，这句话现在
**只适用于 `file` / `share` 两组**（原口径「与 `trash get` / `trash delete`」**已作废**）；
命令总览里 `(bucket)` 与 `(trash) list get restore delete`
已移进「可用命令」组。

**提交 `188e85d` 之后再收一次**：`file.upload` / `file.list` / `file.get` / `file.delete`
也落地了，所以这句话**只适用于 `share` 一组**；`(file) upload list get delete`
同样移进了「可用命令」组（上面的逐字输出就是现在的命令总览）。
`help bucket` 的实际输出（**提交 `9c3d2cb` 新增中间两行、`0fc242b` 新增 `delete` 的提醒，照源码抄**）：

```text
bucket —— 存储空间（Bucket 就是一个目录，没有独立 ID）
  create <名称>   创建；第一个 Bucket 会自动成为当前 Bucket
  list            列出所有 Bucket；当前的那个前面标 *
  get <名称>      查看名称、是否当前、目录路径
  use <名称>      切换当前 Bucket（只改 current_bucket，不动数据）
  delete <名称>   移到回收站，名字变成 <名称>_<时间戳>；
                  删的是当前 Bucket 时置空，不自动切换
                  **桶里有文件时会先提醒**：删除后只能整体恢复这个桶，
                  没法只恢复其中某个文件；一次性命令要加 --yes

名称统一使用小写：create WORK 会建成 work（会提示你）；
其余命令按名找桶时不区分大小写，找得到就按磁盘上的实际名字处理。
回收站的桶级记录在 trash/<用户>/.original 里，回退用 trash restore。
```

`help trash` 的实际输出（**提交 `0fc242b` 改过，照源码抄**）：

```text
trash —— 回收站（两类条目：文件级 [文件] 与桶级 [桶]，都会标出来）
  list             列出回收站里的条目，标出是文件还是桶
  get <标识>       查看单个条目：类型、标识、删除时间、路径、大小/文件数
  restore <标识>   [文件] 按 file_id 回退到原 Bucket 的原位置；
                   [桶]   回退整个 Bucket（名称可以是回收站里的名字，
                          也可以是原桶名——同名只有一个时）。
                   目标位置已有同名正常文件/Bucket 就拒绝，不覆盖、不改名；
                   随桶一起删除的文件**只能整体恢复那个桶**，单独恢复会被拒绝。
  delete <标识>    **永久删除，不可恢复**：删掉数据与记录。
                   交互窗口里会先说明要删什么、再问一次；
                   一次性命令必须加 --yes，例如
                   fmt.exe trash delete lazy-fox_20261008012233 --yes

标识可以是 file_id（文件）、回收站里的目录名（桶）或原名；
命中多条会报候选，请用 file_id 或完整的回收站名指定。
文件级条目记在 file.json 里（is_trash / trash_reason / deleted_at 是权威），
数据收在 trash/<用户>/.files/ 下；桶级记录在 trash/<用户>/.original，
目录名一律带删除时间戳，两者不会互相干扰。
```

> 上面 `get` / `delete` 两行原来写的是「尚未实现（阶段 7）」的 `get <id>` / `delete <id>`，
> 标题原来是「当前是桶级条目；文件级条目随阶段 5/7 进来」，**该口径已作废**：
> 桶级四条在 `4fee290` 落地，**文件级读取侧在 `0fc242b` 落地**，标题换成
> 「两类条目：文件级 `[文件]` 与桶级 `[桶]`，都会标出来」（见 10.1、10.4、12.3.2.1）。

`help file` 的实际输出（提交 `188e85d`，`delete` 那两行在 `0ad9efc` 改过，
`upload` / `get` 与末行在 `9c3d2cb` 改过，**照源码抄**）：

```text
file —— 文件（当前用户在当前 Bucket 里的文件）
  upload <来源> [文件名]   来源可以是 http:// 或 https:// 的 URL，也可以是本机路径。
                           文件名省略时取来源的最后一段；重名不会自动改名，
                           会提示换一个名字；相同内容（MD5 相同）会被拒绝，
                           不重复入库。大小上限取 config.json 的 max_upload_size。
                           文件名不能与文件标识同形（fmt-YYYYMMDD-N）：
                           那会和 file_id 混淆，属保留形状（FMT-106）。
  list                     列出当前 Bucket 的正常文件
  get <file_id|文件名>     按文件名只查正常文件；按 file_id 连回收站里的
                           也查得到（带 is_trash 与 trash_path）
  delete <file_id|文件名>  软删除进回收站，file_id 不变
                           （文件级回收站目前只能写、还不能从 trash 查回，待阶段 7）

文件落在 repository/<用户>/<Bucket>/YYYY/MM/DD/ 下；上传先写 temp/，
校验（大小上限、MD5、文件名）通过后才移动入库。
名字与 file_id 的比较都不区分大小写（Windows 习惯）。
网络下载走系统组件（WinHTTP + Schannel），支持 https，不需要 OpenSSL。
```

> **提交 `0ad9efc` 改了 `delete` 那两行（本条已同步）**：原文是
> `delete <file_id>   软删除进回收站，file_id 不变（可用 trash 查回）`
> ——参数扩成两种，「可用 trash 查回」这句跑在实现前面，已换成实况说明。
> 标题里「当前用户在当前 Bucket 里的文件」说的是 `list`；`get <file_id>` 是**全局唯一**
> 的例外、`get <文件名>` 是「当前用户 + 正常文件」（10.2.4），`help` 正文没细说。
> `delete` 那行的「只能写、还不能从 trash 查回」与实现一致：文件级条目已经写进
> `trash.json`、文件也在 `trash/<用户>/.files/…` 下，但 `trash list` 目前只列桶级条目
> （10.4、18.19）。
> **提交 `674d0b0` / `d5779db` 之后 `help` 又变了四处**（照源码抄见 11.4 与开发文档第 68 节）：
> ① `help file` 的 `list` 行多了 `--sort name|size|id`，`delete` 那行的
> 「只能写、还不能从 trash 查回」换成「之后用 `trash list` / `trash restore` 找回来」；
> ② `help trash` 多了 `empty` 一段；③ 新增 `help config`；
> ④ `help share` 换成「数据面已落地、HTTP 下载端点还没做」的正文。
> 原口径「`help share` 仍写着『服务端尚未实现』」**已作废**（10.3、18.37、18.38）。
>
> **提交 `9c3d2cb` 又改了三处**：`upload` 追加「文件名不能与文件标识同形
> （fmt-YYYYMMDD-N）……属保留形状（FMT-106）」两行（9.1、9.3）；`get` 那行改成
> 「按文件名只查正常文件；按 file_id 连回收站里的也查得到（带 `is_trash` 与
> `trash_path`）」（10.2.4）；末尾补一行「名字与 file_id 的比较都不区分大小写
> （Windows 习惯）」——原来「`help` 正文没有写这一点」的注记到此结清（9.4）。
>
> **顺带清掉一条残留**：`upload` 那两行以前照抄的是 `188e85d` 的旧文案
> （「v1 不支持 https（需要 OpenSSL）；同时只支持 http」，提交 `a2b6cd1` 起与行为相反）。
> 那段源码在 `a2b6cd1` 里已经改成现在这两行（`http://` 或 `https://`、
> 「大小上限取 config.json 的 max_upload_size」、末行「网络下载走系统组件…」），
> 本节原先标注的「⚠ 残留不一致」**到此结清**。

**「尚未实现，返回 FMT-602」这句话的范围再收一次（提交 `188e85d`）**：原来写的是
「只适用于 `file` / `share` 两组」，现在**只适用于 `share` 一组**——
`file` 四条命令的 op（`file.upload` / `file.list` / `file.get` / `file.delete`）
都已落地，`help file` 的正文里也不再含「尚未实现」字样。

`exit` / `quit` 是**正式命令**（不只是「Ctrl+Z 退出」）：交互循环里输入它们即跳出循环、
以退出码 `0` 结束，`help exit` 给出说明。

**`help <未知组>`**：stderr 打印一行、退出码 2（`FMT-001 InvalidArgument`）：

```text
没有 <组> 的帮助；输入 help 查看命令列表
```

同一套规则适用于交互式 `help 未知组` 与一次性 `fmt.exe help 未知组`。
一次性形式 `fmt.exe help` / `fmt.exe help service` 都**不提权、不连服务、不写日志**。
`--help` 一次性打印的是带横幅与退出码表的完整用法，其中包含**同一份命令总览**。

### 11.5 Service 未运行时的行为

业务命令连不上管道时，CLI 打印：

```text
无法连接 FMT Service，请先执行 service install
```

然后把管道连接失败的 `ERROR_FILE_NOT_FOUND` 映射为 `FMT-601` / 退出码 8
（连接超时同样归到 `FMT-601` / 8）；管道**还不存在**时先重试最多 **5 秒**
（权限类错误不重试），见 11.12 与 13.9.4。

引导命令（`service *`）走 SCM 路径，因此用户始终有办法把 Service 起起来，
不会死锁；`service status` 更是**在服务没装时也能直接回答「未安装」**（FMT-601 / 退出码 8），
不需要任何提权、也不依赖管道。

### 11.6 输出约定

```text
正常结果 → stdout
错误信息 → stderr
```

CLI 输出保持简洁、明确、用户可读。**CLI 也写日志文件**：它与 Service 追加同一个
`<数据根>/log/fmt.log`（见 11.12、14.2），控制台输出只是同一次操作的面向用户的一面。

具体规则：

| 内容 | 去向 | 说明 |
|---|---|---|
| 横幅、提示符 | stdout | 见 11.9 |
| 命令结果 | stdout | 业务成功信息 |
| 错误提示、失败原因 | stderr | 可被脚本单独捕获 |
| 提权过程提示 | stdout | 「需要管理员权限」「正在提权...」 |
| 服务端 `note` 提示 | stdout | **提交 `9c3d2cb`**：`data.note` 存在时单独打一行 `提示：<note>`（例如「Bucket 名称统一使用小写：已把 WORK 转为 work」），见 11.14 |
| 错误码行 | stderr | 与失败信息成对出现 |

控制台编码：`wmain` 一进来就 `SetConsoleOutputCP(CP_UTF8)`，
所有输出走 UTF-8 字节流，中文与 emoji 不会乱码。

**V1 不做结构化输出（`--json` 等），也不预留参数名。** 机器可读通道已经有两个，够用：

```text
机器可读   退出码（0 成功；未安装 → FMT-601 对应的 8；查询失败 → 8）
人类可读   那几行文本（服务状态 / 服务宿主 / 服务数据根 / 错误码）
```

脚本要判断「服务在不在」，读退出码即可，不需要解析文本；要拿具体状态，`service status`
的输出行已经固定且顺序稳定。等真的出现「必须让脚本读到 `wait_hint_ms` 之类的结构化字段」
的需求时再加，现在加只是提前冻结一个还没想清楚的参数形状。

### 11.7 错误原则

| 情况 | 处理 |
|---|---|
| 未知命令 | 报错 + 提示帮助 + 非 0 退出码（**`version` 不再是未知命令**——提交 `d108c80` 起它是正式命令，见 11.6、18.32） |
| 缺少参数 | 报错 + 提示正确用法 + 非 0 退出码 |
| 未知参数 | 拒绝执行 + 非 0 退出码 |
| `help <未知组>` | stderr：`没有 <组> 的帮助；输入 help 查看命令列表` + `FMT-001` + 退出码 2 |
| Service 未运行 | 提示 `service install` + `FMT-601` + 退出码 8 |
| `service status` 发现服务未安装 | 打印 `服务状态：未安装` + `FMT-601` + 退出码 8（**不提权**） |
| `service status` 发现已安装但未运行 | 照常打印（如 `已停止`）+ 退出码 0，不是错误 |
| 命令执行失败 | 打印失败原因 + 错误码 + 退出码，**继续交互循环** |

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

**`FMT-NNN` 与退出码分属两层**，不要互相代替：

```text
FMT-NNN   业务/系统错误码，跨进程传递（管道、HTTP 信封），稳定不复用
退出码     进程退出状态，给脚本与用户看，粒度粗（0~8）
```

本次重构涉及的映射（与 12.7 的冻结表一致）：

| 场景 | 错误码 | 退出码 |
|---|---|---|
| 服务已存在，重复 `service install` | `FMT-600 ServiceAlreadyInstalled` | 8 |
| 服务不存在（`service start` / `stop` / `uninstall`，或管道连不上） | `FMT-601 ServiceNotInstalled` | 8 |
| SCM 操作失败 / 提权副本等待超时 / `ShellExecuteExW` 其它失败 | `FMT-602 ServiceOperationFailed` | 8 |
| 提权副本没有返回结果（结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 不存在） | `FMT-602 ServiceOperationFailed` | 8 |
| 需要管理员权限（打印「需要管理员权限」） | `FMT-603 AdminRequired` | 5 |
| `ShellExecuteExW` 失败且 `ERROR_ACCESS_DENIED`（5）（非管理员账户、策略禁止提权） | `FMT-603 AdminRequired` | 5 |
| 用户在 UAC 点「否」（`ERROR_CANCELLED` 1223） | `FMT-004 PermissionDenied` | 5 |
| 管道 `ERROR_ACCESS_DENIED`（DACL / MIC 不匹配） | `FMT-004 PermissionDenied` | 5 |
| 入口分发遇到非 1053 的 SCM 错误 | `FMT-602 ServiceOperationFailed` | 8 |

还原函数（CLI/HTTP 两侧共用，见 11.1）：`error_code_from_string(std::string_view)`，
把信封里的 `"FMT-305"` 这类字符串还原成 `ErrorCode`；反向用 `to_string(ErrorCode)`。
这两个函数名是实现细节，原 `FMT 重构设计.md`（已并入本文）把它表述为「同一个字符串还原函数」。

### 11.9 界面与交互模式

**横幅**（进入交互循环前打印；**提交 `8f2fbc5` 起永远多一行「数据根：…」**）：

```text
File Manager Tool  v1.0  ( build  2026.10.09 )
Service Running...
数据根：D:/Data/Temp/JMT/fmt

fmt >
```

服务未运行时第二行改为：

```text
File Manager Tool  v1.0  ( build  2026.10.09 )
Service Stopped...
数据根：D:/Data/Temp/JMT/fmt

fmt >
```

**「数据根：…」与它下面那一行（提交 `8f2fbc5`）**：

```text
数据来源    service::load_state() 的 current_root；**服务记的数据根为空时不打这一行**
不一致时    与本程序所在目录（to_forward_slashes(options.data_root)）**不**相等
            （iequals 比较）时，紧跟一行：
            注意：本程序所在目录是 D:/Data/CLionProjects/FMT/cmake-build-debug/bin，
            连上之后服务会切到本目录（数据根由 CLI 声明）。
触发条件    双击引导会**先连上服务并声明本目录**，所以服务在跑时横幅上两边通常已经一致；
            这一行主要在**服务不可用**（引导没能连上）时出现，用来说明
            「服务记录的数据根与你所在目录不同」——**如实现，不是 bug**
```

横幅文本来自 `banner_text()`（`src/cli/cli.cpp`），它拼的是 `fmt/version.hpp` 的
`MAJOR` / `MINOR` / `BUILD_DATE`（见 3.4），**不得硬编码**；`--version` 与 `version` 命令
调的是同一个 `version_text()`。

**`version` 命令：三处共用一份文本（提交 `d108c80`）**：

```cpp
// include/fmt/cli/cli.hpp
// 版本文本的**唯一来源**：横幅、--version / -v、version 命令都走它。
std::string version_text();     // 实现就是 return banner_text();
```

```text
三种用法，同一份输出：
  fmt.exe version            一次性执行
  fmt.exe --version / -v     一次性执行（原有）
  fmt> version               窗口里执行（也接受 --version / -v）
输出一行：File Manager Tool  v1.0  ( build  2026.10.09 )
          （版本取 version::MAJOR.MINOR，构建日期由 CMake 配置时生成）

两个性质：
  ① 不需要服务在运行——不连管道、不查服务状态、不弹 UAC
  ② 不写任何磁盘内容——一次性分支放在「开日志器」之前，log/fmt.log 不会因为敲
     version 多出记录（与 --help / --version 同一条口径：从这里开始都是真的干活，
     才值得写日志，见 4.4.2 与 11.13）

原来：窗口里敲 version 得到「未知命令」（只有旗标实现了）——**该口径已作废**。
用例：Cli.版本文本只有一个来源 断言含 File Manager Tool / v1.0 / build
      （**不钉具体日期**，它是配置时生成的）。
```

**提示符：`fmt> `**（`fmt` + `>` + 一个空格，无换行）。

**提示符前先空一行**：交互循环在打印 `fmt> ` 之前先输出一个空行——横幅之后一次，
以及**每条命令执行完之后**一次，这样输出不会和提示符挤在一起。
**用户只敲回车（空命令）时不再重复空行**，不会叠出连续两个空行。

- 命令集与命令行参数形式一致
- 空行忽略
- `help` 打印命令总览，`help <组>` 看详情（`--help` 在交互里是别名）；`exit` / `quit` 退出
- 命令失败时输出错误并**继续**循环
- **同时只允许一个 CLI 窗口**（命名互斥体，见 11.11）
- **控制台只留交互与异常**：数据根体检结果、服务当前状态只进日志（见下）；**换根通知是例外**——提交 `8f2fbc5` 起切换时在 stderr 报一次，横幅还常驻一行「数据根：…」（**原口径「换根通知也只进日志」已作废**）

**输出样例（CLI 双击引导 + 横幅）**——**控制台只留交互**，数据根体检的结果不进控制台：

```text
File Manager Tool  v1.0  ( build  2026.10.08 )
Service Running...

fmt>
```

数据根体检的每一行都**只进日志**（`log/fmt.log`，模块 `Cli`），例如首次双击：

```text
2026-10-08 09:12:03 [INFO] [Cli] 数据根检查：D:/FMT2
2026-10-08 09:12:03 [INFO] [Cli] 数据根新建目录：repository, trash, config, data, log, temp
2026-10-08 09:12:03 [INFO] [Cli] 数据根新建文件：D:/FMT2/config/config.json, ...
2026-10-08 09:12:03 [INFO] [Cli] CLI 启动 v1.0.0，数据根：D:/FMT2，交互模式
2026-10-08 09:12:03 [INFO] [Service] 当前状态：未安装
2026-10-08 09:12:04 [INFO] [Service] 落定后的状态：运行中
```

第二次双击（数据根已完整、服务同一根）只是**日志**里换几行：

```text
2026-10-08 09:20:11 [INFO] [Cli] 数据根检查：D:/FMT2
2026-10-08 09:20:11 [INFO] [Cli] 数据根完整
2026-10-08 09:20:11 [INFO] [Service] 当前状态：运行中
2026-10-08 09:20:12 [INFO] [Service] 服务数据根已经是：D:/FMT2
```

换到另一个目录双击（hello 回执 `switched=true`）时，**换根通知现在也上控制台**
（提交 `8f2fbc5`，**原口径「同样只在日志里、控制台不刷这一行」已作废**）——
切换的那一刻在 **stderr** 打印 `cli::root_switch_notice(previous, current)`：

```text
注意：服务的数据根已切换
  原来：D:/Data/Temp/JMT/fmt
  现在：D:/Data/CLionProjects/FMT/cmake-build-debug/bin
原因是「数据根由 CLI 声明」：谁连上服务，服务就用谁的目录。
如果这不是你想要的，请用数据根正确的那个 fmt.exe 再执行一条命令切回去。
```

```text
（仍然照旧写日志，完整运行记录不变）
2026-10-08 09:31:07 [INFO] [Service] 数据根切换：D:/FMT -> D:/FMT2
```

**只有异常才走 stderr**（两条）：数据根无法补齐、以及数据根里的文件损坏。

```text
数据根无法补齐：FMT-013 目录创建失败 ...
数据根文件损坏（未自动修复）：data/file.json
```

> 归位理由：本工程原本就有「**控制台负责用户交互与重要异常，日志文件负责完整运行记录**」
> 这条原则（见 14.2）。数据根体检的「建了什么 / 完不完整」、服务的当前状态都属于
> **例行运行记录**，放在控制台只会把提示符淹掉——尤其是双击时用户只想知道
> 「能不能开始敲命令」。
> **但换根通知是个例外**（提交 `8f2fbc5`）：它意味着「你看到的数据、桶、回收站整体换成了
> 另一个目录的内容」，用户必须立刻知道，藏在日志里等于没说。所以它**两者都做**：
> 控制台报一次 + 日志照旧记（另有横幅上的「数据根：…」常驻显示）。
> 为什么「连接那一刻报警」就够——见 15.1 与 13.10 末尾。

**输出样例（`service status`，成功）**：

```text
fmt> service status
服务状态：运行中
服务宿主：D:/FMT2/fmt.exe
服务数据根：D:/FMT2
错误码：0

fmt>
```

**输出样例（`service status`，未安装）**：

```text
fmt> service status
服务状态：未安装
错误码：8

fmt>
```

**输出样例（成功）**：

```text
File Manager Tool  v1.0  ( build  2026.10.08 )
Service Running...

fmt >service stop
需要管理员权限
正在提权...
执行成功...
错误码：0

fmt >
```

**输出样例（失败）**：

```text
File Manager Tool  v1.0  ( build  2026.10.08 )
Service Running...

fmt >file list
无法连接 FMT Service，请先执行 service install
执行失败：FMT-601 服务未安装
错误码：8

fmt >
```

失败时最后两行固定为「执行失败：`FMT-NNN` 说明」与「错误码：`<退出码>`」。

输出里的错误码按 11.8 的冻结表取，例如：

```text
需要管理员权限（FMT-603 AdminRequired）   提权前的提示行，退出码最终 5
执行失败：FMT-600 服务已安装              重复 install
执行失败：FMT-601 服务未安装              start/stop/uninstall 时服务不存在，或管道连不上，
                                          或 service status 发现服务未安装
执行失败：FMT-602 服务操作失败            SCM 操作失败、提权等待超时、根切换中
执行失败：FMT-004 权限不足                用户在 UAC 点「否」，或管道 ACCESS_DENIED
服务启动失败：FMT-008 配置错误            服务起不来时读 dwServiceSpecificExitCode 还原（见 13.2.3）
```

**行文约束**：「需要管理员权限」这一行是 FMT-603；一旦用户点了 UAC 的「否」，
后续换成 FMT-004。两行不会同时出现。`service status` 全程不打印「需要管理员权限」。

### 11.10 首次运行

```text
current_user 为空   → 由 initialize_root 自动补占位名 user（不需要用户设置，见 4.4.1）
current_bucket 为空 → bucket create 的第一个桶自动成为当前（第 28 节）；
                      文件类命令（file upload / file list）才返回 FMT-305 并提示先创建或选择 Bucket
```

**阶段 4 的口径**：`current_user` 的空值由**服务侧的初始化**补掉（`initialize_root()`，
见 4.4.1 第 5.5 步），CLI 不提示、也不写配置文件内容。注意区分两个函数：
CLI 双击时调的是共用的 `ensure_root` / `check_root`（只补缺失的目录与默认 JSON），
**不**调 `initialize_root()`——所以占位用户名永远是**服务**在启动/换根时写进去的
（时机：CLI 连上后服务初始化它声明的那个根）。

```text
正常首启（config.json 缺失或 current_user 为空）
  → 初始化写出占位名 user 并保存 → 用户敲 bucket create 工作 直接成功
  → 磁盘上是 repository/user/工作/
异常路径（current_user 被外部显式清空）
  → Bucket 服务的业务命令返回 FMT-604 NoCurrentUser / 退出码 7
```

> 旧文写的「引导用户设置当前用户」与「上一次实现自动落到 `default` 与默认 Bucket「工作」」
> **都已作废**：V1 没有设置用户的命令，占位名是 `user`（不是 `default`），
> Bucket 也不再自动创建——由用户的第一条 `bucket create` 决定。

用户系统正式开发后，替换为正式登录机制。

### 11.11 单实例与窗口激活

**同时只允许一个 CLI 窗口**，实现方式：

```cpp
// 1. 命名互斥体（Local\ 前缀 = 当前会话，不跨用户/会话）
HANDLE h = CreateMutexW(nullptr, FALSE, L"Local\\FMT.CLI.v1");
if (h != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
    activate_existing_window();   // 已有实例
    return ExitCode::Success;     // 本进程直接退出，**不新建窗口**
}
```

**激活已有窗口**：

```text
1. EnumWindows 遍历顶层窗口
2. 取窗口类名（GetClassNameW），匹配 ConsoleWindowClass
3. 取窗口所属进程（GetWindowThreadProcessId）→ 确认是 fmt.exe
4. SetForegroundWindow(hwnd)
5. 失败（返回 0）→ 退化用 FlashWindowEx(hwnd, FLASHW_ALL, ...) 闪烁任务栏
```

第 5 步不是可选项。**前台锁定（foreground lock）**会让 `SetForegroundWindow`
静默失败——用户正在别的程序里打字时 Windows 不允许后台进程抢焦点。
闪烁提示是唯一还能用的反馈手段。

**UIPI（User Interface Privilege Isolation）** 是这条路径上的硬限制：

```text
高完整性（提权）进程创建的窗口，不能被中完整性（普通）进程置前
   → 提权副本必须无窗口（nShow = SW_HIDE），见 13.8
   → 否则会出现「闪一下又消失的窗口」，且父进程无法把它置前
```

因此单实例互斥体只保护 **CLI 形态**；提权副本是短命进程，
**不进交互循环、不参与单实例检查**（它带着 `--elevated` 内部参数启动，
入口分发时直接走提权分支，见 13.1）。

互斥体句柄在 CLI 生命周期内一直持有（不 `CloseHandle` 到退出为止），
否则互斥体被释放，单实例失效。

### 11.12 CLI 日志与管道连接重试

**CLI 也写日志文件，且与 Service 追加同一个文件**（见 14.2）。

```text
文件      <数据根>/log/fmt.log（全部）与 error.log（仅 ERROR），与 Service 同一个文件
打开方式  两个进程都以「追加」打开（MSVC 文件流是共享模式），每行一次写入
```

CLI 记录的字段与 Service 完全同一套格式（见 14.3）：

| 字段 | CLI 侧取值 |
|---|---|
| 时间 | `YYYY-MM-DD HH:MM:SS`，本地时间 |
| 级别 | INFO / WARN / ERROR |
| 模块 | **`Cli`**（CLI 本体）、**`Elevated`**（提权副本） |
| 消息 | 见下 |

```text
Cli       CLI 启动 / 退出
Cli       用户敲的原始命令（形如  fmt> service stop ）
Cli       每条 service 命令的结果（四条动作命令还记提权过程；status 只记查询结果）
Cli       数据根体检结果：数据根检查 / 数据根新建目录 / 数据根新建文件 /
          数据根完整 / 数据根损坏（未自动修复）/ 数据根无法补齐
Elevated  提权副本自身的执行与结果
Cli       业务命令的请求与结果
Cli       连接失败
Service   服务的当前状态与落定后的状态（如「当前状态：运行中」）
```

三条边界，必须同时成立：

```text
1. CLI 双击时先对自己所在数据根执行与服务共用的幂等体检与补齐
   （六个目录 + 六个默认 JSON，只补缺失、已存在不动、损坏 JSON 只报告不重置，见 11.13）；
   这一步在打开日志器之前完成，所以 log/ 与 temp/ 都在同一条「新建目录」清单里，
   结果攒成若干行、等日志器开好后写进 log/fmt.log（模块 Cli），**控制台一行都不打**。
   除此之外 CLI 不改任何业务数据：不写 data/*.json 的内容、不删文件、不改名。
   旧口径里「CLI 只允许创建 log/ 与 temp/ 这两个目录」的说法已被这一步覆盖。
2. `--help` / `--version` / `-v` / `version` / `help` / `exit` 不写日志、不创建任何目录（它们不该在磁盘上留下东西；`version` 是提交 `d108c80` 新增的命令，11.6）。
3. 两个进程的数据根可能不同（服务可能被别人启动在另一个目录）：各写各自数据根下的
   log/fmt.log；这种情况下 CLI 会额外写一行 WARN，指明服务当前数据根与服务侧日志的位置。
```

`temp/` 的创建与清理口径见 4.2：CLI 双击时的幂等补齐会一并建出 `<数据根>/temp`，
执行提权类命令前若仍不存在就退回系统临时目录 `%TEMP%`，并写一行 WARN 说明原因与
改用后的路径；提权副本在写入结果文件前也会确保目录存在。**Service 启动时**删除 `temp/` 下
以 `fmt-` 开头的遗留文件，用户手放进去的其它文件一律不动，删除数量记一行 INFO。

为什么 CLI 也写：用户在 CLI 里敲 `service stop`，服务随即被停掉，旧口径下这次操作
在日志里**一个字都没有**，日志跟不上用户做过什么。日志要能回答「谁在什么时候对服务
做了什么、结果如何」，因此两个进程都写——追加到同一个文件即不再有争抢问题。

**管道连接重试**（细节见 13.9.4）：CLI 连接命名管道时，如果管道**还不存在**
（服务刚被 `service start` 拉起、监听尚未就绪），最多重试 **5 秒**，仍连不上才报
`FMT-601` / 退出码 8；**权限类错误（`ERROR_ACCESS_DENIED`）不重试**，直接报
`FMT-004 PermissionDenied` / 退出码 5。旧写法是「一次失败即报 `FMT-601`」，
实测会出现「刚启动完立刻敲命令却连不上」。

**测试隔离**：ipc 的管道名做成**可参数化**，测试使用独立管道名，避免与机器上真实运行的
服务抢同一个管道实例（`\\.\pipe\fmt.control` 仍是默认名）。

### 11.13 CLI 双击时的数据根体检与补齐（`check_root`）

**CLI 不再只是「只读客户端」**：双击时它先对自己 exe 所在的数据根跑一次与服务**共用同一份实现**
的幂等检查（`ensure_root` / `check_root`）。规则与 4.4 完全一致，两边都只补不缺、都不碰业务数据内容。

```text
输入    CLI 自身 exe 所在的目录（GetModuleFileNameW → 父目录）
时机    单实例检查之后、查 SCM 之前；**在打开日志器之前**，所以 log/ 也由它创建
做什么  六个目录：repository/ trash/ config/ data/ log/ temp/
        六个默认 JSON：config/config.json、config/server.json、
                       data/file.json、data/share.json、data/trash.json、data/user.json
规则    缺则补；已存在一律不动（不删除、不覆盖、不改名）
        已有的 JSON **真正读一遍**（解析 + 版本检查）确认完整性
        读不出来或版本不受支持 → 只报告、绝不重置（沿用「JSON 损坏不能静默重置」）
返回    本次「新建目录 / 新建文件 / 损坏文件」三个清单
```

输出（`check_root` 的返回值决定日志里写哪几行；**控制台不打印这些常规结果**）：

```text
日志（log/fmt.log，模块 Cli；每行以「数据根」开头）：
  [Cli] 数据根检查：D:/FMT2
  [Cli] 数据根新建目录：repository, trash, config, data, log, temp
  [Cli] 数据根新建文件：D:/FMT2/config/config.json, D:/FMT2/config/server.json,
                        D:/FMT2/data/file.json, D:/FMT2/data/share.json,
                        D:/FMT2/data/trash.json, D:/FMT2/data/user.json
```

第二次及以后双击（什么都没缺）只是日志里换成一行：

```text
[Cli] 数据根检查：D:/FMT2
[Cli] 数据根完整
```

发现已有 JSON 损坏或版本不受支持时（**不修复、不重置、不重命名**）：日志里记一行，
同时**控制台走 stderr**（这是异常，不是常规结果）：

```text
日志：[Cli] 数据根损坏（未自动修复）：data/file.json
stderr：数据根文件损坏（未自动修复）：data/file.json
```

实现约束：

```text
1. 与服务侧 bootstrap_root 是同一份代码：不允许出现「CLI 版本」的目录规则
   ——两边清单必须一致，否则会出现「CLI 说完整、服务说缺东西」
2. 只做「存在性 + 可解析性 + 版本」三件事，不校验业务字段、不迁移、不补字段
3. 失败（例如目录只读、FMT-013 DirectoryCreateFailed）不阻止后续流程：
   控制台走 stderr 打印原因，然后继续查 SCM 并连服务，服务侧还会再尝试一次（同一套规则）
4. --help / --version 不执行这一步、不创建任何目录
5. 这一步在日志器打开**之前**跑，所以它先把结果攒成若干行，等日志器开好再一次性写进
   log/fmt.log（模块 Cli）；**控制台一行都不打**，只有异常走 stderr
6. 这样归位是为了守住「控制台负责用户交互与重要异常，日志负责完整运行记录」（14.2）：
   双击时用户只想知道「能不能开始敲命令」，不该被一串「建了什么」淹掉
```

**服务侧的初始化逻辑没有变**：它仍在启动时与 hello 换根时执行同一个 `bootstrap_root`
（4.4.1）。变的是「谁有权调用它」——现在 Service 与 CLI 都调，规则一致、都是幂等的。

### 11.14 业务命令的输出样例（阶段 4：Bucket 与桶级 Trash）

**服务端只回结构化数据，展示在 CLI 一侧完成**（`src/cli/cli.cpp` 的
`print_business_data()` / `print_trash_entry()`）：有 `buckets` 数组按列表打印、
有 `entries` 数组按回收站列表打印（**文件与桶都标出来**，提交 `0fc242b`）、
有 `entry` 对象按**回收站条目详情**打印（提交 `0fc242b`，`trash get` / `restore` / `delete`）、
有 `bucket` + `is_current` 按单条打印、否则打印服务给的 `message`。
**提交 `9c3d2cb`**：`data` 里有 `note` 就先单独打一行 `提示：<note>`；单条文件打印里
有 `trash_path` 就多打一行「回收站路径：…」。
`bucket` 五条命令 + `trash` 四条命令（两级）已可用，真实输出：

```text
fmt> bucket create 工作
Bucket 已创建：工作（已设为当前 Bucket）
执行成功...
错误码：0

fmt> bucket create 生活
Bucket 已创建：生活
执行成功...
错误码：0

fmt> bucket list
* 工作  (当前)
  生活
共 2 个 Bucket
执行成功...
错误码：0

fmt> bucket get 工作
Bucket：工作
当前：是
路径：repository/user/工作
执行成功...
错误码：0

fmt> bucket use 生活
已切换到 Bucket：生活
执行成功...
错误码：0

fmt> bucket create WORK          ← 提交 9c3d2cb：名称统一小写，note 单独一行先打
提示：Bucket 名称统一使用小写：已把 WORK 转为 work
Bucket 已创建：work
执行成功...
错误码：0

fmt> bucket use WORK             ← 规范化到磁盘上的实际名字，被改过时也给 note
提示：Bucket 名称统一使用小写：已把 WORK 规范为 work
已切换到 Bucket：work
执行成功...
错误码：0

fmt> bucket delete 生活
「生活」里有 2 个文件（2.4KB）。删除后整个桶移入回收站，之后只能整体恢复这个桶，
无法只恢复其中某个文件；当前 Bucket 会被置空          ← 预检（提交 0fc242b）
Bucket：生活
当前：是
文件数：2
占用：2.4KB
确认执行？(y/N) y
Bucket 已删除（移入回收站）：生活  ->  生活_20261008012233（之后只能整体恢复这个桶）
执行成功...
错误码：0

fmt> trash list
  [文件]  a.txt（Bucket 工作，1.2KB）
  [桶]    lazy-fox（2 个文件，1.2KB）  ->  lazy-fox_20261008012233
  [桶]    manual-copy（1 个文件，512B）  ->  manual-copy_20261008013000
  [桶]    gone  ->  gone_20261008014000
共 4 项（1 个文件、3 个桶）
执行成功...
错误码：0

fmt> trash restore lazy-fox_20261008012233
[桶] lazy-fox
  标识：lazy-fox_20261008012233
  文件数：2
  删除时间：2026-10-08T01:22:33
  回收站路径：trash/user/lazy-fox_20261008012233
Bucket 已回退：lazy-fox
执行成功...
错误码：0

fmt> trash restore lazy-fox
执行失败：FMT-401 回退失败：Bucket 已存在：lazy-fox
错误码：4

fmt> trash get lazy-fox_20261008012233
[桶] lazy-fox
  标识：lazy-fox_20261008012233
  文件数：2
  删除时间：2026-10-08T01:22:33
  回收站路径：trash/user/lazy-fox_20261008012233
执行成功...
错误码：0

fmt> trash get fmt-20261008-0
[文件] a.txt
  标识：fmt-20261008-0
  Bucket：工作
  大小：1.2KB
  删除时间：2026-10-08T02:10:00
  回收站路径：trash/user/.files/工作/2026/10/08/a.txt
执行成功...
错误码：0

fmt> trash delete lazy-fox_20261008012233
永久删除后不可恢复：lazy-fox_20261008012233（原桶 lazy-fox，2 个文件，1.2KB）
[桶] lazy-fox
  标识：lazy-fox_20261008012233
  文件数：2
  删除时间：2026-10-08T01:22:33
  回收站路径：trash/user/lazy-fox_20261008012233
确认执行？(y/N) y
已永久删除：lazy-fox
执行成功...
错误码：0

fmt> trash delete lazy-fox_20261008012233
永久删除后不可恢复：lazy-fox_20261008012233（原桶 lazy-fox，2 个文件，1.2KB）
[桶] lazy-fox
  标识：lazy-fox_20261008012233
确认执行？(y/N) n
已取消
错误码：0
```

| 服务端 `data` 形状 | CLI 打印 |
|---|---|
| `{buckets:[{name,is_current}], count, current_bucket}` | 每行一个：当前项 `* 名称  (当前)`，其余 `  名称`；末行 `共 N 个 Bucket` |
| `{entries:[{type,id,name,bucket,deleted_at,bytes,files,present,restorable,trash_path?,reason?}], count, files, buckets}`（提交 `0fc242b`） | 每行按 `type`：`  [文件]  <name>（Bucket <bucket>，<人类可读大小>）` 或 `  [桶]    <name>（N 个文件，<人类可读大小>）  ->  <id>`（桶那一段字数只在 `files > 0` 时打，`name` 与 `id` 相同时不打 `  ->  <id>`；**提交 `18f16ca` 起桶级条目在列表里就带这两个数**）；末行 `共 N 项（X 个文件、Y 个桶）`。**`deleted_buckets` 形状已作废** |
| `{entry:{…}}`（提交 `0fc242b`，`trash get` / `restore` / `delete`） | `print_trash_entry()`：`[文件]\|[桶] <name>` / `  标识：<id>` / 文件→`  Bucket：…` + `  大小：…`，桶→`  文件数：N`（>0）/ `  删除时间：…` / `  回收站路径：…` / `  状态：数据已不存在` / `  可回退：否（<reason>）`；`restore` / `delete` 再打一行 `message` |
| `{bucket, is_current, path}` | `Bucket：…` / `当前：是\|否` / `路径：…`（有 `path` 才打印第三行）。**提交 `9c3d2cb` + `6a40742`**：`bucket` 与 `path` 都是**磁盘上的实际名字**（`bucket get WORK` → `Bucket：work`、`路径：repository/user/work`），命令层传的是 `canonical_name()` 的结果 |
| `{files:[{file_id,file_name,extension,file_type,size,md5}], count, current_bucket}`（提交 `188e85d`） | 每行 `  <file_name>  <人类可读大小>`；末行 `共 N 个文件`（注意是「文件」，不是 Bucket） |
| `{file_id, file_name, bucket, extension, file_type, size, md5, is_trash, trash_reason, path?, trash_path?}`（提交 `188e85d`；`trash_path` 提交 `9c3d2cb`） | `文件：…` / `file_id：…` / `Bucket：…` / `类型：<file_type><extension>` / `大小：<人类可读>` / `MD5：…` / `路径：…`（**有 `path` 才打印**）/ `回收站路径：…`（**有 `trash_path` 才打印**）/ `状态：正常` 或 `状态：在回收站（<trash_reason>）` |
| `{…, note}`（提交 `9c3d2cb`） | **先**单独打一行 `提示：<note>`，再按对应形状打印其余字段 |
| `{…, message}` | 打印 `message` 一行（`file upload` / `file delete` 走这一行；`bucket use` / `bucket delete` / `trash restore` / `trash delete` 同样是这一类；**跨 Bucket 的 `file delete` 带 `（Bucket：<桶>）`**） |
| 其它 / 空 | 退回 `data.dump(2)`，不吞输出 |

**破坏性操作的预检输出不走 `print_business_data`**（那是给真实结果用的），
走 `print_precheck()`：先打 `message`，再打 `candidates`（歧义候选：`file_name（Bucket：…，file_id …）`）
或条目详情 / 桶的 `files` + `bytes`。见 11.15。

失败仍走 stderr 的固定两行：

```text
fmt> bucket create 工作
执行失败：FMT-201 Bucket 已存在：工作
错误码：4

fmt> trash restore lazy-fox
执行失败：FMT-401 回退失败：Bucket 已存在：lazy-fox
错误码：4

fmt.exe trash delete lazy-fox_20261008012233
该操作需要确认（FMT-016）：请加 --yes，或在交互窗口里执行
错误码：2

fmt> trash delete lazy-fox_20261008012233        ← 服务端侧的兜底（任何入口都不例外）
执行失败：FMT-016 永久删除不可恢复，需要确认（force = true）
错误码：2
```

`路径` 是**相对数据根、正斜杠**的形式（`relative_path_text`），所以换根后它依然成立（7.6）。
CLI **不做任何业务判断**：`is_current`、`message`、`path`、`original` 是否为空全部由服务给，
CLI 只负责排版；中文参数经管道 argv 传递（UTF-8），不经过控制台代码页转换。
`trash get` 的 `path` 走的也是 `relative_path_text`，所以样例里是
`trash/user/lazy-fox_20261008012233`。

> **帮助文案已同步**（提交 `8a5e554`、`c2d545d`，`4fee290` 收尾）：命令总览把 `(bucket)` 与
> `(trash) list get restore delete` 放进了「可用命令」组，`help bucket` / `help trash` 写明各
> 子命令的真实行为、不含与实际不符的「尚未实现」字样；仍返回 `FMT-602` 的**只有 `file` /
> `share`**（**注意：这句话是提交 `4fee290` 收尾时的状态**；提交 `188e85d` 把 `file` 四条
> 也做掉了，现在只剩 `share`，见下面一段；原口径「与 `trash get` / `trash delete`」已作废，11.4）。
>
> **提交 `188e85d` 再同步一次**：`(file) upload list get delete` 也进了「可用命令」组，
> 仍返回 `FMT-602` 的**只剩 `share`**；`file` 三条命令的输出样例见本节末尾那一段。

`file` 四条命令的真实输出（提交 `188e85d`；服务端 `data` 形状见 12.3.2.1）：

```text
fmt> file list
  报告.txt  1.2KB
  改名.bin  512B
共 2 个文件
执行成功...
错误码：0

fmt> file get fmt-20261008-0
文件：报告.txt
file_id：fmt-20261008-0
Bucket：工作
类型：text.txt
大小：1.2KB
MD5：d41d8cd98f00b204e9800998ecf8427e
路径：repository/user/工作/2026/10/08/报告.txt
状态：正常
执行成功...
错误码：0

fmt> file get fmt-20261008-0        ← 软删除之后（提交 9c3d2cb）：仓库里没有它，
                                 ← 所以**不打「路径」**，改打「回收站路径」（trash_path）
文件：报告.txt
file_id：fmt-20261008-0
Bucket：工作
类型：text.txt
大小：1.2KB
MD5：d41d8cd98f00b204e9800998ecf8427e
回收站路径：trash/user/.files/工作/2026/10/08/报告.txt
状态：在回收站（file）
执行成功...
错误码：0

fmt> file upload D:/test/报告.txt
文件已入库：报告.txt（fmt-20261008-0）
执行成功...
错误码：0

fmt> file delete fmt-20261008-0        ← 提交 711da4c：先发只读预检，不需要确认时不打扰
文件已移入回收站：报告.txt
执行成功...
错误码：0

fmt> file delete a.txt                ← 跨 Bucket：预检说清归属，确认后才发
「a.txt」属于 Bucket「工作」，而当前 Bucket 是「生活」
确认执行？(y/N) y
文件已移入回收站：a.txt（Bucket：工作）
执行成功...
错误码：0

fmt> file delete fmt-20261008-0       ← 歧义：blocked，y/N 解决不了
「fmt-20261008-0」既是文件标识、又是另一个文件名，无法确定删哪一个：
a.txt（file_id fmt-20261008-0）与 fmt-20261008-0（file_id fmt-20261008-1）。
请用 file_id 明确指定
候选：
  a.txt（Bucket：工作，file_id fmt-20261008-0）
  fmt-20261008-0（Bucket：工作，file_id fmt-20261008-1）
这项操作不能靠确认解决，请按上面的提示指定具体对象   ← stderr

fmt> file upload ftp://example.com/a.bin
执行失败：FMT-300 只支持 http:// 与 https:// 的来源：ftp://example.com/a.bin
错误码：2

> **口径已改（提交 `a2b6cd1`）**：这一段原来举的是
> `file upload https://example.com/a.bin` →「FMT-300 V1 不支持 https（需要 OpenSSL）」。
> **https 现在支持**，所以改成 `ftp://` 的例子——`FMT-300` 仍然出现在这个位置上，
> 只是理由从「不支持 https」变成「协议不是 http/https」。

fmt> file upload http://example.com/
执行失败：FMT-100 无法从来源推断文件名，请显式给出文件名
错误码：2

fmt> file upload D:/test/报告.txt
执行失败：FMT-304 该文件已经存在：报告.txt（fmt-20261008-0）
错误码：4

fmt> file upload D:/test2/报告.txt 报告.txt
执行失败：FMT-105 同名文件已存在：报告.txt（换一个文件名再上传）
错误码：4

fmt> file upload ‪C:\Users\lenovo\Pictures\pet-food-store\头像\asdva.jpg
                 ↑ 首尾是不可见的 U+202A / U+202C（聊天窗口/网页复制粘贴带进来的）
执行失败：FMT-002 本地文件不存在：C:\Users\lenovo\Pictures\pet-food-store\头像\asdva.jpg
（你粘贴的路径里有不可见字符 U+202A、U+202C，它会让路径对不上；已自动清掉，
请检查路径是否还有别的问题）
错误码：3

fmt> file upload "C:\图片\头像\asdva.jpg"   ← Explorer「复制路径」的一对引号，已清掉
文件已入库：asdva.jpg（fmt-20261008-4）
执行成功...
错误码：0

fmt> bucket create 工作‪                   ← 名字里带 U+202A：不允许进名字
执行失败：FMT-202 Bucket 名称里有不可见字符（U+202A），请把名字重敲一遍
错误码：2

fmt> file upload D:/test/fmt-20261008-0       ← 提交 9c3d2cb：与 file_id 同形，属保留形状
执行失败：FMT-106 文件名不能与文件标识同形（fmt-YYYYMMDD-N）：fmt-20261008-0（会与 file_id 混淆，请换一个名字）
错误码：2
```

> 三个容易写错的细节：`file list` 的末行是「共 N 个**文件**」（不是 Bucket）；
> `file get` 的「类型」行是源码里 `<file_type><extension>` 两个 `%s` 拼出来的
> （`text` + `.txt` → `text.txt`）；`file delete` 走 `message` 那一类，
> 只打一行「文件已移入回收站：<文件名>」，库里带 `file_id` / `moved_to` 但 CLI 不打印。
>
> 大小文本由 `format_size()`（`src/common/string.cpp`）生成：**单位与数字之间没有空格**
> （`512B` / `1.2KB` / `1.5MB`；≥ 1 KiB 时保留两位小数并去掉尾随 `0` 与 `.`）。
> 本节与 11.14 里桶级样例的 `1.2 KB`（带空格）是**旧样例的排版**，与源码不符——以源码为准。

### 11.15 破坏性操作：先检查 → 说清楚 → 再确认（提交 `711da4c`）

`file.delete` / `trash.delete` / `bucket.delete` 在 CLI 侧走同一条流程
（`src/cli/cli.cpp` 的 `confirm_before_acting()` / `print_precheck()`）：

```text
① ensure_connected
② 发一次**预检**：同一个 op + 由 argument_envelope() 产出的**同级开关**信封
   （{"argv":[…], "dry_run": true}；HTTP 对应 ?dry_run=1），**只读、零副作用**
③ print_precheck() 把预检说的原样打印：
     file.delete   目标属于哪个 Bucket / 当前 Bucket 是哪个；歧义时列出**两条候选**
                   （各带 file_name / Bucket / file_id）
     trash.delete  条目详情（type / id / name / 删除时间 / 回收站路径 / 大小或文件数）
                   + 一句「永久删除后不可恢复：…」
     bucket.delete Bucket 名 / 是否当前 / 文件数 / 占用 +「之后只能整体恢复这个桶」
④ 交互窗口问「确认执行？(y/N)」；一次性命令必须 --yes / -y
⑤ 同意之后**才**把真实请求带上 force 发出去
```

**请求形状由一个构造函数产出（提交 `2c841c8`）**：

```cpp
// include/fmt/cli/cli.hpp
// 位置参数放 args.argv，开关（dry_run / force）放**同级**字段。
nlohmann::json argument_envelope(const nlohmann::json& positional,
                                 bool dry_run = false, bool force = false);
```

```text
预检       argument_envelope(arguments, /*dry_run=*/true)  → {"argv":["a7.jpg"],"dry_run":true}
真实请求   argument_envelope(arguments, false, confirmed)  → {"argv":["a7.jpg"],"force":true}
崩溃（实测）fmt> file delete a7.jpg → 「Debug Error! abort() has been called」
原因       原来写成 check.args = arguments; check.args["dry_run"] = true;——
           arguments 是位置参数**数组**，nlohmann 对数组用字符串下标抛 type_error.305，
           没人接就是 abort()。服务端期望的是开关与 argv **同级**
教训       单元测试原来**自己照着服务端期望的形状拼请求**，CLI 拼的是另一种形状：
           **测试全绿，CLI 一敲就崩**。规则：形状必须由同一个构造函数产出，
           测试不许自己拼；用例 Cli.位置参数的信封形状 钉住机制本身
           （旧写法必须抛 type_error.305，新写法必须是带 argv 且开关同级的对象）
```

**关键性质：用户确认之前，一个破坏性请求都不会发出去**（预检只读）。细节：

```text
预检本身失败       直接报预检的错误（文件不存在 / 已在回收站 / 桶不存在），不再发执行请求，
                   **退出码用预检自己的错误码**（提交 18f16ca 起，见下表）
--yes / -y         CLI **本地**开关：只置 args.force，**不作为位置参数发给服务端**
交互窗口答 n        打印「已取消」，**退出码 0**（用户主动取消不是错误），不发请求
一次性缺 --yes      stderr「该操作需要确认（FMT-016）：请加 --yes，或在交互窗口里执行」，
                   **退出码 2**（提交 18f16ca 起提示语带上了 FMT-016）
blocked 的情况      歧义（file.delete）、同名冲突 / 随桶删除 / 数据缺失（trash.restore）：
                    打印候选或原因 + stderr「这项操作不能靠确认解决，请按上面的提示指定
                    具体对象」，**不发执行请求**，**退出码 2**（FMT-001）
服务端独立校验       force 由服务端再查一遍（10.1 / 10.2.4 / 10.4）：预检被绕过
                    （别的客户端直接发）时，跨桶删除 / 非空桶删除 / 永久删除照样被
                    **FMT-016** 拦下 —— **预检负责「说清楚」，force 负责「兜底」**
```

**四种「不要继续」的退出码（提交 `18f16ca` 修正）**：`confirm_before_acting()` 的返回值
从 `bool` 改成 `ConfirmOutcome{proceed, exit_code}`，调用点直接用 `outcome.exit_code`：

| 情况 | 退出码 |
|---|---|
| 预检自身失败（文件不存在 `FMT-002`、已在回收站等） | **预检的那个错误码**（`FMT-002` → 3） |
| 预检通信失败（`FMT-601` 等） | **通信错误的码**（8） |
| `blocked`（歧义 / 同名冲突 / 随桶删除 / 数据缺失） | 2（`FMT-001`） |
| 一次性命令缺 `--yes` | 2（`FMT-016`） |
| 交互窗口里答 n（用户主动取消） | **0** |

> **原口径「预检失败时 stderr 打真实错误码、进程退出码却统一走 2」已作废**
> （那是 `711da4c` 到 `18f16ca` 之间的实况）：预检自身的错误码现在原样透出——
> 文件不存在就是 3、通信失败就是 8，不会被「需要确认」的 2 盖掉，脚本不会误读。
> 用例 `Service.破坏性操作先预检再确认` 覆盖这条路（18.25）。

---

## 12. HTTP Server

### 12.1 实现方式

使用 **cpp-httplib**（`third_party/cpp-httplib/httplib.h`）作**浏览器入口的服务端**，
无需自研 HTTP 服务栈。

```text
Service 侧：httplib::Server    监听 localhost:4122，注册路由（浏览器用）
下载侧    ：WinHTTP + Schannel file upload <url> 时由 Service 下载远程文件（10.2.2）
CLI 侧    ：不使用 HTTP        CLI 走命名管道（第 11.1 节 / 第 13.9 节）
```

> **口径已改（提交 `a2b6cd1`）**：原文写的是「它同时提供 `httplib::Server` 与
> `httplib::Client`，服务端与下载客户端共用一套代码」。**下载侧不再用 `httplib::Client`**——
> 那个 Client 走 https 需要 OpenSSL，与本项目的 `/MT` 静态 CRT、离线可构建前提冲突；
> 现在下载走 `src/common/http_client.cpp`（**WinHTTP + Schannel**，系统组件，自动用系统代理，
> 不分发任何 DLL）。因而 `src/file/CMakeLists.txt` **不再链 cpp-httplib**，
> `httplib::Client` 在**业务代码里没有调用点**（测试仍用它：`tests/server_test.cpp` 拿它打
> 同进程的 `httplib::Server`；`tests/http_client_test.cpp` 用 `httplib::Server` 起下载桩）。
> 详见 1.2 与 10.2.2.1。

**冻结决策：CLI 不再走 HTTP。** 早期设计的「CLI 是 HTTP 客户端、
Service 是 HTTP 服务端，两者通过 localhost:4122 通信」已作废：

| 对比 | 旧（作废） | 新（冻结） |
|---|---|---|
| CLI ↔ Service 通道 | `httplib::Client` → `localhost:4122` | 命名管道 `\\.\pipe\fmt.control` |
| HTTP 端口 4122 的使用者 | CLI + 浏览器 | **仅浏览器** |
| 数据根声明 | 无法表达 | 管道 hello 帧声明（13.10） |
| 是否需要端口监听 | CLI 依赖端口可用 | CLI 与端口无关，端口占用不影响 CLI |

**响应信封与错误码还原函数两条入口共用**（见 12.3.2），这是唯一的共同点：

```text
service 层（业务实现，只有一份）
      ├── ipc 管道服务端   → 信封 {ok,data} / {ok,error{code,message}}
      └── server HTTP 端   → 同一个信封 + HTTP 状态码
```

自己只写两薄层封装：

```text
src/common/
├── http_client.hpp/.cpp    WinHTTP + Schannel 的流式 GET 下载（提交 a2b6cd1；10.2.2）
└── envelope.hpp/.cpp       响应信封 {ok,data} / {ok,error{code,message}} 的构造与还原
```

> **口径已改**：原文画的是 `src/common/net/` 下的 `http_client.hpp/.cpp`（「对
> `httplib::Client` 的封装」）与 `api.hpp`。实况是 `src/common/` 下**没有 `net/` 子目录**，
> `http_client.*` 直接放在 `src/common/`，也不是对 `httplib::Client` 的封装；
> 信封相关的函数在 `src/common/envelope.cpp`。

注意：cpp-httplib 处理请求时会为每个连接起线程，因此业务层的锁策略（第 15 节）
必须成立。Winsock 的初始化由 httplib 内部完成，无需手动 `WSAStartup`。

### 12.2 启动

```text
读取 server.json → enabled?
    ├─ false → 不启动 HTTP
    └─ true  → server.listen(host, port)
```

默认 **`localhost:4122`**（提交 `22c3c3e`：`ServerConfig::host` 的**代码默认值从
`127.0.0.1` 改成 `localhost`**，用户明确要求写 `localhost` 而不是 `127.0.0.1`）。
绑定失败（端口占用）→ 记录 ERROR 日志，Service 模式下不中断其他功能。

> **HTTP 入口的口径（提交 `22c3c3e`，真机验收 2026-10-09 21:29）**
>
> ```text
> 代码默认   ServerConfig::enabled = false、host = "localhost"、port = 4122
>            （enabled 仍是默认关闭：由 config/server.json 打开）
> 线上配置   config/server.json 已改成 enabled: true, host: "localhost"
>            → 真机日志「HTTP 监听 localhost:4122」，接口全部可用
> **仍是缺口**：**安装流程目前不会自动打开** `enabled`（那句「Service 场景下由安装流程
>            置为 true」一直没实现）。所以全新数据根装完服务后，HTTP 仍是关的，
>            需要手改 `config/server.json`（或将来加 `config http on` 这类命令）。
> ```
>
> **原口径「localhost:4122 没有监听、enabled 没人打开」只对「全新数据根 + 安装流程」
> 成立**——线上那台已经手动打开并真机验收通过（18.39 有完整记录）。
> 待决项见 19.1。

> **端口占用不再是致命问题。** 旧的 CLI 依赖 4122，端口被占时整条命令链路失效；
> 现在 CLI 走管道（13.9），HTTP 只服务浏览器，起不来只是「浏览器访问不了」。
> 服务必须继续工作，并在日志里写明端口与原因。

Service 启动顺序：先初始化业务与存储，再启动 HTTP，避免请求到达时数据层未就绪。

**根切换（13.10）时 HTTP 会按新根的 `server.json` 重新评估**（阶段 4 实现为准）：
`ServerRuntime::apply_root()` 换完根与配置后调用 `restart_http()`，把旧实例停掉、按新根的
`enabled` / `host` / `port` 起一个新实例。旧文写的「HTTP 不需要重启、最迟下一次请求生效」
**与实现不符**——那样会让浏览器在旧根的 `server.json`（可能 `enabled=false` 或另一个端口）
下继续服务，与新根不一致。

```text
换根 → 摘出旧 HttpServer（持锁，只做 move）→ **放锁** → stop()（join 工作线程）
     → enabled? 是 → 按新 host/port 起新实例 → 持锁装回
     → 失败只记 ERROR 日志，不中断服务的其他功能（同「端口占用」的处理）
```

**`stop()` / `start()` 必须在锁外做**：HTTP 的请求处理器要拿业务锁，持锁去 join 它的工作线程
会互相等待（15.1）。这条同时适用于换根、启动与停止。
（`server.json` 的加载时机与 `root` 的绑定关系见 5.2、13.10。）

### 12.3 路由

分两组：**浏览器直接访问的下载/预览路由**（12.3.1），和**业务 API**（12.3.2，
现在只服务浏览器页面与调试工具——CLI 走命名管道，见 11.1）。

#### 12.3.1 浏览器路由

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/file/download/<文件名>` | 下载，流式传输 |
| GET | `/file/preview/<文件名>` | 浏览器内预览 |
| GET | `/share/download/<share_id>` | 通过分享下载，**成功后计数** |
| GET | `/share/preview/<share_id>` | 通过分享预览 |

这两类路由**不经过响应信封**：成功直接返回文件字节流，失败返回明确的
HTTP 状态码（12.5）与简短文本。CLI 不访问它们，只把链接打印给用户。

#### 12.3.2 业务 API（浏览器与调试用）

> **这一组路由的服务对象变了。** 早期的 `/api/*` 是给 CLI 用的命令通道；
> 现在 CLI 走命名管道（11.1 / 13.9），`/api/*` 只保留给浏览器页面与调试工具。
> **业务实现仍是一份**：`/api/*` 与管道 `op` 调用的是同一个 service 层函数，
> 因此两条入口的行为、错误码、信封完全一致。

请求与响应体均为 JSON。

**所有 `/api/*` 都要 token（提交 `4b812b5`）**，唯一两个例外是
**`/api/ping`**（健康检查）与 **`/api/share/<id>/download`**（别人拿分享链接下载——
**分享链接本身就是凭证**，这是用户选的方案 B）。缺 token / token 不对 → **401 + `FMT-018
Unauthorized`**（退出码 5）。细节见下面的「认证」小节。

| 方法 | 路径 | 对应命令 |
|---|---|---|
| GET | `/api/file` | `file list`（**已落地**，提交 `188e85d`；响应里带 `sort`；**只有文件信息**——`current_bucket` 与 `path` 都不在里面，用户要求） |
| POST | `/api/file/upload` | `file.upload_stream`（**提交 `d3aeb3d`，流式**）：**请求体就是文件内容**，文件名来自 `?name=` 或 `Content-Disposition`（见下）。**旧的 `POST /api/file` + `{"url"/"path"}` 已删除**——让服务端去读本地路径对远端客户端没有意义 |
| GET | `/api/file/<id_or_name>` | `file get <file_id>` / `file get <文件名>`（**已落地**，提交 `188e85d`，路径参数百分号解码；**响应含相对数据根的 `path`**，提交 `d3aeb3d`） |
| GET | `/api/file/<id_or_name>/download` | **流式下载**（提交 `d3aeb3d`）：`Content-Disposition: attachment`、`Content-Type` 猜不出来就给 `application/octet-stream`；**任何类型都能下载**——不受预览策略限制 |
| GET | `/api/file/<id_or_name>/preview` | **流式预览**（提交 `d3aeb3d`）：`Content-Disposition: inline`，`Content-Type` 由**唯一一份**预览策略给出（`preview_content_type()`）；不支持的 → `400 + FMT-701` |
| DELETE | `/api/file/<file_id_or_name>` | `file delete <file_id\|文件名>`（**软删除，已落地**，提交 `188e85d`；路径参数在提交 `0ad9efc` 起也可给文件名；**同桶不需要确认**；**跨 Bucket 需要 `force`**，否则 `400 + FMT-016`（提交 `711da4c`）；`?dry_run=1` 只预检） |
| POST | `/api/share` | `share create <file_id>`（**已落地**，提交 `4b812b5`；请求体 `{"file_id": "fmt-20261009-0"}`） |
| GET | `/api/share/<share_id>` | `share get <share_id>`（**已落地**，提交 `4b812b5`） |
| GET | `/api/share?file_id=…` | `share list <file_id>`（**已落地**，提交 `4b812b5`） |
| DELETE | `/api/share/<share_id>` | `share delete <share_id>`（**已落地**，提交 `4b812b5`） |
| GET | `/api/share/<id>/download` | **公开、不要 token**；**流式下载 + 先记账再放行**（提交 `d3aeb3d`，开发文档第 50/51 节在这里生效：计数写不进去就不下载，避免超发） |
| GET | `/api/trash` | `trash list`（**两级，`0fc242b` 起统一 `entries` 形状**） |
| POST | `/api/trash/<标识>/restore` | `trash restore <标识>`（**两级，`0fc242b` 起文件级也可**，路径参数百分号解码；`?dry_run=1` 预检） |
| GET | `/api/trash/<标识>` | `trash get <标识>`（**两级**，提交 `4fee290` 起桶级、`0fc242b` 起文件级） |
| DELETE | `/api/trash/<标识>` | `trash delete <标识>`（**永久删除，两级**；需 `?force=1`（或 `force=true`，大小写不敏感）或请求体 `{"force":true}`，否则 `400 + **FMT-016**`（提交 `711da4c` 起，原来记的是 `FMT-001`）；`?dry_run=1` 只预检） |

> **桶的 HTTP 接口已全部删除（提交 `4b812b5`，用户明确「桶不要」）**：
> `/api/bucket*` 现在返回 **404 + `FMT-017`**，**不是 501**——它是**故意不要**，
> 不是「还没做」。所以**「已知模块」列表里没有 `bucket`**（见下面的兜底路由）。
> 桶继续由 CLI 管；HTTP 客户端操作的是**当前桶**。
> 原口径的 `/api/bucket` 五条路由（以及 `/api/config` 两条）**已作废**——
> `config` 目前也没开 HTTP 接口，`config list` 的 token 要**从 CLI 拿**。

**流式上传（提交 `d3aeb3d`，`POST /api/file/upload`）**：

```text
请求体     **就是文件内容**（Java / Python 客户端直接推字节流）。
           **旧的「请求体里给服务端一个本地路径」那套已删除**——对远端客户端没有意义。
文件名     ?name=xxx，或请求头 Content-Disposition: attachment; filename="x.jar"
           （也支持 filename*=UTF-8''… 的百分号编码形式）；
           **没给文件名 → FMT-100（400）**
边收边写   temp/ 下的暂存文件 fmt-upload-<pid>-<序号>.tmp，**沿用 fmt- 前缀**，
           所以服务启动时的清理会收走中断留下的碎片（有 10 分钟年龄保护，4.2）
上限       超过 max_upload_size **立刻中止接收并删掉暂存文件**（FMT-303 → 400）——
           不是「写完再看」
入库       落盘后交给业务层补算大小与 MD5 并入库：**全程只有一次移动、不二次拷贝**
           （这正是大文件走流式上传的意义）；失败路径一律删暂存文件
新增 op    **file.upload_stream**（args.argv = 暂存路径 + 文件名）——管道也能调，
           但它是给 HTTP 流式上传用的
新增函数   prepare_staged_upload(paths, staged, name, size_limit, logger)（流式版第一阶段）、
           content_type_of(file_name)（按扩展名猜 MIME，兜底 application/octet-stream）
```

**下载与预览（提交 `d3aeb3d`）**：

```text
实现     都用 httplib 的 set_content_provider **流式回**，不把整个文件读进内存
下载     **不受预览策略限制**：任何类型都能下载；Content-Type 猜不出来就给
         application/octet-stream；Content-Disposition: attachment;
         filename*=UTF-8''<百分号编码>（中文名任何客户端都能正确落地）
预览     Content-Disposition: inline；Content-Type 由**唯一一份**预览策略给出——
         `preview_content_type(file_type, file_name)` 在文件模块里：
           file_type == "image" → 按扩展名给 image/*
           文本类（.txt/.md/.json/.csv/.log/.xml）→ 对应 MIME
           **其余一律 FMT-701（400，可下载但不可预览）**
         HTTP 与将来的其它入口**共用这一份**，不各写一套
data.path file.get 与 share.download 的响应新增 **path**（**相对数据根**，例如
         repository/user/lazy/2026/10/09/x.txt），HTTP 层按它定位文件；
         **file.list 不加**（用户要求列表只输出文件信息）
```

**认证（提交 `4b812b5`）**：

```text
放哪      **只在一处**：httplib 的 **pre-routing 钩子**（set_pre_routing_handler）
为什么    「漏给某条路由加认证」是这类代码最典型的事故，所以不逐个路由判断；
          一处集中检查，新增路由不可能忘
fail-closed   **没有注入校验器时一律 401**（默认关着）——宁可全拒，也不放行
请求头    X-FMT-Token: <token>  或  Authorization: Bearer <token>（两种都收）
比较      常量时间（token 比较与密码哈希比较都是），避免时序侧信道
token 从哪拿  **`config list`** 会打印「用户 ID / 访问 token / 建议的请求头」——
          这是机主唯一方便拿到它的地方（V1 没有登录接口）
```

**兜底路由（提交 `4ddb515`，路由表的最后一层）**：

| 方法 | 路径 | 情况 | 响应 |
|---|---|---|---|
| GET | `/api/.*`（兜底，上面都没命中） | 路径在**已知模块**下但没这个接口（`/api/share/x`、`/api/file/list` 这类） | **501 + `FMT-602`**，消息「接口尚未实现：<path>」 |
| GET | `/api/.*`（兜底） | **完全打错**的 `/api/...` 路径（如 `/api/nosuch`） | **404 + 新错误码 `FMT-017 RouteNotFound`**，消息「没有这个接口：<path>」 |
| 任意 | 已知路由 | 路由存在、业务上找不到对象（`GET /api/file/nope.bin`） | 404 + `FMT-002`（不变） |

> **原口径「兜底一律 `response.status = 500` + `FMT-602 操作尚未实现`」已作废**（提交 `4ddb515`）：
> 500 等于告诉调用方「服务器坏了」，而真相是「没这个接口」。实测就是
> `GET /api/nosuch` → `500 + FMT-602 操作尚未实现：/api/nosuch`。
> 详见 12.5 的三条口径与 `FMT-017` 的说明。

> **阶段 4 已落地的路由共九条**：`/api/bucket` 五条 + `/api/trash` 四条
> （`GET /api/trash`、`POST /api/trash/<标识>/restore` 见 commit c2d545d，
> `GET` / `DELETE /api/trash/<标识>` 见收尾提交 `4fee290`）。原口径「共七条……`GET/DELETE
> /api/trash/<id>` 属阶段 5、6、7（现在打到它们会返回 `FMT-602`）」**已作废**。
> `/api/trash/<标识>/restore` 里的 `<标识>` 同样用 `([^/]+)` 正则片段匹配并
> `url_decode`，所以它既能接回收站里的名字（`lazy-fox_20261008012233`），
> 也能接 `file_id`、唯一的原桶名或文件名（标识解析见 10.4）；`GET` / `DELETE` 两条同理。
> `DELETE` 的 `dry_run` / `force` 处理写在路由里（提交 `711da4c` 起由 `delete_args()`
> 统一解析 `?dry_run=1` / `?force=1` / 请求体 `{"force":true}`，`/api/trash/<标识>` 与
> `/api/file/<标识>` 共用），缺确认就不往 `args` 里放 `force`，由 `service` 层统一拒绝，
> 因此**管道与 HTTP 的确认语义完全一致**。
>
> **提交 `188e85d` 追加 `/api/file` 四条**，与管道 op 一一对应：
> `GET /api/file` → `file.list`、**`POST /api/file/upload` → `file.upload_stream`**
> （**提交 `d3aeb3d` 改的**：原来的 `POST /api/file` + `{"url"/"path"}` 让服务端读本地路径，
> 对远端客户端没有意义，已删除）、
> `GET /api/file/<id_or_name>` → `file.get`、`DELETE /api/file/<file_id_or_name>` → `file.delete`；
> **提交 `d3aeb3d` 再加流式下载与预览两条**（`GET .../download`、`GET .../preview`，
> 都走 `set_content_provider`）。
> 这些路由都走同一份 `run` 闭包 → `BusinessHandler`，`file.upload` 也不例外
> （HTTP 侧转给 `ServerRuntime::run_upload()`，与管道完全同一份两段式实现）。
> `DELETE /api/file/<file_id_or_name>` **不需要 `force`**：软删除可恢复，服务端没有任何确认检查。
> 路径参数名在提交 `0ad9efc` 从 `<file_id>` 改成 `<file_id_or_name>`：服务端
> `file.delete` 现在与 `file.get` 一样按「先 file_id、再文件名」定位（10.2.4）。
> **路由本身没有动**——`src/server/server.cpp` 两条都还是 `R"(/api/file/([^/]+))"` +
> `args_with_encoded_name()`（`url_decode` 之后装进 `args.argv`），只是语义放宽了。
>
> **提交 `4b812b5` 之后（路由的最终形态）**：
> ① **`/api/share` 四条已落地**：`POST /api/share`（`{"file_id":…}`）、
> `GET /api/share/<share_id>`、`GET /api/share?file_id=…`、`DELETE /api/share/<share_id>`；
> 全部**要 token**（除下面那条下载）。`GET /api/share/<id>/download` **公开、不要 token**
> （分享链接本身就是凭证）；**提交 `d3aeb3d` 起它已真正实现**：流式回文件，且
> **先记账再放行**（开发文档第 50/51 节在这里生效——计数写不进去就不下载，避免超发）。
> ② **`/api/bucket*` 五条已删除**（用户明确「桶不要」）→ **404 + `FMT-017`**（不是 501：
> 这是故意不要，不是还没做）；桶继续由 CLI 管。
> ③ `/api/config` 两条仍未加（`config list` 是**从 CLI** 拿 token 的地方）。
> ④ `trash empty` 也没加 HTTP 路由（`DELETE /api/trash` 仍不存在；`DELETE /api/trash/<标识>`
> 是**永久删除单条**，两者不是一回事）。
> ⑤ **所有 `/api/*` 都要 token**，只有 `/api/ping` 与分享下载公开（见上面的「认证」）。
> 原口径「`share` 四条仍是设计约定、`/api/bucket` 五条已落地」**已作废**。

> **提交 `d3aeb3d` 之后：HTTP 路由表就是上面这张，第 3 步（流式）已完成**
> ——上传走 `POST /api/file/upload`（请求体即内容）、下载/预览各一条流式路由、
> 分享下载公开且**先记账再放行**。**两个真实 bug 已修**（18.40）：
> ① ContentReader 型处理器**早退前必须 `drain_reader()` 把请求体读干净**，否则 httplib
> 直接断开连接，客户端拿到的是「没有响应」而不是我们精心写的错误码；
> ② **下载曾经误用预览策略**，`.bin` 的下载被 `FMT-701` 挡掉——下载与预览是两件事，
> 下载不受预览策略限制。

**`service *` 没有、也不会有 HTTP 路由**：它操作的是 SCM，与服务进程内的业务无关，
且服务可能尚未安装/运行。见 13.8。

#### 12.3.2.1 请求参数（阶段 4 已冻结）

**两条入口用同一套参数形状**，HTTP 只多一层「怎么把参数塞进请求」的翻译：

```text
管道   op 形如 "bucket.create"，位置参数放在 args.argv（字符串数组）
       {"id":7,"op":"bucket.create","args":{"argv":["工作"]}}
       ↔ CLI：bucket create 工作
HTTP   路由固定，参数可以来自请求体，也可以来自路径
       POST /api/bucket            body: {"name":"工作"}  或  {"argv":["工作"]}
       POST /api/bucket/工作/use   路径参数（无 body）
       GET  /api/bucket/工作
       DELETE /api/bucket/工作
```

- **路径参数里的中文会被客户端百分号编码**（浏览器/curl 都会）：`工作` 到达服务端时是
  `%E5%B7%A5%E4%BD%9C`。服务端**必须解码**再当业务参数用，实现里就是
  `common/string` 的 `url_decode()`（配套 `url_encode()` 给客户端用），然后统一装成
  `args.argv` 交给同一份业务实现——HTTP 侧不存在「第二种参数格式」。
- 请求体**两种写法都接受**：与管道一致的 `{"argv":[...]}`，或更好写的 `{"name":"工作"}`；
  两者最终都变成 `args.argv`。都不是 → `FMT-001`。
- **位置参数读取的唯一入口是 `service/commands.cpp` 的 `argument()`**（提交 `a9af276`）：
  它负责取 `args.argv` 的第 N 项并**先过一遍 `clean_user_path()`**——清掉粘贴带进来的
  不可见格式字符与成对引号（9.5）。CLI 与 HTTP 共用这一个函数，所以**一处收口**：
  两条入口的粘贴污染在同一处被清掉，业务代码拿到的已经是干净参数。
  唯一的例外是 `file.upload`，它由运行体的两段式路径处理，`prepare_upload()` 里再清一次
  （来源与显式文件名）。
- `url_decode` 的规则：`%XX` 还原为字节（UTF-8 多字节序列因此自然还原）、非法转义原样保留、
  `+` **不**当空格（路径参数不是表单）。ASCII 不受影响，所以「不解码也能用」的错觉只在
  纯英文名字下成立。
- **路由表已冻结**（阶段 4 落地）：`GET/POST /api/bucket`、`GET/POST /api/bucket/<name>[/use]`、
  `DELETE /api/bucket/<name>`、`GET /api/trash`、`POST /api/trash/<名字>/restore`、
  `GET /api/trash/<名字>`、`DELETE /api/trash/<名字>`（后两条提交 `4fee290`）；
  其中 `<name>` / `<名字>` 这一段用 httplib 的正则片段匹配
  `([^/]+)`，不接受带 `/` 的名字（Bucket 名本身也不允许分隔符，9.1），
  匹配到的片段交给 `args_with_encoded_name()` → `url_decode()` 再装成 `args.argv`。
- **`/api/file` 四条也已在提交 `188e85d` 落地**（共十三条路由）：
  `GET /api/file`、`POST /api/file`、`GET /api/file/<id_or_name>`、
  `DELETE /api/file/<file_id_or_name>`（最后一段的**名字**在 `0ad9efc` 改过，
  路由正则不变）；
  后两条的 `<id_or_name>` / `<file_id_or_name>` 同样用 `([^/]+)` + `url_decode()`
  （文件名里的中文在浏览器/curl 下必然被百分号编码）。
  `POST` 的请求体形状见本节上面的「上传语义」，四条都不是 `force` 型命令。

**Bucket / 桶级 Trash / File 命令的 `data` 字段（管道与 HTTP 完全相同）**：

| op | data |
|---|---|
| `bucket.create` | `{bucket, current_bucket, message}`（**提交 `9c3d2cb`**：`bucket` 是**实际建成的名字**（小写），名称被转换时**增加 `note`**，形如 `"note": "Bucket 名称统一使用小写：已把 WORK 转为 work"`；`message` = `Bucket 已创建：<name>` +（首个桶时）`（已设为当前 Bucket）`） |
| `bucket.list` | `{buckets:[{name,is_current}], count, current_bucket}` |
| `bucket.get` | `{bucket, is_current, path}`（`path` 相对数据根、正斜杠；**提交 `9c3d2cb`：`bucket` 是磁盘上的实际名字**） |
| `bucket.use` | `{bucket, previous, current_bucket, message}`（**提交 `9c3d2cb`**：`bucket` 与 `current_bucket` 都是**规范化后的名字**（`use WORK` → `work`），值被改过时**增加 `note`**（「…已把 WORK 规范为 work」）；`previous` 是切换前的 `current_bucket`） |
| `bucket.delete` | `{bucket, moved_to, trashed_name, files_affected, was_current, current_bucket, message}`；**提交 `0fc242b`**：`message` 末尾补「（之后只能整体恢复这个桶）」，且 `dry_run` 时回预检形状 `{bucket, is_current, files, bytes, has_content, needs_confirm, blocked, message?}`（有内容才 `needs_confirm`；缺 `force` 且有内容 → `FMT-016`） |
| `trash.list` | `{entries:[{type,id,name,bucket,deleted_at,bytes,files,present,restorable,trash_path?,reason?}], count, files, buckets}`（**提交 `0fc242b` 的统一形状**；`type` 是 `"file"` / `"bucket"`，`files` / `buckets` 是两类的条数；**提交 `18f16ca` 起桶级条目的 `files` / `bytes` 也真的算出来**——对每个 `present` 桶条目遍历一次目录，见 10.4 的代价说明。**旧的 `deleted_buckets` 形状已作废**） |
| `trash.get` | `{entry:{…}}`（同上的 `TrashEntry` 形状；**旧的顶层 `trashed` / `original` / `path` / `files` / `bytes` 已作废**） |
| `trash.restore` | `{entry:{…}, message}`；`message` = `文件已回退：<name>` 或 `Bucket 已回退：<name>`；`dry_run` 时回 `{needs_confirm, blocked, entry, message?}` |
| `trash.delete` | `{entry:{…}, message}`；`message` = `已永久删除：<name>`；`dry_run` 时回 `{needs_confirm（恒真）, blocked, entry, message?}`。**旧的 `removed_files` / `removed_records` 已作废**（那些数字改由预检在动手前给出）。缺 `force` → `FMT-016`（提交 `711da4c`） |
| `file.list` | `{files:[{file_id,file_name,extension,file_type,size,md5}], count, current_bucket}`（提交 `188e85d`；**列表里没有 `user` / `bucket` / `is_trash`**——作用域已经限定，这三个字段没有信息量） |
| `file.upload` | `{file_id, file_name, bucket, extension, file_type, size, md5, message}`（提交 `188e85d`；`message` = `文件已入库：<file_name>（<file_id>）`） |
| `file.get` | `{file_id, file_name, bucket, extension, file_type, size, md5, is_trash, trash_reason, path, trash_path}`（提交 `188e85d`；`path` 相对数据根、正斜杠，走 `relative_path_text`；**推不出路径或磁盘上没有时整个字段不出现**，其余字段照常返回。**提交 `9c3d2cb`：`is_trash == true` 时增加 `trash_path`**（`trash_path_of()` 相对数据根、正斜杠，形如 `trash/user/.files/工作/2026/10/08/test.txt`）——回收站里的记录**没有 `path`**，这是设计；两种查询范围差异见 10.2.4） |
| `file.delete` | `{file_id, file_name, bucket, moved_to, message}`（**提交 `711da4c` 增加 `bucket`**；软删除，`file_id` 不变；`moved_to` = `trash/<user>/.files/<bucket>/YYYY/MM/DD/<file_name>`；`message` = `文件已移入回收站：<file_name>`，跨 Bucket 时补 `（Bucket：<bucket>）`）。**同桶不需要 `force`**；跨 Bucket 缺 `force` → `FMT-016`。`dry_run` 时回预检形状：`{ambiguous, other_bucket, blocked（= ambiguous）, needs_confirm（= other_bucket）, current_bucket, file_id?, file_name?, bucket?, path?, candidates[]?, message?}` |

`bucket.delete` 之所以回这么多字段，是因为它产出的 `BucketRemoval`（10.1）本身就带
「移到哪儿（回收站里的名字）、影响了几个文件、删的是不是当前桶」；
`bucket.delete` 的 `message` 是 `Bucket 已删除（移入回收站）：<原名>  ->  <回收站名>（之后只能整体恢复这个桶）`，
`trash.restore` 的 `message` 是 `文件已回退：<name>` 或 `Bucket 已回退：<原名>`，
`trash.delete` 的 `message` 是 `已永久删除：<name>`。
`trash.list` 里 `restorable=false` 表示这条**不能单独回退**（随桶删除、或桶级缺身份记录），
带 `reason` 说明；`present=false` 表示数据不在了——两者都**如实报告**，不擅自清理。
`trash.get` / `trash.delete` / `trash.restore` **两级都已落地**
（桶级 `4fee290`、文件级 `0fc242b`）——原口径「文件级属阶段 7、返回 `FMT-602`」**已作废**；
永久删除还多一道确认（缺了就是 **`FMT-016`** / 退出码 2，提交 `711da4c`）。
CLI 的展示规则见 11.14，CLI 的确认流程见 11.15。

**`file.*` 四种 op 也已经落地（提交 `188e85d`）**，名字与参数如下（原口径
「`file.*` / `share.*` / `config.*` 的精确名字与参数随各命令实现确定」里的 `file` 一条已兑现）：

```text
file.list    无参数                                            → GET  /api/file
file.upload  args.argv = [<来源>] 或 [<来源>, <文件名>]         → POST /api/file
             （文件名与 file_id 同形 → FMT-106，提交 9c3d2cb；显式名与推断名都拦，9.1）
file.get     args.argv = [<file_id 或 文件名>]                  → GET  /api/file/<id_or_name>
             （按 file_id 全局含回收站、按文件名只查正常文件；命中回收站记录时回
               trash_path，提交 9c3d2cb；两种情况都不加歧义判定，10.2.4）
file.delete  args.argv = [<file_id 或 文件名>]                  → DELETE /api/file/<file_id_or_name>
             （**不需要 force**；与 trash.delete 的确认语义不同。
               提交 0ad9efc 起与 file.get 同一套定位规则——先当 file_id、
               再当文件名；名字在但已软删除 → FMT-001 并给出 file_id；
               **提交 9c3d2cb：两个索引命中不同记录 → FMT-001 报歧义**，10.2.4）
```

**上传语义（已冻结）：CLI 传路径或 URL，不传文件内容。**

```json
POST /api/file
{ "path": "D:/test/a.txt", "file_name": "a.txt" }
```

```json
POST /api/file
{ "url": "http://example.com/a.zip", "file_name": "a.zip" }
```

Service 依据字段判断来源：`path` 为本地文件（Service 直接读盘），`url` 为网络地址
（Service 下载）。**V1 不让文件内容经过 HTTP**，避免无谓的数据搬运。
管道侧的 `file upload <来源> [文件名]` 用的是同一份语义。

**提交 `188e85d` 之后这块的实况**（`src/server/server.cpp` 的 `upload_args_from_body()`）：

```text
请求体为空            → 400 + FMT-001（「请求体不能为空」）
请求体不是合法 JSON   → 400 + FMT-006（JsonParseError，「请求体不是合法 JSON：…」）
不是 JSON 对象        → 400 + FMT-001（「请求体必须是 JSON 对象」）
既没有 url 也没有 path → 400 + FMT-001（「请求体需要 url 或 path」）
有 url 优先用 url；否则用 path；两者最终都变成 args.argv = [<来源>] (+ [<文件名>])
可选的 "file_name"    → 追加成 args.argv[1]，语义与管道的第二个位置参数相同
url 是 http:// / https:// → 合法来源，交给业务层下载（提交 a2b6cd1）
url 是 ftp:// 等其它协议 → 400 + FMT-300（在业务层 prepare_upload() 处拒绝，10.2.2）
```

> **口径已改（提交 `a2b6cd1`）**：原文这一行写「`url` 是 `https://` → 400 + `FMT-300`
> （10.2.2：V1 不支持 https）」。两处都要更正：
>
> 1. **https 现在支持**，`url` 写成 `https://…` 是合法的。
> 2. **`upload_args_from_body()` 本身不校验协议**——它只做「空体 / JSON 合法性 / 是否对象 /
>    有没有 url 或 path」四项检查，然后把来源原样塞进 `args.argv`。
>    `FMT-300` 是**下游 `prepare_upload()`** 报出来的，HTTP 状态码 400 由
>    `http_status_for(ErrorCode::UrlInvalid)` 映射得到（12.5 的表）。

> `file_name` **可省略**（`args.argv` 只有一个元素），此时由服务端从来源推断文件名
> （10.2.1）。旧文的示例里 `file_name` 总是带着，容易读成必填——不是必填。

**共用响应信封**：管道响应与 HTTP 响应体是同一个结构，
区别只在 HTTP 额外带状态码（12.5）：

```json
{ "ok": true,  "data": { } }
```

```json
{ "ok": false, "error": { "code": "FMT-305", "message": "Bucket 不存在" } }
```

客户端用同一个函数把 `code` 字符串还原成 `ErrorCode`：

```cpp
// common/error
ErrorCode error_code_from_string(std::string_view text);   // "FMT-305" → ErrorCode::BucketNotFound
std::string to_string(ErrorCode code);                     // 反向，写进信封
```

这条「字符串 ↔ 枚举」的往返是**跨进程契约**的一部分：管道（CLI）与 HTTP
（浏览器）都靠它，因此枚举增删错误码时不得改动已有字符串。

> 完整请求/响应字段在实现阶段随各命令一同确定。本表的路径形式为设计约定，
> 实现前可微调，但**「service 层是唯一业务执行者、两条入口共用信封」这一结构不变**。

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
| 401 | 缺少 / 无效的访问 token（`/api/ping` 与 `/api/share/<id>/download` 除外） |
| 403 | 分享过期 / 次数耗尽 / 文件不可用 |
| 404 | 资源不存在（含**没有这个接口**） |
| 405 | 非 GET 方法 |
| 409 | 资源冲突 |
| 500 | 内部错误 |
| **501** | **接口尚未实现**（提交 `4ddb515`）——见下面的兜底路由 |

删除类操作无返回内容时用 204，且必须保证 204 **不带响应体**。

**错误码 → 状态码映射（阶段 4 已冻结在实现里）**：

| 状态码 | 错误码 |
|---|---|
| 400 | `FMT-001` **`FMT-016`** `FMT-012` `FMT-014` `FMT-100` `FMT-101` `FMT-102` `FMT-103` `FMT-104` **`FMT-106`** `FMT-202` `FMT-300` `FMT-303` `FMT-700` `FMT-701` |
| **401** | **`FMT-018 Unauthorized`**（提交 `4b812b5`）：缺 token / token 不对（`/api/ping` 与分享下载除外），由 **pre-routing 钩子**一处拒绝；退出码 5 |
| 403 | `FMT-004` `FMT-501` `FMT-502` `FMT-503` |
| 404 | `FMT-002` `FMT-200` `FMT-400` `FMT-500` `FMT-305` `FMT-402` **`FMT-017`** |
| 409 | `FMT-003` `FMT-105` `FMT-201` `FMT-203`(保留未用) `FMT-304` `FMT-401` | 冲突（已存在、重名、仍被引用；**`FMT-203 BucketInUse` 保留、V1 未使用**——没有代码会产生它，见 12.5 下方的说明） |
| 500 | 其余（JSON / 配置 / 存储 / IO 等） |
| **501** | **`FMT-602 ServiceOperationFailed`**（提交 `4ddb515` 起：它表示「接口尚未实现」，原来是落 `default: 500`） |

```text
400  InvalidArgument ConfirmRequired PathTooLong PathEscape FileName* BucketNameInvalid
     UrlInvalid SizeLimitExceeded HttpRequestInvalid PreviewUnsupported
403  PermissionDenied ShareExpired ShareDownloadLimitReached ShareFileUnavailable
404  FileNotFound BucketNotFound TrashEntryNotFound ShareNotFound
     NoCurrentBucket RestoreBucketMissing RouteNotFound
409  FileAlreadyExists FileNameConflict BucketAlreadyExists BucketInUse
     Md5Duplicate RestoreConflict
500  default（JsonParseError / JsonWriteError / ConfigError / StorageError / IoError …）
501  ServiceOperationFailed（「接口尚未实现」；提交 4ddb515 起显式登记）
```

映射实现在 `src/server/server.cpp` 的 `http_status_for(ErrorCode)`：一个 `switch` +
`default: 500`。**新增错误码若不显式登记就落到 500**——宁可报「内部错误」，也不要猜一个
语义不匹配的 4xx。响应体仍是 12.3.2 的统一信封，状态码只是额外一层（管道没有这一层，
CLI 只看信封里的 `FMT-NNN`）。

映射的语义分组：`400` 参数/名称/路径/URL 类（用户改一下就能过）、`403` 权限与分享不可用、
`404` 对象不存在（含「没选当前 Bucket」`FMT-305`、「原 Bucket 已永久删除」`FMT-402`、
**「没有这个接口」`FMT-017`**）、`409` 冲突（已存在、重名、仍被引用）、
`500` 其余（多半是数据根坏了，用户改不了）、**`501` 功能还没做**。

**兜底路由：没有的路由不该报「服务器坏了」（提交 `4ddb515`）**：

```text
原来      `Get(R"(/api/.*)")` 兜底里**硬编码 response.status = 500**，
          消息「操作尚未实现：<path>」——调用方看到 500 只会以为服务器坏了
实测      GET /api/nosuch → HTTP 500 + FMT-602 操作尚未实现：/api/nosuch
现在      ① 路径落在**已知模块**下但没有这个接口（/api/file | /api/trash |
             /api/share | /api/config | /api/server | /api/preview）→ **501 + FMT-602**，
             消息「接口尚未实现：<path>」
          ② 完全打错的 /api/... 路径 → **404 + 新错误码 FMT-017 RouteNotFound**，
             消息「没有这个接口：<path>」
          ③ 已知路由但业务上找不到对象（如 GET /api/file/nope.bin）→ 404 + FMT-002（不变）
```

> **`FMT-017 RouteNotFound`（提交 `4ddb515`，退出码 3）**：只由 **HTTP 兜底路由**产生——
> 管道入口没有「路由」概念（op 名写错是另一回事）。它属 `FMT-0xx` 通用一组，
> 默认消息「没有这个接口」。
>
> **`FMT-602` 的映射也一并改了**：从「落 `default: 500`」改成**显式 501**——
> 「服务端还没实现这个接口」是 501，而 500 是在说服务器内部坏了。用例
> `Server.Bucket路由与状态码` / `Server.File路由与上传` 附近覆盖了这三类响应。

> **`FMT-106 FileNameLikeFileId` 已登记为 400（提交 `6a40742`）**：`http_status_for()` 的
> 400 那组 `case` 里已有 `case ErrorCode::FileNameLikeFileId:`，所以
> `POST /api/file` 带 `file_name = fmt-20261008-0` 现在是 **400 + `FMT-106`**
> （用例 `Server.File路由与上传` 钉着这条）。原口径「它没登记、会落 `default: 500`」
> **已作废**（`6a40742` 之前确实是 500）。
>
> **`FMT-016 ConfirmRequired`（提交 `711da4c`）同样属 400**：永久删除（两级）、
> 跨 Bucket 删除、非空桶删除在缺 `force` 时返回它（通用规则见 `FMT 开发文档.md` 第 82 节）。
> 原口径「缺确认 → `FMT-001`」**已作废**——独立编号让脚本能区分「参数写错」与「忘了确认」。
> 缺确认的 DELETE 请求因此是 **400 + `FMT-016`**。

**回收站路由的状态码（提交 `4fee290`；`0fc242b` 起两级通用，标识见 10.4）**：

```text
GET    /api/trash                    200（列表：entries + count + files + buckets）
GET    /api/trash/<标识>             200（条目详情 {entry}）；找不到条目 → 404 + FMT-400；
                                     标识命中多条 → 400 + FMT-001（并列候选）
POST   /api/trash/<标识>/restore     200（回退 {entry, message}）；
                                     目标已存在 → 409 + FMT-401；随桶删除 → 404 + FMT-402；
                                     数据缺失 → 404 + FMT-002；?dry_run=1 → 200 + 预检
DELETE /api/trash/<标识>             200（永久删除 {entry, message}）；
                                     **?dry_run=1 → 200 + 预检**（needs_confirm / blocked /
                                     entry / message）；**缺确认 → 400 + FMT-016**
                                     （提交 711da4c 起，原来记的是 FMT-001）：
                                     ?force=1 / force=true / body {"force":true}；
                                     找不到条目 → 404 + FMT-400
```

`FMT-001` 在回收站这一组里只剩「标识有歧义 / 条目缺记录」一种含义；
**「永久删除没有确认」现在是 `FMT-016`**（提交 `711da4c`）。
`DELETE` 的 `?dry_run=1` 与 `?force=1` 由 `src/server/server.cpp` 的 `delete_args()`
统一解析（`/api/trash/<标识>` 与 `/api/file/<标识>` 共用），翻译成 `args.dry_run` / `args.force`。

### 12.6 下载与流式传输

```text
查询 File → 检查 is_trash → 检查实际文件 → 检查 metadata
   → 检查 size → 必要时校验 MD5 → 打开文件 → 缓冲区流式传输 → 完整完成
```

- 大文件**不得**一次性读入内存，用固定大小缓冲区（如 64 KB）循环发送
- 传输中断视为失败，不计数
- 只有**完整成功**才 `download_count + 1`

---

### 12.7 错误码 / 退出码 场景表（原 `FMT 重构设计.md` 第 9 节）

> **来源**：整体搬自 `FMT 重构设计.md` 第 9 节（该文件已并入本文与其他两份文档，随后删除）。
> 它是「**什么场景 → 哪个 FMT 编号 → 哪个退出码**」的对照表，与 12.5 的「错误码 → HTTP 状态码」互补；
> 错误码本身的含义与分组见 `FMT 项目架构.md` 附录 A。

退出码沿用旧表：`0` 成功、`1` 通用、`2` 参数、`3` 对象不存在、`4` 冲突、`5` 权限/访问、`6` 数据一致性、`7` 配置、`8` Service。`FMT-NNN` 定位原因，退出码给脚本分类，两层不混用。

| 场景 | 错误码 | 退出码 |
|---|---|---|
| 服务已存在，重复 install | `FMT-600 ServiceAlreadyInstalled` | 8 |
| 服务不存在 | `FMT-601 ServiceNotInstalled` | 8 |
| `service status` 发现服务未安装（打印 `服务状态：未安装`，但**不提权**） | `FMT-601 ServiceNotInstalled` | 8 |
| `service status` 成功（含「已安装但已停止」） | — | 0 |
| SCM 操作失败 / 提权等待超时（60 秒） | `FMT-602 ServiceOperationFailed` | 8 |
| `ShellExecuteExW` 其它失败 / 结果文件不存在（提权副本崩了） | `FMT-602 ServiceOperationFailed` | 8 |
| **提权结果文件被 temp 清理误删**（提交 `5b316b3` 已修：启动清理只清十分钟以前的 `fmt-*`） | `FMT-602 ServiceOperationFailed`（「提权副本没有返回结果」）——**假失败**：提权副本报成功、服务其实已装好并启动，父进程却读不到结果文件，因为 `service install` 会在同一次操作里启动服务，而服务启动时把 `fmt-elev-<pid>.json(.tmp)` 一起清掉了 | 8 |
| 需要管理员权限（打印「需要管理员权限」，或 `ShellExecuteExW` 失败且 `ERROR_ACCESS_DENIED` 5） | `FMT-603 AdminRequired` | 5 |
| 用户在 UAC 点「否」（`ERROR_CANCELLED` 1223） | `FMT-004 PermissionDenied` | 5 |
| **需要显式确认的操作缺 `force`**——永久删除（两级 `trash delete`）、跨 Bucket 的 `file delete`、非空桶的 `bucket delete`（提交 `711da4c`） | **`FMT-016 ConfirmRequired`**（默认消息「该操作需要显式确认（force）」；具体消息说明要确认什么，例如「永久删除不可恢复，需要确认（force = true）」「<预检消息>；确认删除请加 force（CLI：--yes）」；**HTTP 400**） | 2 |
| **HTTP ：缺少 / 无效的 token**（提交 `4b812b5`） | **`FMT-018 Unauthorized`**（**HTTP 401**，退出码 5）：`/api/*` 都要 token，唯一例外是 `/api/ping` 与 `/api/share/<id>/download`（分享链接本身就是凭证）；认证**只在一处**（httplib pre-routing 钩子——「漏给某条路由加认证」是这类代码最典型的事故），且**没有注入校验器时一律 401**（fail-closed）；请求头收 `X-FMT-Token` 与 `Authorization: Bearer`，比较常量时间；token 从 `config list` 拿 | 5 |
| **HTTP 兜底路由：完全打错的 `/api/...` 路径**（提交 `4ddb515`，真机实测后定；`GET /api/nosuch` 原来是 `500 + FMT-602`） | **`FMT-017 RouteNotFound`**（默认消息「没有这个接口」，**HTTP 404**；**只由 HTTP 兜底路由产生**——管道入口没有「路由」概念）。**提交 `4b812b5` 追加一种用法**：`/api/bucket*` 也返回它——**故意不要**（用户明确「桶不要」），不是「还没做」 | 3 |
| **HTTP 兜底路由：已知模块下没有这个接口**（`/api/file/list` 这类，提交 `4ddb515`；`bucket` **不在**已知模块里） | `FMT-602 ServiceOperationFailed`（消息「接口尚未实现：<path>」，**HTTP 501**——原口径落 `default: 500`，等于说「服务器坏了」；分享的流式下载端点属于这一类） | 8 |
| 回收站条目不存在（`trash get` / `trash delete` 也走它，不只是回退） | `FMT-400 TrashEntryNotFound` | 3 |
| 上传时来源协议不是 http/https（如 `ftp://`，提交 `a2b6cd1`；原文此处写的是 `https://`，已作废） | `FMT-300 UrlInvalid`（「只支持 http:// 与 https:// 的来源：<来源>」；**不是 `FMT-002`**） | 2 |
| 上传时 URL 带用户名密码 / 端口非法 / 缺主机名（提交 `a2b6cd1`） | `FMT-300 UrlInvalid` | 2 |
| 下载超时（`ERROR_WINHTTP_TIMEOUT`，提交 `a2b6cd1`） | `FMT-302 DownloadTimeout` | 1 |
| 下载因域名/连接/TLS/响应异常失败，或服务器返回非 identity 的压缩内容（提交 `a2b6cd1`） | `FMT-301 DownloadFailed`（证书类失败附「根证书不受信任 / 证书主机名不符 / 证书已过期」） | 1 |
| 上传时文件名推不出来（如 `http://example.com/`，提交 `188e85d`） | `FMT-100 FileNameEmpty`（「无法从来源推断文件名，请显式给出文件名」） | 2 |
| 上传时本地路径找不到，而**原始输入里带不可见格式字符**（从聊天窗口/网页/终端复制路径夹进来的 `U+202A` 等，提交 `a9af276`；清掉后仍找不到） | `FMT-002 FileNotFound`（「本地文件不存在：<清理后的路径>（你粘贴的路径里有不可见字符 U+202A、U+202C，它会让路径对不上；已自动清掉，请检查路径是否还有别的问题）」；只去掉引号/空白时另有一句说明） | 3 |
| 文件名 / Bucket 名里**带**不可见格式字符（提交 `a9af276`；名字要长期存下来、还要被用户重敲一遍） | 文件名 `FMT-101 FileNameInvalidChar`、Bucket 名 `FMT-202 BucketNameInvalid`（消息点名码位，「请把名字重敲一遍」） | 2 |
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

## 13. Windows Service

### 13.1 实现方式：单一 exe 的三种形态

**冻结决策：产物只有一个 `fmt.exe`，它有三种形态，全部是同一个 target 的运行期分支。**

| 形态 | 启动方式 | 完整性/账户 | 生命周期 | 干什么 |
|---|---|---|---|---|
| **CLI 形态** | 用户双击 / 命令行（`asInvoker`） | 中完整性，普通用户 | 交互会话 | 解析命令；双击时对自己数据根做幂等体检与补齐（11.13）；`service install/uninstall/start/stop` 走提权路径、`service status` 本地直连 SCM；业务命令走管道 |
| **Service 形态** | SCM 调用 `StartService` | 高完整性，`LocalSystem` | 常驻 | 管道服务端 + HTTP + 业务执行者；失败时把 FMT 编号写进 `dwServiceSpecificExitCode`（13.2.3） |
| **提权短命副本** | CLI 用 `runas` 拉起 | 高完整性 | 做完就退 | 只做一次 SCM 操作，结果回传父进程。**`service status` 不进这里**——它不提权 |

```text
fmt.exe（同一个二进制）
├── wmain → StartServiceCtrlDispatcherW 成功 → Service 形态
├── wmain → 失败且 ERROR_FAILED_SERVICE_CONTROLLER_CONNECT → CLI 形态
├── wmain → 其它失败 → FMT-602 / 退出码 8
└── wmain → 命令行含 --elevated（内部参数）→ 提权副本形态
```

**manifest 必须是 `asInvoker`，绝不 `requireAdministrator`**（见 1.6）：
服务由 SCM 拉起，清单对它没有影响；而 CLI 要能在普通用户下执行查询类命令，
写成 `requireAdministrator` 会导致「每次双击都弹 UAC」，连 `file list` 都跑不了。

Windows SCM API（`advapi32`）脉络：

```text
[CLI / 提权副本侧]
OpenSCManagerW → CreateServiceW / OpenServiceW / DeleteService
   → ChangeServiceConfig2W(SERVICE_CONFIG_FAILURE_ACTIONS)
   → StartServiceW / ControlService(SERVICE_CONTROL_STOP)
   → QueryServiceConfigW（读 binPath）

[CLI 侧，状态查询 —— 不提权、不进提权副本，统一封装成 service::query_status()]
OpenSCManagerW(SC_MANAGER_CONNECT) → OpenServiceW(SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)
   → QueryServiceStatusEx(SC_STATUS_PROCESS_INFO)  → StatusInfo{state, wait_hint_ms,
                                                              win32_exit_code, service_exit_code}
   → QueryServiceConfigW（服务宿主 binPath）
   → 读 %ProgramData%\FMT\service.json（当前数据根）
   ↑ service status 命令、等待落定、last_start_failure() 都只走这一个入口（13.4.2）
     —— 旧的「QueryServiceStatus + map_state」写法已被取代

[服务侧]
wmain → StartServiceCtrlDispatcherW(serviceTable)
   → ServiceMain
      → RegisterServiceCtrlHandlerExW → SetServiceStatus
      → 初始化 → 起 HTTP 线程 → 等停止事件 → 收尾 → SetServiceStatus(STOPPED)
      → 初始化失败 → SetServiceStatus(STOPPED, ERROR_SERVICE_SPECIFIC_ERROR, FMT 编号)
```

**`service *` 直连 SCM API，不走管道也不走 HTTP**——Service 可能尚未安装，
通过它自己转发会引导死锁。其中 `status` 连提权都不需要（13.4.1）。
其余命令一律走命名管道（11.2）。

### 13.2 入口分发与 30 秒限制

#### 13.2.1 `wmain` 的分发骨架

```cpp
// src/main.cpp
SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS        g_status{};
HANDLE                g_stop_event = nullptr;

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 1) 提权短命副本：内部参数，优先级最高
    //    形如：fmt.exe --elevated install --result "D:\FMT\temp\fmt-elev-1234.json"
    //    （数据根不可写时才是 C:\Users\me\AppData\Local\Temp\fmt-elev-1234.json，见 4.2 / 13.8.3）
    //    operation ∈ install | uninstall | start | stop | reinstall（见 13.8.2 / 13.8.3）
    //    注意：service status 不提权，永远不会走到这个分支（见 13.4.1）
    if (has_argument(argv, argc, L"--elevated")) {
        return service::run_elevated(argc, argv);   // 只做一次 SCM 操作后退出
    }

    // 2) 让 SCM「认领」本进程：成功了就是 Service 形态
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(L"FMT"), reinterpret_cast<LPSERVICE_MAIN_FUNCTIONW>(ServiceMain) },
        { nullptr, nullptr },
    };
    if (StartServiceCtrlDispatcherW(table)) {
        return ExitCode::Success;                    // Service 形态：正常收到 STOP 后返回
    }

    // 3) 分发失败：只有一种错误码代表「不是被 SCM 启动的，我是普通程序」
    const DWORD error = GetLastError();
    if (error != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        std::cerr << "入口分发失败：" << fmt::win::format_message(error) << '\n';
        return ExitCode::ServiceError;               // FMT-602 / 8
    }

    // 4) CLI 形态
    try {
        return cli::run(argc, argv);                 // 单实例 → 权限检查 → 管道/SCM
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << '\n';
        return ExitCode::GeneralError;
    } catch (...) {
        std::cerr << "Fatal: unknown error\n";
        return ExitCode::GeneralError;
    }
}
```

`StartServiceCtrlDispatcherW` 的行为要点：

```text
成功   → 阻塞在内部，直到 ServiceMain 返回（SCM 停止服务）
1053   → ERROR_FAILED_SERVICE_CONTROLLER_CONNECT：本进程不是 SCM 启动的
其他   → 真的出问题了，按 FMT-602 / 退出码 8 处理，不要静默当 CLI 跑
```

**注意 `1053` 这个名字的两种出现场合**（容易混淆）：

| 场合 | 含义 |
|---|---|
| `StartServiceCtrlDispatcherW` 返回失败且 `GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT`（1053） | **正常**：说明是用户在命令行直接运行，应进入 CLI 形态 |
| SCM 启动服务时超时 | **故障**：SCM 报「服务没有及时响应启动或控制请求」（错误 1053），见 13.2.2 |

#### 13.2.2 SCM 的 30 秒启动时限

**SCM 只等服务 30 秒**（默认 `ServicesPipeTimeout`）。`ServiceMain` 里如果初始化很慢
（建目录、扫 `data/file.json` 算 file_id 基数、起 HTTP、等管道就绪），
超过 30 秒没有上报状态，SCM 就会：

```text
判定启动失败 → 报错 1053「服务没有及时响应启动或控制请求」
            → 若配了 Recovery，还会按 13.5 的规则把它反复重启
```

**因此初始化期间必须先上报 `SERVICE_START_PENDING` 并周期性递增 `dwCheckPoint`。**

```text
三阶段上报（ServiceMain 内部）：

1) 一进 ServiceMain（还没做任何初始化）：
   SetServiceStatus(SERVICE_START_PENDING, checkpoint = 1, wait_hint = 30000)

2) 初始化过程中（每完成一个耗时步骤后）：
   纯计算步骤仍需 < 3 秒，不做额外上报
   超过 3 秒仍不能完成的步骤（扫描大数据目录、等待管道实例就绪）
     → checkpoint++ → SetServiceStatus(SERVICE_START_PENDING, ...)
   wait_hint 给「预计还要多久」，不是「已经等了多久」

3) 初始化完成、可以接受请求：
   SetServiceStatus(SERVICE_RUNNING, checkpoint = 0, wait_hint = 0)

初始化中途失败：
   SetServiceStatus(SERVICE_STOPPED, win32_exit_code = ERROR_SERVICE_SPECIFIC_ERROR,
                    service_specific_exit_code = <FMT 编号的数字部分>)
   —— 例：FMT-008 ConfigError → service_specific_exit_code = 8
   —— **写 FMT 编号，不写退出码**；CLI 读回后用同一张错误码表还原（见 13.2.3）
```

`SERVICE_STATUS` 的字段约定：

| 字段 | 取值 |
|---|---|
| `dwServiceType` | `SERVICE_WIN32_OWN_PROCESS` |
| `dwControlsAccepted` | `SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN`（**不含 PAUSE_CONTINUE**） |
| `dwCurrentState` | `SERVICE_START_PENDING` / `SERVICE_RUNNING` / `SERVICE_STOP_PENDING` / `SERVICE_STOPPED` |
| `dwWin32ExitCode` | 正常 `NO_ERROR`；启动失败用 `ERROR_SERVICE_SPECIFIC_ERROR` |
| `dwServiceSpecificExitCode` | **仅在启动失败时使用**：填 FMT 编号的数字部分（`FMT-008` → `8`），CLI 据此还原原因 |
| `dwCheckPoint` | `START_PENDING` 时递增，`RUNNING` 时归 0，`STOP_PENDING` 时也递增 |
| `dwWaitHint` | 毫秒；仅状态切换过程中有意义 |

**没有任何初始化步骤会主动 sleep 到超时**；如果某一步做不到 30 秒内完成，
就应该在它之前先上报一次 `START_PENDING`，而不是等 SCM 报 1053。

#### 13.2.3 启动失败的编号上报与 CLI 读回

服务起不来时，SCM 自己只知道「失败了」，说不出**为什么**。为了让双击时只看到一个窗口的用户
也能知道原因，`ServiceMain` 把失败原因编码进 `SERVICE_STATUS`（读取侧见 13.4.2）：

```cpp
// ServiceMain 内的失败路径（伪代码）
void report_start_failure(ErrorCode code) {
    g_status.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState            = SERVICE_STOPPED;
    g_status.dwWin32ExitCode           = ERROR_SERVICE_SPECIFIC_ERROR;   // 1066
    g_status.dwServiceSpecificExitCode = static_cast<DWORD>(code_number(code));  // FMT-008 → 8
    g_status.dwCheckPoint              = 0;
    g_status.dwWaitHint                = 0;
    SetServiceStatus(g_status_handle, &g_status);
    log_error("服务启动失败: " + code_string(code) + " " + message_of(code));
}
```

规则：

```text
1. 填的是 **FMT 编号的数字部分**，不是进程退出码
   —— 例：FMT-008 ConfigError → 8；FMT-013 DirectoryCreateFailed → 13
2. dwWin32ExitCode 必须是 ERROR_SERVICE_SPECIFIC_ERROR，SCM 才会认 dwServiceSpecificExitCode
3. 失败时**不把异常抛给 SCM**：先上报 STOPPED，再正常返回，
   否则 SCM 只会记一个「进程异常退出」，CLI 读不到编号
4. 0 表示「没有具体编号」，CLI 侧按「启动失败（原因未知）」处理
```

CLI 侧读回（服务已安装未运行时，例如双击引导第 3 步）：

```text
1. OpenSCManagerW + OpenServiceW(SERVICE_QUERY_STATUS) → StartServiceW
2. **等它落定**：CLI 内的 settle_state()（13.4.3）
     每轮 service::query_status() 读 SCM 的 dwWaitHint，夹在 100 ms – 2000 ms 作为下次间隔
     不是等待类状态就立即结束；兜底上限 30 秒（kSettleCapMs）
3. 仍未 RUNNING → service::last_start_failure()（13.4.2），它读 query_status() 的
     win32_exit_code / service_exit_code：
     是 → 由 service_exit_code 还原 FMT 编号 → 打印「服务启动失败：FMT-008 配置错误」
     否（未安装 / 没有失败编号 / 编号不认识）→ 打印「服务启动失败（原因未知）」
```

打印形如：

```text
执行失败：FMT-602 服务操作失败
服务启动失败：FMT-008 配置错误
错误码：8
```

**这份编号同时决定「要不要重装」**：命中数据根/配置类错误码集合的，重装服务也解决不了，
只打印原因让用户先处理；其余才提权 `reinstall` 一次（见 13.12）。
**在此之前先把时间等够**——等待长度取自 SCM 的 `dwWaitHint`，不写死（13.4.3）；
否则在慢机器上会把「还在启动」误判成「启动失败」，白弹一次解决不了问题的 UAC 重装。

```text
跳过重装的数据根/配置类错误码集合：
  FMT-005 IoError                FMT-006 JsonParseError
  FMT-007 JsonWriteError         FMT-008 ConfigError
  FMT-009 StorageError           FMT-011 JsonUnsupportedVersion
  FMT-013 DirectoryCreateFailed  FMT-014 PathEscape
```

### 13.3 安装：`service install` 的服务控制程序集

```text
检查管理员权限（13.8；未提权 → 重新以 runas 启动自己）
    ↓
OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE)
    ├─ 失败 → FMT-603 AdminRequired / 退出码 5
    ↓
OpenServiceW(SCM, L"FMT", SERVICE_ALL_ACCESS)
    ├─ 成功（已存在）→ 按「幂等」处理：
    │     · 不重复创建，不修改已有配置
    │     · 提示「服务已安装」（`FMT-600 ServiceAlreadyInstalled`）
    │       → 退出码 8；随后是否继续 start 由命令决定
    └─ 失败且 GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST → 继续创建
    ↓
CreateServiceW(
    hSCM,
    L"FMT",                                  // 服务名（SCM 键）
    L"FMT File Management Service",          // 显示名
    SERVICE_ALL_ACCESS,
    SERVICE_WIN32_OWN_PROCESS,
    SERVICE_AUTO_START,                      // 开机自启
    SERVICE_ERROR_NORMAL,
    <自身绝对路径>，                           // lpBinaryPathName（带引号）
    nullptr,                                 // 无加载组
    nullptr,                                 // 不做 tag 排序
    nullptr,                                 // 无依赖
    L"LocalSystem",                          // 运行账户
    nullptr)                                 // 无密码
    ↓
ChangeServiceConfig2W(SERVICE_CONFIG_DESCRIPTION)         // 描述（可选）
ChangeServiceConfig2W(SERVICE_CONFIG_FAILURE_ACTIONS)     // Recovery，见 13.5
    ↓
（安装完成。是否立即启动由命令决定：install 之后单独 start）
```

关键字段（冻结）：

| 项 | 值 |
|---|---|
| 服务名 | `FMT` |
| 显示名 | `FMT File Management Service` |
| 启动类型 | `SERVICE_AUTO_START`（开机自启） |
| 运行账户 | `LocalSystem` |
| `lpBinaryPathName` | **首次安装时 `GetModuleFileNameW` 得到的绝对路径** |

`lpBinaryPathName` 的两条纪律：

```text
1. 绝对路径，且带引号（路径含空格时不带引号会被 SCM 拆成参数）
2. 不复制 exe 到别处，也不指向临时目录：
   宿主 exe 被移动或删除 → SCM 找不到映像 → 启动报 1053
   → 因此升级/换位置要用「复制」，不要「移动/删除」
```

**`binPath` 是安装时冻结的信息**，后续双击时要用它来对比（13.12）：

```cpp
QueryServiceConfigW(hService) → QUERY_SERVICE_CONFIG::lpBinaryPathName
```

### 13.4 启动 / 停止 / 卸载

服务命令**有五条**：

| 命令 | API 序列 | 要求 |
|---|---|---|
| `service install` | 见 13.3 | 已存在时幂等：报 `FMT-600` / 8，不重复创建；**提权** |
| `service uninstall` | `OpenServiceW(SERVICE_STOP|DELETE)` → 未停止先 `ControlService(SERVICE_CONTROL_STOP)` 并等 `SERVICE_STOPPED` → `DeleteService` | 要求服务已停止；**不得删除** `repository`、`trash`、`data`、`config`、`log`、`temp`；**提权** |
| `service start` | `OpenServiceW(SERVICE_START)` → `query_status()` 查状态 → 已是 `Running` 则视为成功（幂等）→ 否则 `StartServiceW` → `settle_state()` 等落定（13.4.3） | 不存在 → `FMT-601` / 8；**提权** |
| `service stop` | `OpenServiceW(SERVICE_STOP)` → 已停止则视为成功（幂等）→ 否则 `ControlService(SERVICE_CONTROL_STOP)` → `settle_state()` 等落定（13.4.3） | 不存在 → `FMT-601` / 8；**提权** |
| `service status` | `query_state()` + `installed_binary_path()` + `load_state()`（13.4.2：`OpenSCManagerW(SC_MANAGER_CONNECT)` → `OpenServiceW(SERVICE_QUERY_STATUS)` → `QueryServiceStatusEx(SC_STATUS_PROCESS_INFO)` / `QueryServiceConfigW`） | **不提权**；未安装 → `FMT-601` / 8（打印「服务状态：未安装」）；已安装未运行 → 照常打印 / 0 |

**没有 `pause`**：服务不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`，
`ControlService(SERVICE_CONTROL_PAUSE)` 会返回 `ERROR_INVALID_SERVICE_CONTROL`。
SCM 也不会为它显示「暂停」入口。

**没有 `delete`**：卸载命令的名字统一是 **`uninstall`**。旧文档里的 `service delete`
作废（`delete` 这个名字留给业务命令，如 `file delete`、`bucket delete`）。

轮询等待的写法（**不要用固定 sleep**）：所有等待都走 CLI 内的 `settle_state()`，
间隔取自 SCM 的 `dwWaitHint`（见 13.4.3），不要在各命令里各写一套 `Sleep`。

```cpp
// 唯一实现：src/cli/cli.cpp
// 间隔 = clamp(dwWaitHint, 100 ms, 2000 ms)；兜底上限 30 秒（kSettleCapMs）
service::State settled = settle_state(service::query_state(), kSettleCapMs);
if (settled != service::State::Running) { /* 按「没起来」处理 */ }
```

**四条动作命令都走 UAC 提权**（见 13.8），即使目标状态已经满足也照常提权——
不做「已运行就免提权」之类的优化：保持单一代码路径比省一次 UAC 确认更重要。

#### 13.4.1 `service status`：不提权的查询命令

`service status` 是**唯一不进入提权流程**的 `service` 命令。它不是「免提权优化」，
而是语义上就不需要：它只读、不改变任何 SCM 状态。

它不自己拼 SCM 调用，而是走统一的查询接口（13.4.2）：

```cpp
// src/cli/cli.cpp（示意，与实现同名）
const service::State state = service::query_state();
const std::string name(service::state_name(state));         // 「运行中」等
std::printf("服务状态：%s\n", name.c_str());

if (state == service::State::NotInstalled) {
    return exit_code(ErrorCode::ServiceNotInstalled);       // FMT-601 / 8
}

// 「服务宿主」：服务注册时写的 binPath（去掉引号）
if (Result<std::string> host = service::installed_binary_path(); ok(host)) {
    std::printf("服务宿主：%s\n", std::get<std::string>(host).c_str());
}
// 「服务数据根」：%ProgramData%\FMT\service.json 里的 current_root
if (Result<service::ServiceState> recorded = service::load_state(); ok(recorded)) {
    const std::string root = std::get<service::ServiceState>(recorded).current_root;
    if (!root.empty()) std::printf("服务数据根：%s\n", root.c_str());
}
return 0;                                                   // 退出码 0
```

要点：

```text
1. 权限只要 SC_MANAGER_CONNECT / SERVICE_QUERY_STATUS / SERVICE_QUERY_CONFIG
   —— 这三项普通用户就有，**不需要管理员**，所以不弹 UAC
2. 不走提权副本、不生成结果文件、不写 temp/
3. 未安装（ERROR_SERVICE_DOES_NOT_EXIST）不是「操作失败」：
   它是 query_status() 的**正常结果**（state = NotInstalled）→ 打印「服务状态：未安装」
   + FMT-601 / 退出码 8
4. 已安装但未运行（如 SERVICE_STOPPED）是**成功**：照常打印「已停止」+ 退出码 0
5. 输出顺序固定：服务状态 → 服务宿主 → 服务数据根 → 错误码
```

输出样例：

```text
服务状态：运行中
服务宿主：D:/FMT2/fmt.exe
服务数据根：D:/FMT2
错误码：0
```

未安装时：

```text
服务状态：未安装
错误码：8
```

**`dwServiceSpecificExitCode` 也在这里被读回**：服务「已安装未运行」时，
CLI 会先尝试 `StartServiceW`，若等不到 `SERVICE_RUNNING` 就调
`service::last_start_failure()`（同样基于 `query_status()`，见 13.4.2）拿到 FMT 编号并打印
`服务启动失败：FMT-008 配置错误`（见 13.2.3）。这就是双击引导第 3 步
「提权 start → 等它落定 → 仍没起 → 读失败编号」的实现，等待时长见 13.4.3。

#### 13.4.2 查询接口：`State` / `StatusInfo` / `query_status()` / `query_state()` / `last_start_failure()`

**所有对 SCM 状态与失败编号的读取都走这一组接口**，不在别处直接调
`QueryServiceStatusEx` + 自己映射状态——旧的「`QueryServiceStatus` + `map_state`」
写法已被取代。接口定义在 `include/fmt/service/service.hpp`：

```cpp
// include/fmt/service/service.hpp
namespace fmt::service {

enum class State {
    NotInstalled,     // OpenServiceW 报 ERROR_SERVICE_DOES_NOT_EXIST
    Stopped,          // SERVICE_STOPPED
    StartPending,     // SERVICE_START_PENDING
    StopPending,      // SERVICE_STOP_PENDING
    Running,          // SERVICE_RUNNING
    ContinuePending,  // SERVICE_CONTINUE_PENDING
    PausePending,     // SERVICE_PAUSE_PENDING
    Paused,           // SERVICE_PAUSED
    Unknown,          // 查询到了服务，但状态码无法识别（不猜测）
};

std::string_view state_name(State state);   // 状态名映射的唯一出处

// 一次状态查询的完整结果。等待类状态会带上 SCM 自己估计的 dwWaitHint：
// 「还要多久」由 SCM 说，别写死秒数。
struct StatusInfo {
    State state            = State::Unknown;
    DWORD wait_hint_ms     = 0;   // SCM 的 dwWaitHint，等待落定用（13.4.3）
    DWORD win32_exit_code  = 0;   // 正常 NO_ERROR；启动失败 ERROR_SERVICE_SPECIFIC_ERROR
    DWORD service_exit_code = 0;  // dwServiceSpecificExitCode：失败时的 FMT 编号数字部分
};

// 查询服务状态。**不需要管理员权限**。
// 「未安装」是一个正常状态（state = NotInstalled）；只有查询本身失败才返回错误。
Result<StatusInfo> query_status();

// query_state() 只是 query_status() 的薄封装：取 state；失败时给 Unknown
// （调用方若需要区分「未知」与「查询失败」，请直接用 query_status()）。
State query_state();

// 已安装服务注册的可执行文件路径（去掉引号）；未安装返回 FMT-601 错误。
// 这就是 service status 打印的「服务宿主」。
Result<std::string> installed_binary_path();

// 服务最近一次启动失败的原因：也基于 query_status()，读 win32_exit_code / service_exit_code。
// 把 service_exit_code 经 code_from_string 还原成 ErrorCode。
// 没有失败信息（未安装 / 不是 SPECIFIC_ERROR / 编号为 0 / 编号不认识）时返回错误（FMT-602）。
Result<ErrorCode> last_start_failure();

// ---- 服务自身状态（%ProgramData%\FMT\service.json），供 status 打印「服务数据根」----
struct ServiceState {
    static constexpr int kVersion = 1;
    int         version = kVersion;
    std::string current_root;   // 当前数据根
    std::string host_path;      // 服务宿主 exe 的绝对路径
    std::string installed_at;   // 安装时间
};
Result<ServiceState> load_state();

} // namespace fmt::service
```

约定（**契约，不是实现细节**）：

| 项 | 约定 |
|---|---|
| 「未安装」 | `query_status()` 的**正常结果**：`ok` + `state == State::NotInstalled`。**不是错误** |
| 「已安装未运行」 | 同样是正常结果：`ok` + `state == State::Stopped`。**不是错误** |
| 什么才算错误 | 只有**查询本身**失败：`OpenSCManagerW` 打不开、`OpenServiceW` 报非「不存在」的错、`QueryServiceStatusEx` 返回失败 → `FMT-602` / 退出码 8 |
| 错误码由谁决定 | `query_status()` 只报「查询失败」；**「未安装要返回 FMT-601 / 退出码 8」是命令层（`service status`）的判断**，见 13.4.1 |
| `wait_hint_ms` | 原样透传 SCM 的 `dwWaitHint`，不做加工；夹取与兜底在 13.4.3 的等待函数里 |
| `service_exit_code` | 只在 `win32_exit_code == ERROR_SERVICE_SPECIFIC_ERROR` 时有意义；否则为 0 |
| 状态名 | `state_name(State)` 是唯一出处：`未安装` / `已停止` / `正在启动` / `正在停止` / `运行中` / `正在继续` / `正在暂停` / `已暂停` / `未知` |
| `ServiceState` 不是状态枚举 | `State` 是运行状态枚举；`ServiceState` 是 `service.json` 的结构体。**两者不要混用** |
| 是否提权 | **不需要**。`query_status()` 只用 `SC_MANAGER_CONNECT` + `SERVICE_QUERY_STATUS`，`installed_binary_path()` 只用 `SERVICE_QUERY_CONFIG`（13.4.1） |

三个调用方：

```text
service status 命令         → query_state() + installed_binary_path() + load_state()（13.4.1）
service start 的落定判定     → settle_state() 内反复 query_status()，用 wait_hint_ms 决定下一次间隔（13.4.3）
双击引导「仍没起」的排查     → last_start_failure() → 打印「服务启动失败：FMT-008 配置错误」，
                              并据此决定要不要重装（13.2.3 / 13.12）
```

#### 13.4.3 等待落定：按 SCM 的 `dwWaitHint` 自适应

**不再写死等待时长。** 服务处于 `State::StartPending` / `State::StopPending` 时，
每一轮查询都把 SCM 给的 `dwWaitHint` 读出来，夹在 **100 ms – 2000 ms** 之间，
作为**下一次查询的间隔**；整体**兜底上限 30 秒**。状态一旦不是等待类
（`Running` / `Stopped` / `Paused` / `NotInstalled` …）就**立即结束等待**，不空转。

实现在 CLI 侧：`src/cli/cli.cpp` 的文件内辅助函数 `settle_state()`，
三个常量 `kMinStepMs = 100`、`kMaxStepMs = 2000`、`kSettleCapMs = 30000`。
它返回**落定后的 `State`**，返回值仍是等待类就说明「没起来」：

```cpp
// src/cli/cli.cpp（示意，与实现同名）
constexpr int kMinStepMs  = 100;
constexpr int kMaxStepMs  = 2000;
constexpr int kSettleCapMs = 30000;   // 兜底上限 30 秒

service::State settle_state(service::State state, int cap_ms) {
    int waited = 0;
    while (waited < cap_ms &&
           (state == service::State::StartPending || state == service::State::StopPending)) {
        Result<service::StatusInfo> info = service::query_status();
        if (!ok(info)) break;                       // 查询本身失败 → 退出循环，不算落定
        state = std::get<service::StatusInfo>(info).state;

        int step = static_cast<int>(std::get<service::StatusInfo>(info).wait_hint_ms);
        if (step < kMinStepMs) step = kMinStepMs;    // dwWaitHint = 0 / 异常值 → 退回下限
        if (step > kMaxStepMs) step = kMaxStepMs;    // 不让一次睡太久
        if (waited + step > cap_ms) step = cap_ms - waited;   // 最后一步不越过上限
        if (step <= 0) break;

        Sleep(static_cast<DWORD>(step));
        waited += step;
    }
    return state;                                    // 仍是 StartPending/StopPending = 没落定
}
```

规则（冻结）：

```text
间隔来源   每轮读 SCM 的 dwWaitHint；**不用固定值、不用「已等多久」反推**
夹取区间   100 ms（下限，避免忙轮询）– 2000 ms（上限，避免一次睡太久错过状态变化）
兜底上限   整体 30 秒（kSettleCapMs）；到此仍未落定 → 返回值仍是等待类，按「没起来」处理
提前结束   状态一旦不是等待类就立即结束，不等满间隔、不等满上限
最后一步   若剩余额度小于本轮间隔，就只睡剩余额度，不越过上限
失败处理   query_status() 返回错误（打不开 SCM 等）→ 直接退出循环，不继续空转
适用范围   service start / service stop 的落定判定、双击引导第 3 步
（人工 service status 只查一次，不等待——它回答的是「现在什么样」）
```

**为什么不能写死 8 秒**：`ServiceMain` 在启动时要建目录、读 JSON、扫 `data/file.json`
算 file_id 基数、起 HTTP、等管道就绪（13.7.1）。在慢机器、大目录、杀毒软件介入的情况下，
这些步骤完全可能超过 8 秒——此时服务**并没有失败**，只是还在 `START_PENDING`。
写死 8 秒会把「还在启动」误判成「启动失败」，接着白弹一次 UAC 去做根本解决不了问题的
`reinstall`（而且重装之后同样慢，于是用户被反复弹 UAC）。**等多久，由 SCM 自己说**：
`dwWaitHint` 就是服务在 `SetServiceStatus` 时上报的「预计还要多久」（13.2.2 的第 2 步），
它比任何硬编码数字都更接近真相。

> 与 13.2.2 的关系：上报侧（`ServiceMain`）负责把 `dwCheckPoint` / `dwWaitHint` 报准，
> 等待侧（CLI 的 `settle_state()`）负责照它等。**两侧必须成对**——只改等待侧而不上报
> `dwWaitHint`，就退化回「每次都等到下限 100 ms 或上限 2000 ms」，虽然不会误判失败，
> 但轮询会偏密。

### 13.5 Recovery 配置（服务崩溃后自动重启）

不实现自建 watchdog，使用 Windows Service Recovery：

```text
服务异常退出 → Windows 检测 → 按下面间隔重新启动 fmt.exe
```

`ChangeServiceConfig2W(SERVICE_CONFIG_FAILURE_ACTIONS)` 的参数（冻结）：

```cpp
SC_ACTION actions[3] = {
    { SC_ACTION_RESTART,  5000 },   // 第一次失败：5 秒后重启
    { SC_ACTION_RESTART, 10000 },   // 第二次失败：10 秒后重启
    { SC_ACTION_RESTART, 30000 },   // 后续失败  ：30 秒后重启
};

SERVICE_FAILURE_ACTIONSW fa{};
fa.dwResetPeriod = 86400;          // 失败计数重置周期：1 天（秒）
fa.cActions       = 3;
fa.lpsaActions    = actions;
fa.lpRebootMsg    = nullptr;       // 不弹重启提示
fa.lpCommand      = nullptr;       // 不跑外部命令

ChangeServiceConfig2W(hService, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
```

要点：

| 项 | 说明 |
|---|---|
| `SC_ACTION_RESTART` | 让 SCM 重新 `StartService`，而不是重启机器 |
| 时间单位 | `SC_ACTION::Delay` 是**毫秒** |
| `dwResetPeriod` | 是**秒**；1 天 = 86400。计数值清零后重新从「第一次失败」开始 |
| `lpCommand` | 留空。服务自己的日志（第 14 节）已经够用，不需要再挂外部命令 |
| 配置时机 | `service install` 时（13.3），不单独提供命令 |

> Recovery 是「进程崩了自动拉起来」，**不是**「业务出错重试」。
> 业务错误由 `Result`/`Error` 返回，不靠重启解决。

### 13.6 服务自身状态文件 `%ProgramData%\FMT\service.json`

**冻结决策：服务自身的状态不写在数据根里**，而是写在机器级的
`%ProgramData%\FMT\service.json`。

```text
%ProgramData%\FMT\service.json
├── 当前数据根路径
└── 安装信息
```

**它不是业务数据**：

| 对比 | 数据根内 | `%ProgramData%\FMT\` |
|---|---|---|
| 内容 | `repository/ trash/ config/ data/ log/ temp/` | `service.json` |
| 归属 | 业务，随数据根切换 | 服务自身，与数据根无关 |
| 谁写 | 服务（业务层、生命周期层） | 服务（生命周期层） |
| CLI 会读吗 | 会（通过服务） | **只读**：`service status` 会读它来打印「服务数据根」（13.4.1）。CLI **从不写**它 |

文件格式（`version` 与业务 JSON 同套路，便于演进）：

```json
{
  "version": 1,
  "current_root": "D:\\FMT",
  "binary_path": "D:\\FMT\\fmt.exe",
  "installed_at": "2026-10-07T22:03:59"
}
```

| 字段 | 说明 |
|---|---|
| `current_root` | **当前数据根**。服务开机自启且无 CLI 连接时，就取这个值 |
| `binary_path` | 安装时登记的宿主 exe 路径（与 SCM 的 `binPath` 应一致，便于诊断） |
| `installed_at` | 首次安装时间，只作诊断信息 |

读写时机：

```text
读：
  1. Service 启动，还没有 CLI 连进来 → 读 current_root 作为初始数据根
  2. **CLI 执行 `service status` 时**（13.4.1）→ 读 current_root 打印「服务数据根」；
     文件不存在或读不出来时，该行留空并记一行 WARN，**不创建文件**
  3. current_root 缺失 / 文件不存在 / JSON 损坏
     → 服务侧回退到「服务宿主 exe 所在目录」（GetModuleFileNameW → 父目录）

写（.tmp 原子替换，见 6.6）：
  1. service install 时写入 binary_path / installed_at
  2. 根切换成功时更新 current_root（见 13.10）
```

目录创建：`%ProgramData%\FMT\` 不存在时由服务创建（**CLI 不创建**）——
`service status` 只读，读不到就当「数据根未知」，绝不替服务建这个目录。
`LocalSystem` 对该目录有写权限，无需改 ACL。

**损坏时不得静默重建**：与业务配置一致——报错并保留原文件，让用户看到问题。
但服务**不应因此拒绝启动**：读失败 → 记 ERROR 日志 → 回退到宿主目录继续运行。

### 13.7 ServiceMain / HandlerEx 骨架与状态机

#### 13.7.1 关键 API 序列

```text
ServiceMain（SCM 在本进程内调用，dwNumServicesArgs 形式参数，本服务不需要）
  1. RegisterServiceCtrlHandlerExW(L"FMT", HandlerEx, this)   → 得到 SERVICE_STATUS_HANDLE
  2. SetServiceStatus(SERVICE_START_PENDING, checkpoint = 1, wait_hint = 30000)
  3. g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr)   // 自动复位=false（手动复位）
  4. 初始化：
       4.1 读 %ProgramData%\FMT\service.json → 定初值数据根（13.6）
       4.2 bootstrap_root(root)（4.4.1：目录 + 默认 JSON + 配置 + 业务服务；
           与 CLI 双击时的 check_root 是**同一份实现**，见 11.13）
       4.3 清理 <root>/temp 下**十分钟以前**的、以 fmt- 开头的遗留文件
           （上次异常退出留下的提权结果等；用户手放的其它文件不动），
           删除数量记一行 INFO（提交 5b316b3 起按年龄过滤：一律清会把 service install
           正在等的提权结果文件删掉 → 假失败 FMT-602，见 4.2、13.8.3）
       4.4 起管道监听线程（ipc::PipeServer，13.8）
       4.5 读 server.json → enabled 时起 HTTP 线程（第 12 节）
     期间每个超过 3 秒的步骤前 SetServiceStatus(START_PENDING, ++checkpoint)
  5. SetServiceStatus(SERVICE_RUNNING, checkpoint = 0, wait_hint = 0)
  6. WaitForSingleObject(g_stop_event, INFINITE)               // 等停止事件
  7. 收尾（见 13.7.3）
  8. SetServiceStatus(SERVICE_STOPPED)                          // 最后一步
  9. CloseHandle(g_stop_event); g_stop_event = nullptr
```

失败路径：第 4 步任一步失败 → 记 ERROR 日志 →
`SetServiceStatus(SERVICE_STOPPED, win32_exit_code = ERROR_SERVICE_SPECIFIC_ERROR,
service_specific_exit_code = <FMT 编号的数字部分>)` → 返回。**不要在这里 `exit()`**，
让 `ServiceMain` 正常返回，SCM 才能正确记账；CLI 双击引导时能读回这个编号并打印
`服务启动失败：FMT-008 配置错误`（13.2.3）。

#### 13.7.2 HandlerEx

```cpp
DWORD WINAPI HandlerEx(DWORD control, DWORD /*event_type*/,
                       LPVOID /*event_data*/, LPVOID /*context*/) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        // 1) 先上报：SCM 最多再等 STOP_TIMEOUT 毫秒
        g_status.dwCurrentState = SERVICE_STOP_PENDING;
        g_status.dwCheckPoint   = 1;
        g_status.dwWaitHint     = 30000;
        SetServiceStatus(g_status_handle, &g_status);
        // 2) 只负责「叫醒」主线程；收尾逻辑全在 ServiceMain 里，见 13.7.3
        SetEvent(g_stop_event);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        // 只回报当前状态，什么都不改
        SetServiceStatus(g_status_handle, &g_status);
        return NO_ERROR;

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;   // 未声明的控制（含 PAUSE_CONTINUE）
    }
}
```

Handler 的纪律：

| 纪律 | 原因 |
|---|---|
| 只处理 `STOP` / `SHUTDOWN` / `INTERROGATE` | 服务不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`，所以不会有 PAUSE/CONTINUE |
| `INTERROGATE` 只返回当前状态 | 它是「问一句」，不是「做一件事」 |
| Handler 里不做耗时工作 | Handler 由 SCM 的控制线程调用；它卡住，SCM 就无法再与这个服务通信 |
| `g_status` 的更新与 `SetServiceStatus` 必须有同步 | 两个线程（SCM 控制线程、ServiceMain）都会碰它；用一把小锁或单一写者（ServiceMain 拥有状态，Handler 只发事件） |

**推荐单一写者模型**：`HandlerEx` 只把 `g_stop_event` 置位并上报 `STOP_PENDING`，
其余状态都由 `ServiceMain` 线程切换。这样 `g_status` 不会有两个写者。

#### 13.7.3 停止流程

```text
收到 SERVICE_CONTROL_STOP / SHUTDOWN
  1. 停止接受新请求
       · gate 置「关闭」：管道服务端不再受理新的 op（已连接的下一帧被拒）
       · HTTP 停止新请求进入（server.stop() 之前先拒绝新连接）
  2. 等在途关键操作完成
       · 上传 / 删除事务 / 根切换（这两类会动磁盘，不能半途被打断）
       · 只读查询（list / get）允许被丢弃
       · 等待上限：STOP_TIMEOUT（约 30 秒）；超时 → 记 ERROR 日志并继续收尾
  3. 停止 HTTP  → server.stop() → join HTTP 线程
  4. 停止管道   → 关闭监听、断开已连接实例
  5. 落盘       → config 节流写入的未落盘改动（退出前必须落盘）
  6. SetServiceStatus(SERVICE_STOPPED)
```

停止过程中如果耗时较长，应在上报 `STOP_PENDING` 后周期性 `dwCheckPoint++`
（与启动侧同理），避免 SCM 认为服务失控。

### 13.8 UAC 提权与结果回传

#### 13.8.1 是否需要提权：查令牌而不是猜

```cpp
bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}
```

**不用「用户名是否在 Administrators 组」判断**：UAC 下管理员账户的普通令牌
也在该组里，判断会得到「已提权」的错误结论。`TokenElevation` 才是真实状态。

#### 13.8.2 提权启动（父进程侧）

**命令行形状（已定稿）**：

```text
结果文件：<数据根>\temp\fmt-elev-<父进程 pid>.json
          ← 跟着 exe 走（数据根下的 temp/），用户一眼能找到、随时可清；
            不是服务目录、也不是 %ProgramData%
            数据根不可写（例如 exe 放在只读位置）时才退回 %TEMP%\fmt-elev-<父进程 pid>.json，
            并写一行 WARN 说明原因与改用后的路径
命令行  ：fmt.exe --elevated <operation> --result "<结果文件的绝对路径>"
operation ∈ install | uninstall | start | stop | reinstall
            （五种，全是「动作」；**service status 是查询、不提权，不在这个集合里**，
              它连 --elevated 分支都进不去，见 13.4.1）
```

其中 `reinstall` = 先卸载（**服务未安装时忽略该错误**）再安装并启动，用于「服务宿主 exe
已丢失、需要重新指向当前目录」的场景（4.8 节表格第三行），**一次 UAC 做完**。

**提交 `c573f14` 起 `reinstall` 不再是「引导流程内部用法」，而是用户能敲的正式命令**：
`is_user_service_command()` 加入它（该函数从匿名命名空间移到 `cli.hpp`，便于用例钉住），
两处用法提示、命令总览、`help service` 正文都跟着更新。于是
**「用户能敲的 service 子命令」=「提权副本的 operation」+ `status`**（六条；上面那五种提权，
`status` 查询不提权）。三个「不变」与「为什么这是更新 exe 的正确路径」见 13.8.4。

```cpp
// 0) 先确保 <数据根>\temp 存在；建不出来就退回 %TEMP% 并记一行 WARN（见 4.2）
//    提权副本在写结果文件前也会自己 ensure 一次目录，它有权限，所以调用方只读时仍能建出来
const std::wstring result = elevation_result_path();   // <root>\temp\fmt-elev-<pid>.json
                                                       // 退路：%TEMP%\fmt-elev-<pid>.json

// 1) 先删除可能残留的结果文件：上一次崩溃留下的旧结果会让本次误判
DeleteFileW(result.c_str());

// 2) 组装参数：--elevated <operation> --result "<绝对路径>"
const std::wstring self = executable_path();                  // GetModuleFileNameW
const std::wstring params = L"--elevated " + op
                          + L" --result \"" + result + L"\"";

SHELLEXECUTEINFOW sei{};
sei.cbSize       = sizeof(sei);
sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
sei.lpVerb       = L"runas";                                  // ← 触发 UAC
sei.lpFile       = self.c_str();                              // ← 必须是具名变量，见下
sei.lpParameters = params.c_str();
sei.nShow        = SW_HIDE;                                   // ← 必须隐藏，见下
if (!ShellExecuteExW(&sei)) {
    const DWORD error = GetLastError();
    if (error == ERROR_CANCELLED)     return /* FMT-004 PermissionDenied / 退出码 5 */;
    if (error == ERROR_ACCESS_DENIED) return /* FMT-603 AdminRequired / 退出码 5 */;
    return /* FMT-602 ServiceOperationFailed / 退出码 8 */;
}

// 3) 等子进程结束（上限 60 秒）→ 取退出码 → 关句柄
const DWORD wait = WaitForSingleObject(sei.hProcess, 60000);   // 最多等 60 秒
if (wait == WAIT_TIMEOUT) return /* FMT-602 ServiceOperationFailed / 退出码 8 */;
DWORD code = 0;
GetExitCodeProcess(sei.hProcess, &code);
CloseHandle(sei.hProcess);

// 4) 读结果文件（读完删除），按 {ok, code, message, exit} 打印，见 13.8.3
```

> **实测教训（必须遵守）：`SHELLEXECUTEINFOW::lpFile` 必须指向具名变量。**
>
> 写成 `info.lpFile = path_from_utf8(self).c_str();` 会让指针指向一个**在语句结束就析构的
> 临时 `std::wstring`**，`ShellExecuteExW` 拿到的是悬垂指针。实测表现为 **Win32 1155
> `ERROR_NO_ASSOCIATION`（「没有应用程序与此操作的指定文件有关联」）**——报错文本里完全
> 看不出和提权有关，最容易往「路径写错了 / exe 没找到」的方向白排查。
> 正确写法是先落到一个具名变量（`const std::wstring self = executable_path();`），再取
> `.c_str()`；`lpParameters` 指向的字符串同理，必须活到 `ShellExecuteExW` 返回为止。

**必须写明：`runas` 启动的控制台程序会另开一个控制台窗口。**

`fmt.exe` 是控制台子系统程序（`/SUBSYSTEM:CONSOLE`）。用 `ShellExecuteExW(L"runas", ...)`
拉起时，Windows 会在**高完整性上下文里新建一个控制台窗口**，
即使用户只是敲了一条 `service stop`，屏幕上也会闪出一个黑框。两个后果：

| 后果 | 处理 |
|---|---|
| 闪窗，观感差、内容还看不清 | `nShow = SW_HIDE` 隐藏窗口 |
| 隐藏之后提权副本的输出用户看不到 | 结果**写进结果临时文件回传父进程**，由父进程读回打印（13.8.3） |

**不能靠「提权副本自己打印」**：窗口被隐藏，它写 stdout 没人看得见；
而且它一旦变成高完整性进程，它的窗口也不能被中完整性进程置前（UIPI，见 11.11）。
所以正确分工是：

```text
提权副本：只做 SCM 操作 → 把 {ok, code, message, exit} 写进结果文件 → 退出
父进程  ：读结果文件（读完删除）→ 自己打印「需要管理员权限 / 正在提权... / 执行成功... / 错误码：N」
```

#### 13.8.3 结果回传：结果临时文件（已定稿）

**定稿：临时文件。命名管道方案作废。**

```text
结果文件：<数据根>\temp\fmt-elev-<父进程 pid>.json
          父进程 pid → 并发双击也不会打架；父子同一个用户 → 两边都能读写
          跟着 exe 走（数据根下的 temp/）→ 用户一眼能找到、随时可清
退路      <数据根> 不可写（例如 exe 放在只读位置）→ %TEMP%\fmt-elev-<父进程 pid>.json，
          并写一行 WARN 说明原因与改用后的路径
```

**为什么不用命名管道**：提权副本是**高完整性进程**，高完整性进程创建的命名管道会带上
高完整性标签；而父进程（非提权的 CLI，中完整性）受**强制完整性级别（MIC）的「禁止向上写」
（No-Write-Up）**限制，**连接和读取都会被拒**——即使 DACL 已经放行也一样。这
与 13.9.2 的「坑 2」是同一个机制。要绕过它就得再给管道加 MIC 标签、放宽安全描述符，等于为了
一条一次性回传通道去削弱一处安全边界。

结果文件写在数据根下的 `temp/` 里（**同一个用户，只是令牌不同**），父子两边都能正常读写，
**不需要额外放宽任何安全描述符**；只有数据根不可写时才退回用户自己的 `%TEMP%` 并记一行 WARN。
提权副本在写入结果文件前会**自己确保 `temp/` 存在**（它有权限，所以调用方数据根只读时仍能建出来）。
`temp/` 是**按定义可清空**的目录，与 `repository/` / `data/` / `config/` 分属不同职责，
把一次性的提权结果放进去不会污染业务数据。

父进程流程（与 13.8.2 的代码一一对应）：

```text
1. 先确保 <数据根>\temp 存在（建不出来 → 退回 %TEMP% + 一行 WARN），
   再删除可能残留的结果文件 <数据根>\temp\fmt-elev-<父进程 pid>.json
2. ShellExecuteExW(lpVerb=L"runas", fMask=SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC |
                  SEE_MASK_FLAG_NO_UI, nShow=SW_HIDE)
3. WaitForSingleObject(hProcess, 60000) → GetExitCodeProcess → CloseHandle
4. 读结果文件（读完即删除），按 {ok, code, message, exit} 打印
```

提权副本（`run_elevated`）执行 SCM 操作后把结果写进结果文件并退出，
**进程退出码就是 `FMT-NNN` 映射出的退出码**（11.8 的映射表）。

结果文件内容：

```json
{ "ok": true,  "code": "FMT-000", "message": "成功",     "exit": 0 }
{ "ok": false, "code": "FMT-601", "message": "服务未安装", "exit": 8 }
```

| 字段 | 说明 |
|---|---|
| `ok` | 提权副本这次 SCM 操作是否成功 |
| `code` | `FMT-NNN` 字符串（与业务信封同格式，13.9.3）；成功为 `FMT-000` |
| `message` | 给用户看的一句话，父进程直接打印 |
| `exit` | 与子进程退出码一致：成功 0；需要管理员权限 5；SCM 操作失败与等待超时 8 |

父进程的失败分类（**1223 / 5 / 8 的分工以这张表为准**）：

| 失败点 | Win32 表现 | 处理 |
|---|---|---|
| 用户点了 UAC 的「否」 | `ShellExecuteExW` 失败，`ERROR_CANCELLED`（1223） | `FMT-004 PermissionDenied` / 退出码 **5** |
| 非管理员账户、策略禁止提权 | `ShellExecuteExW` 失败，`ERROR_ACCESS_DENIED`（5） | `FMT-603 AdminRequired` / 退出码 **5** |
| `ShellExecuteExW` 其它失败 | 其它 `GetLastError()` | `FMT-602 ServiceOperationFailed` / 退出码 **8** |
| 提权副本卡住 / 不退出 | `WaitForSingleObject` 返回 `WAIT_TIMEOUT`（60 秒） | `FMT-602 ServiceOperationFailed` / 退出码 **8** |
| 结果文件不存在（子进程崩了） | `GetFileAttributesW` 失败 | `FMT-602 ServiceOperationFailed` / 退出码 **8**，提示「提权副本没有返回结果」 |
| **结果文件被 temp 清理误删**（提交 `5b316b3` 已修） | 提权副本报 `ok=true`，父进程却读不到结果文件 | `FMT-602 ServiceOperationFailed` / 退出码 **8**——**假失败**，见下面的实测记录 |

**`FMT-602` 的一个已修成因：temp 清理与结果文件抢时间（提交 `5b316b3`）**：

```text
[Elevated] 提权副本执行成功：install（错误码 0）
[Elevated] 结果文件写入失败：无法打开文件：D:\...\temp\fmt-elev-2296.json.tmp
[Service]  清理 temp/ 中 1 个遗留临时文件
[Cli]      service install 提权失败：FMT-602 提权副本没有返回结果（退出码 1）
```

**服务其实装好并启动了，用户看到的却是失败。** 原因：`service install` 会在同一次操作里
**启动服务**，而服务启动时清理 `temp/` 里所有 `fmt-*` 文件——提权副本回传结果的临时文件
正好叫 `fmt-elev-<pid>.json(.tmp)`，**也被清掉了**。
**修复**（`src/service/runtime.cpp` 的 `clean_temp_directory()`）：**只清十分钟以前**的
（正在回传的结果文件寿命只有几十毫秒），读不到时间戳的也不动（4.2 的 temp 表同注）。

- 结果文件**读完即删**，不留在 `temp/` 里（退路情形下也不留在 `%TEMP%`）；
  也避免下次同 pid 复用时把旧结果当成新结果。上次异常退出留下的 `fmt-` 前缀文件，
  由 Service 启动时统一清理——**只清十分钟以前的**（提交 `5b316b3`：一律清会把父进程
  正在收的结果文件删掉，见上面的实测记录；见 4.2 与 13.7.1 第 4 步）。
- 提权副本退出码非 0 但结果文件存在时，**以结果文件为准**（`code` 决定错误码、`exit` 决定退出码）。
- 读取发生在 `GetExitCodeProcess` 之后（子进程已退出），所以读文件本身不需要再设超时；
  但**等待子进程必须有 60 秒上限**，否则提权副本卡住会把 CLI 一起拖死。

#### 13.8.4 失败与错误的映射

| 情况 | Win32 表现 | 处理 |
|---|---|---|
| 用户点了 UAC 的「否」 | `ShellExecuteExW` 失败，`GetLastError() == ERROR_CANCELLED`（1223） | `FMT-004 PermissionDenied`；子进程不打印「执行失败」，只打印「已取消提权」；退出码 **5** |
| 用户等了很久没确认 | 同上（也可能更晚才返回 1223） | 同上 |
| 提权副本卡住 / 不退出 | `WaitForSingleObject` 返回 `WAIT_TIMEOUT` | `FMT-602 ServiceOperationFailed`；**`TerminateProcess` 结束子进程**；退出码 **8** |
| 提权副本返回非 0 | `GetExitCodeProcess` 得到 5 / 8 等 | 原样透传父进程的退出码 |
| 提权副本回传 `ok=false` | 结果文件里 `"ok": false` | 打印 `message` + `FMT-NNN`，退出码取 `exit`（与 `code` 映射一致） |
| 提权副本没有返回结果 | 结果文件不存在 | `FMT-602 ServiceOperationFailed` / 退出码 **8**，提示「提权副本没有返回结果」（**注意一个已修成因**：结果文件被 temp 清理误删，见 13.8.3 的实测记录） |
| 当前用户不是管理员 / 策略禁止提权 | `ShellExecuteExW` 失败，`ERROR_ACCESS_DENIED`（5） | `FMT-603 AdminRequired` / 退出码 5，提示「需要管理员账户」 |
| 服务已经装过（install 时） | `OpenServiceW` 成功 | `FMT-600 ServiceAlreadyInstalled` / 退出码 8 |
| 服务不存在（start / stop / uninstall） | `OpenServiceW` 失败，`ERROR_SERVICE_DOES_NOT_EXIST` | `FMT-601 ServiceNotInstalled` / 退出码 8 |
| `reinstall` 时服务不存在 | `OpenServiceW` 失败，`ERROR_SERVICE_DOES_NOT_EXIST` | **忽略**卸载阶段的这个错误，继续安装并启动（不是失败） |

**`service reinstall` 的语义、三个「不变」与更新路径（提交 `c573f14` 起是正式命令）**：

`reinstall` 是**唯一一条会同时「卸载 + 安装 + 启动」**的子命令，实现就是提权副本里那段：

```cpp
} else if (operation == "reinstall") {
    const Status removed = service::uninstall();          // 未安装也当作可继续
    const bool removable = ok(removed) || error_of(removed)->code == ErrorCode::ServiceNotInstalled;
    status = removable ? service::install(path_to_utf8(executable_path()), true) : removed;
}
```

```text
① 业务数据不变     uninstall() 只 DeleteService，**明确不删** repository / trash / config /
                   data / log / temp（service.cpp 里那句注释就是这条口径）——
                   桶、文件、回收站、配置全部原样
② service.json 不变 卸载**不删**状态目录，所以 C:\ProgramData\FMT\service.json 里的
                   current_root 保留——重装后数据根**仍是原来那个**，
                   直到某个 CLI 连上来重新声明
③ 数据根仍由 CLI 声明   与「宿主 exe 是谁」无关；窗口横幅上的「数据根：…」（11.9）
                   与连接那一刻的换根提示（13.10、18.33）照旧生效

为什么这是「更新 exe」的正确路径（原来用户要 uninstall → install，两次 UAC）：
  服务在跑时旧宿主 exe 被占用，直接覆盖会失败；reinstall 先停掉已注册的宿主
  （解开占用），再把服务注册指向**你运行的那一份** exe 并启动。
  于是**不需要先把新 exe 复制过去**，也**只要一次 UAC**。
  宿主 exe 被移动或删除时同理（4.8 节表格第三行的修复路径就是它）。

实测（2026-10-09，对已安装服务）：服务已停止时 → exit 0、服务 RUNNING、
  宿主 = 执行它的那份 exe、数据根仍是 D:/Data/Temp/JMT/fmt、桶/文件/回收站全未变；
  服务正在运行时 → exit 0、宿主 PID 2088 → 9212（换成新进程）、STATE 仍 RUNNING、
  业务命令正常（完整记录见 18.34）。
```

**父进程的输入锁**：提权期间父进程在 `WaitForSingleObject`（以及随后的读结果文件），
不应响应 `Ctrl+C` 之外的交互。
`Ctrl+C` → 记一次中断 → 终止子进程 → 退出码 1。

#### 13.8.5 提权副本的边界（硬约束）

```text
只做 SCM 操作：install / uninstall / start / stop / reinstall（五种 operation）
  **service status 不进这里**：它不提权，直接在 CLI 进程内查 SCM（13.4.1）
  不做业务命令（不连 \\.\pipe\fmt.control，不读写数据根）
  不进入交互循环，不检查单实例互斥体（11.11）
  不带窗口（nShow = SW_HIDE），完成后立刻退出
  它的全部输出写进结果文件（<数据根>\temp\fmt-elev-<父进程 pid>.json，
  数据根不可写时退回 %TEMP%），不留控制台痕迹
```

理由：它是**高完整性进程**。让高完整性进程去碰业务数据、长时间存活，
就把「提权」从一次性动作变成了常驻攻击面。短命 + 单一职责是这条路径的安全边界。

### 13.9 命名管道协议（帧格式、DACL、MIC）

#### 13.9.1 管道与帧

```text
管道名：\\.\pipe\fmt.control（服务端监听，多实例）
        —— 提交 a340d1e 起**可被环境变量 FMT_PIPE 覆盖**（见下）
打开模式：PIPE_ACCESS_DUPLEX
类型/读模式/等待：PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT
最大实例：PIPE_UNLIMITED_INSTANCES
缓冲区：建议 64 KB 输入 / 64 KB 输出
```

```cpp
CreateNamedPipeW(L"\\\\.\\pipe\\fmt.control",
    PIPE_ACCESS_DUPLEX,
    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
    PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024,
    0, &sa);                                  // sa 见 13.9.2
ConnectNamedPipe(hPipe, nullptr);             // 每个实例一次
```

**管道名可被 `FMT_PIPE` 覆盖（提交 `a340d1e`，端到端测试的隔离前提）**：

```cpp
// include/fmt/ipc/pipe.hpp
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\fmt.control";
std::wstring pipe_name();     // 环境变量 FMT_PIPE 非空时用它，否则返回 kPipeName
```

```text
实现      src/ipc/pipe.cpp 的 pipe_name()：GetEnvironmentVariableW(L"FMT_PIPE", …)，
          取到非空值就用它；取不到、为空都回默认名
使用点    ① ServerRuntime::run() 的 accept 循环（循环外算一次，不每轮查环境）
          ② CLI 的 ensure_connected() 连接处
线上行为不变   真服务由 SCM 启动、不会带这个变量，所以默认名照旧是
          \\.\pipe\fmt.control；CLI 不带变量时也连默认名

为什么留这个口子（端到端冒烟测试的前提，tests/cli_e2e_test.cpp）
  端到端测试要在同一台机器上再起一个进程内服务，而真服务通常正占着默认管道名。
  不换名字的话：要么测试服务起不来（名被占），要么更糟——把命令打到用户的真服务上、
  动到真数据。测试进程设好 FMT_PIPE，它自己起的服务与它拉起的 fmt.exe 子进程都用
  同一个名字，于是与真服务完全隔离（实测见 18.35）。

安全边界    这是本机同一用户范围内的名字覆盖，不改变 DACL / MIC 那套
          （13.9.2 不变：管道仍然只授权交互用户、仍然降中完整性标签）。
          服务侧的变量来自 SCM 的环境，不受交互用户环境影响；谁能设 FMT_PIPE
          谁本来就能连那条管道，所以没有扩大权限面。
```

**阶段 4 的实况：`PIPE_UNLIMITED_INSTANCES` 只是创建参数，服务端一次只 accept 一条连接。**
`ServerRuntime::run()` 是唯一的 accept 循环：`accept(500)` 建一条连接 → `serve()` 在这条连接上
循环「读一个请求 → `handle()` → 写一个响应」 → **客户端断开才回到 accept**。
所以同一时刻只有一条 CLI 连接，第二个客户端 `CreateFileW` 得到 `ERROR_PIPE_BUSY`
（客户端 `WaitNamedPipeW` 等 3 秒重试，最终 `FMT-601`）。
**没有「每连接一个线程」的实现**——并发只出现在 HTTP 那侧（线程池），见 15.1。

**帧格式：`[4 字节小端长度][UTF-8 JSON]`**

```text
+--------+--------------------------------+
| length | payload（UTF-8 JSON，length 字节）|
| 4 字节 |                                |
+--------+--------------------------------+
```

- `length` 只算 payload，不含自身这 4 字节
- **一请求一响应**，靠 `id` 配对
- 因为用了 `PIPE_READMODE_MESSAGE`，一次 `ReadFile` 通常就是一个完整帧；
  但**不要依赖这点**——仍然按长度字段循环读满，避免恰好写满缓冲区时被拆帧
- 收到 `length == 0`、`length > 上限（如 64 MB）`、JSON 解析失败 → 回错误帧并断开

#### 13.9.2 两个必须处理的 Windows 坑

**坑 1：服务以 `LocalSystem` 运行，管道默认 DACL 只允许 SYSTEM / Administrators。**

`CreateNamedPipeW` 不指定 `SECURITY_ATTRIBUTES` 时用的是创建者的默认 DACL，
于是普通用户（中完整性、非管理员）连接会直接拿到 `ERROR_ACCESS_DENIED`，
表现为「CLI 永远连不上服务」。

解决：用 SDDL 显式授权交互用户。

```cpp
// D:(A;;GA;;;SY)   SYSTEM 完全控制
// D:(A;;GA;;;BA)   Administrators 完全控制
// D:(A;;GRGW;;;IU) Interactive Users 读写（Generic Read + Generic Write）
const wchar_t* kSddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";

PSECURITY_DESCRIPTOR sd = nullptr;
ConvertStringSecurityDescriptorToSecurityDescriptorW(
    kSddl, SDDL_REVISION_1, &sd, nullptr);

SECURITY_ATTRIBUTES sa{};
sa.nLength              = sizeof(sa);
sa.lpSecurityDescriptor = sd;
sa.bInheritHandle       = FALSE;

// CreateNamedPipeW(..., &sa);
// LocalFree(sd)  用完释放
```

**坑 2：强制完整性级别（MIC）——中完整性 CLI 会被「禁止向上写」挡住。**

高完整性进程创建的对象会带上**高完整性标签**；Windows 的强制完整性控制规则是
「低/中完整性进程不能写入高完整性对象」。即使 DACL 已经放行，非提权 CLI 连接
仍然会得到 `ERROR_ACCESS_DENIED`。

解决：给管道加 MIC 标签，把它降到中完整性（`ME` = Medium）。

```cpp
// S:(ML;;NW;;;ME)  Mandatory Label：No-Write-Up，标签级别 = Medium
const wchar_t* kSddlWithMic = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)S:(ML;;NW;;;ME)";
```

要点：

```text
1. DACL(S:)n 标签必须与 D: 写在同一个 SDDL 字符串里
2. 只用 SetSecurityInfo 单独设标签容易漏掉 DACL，反之亦然 → 一次设置
3. ME（中完整性）意味着：中完整性 CLI 可连接；低完整性（如 IE 保护模式）仍会被拒
4. 提权副本不连这个管道（它只做 SCM，见 13.8.5），所以不需要更高标签
5. 反过来，**提权副本到父进程的回传通道也不能用命名管道**：高完整性副本创建的管道带高
   完整性标签，中完整性父进程「禁止向上写」，连接与读取都会被拒——与坑 2 同一机制。
   这是 13.8.3 定稿「结果走临时文件」的直接原因
```

排查手法：连接返回 `ERROR_ACCESS_DENIED` 时，先用 `accesschk.exe <pipe>` 看 DACL，
再用进程资源管理器看标签；两个都要对，缺一个都连不上。

#### 13.9.3 请求与响应

请求：

```json
{ "id": 1, "op": "hello",     "root": "D:\\FMT", "pid": 12345 }
{ "id": 2, "op": "bucket.list", "root": "D:\\FMT" }
```

| 字段 | 说明 |
|---|---|
| `id` | 请求序号，服务原样回填到响应里，用于配对 |
| `op` | 操作名，点分层级：`hello`、`bucket.*`、`file.*`、`share.*`、`trash.*`、`config.*` |
| `root` | CLI 声明的数据根（**每个请求都带**，见 13.10） |
| `pid` | CLI 进程 id，只用于日志与诊断 |

`hello` 是握手帧：CLI 连上后第一帧就是它，字段只有 `id` / `op` / `root` / `pid`；
服务在该帧内完成数据根比较与（必要时）切换，回 `ok` 之后 CLI 才开始发业务命令。

**业务命令的 `args` 是「位置参数 + 同级开关」的信封（提交 `2c841c8`）**：

```json
{ "id": 3, "op": "file.delete", "root": "D:\\FMT", "args": { "argv": ["a7.jpg"], "dry_run": true } }
{ "id": 4, "op": "file.delete", "root": "D:\\FMT", "args": { "argv": ["a7.jpg"], "force": true } }
```

```text
形状       {"argv":[…], "dry_run":true, "force":true}——开关与 argv **同级**，
           不是塞进数组、也不是塞进 argv 里的字符串
谁产出     CLI 侧由 fmt::cli::argument_envelope(positional, dry_run, force) 统一产出
           （include/fmt/cli/cli.hpp，11.15）；预检与真实请求都用它
为什么     nlohmann 对**数组**用字符串下标会抛 type_error.305
           （"cannot use operator[] with a string argument with array"），
           未捕获就是 abort()：实测 fmt> file delete a7.jpg 弹出的
           「Debug Error! abort() has been called」正是它
不要自己去拼  测试与调用方都**不许**自己拼这个形状——照抄契约会漂移，
           必须共用同一个构造函数（Cli.位置参数的信封形状 钉住机制）
```

`hello` 的响应 `data` 带两个字段，用来告诉 CLI「服务现在在哪个根、这次有没有跟着换」：

```json
{ "id": 1, "ok": true, "data": { "root": "D:/FMT2", "switched": true, "previous_root": "D:/FMT" } }
```

| 字段 | 说明 |
|---|---|
| `root` | 服务处理后确认的当前数据根（等于 CLI 声明的 root） |
| `switched` | 这次声明是否**导致服务切换了数据根**；未切换时该字段不出现（缺省视为 false） |
| `previous_root` | 切换前的旧数据根；**只在 `switched` 为真时出现** |

CLI 侧：`switched == true` → **在 stderr 打印 `cli::root_switch_notice(previous, current)`**（提交 `8f2fbc5`）并记一行日志 `[Service] 数据根切换：旧 -> 新`；否则静默（横幅上的「数据根：…」照常显示，见 11.9）。
未切换时响应就是 `{ "id":1, "ok":true, "data":{ "root":"D:/FMT2" } }`。

其余业务参数按 `op` 决定，就放在同一层或 `args` 子对象里；
**字段细节随各命令实现确定**（见 19.1）。

**阶段 4 已冻结的部分：位置参数统一放 `args.argv`。**

```json
{ "id": 7, "op": "bucket.create", "root": "D:\\FMT", "args": { "argv": ["工作"] } }
{ "id": 8, "op": "bucket.delete", "root": "D:\\FMT", "args": { "argv": ["工作"] } }
```

```text
args.argv   字符串数组，与 CLI 的位置参数一一对应：
            `bucket create 工作` → op = "bucket.create"，argv = ["工作"]
            缺失或元素不是字符串 → FMT-001 InvalidArgument（service 层统一判定）
args 为空   无位置参数的命令（bucket.list）不带 args 字段即可
op 命名     "<组>.<动作>"：bucket.create / bucket.list / bucket.get / bucket.use /
            bucket.delete；file.* / share.* / trash.* / config.* 同规则
            "hello" 是唯一的非业务 op（见上）
```

这样「CLI 的哪一段是参数」不需要另做协议：`fmt> bucket create 工作` 拆出来的
`parts[2..]` 就是 `argv`，HTTP 侧则把 body 的 `name` 或路径参数解码后装进同一个
`argv`（12.3.2.1）。**两条入口因此共用同一份参数解析与同一份业务实现**。

响应（与 HTTP 响应体同一个信封，见 12.3.2）：

```json
{ "id": 1, "ok": true,  "data": { } }
{ "id": 1, "ok": false, "error": { "code": "FMT-305", "message": "Bucket 不存在" } }
```

`id` 必须在响应里原样回填；CLI 用它对齐请求，否则一次乱序就会把两个命令的输出串台。

#### 13.9.4 超时与连接失败

| 场景 | 超时 | 失败处理 |
|---|---|---|
| `WaitNamedPipeW` / `CreateFileW` 连接 | 管道不存在时最多重试 **5 秒**；权限类错误立即返回，不重试 | `ERROR_FILE_NOT_FOUND`（管道不存在）→ 重试 5 秒仍无 → `FMT-601` / 退出码 8，提示「无法连接 FMT Service，请先执行 service install」 |
| 普通命令（list / get / config 等） | **30 秒**（`ipc::kCommandTimeoutMs`） | 超时 → `FMT-602` / 退出码 8 |
| 长耗时命令（`file.upload`） | **30 分钟**（`ipc::kUploadTimeoutMs = 30 * 60 * 1000`，提交 `a2b6cd1`） | 同上，但 CLI 额外打印「服务端可能仍在处理，稍后用 file list 确认；也可以查看 log/fmt.log」 |
| 服务端写响应 | 30 秒（`ipc::kCommandTimeoutMs`） | 写失败 → 记 WARN，断开该实例，**不影响其他连接** |

> **长耗时命令那一行已从「设想」变成「实现」（提交 `a2b6cd1`）**。原文写的是
> 「由 `op` 单独声明（如 10 分钟）……要在 CLI 侧显示进度而不是静默等待」：
> 实况是**按 op 选常量**，不是每条 op 自报配置——`include/fmt/ipc/protocol.hpp` 里
> 加了 `kUploadTimeoutMs = 30 * 60 * 1000`，`src/cli/cli.cpp:651` 写死
> `operation == "file.upload" ? ipc::kUploadTimeoutMs : ipc::kCommandTimeoutMs`；
> **进度提示没做**（CLI 仍然是静默等待，只是等得更久 + 超时后明确提示）。
>
> **这是「已知边界 + 现有缓解」，不是彻底解决**：30 分钟到了仍可能出现
> 「用户看到超时、服务端还在下载甚至已入库」。原文里「10 分钟」只是个例子，
> 现取 **30 分钟**（19.1、18.20 ⑤）。

连接阶段的错误分类：

```text
ERROR_FILE_NOT_FOUND (2)        管道不存在 → 服务刚被拉起、监听还没就绪
                                → 5 秒内轮询重试；仍无 → FMT-601 / 8（见 11.12）
ERROR_PIPE_BUSY (231)           实例被占满 → WaitNamedPipeW 重试一次
ERROR_ACCESS_DENIED (5)         DACL 或 MIC 不对 → FMT-004 PermissionDenied / 5（见 13.9.2）
                                **不重试**：权限不会因为等待而变好
WAIT_TIMEOUT                    5 秒内没等到 → FMT-601 / 8
```

**服务端也必须设超时**：`ConnectNamedPipe` 之后的读操作不要让线程无限期挂着，
否则停止服务时收不回来（13.7.3 第 4 步）。

### 13.10 数据根切换流程

#### 13.10.1 谁声明、谁持有

```text
声明者：CLI
  用 GetModuleFileNameW 取自身路径的父目录 → 作为 root
  双击时先对自己这个 root 做一次幂等体检与补齐（11.13：只补缺失、不改业务数据内容）
  连接时用 hello 帧 / 每个请求的 "root" 字段声明（见 13.9.3）

持有者：Service
  维护「当前数据根」；root 与当前根不同 → 切换
  切换时对新根做幂等初始化（4.4.1，与 CLI 的体检是同一份实现）
```

#### 13.10.2 切换步骤

```text
CLI：连上管道 → 首帧发 hello{root} → 收到 ok 后再发业务命令

Service（收到任意请求中的 root）：
 1. 规范化 root（绝对路径、去尾部分隔符、大小写不敏感比较）
 2. 与「当前数据根」比较
      ├─ 相同 → 直接进入该请求的处理，不切换
      └─ 不同 → 走下面的切换流程
 3. 获取根切换锁（唯一的排他段；同一时刻只允许一次切换）
 4. 关闭 gate：新请求排队或直接被拒（返 FMT-602 ServiceOperationFailed「正在切换数据根」）
 5. 等在途关键操作（上传/删除事务）完成，上限 30 秒
 6. bootstrap_root(new_root)（4.4.1）
      ├─ 失败 → 回滚：保持原数据根，gate 重开，向 CLI 返 FMT-6xx / 退出码 7
      └─ 成功 → 继续
 7. 整体替换：paths / logger / config / 业务服务 实例（4.3）
 8. 更新 service.json 的 current_root（.tmp 原子替换，13.6）
 9. 写 INFO 日志：「数据根切换: 旧 → 新」
10. hello 响应回填 switched = true 与 previous_root = 旧根；CLI 收到后
    **在 stderr 打印换根提示**（`root_switch_notice()`，提交 `8f2fbc5`）并记一行日志
    「数据根切换：旧 -> 新」（模块 Service）——**原口径「控制台不打印这一行」已作废**
11. 开 gate；该连接上的后续命令都在新根下执行
```

三条不可动摇的规则：

```text
1. 不删旧根数据。切换只换指针，旧根原样留在磁盘上
2. 已存在的目录与 JSON 不改、不删、不覆盖（4.4.2 的「已存在 → 保持原样」）；
   已有 JSON 会被读一遍确认完整性，损坏只报告不重置
3. 切换是幂等的：声明回原来的根，再切回去，不报错、不重复初始化
   （那时 switched = false，响应里不含 previous_root）
```

**并发保护依赖单实例**（11.11）：同时只允许一个 CLI 窗口，也就同时只有一个数据根，
不会出现两个根互相打架。因此：

```text
· 服务只维护一个「当前数据根」，不按连接分别记录
· 管道一次只 accept 一条连接、连接内一问答（严格串行，15.1 ①），
  所以同一时刻只有一个 CLI 在发命令；第二个客户端会被 ERROR_PIPE_BUSY 挡住
· 业务命令共用运行体的一把互斥锁（15.1 ③），「切换」这段临界区不会被并发的业务请求穿插
· 多 CLI 窗口的场景不在本次范围（见 19.1）
```

#### 13.10.3 初始化规则由 Service 与 CLI 共用

```text
CLI                     Service
 │                        │
 ├─ check_root(D:\A)      │   ← 双击时 CLI 先对自己所在根做同一套幂等体检与补齐（11.13）
 │   建出缺失的目录与默认 JSON，读已有 JSON 确认，损坏只报告
 ├─ hello{root:D:\A} ────→│  当前根 = C:\FMT
 │                        ├─ 建 D:\A/{repository,trash,config,data,log,temp}（缺哪个补哪个）
 │                        ├─ 写默认 JSON（缺失时才写，.tmp 原子替换）
 │                        ├─ 读已有 JSON 确认可解析、版本受支持（不重置）
 │                        ├─ 加载配置、初始化业务服务
 │←── ok{root,switched,  ─┤  current_root := D:\A（写 service.json）
 │     previous_root}     │
 │   switched=true → stderr 打印换根提示 + 记一行日志「数据根切换：旧 -> 新」
 ├─ file.list ───────────→│  在 D:\A 下执行
```

规则是**同一套、幂等的**：Service 在启动/换根时执行 `bootstrap_root`，CLI 在双击时执行
`check_root`，两者是同一份实现、同一份清单，都只补缺失、都不碰业务数据内容。
CLI 仍然**不写任何业务 JSON 的内容**、不删文件、不改名；它连「这个根有没有初始化过」
都不需要问——自己先补齐，服务再用同样的规则确认一遍。

> 旧口径「初始化只由服务执行、CLI 只读不建业务目录」已作废：
> 双击时应立刻得到可用的数据根，而不是等一个可能启动失败的服务。
> 变的是**谁有权调用那套规则**，规则本身没变。

**为什么换根提示在「连接那一刻」报就够（提交 `8f2fbc5`，与 15.1 ① 互相印证）**：

```text
服务**一次只接受一条连接**（15.1 ①：管道 accept/serve 严格串行，没有每连接一线程）。
所以交互窗口握着管道时，别的 CLI **连不上**（ERROR_PIPE_BUSY → 重试 → 最终 FMT-601），
「会话中途被别人把数据根搬走」**不可能发生**——切换只可能发生在某个 CLI 连上来的那一刻。
因此不需要在每条命令前后都检查根，只需在 hello 回执 switched=true 时当场报出来
（外加横幅上常驻的「数据根：…」），覆盖就是完整的。18.33 有完整行为与实测。
```

### 13.11 单实例与窗口激活

CLI 形态的单实例由**命名互斥体**保证：`Local\FMT.CLI.v1`。

```cpp
HANDLE h = CreateMutexW(nullptr, FALSE, L"Local\\FMT.CLI.v1");
if (h == nullptr) { /* 创建失败：不阻断，退化为「允许多开」并记 WARN */ }
if (GetLastError() == ERROR_ALREADY_EXISTS) {
    activate_existing_window();    // EnumWindows → SetForegroundWindow / FlashWindowEx
    return ExitCode::Success;      // 不新建窗口
}
// 首次实例：继续往下走（连管道 / 提权 / 交互循环）
```

要点：

| 项 | 说明 |
|---|---|
| 名字 | `Local\FMT.CLI.v1`。`Local\` = 当前登录会话，不跨用户/会话互相干扰；`v1` 便于将来换协议时并存 |
| 判断方式 | `CreateMutexW` 之后立刻看 `GetLastError() == ERROR_ALREADY_EXISTS` |
| 句柄 | 进程生命周期内**不关闭**；提前 `CloseHandle` 会让互斥体消失，单实例失效 |
| 为什么不用 `FindWindow` | 窗口标题会变（版本号、状态），不可靠；互斥体是原子的 |
| 中途崩溃 | 内核对象随进程消失，不会留下「僵尸锁」 |
| 提权副本 | **不参与**单实例检查（13.8.5），所以 `service stop` 不会被「已有窗口」挡掉 |

窗口激活的细节（类名匹配、前台锁定、UIPI）见 **11.11**。

### 13.12 双击流程

双击 `fmt.exe`（无参数）时的完整技术步骤：

```text
 1. 控制台编码：SetConsoleOutputCP(CP_UTF8)
 2. 单实例互斥体 Local\FMT.CLI.v1
       ├─ 已存在 → 激活已有窗口（EnumWindows → SetForegroundWindow /
       │           FlashWindowEx）→ 退出码 0，结束
       └─ 否则继续
 3. 计算 root：GetModuleFileNameW → 父目录
    3.1 **数据根体检与补齐**：check_root(root)（11.13，与服务共用同一份实现）
          六个目录 + 六个默认 JSON，只补缺失；已有 JSON 读一遍确认，损坏只报告不重置
          这一步在打开日志器之前，所以 log/ 也在这条「新建目录」清单里
          结果**攒成几行日志**（[Cli] 数据根检查：<root> / 数据根新建目录：… /
          数据根新建文件：…，什么都没缺时是 [Cli] 数据根完整）；
          **控制台不打印任何一行**
    3.2 异常才进控制台（stderr）：数据根无法补齐、数据根文件损坏（未自动修复）：…
 4. 查 SCM 状态（**查询不需要提权**）—— 走 service::query_status()（13.4.2）
       ├─ 未安装（state == NotInstalled）→ **正常结果**，不是错误
       │     → 提权：service install（13.8）（装 + 启动，一次 UAC）
       ├─ 已安装且已停止（state == Stopped）
       │     → 提权：service start → **等它落定**：settle_state()（13.4.3）
       │           每轮 query_status() 读 dwWaitHint，夹在 100 ms – 2000 ms 作为下次间隔；
       │           不是等待类状态就立即结束；兜底上限 30 秒（kSettleCapMs）
       │           ├─ 起来了（state == Running）→ 完成
       │           └─ 仍然没起 → last_start_failure()（13.4.2）读
       │                 win32_exit_code / service_exit_code：
       │                 → 由 service_exit_code 还原 FMT 编号（code_from_string）
       │                 ├─ 命中数据根/配置类错误码集合 → **不重装**，打印
       │                 │    「服务启动失败：FMT-008 配置错误」让用户先处理
       │                 └─ 其它 → 提权 reinstall 一次（卸载 + 安装，一次 UAC），仍失败则报错
       └─ 已安装且运行中（state == Running）
             → 跳过，不提权
 5. 比较服务 binPath 与本 exe 路径
       QueryServiceConfigW(...).lpBinaryPathName  vs  自身路径
       ├─ 相同 → 正常
       ├─ 不同但该文件仍存在 → 按「作为客户端继续」处理
       │      （说明服务宿主是另一份 exe；本进程声明自己的 root 连管道）
       └─ 不同且该文件已丢失 → 询问是否重新安装服务（reinstall 指向当前目录，一次 UAC）
              → 未获同意则不再尝试连管道
 6. 连管道 \\.\pipe\fmt.control（管道不存在时最多重试 5 秒，13.9.4）
       + hello 声明 root（13.10）
       ├─ 连接失败 → 「无法连接 FMT Service，请先执行 service install」
       │              FMT-601 / 退出码 8
       └─ 成功 → 继续；响应 switched=true 时 stderr 打印换根提示并记一行日志「数据根切换：旧 -> 新」
 7. 打印横幅：
       File Manager Tool  v1.0  ( build  2026.10.08 )
       Service Running...        （未运行时为 Service Stopped...）
 8. 进入交互循环：先输出一个空行，再打印提示符 `fmt> `（11.9）；
    之后每条命令执行完同样先空一行再打印下一个提示符，空命令不重复空行
```

第 4 步「数据根/配置类问题」的错误码集合（命中就**跳过重装**）：

```text
FMT-005 IoError                FMT-006 JsonParseError
FMT-007 JsonWriteError         FMT-008 ConfigError
FMT-009 StorageError           FMT-011 JsonUnsupportedVersion
FMT-013 DirectoryCreateFailed  FMT-014 PathEscape
```

这些都不是「服务注册坏了」，重装服务解决不了；集合之外的失败（如 `FMT-602
ServiceOperationFailed`）才走「提权 reinstall 一次」。

两个容易搞错的点：

```text
· 第 4 步的查询用 SERVICE_QUERY_STATUS，普通用户就能做
  → 因此「已安装且运行中」这一常见路径完全不弹 UAC（双击无感）
  → service status 走的是同一条权限路径，所以它也不需要管理员（13.4.1）
· 「未安装」是 query_status() 的正常结果，不是错误
  → 所以「未装服务」这条路径也能走得很干净：确认 NotInstalled 之后才去提权 install（13.4.2）
· 等待时长不写死：由 SCM 的 dwWaitHint 决定（100 ms – 2000 ms 夹取，兜底 30 秒，13.4.3）
  → 慢机器上「还在启动」不会被误判成「启动失败」，也就不会白弹一次 UAC 重装
· 服务宿主 = 首次安装时注册的绝对路径，不是「当前的 fmt.exe」
  → 把 exe 挪走后双击，第 5 步就会走到「binPath 指向的文件已丢失」分支
· 第 3 步的体检在**服务之前**发生，所以服务没装/起不来时用户也已经有可用的数据根
  （这正是旧口径「初始化只由服务执行」被推翻的原因）
```

**升级/换位置的正确做法是「复制」**：复制一份新的 `fmt.exe` 到目标位置，
在那里执行一次 `service install`（覆盖注册）或先 `service uninstall` 再安装。
**移动或删除宿主 exe 会破坏服务**——SCM 找不到映像，启动时报 1053。

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
├── fmt.log        全部日志（超过 5 MB 轮转成 fmt.log.1，只留一代，14.6）
└── error.log      仅 ERROR 级（同样轮转）

FMT_ROOT/temp/     临时文件，随时可以清空（既不是业务数据、也不是日志，见 4.2）
```

日志**不是业务数据**，因此与 `data/` 分离；`temp/` 同理，只是它连「要留多久」都不承诺——
按定义随时可以清空。

**Service 与 CLI 都写日志文件，两个进程追加同一个文件：**
`<数据根>/log/fmt.log`（全部）与 `error.log`（仅 ERROR）由两个进程都以「追加」方式打开，
每行一次写入；MSVC 文件流是共享模式，因此不存在两个进程争抢同一个日志文件的问题。

旧口径是「只有 Service 写日志文件，CLI 不写 `fmt.log`」，已被推翻：用户在 CLI 里敲
`service stop`，服务随即被停掉，旧口径下这次操作在日志里**一个字都没有**——日志跟不上
用户做过什么。日志要能回答「谁在什么时候对服务做了什么、结果如何」，因此 CLI 也写。

CLI 侧的三条边界（完整清单与字段见 11.12）：

```text
1. CLI 双击时先对自己所在数据根执行与服务共用的幂等体检与补齐
   （六个目录 + 六个默认 JSON，只补缺失、已存在不动、损坏 JSON 只报告不重置，见 11.13）；
   这一步在打开日志器之前完成，所以 log/ 与 temp/ 都在同一条「新建目录」清单里。
   除此之外 CLI 不改任何业务数据：不写 data/*.json 的内容、不删文件、不改名。
   旧口径里「CLI 只允许创建 log/ 与 temp/ 这两个目录」的说法已被这一步覆盖。
2. --help 与 --version 不写日志、不创建任何目录。
3. 两个进程的数据根可能不同（服务可能被别人启动在另一个目录）：各写各自数据根下的
   log/fmt.log；这种情况下 CLI 会额外写一行 WARN，指明服务当前数据根与服务侧日志的位置。
```

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
| 模块 | 短名：`Main`、`Config`、`File`、`Share`、`Trash`、`Bucket`、`Storage`、`Http`、`Service`、`Ipc`；CLI 侧另有 `Cli`（CLI 本体）、`Elevated`（提权副本，见 11.12） |
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
- **两个进程共用同一份实现**：Service 与 CLI 各自用同一个 `log_dir` open 同一个文件，
  一律以追加方式打开（11.12）；CLI 只负责创建 `log/` 目录本身，不建任何业务目录
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

### 14.6 轮转（提交 `821aba3`）

```text
上限      fmt.log 超过 **5 MB** 轮转成 fmt.log.1（**只留一代**）；error.log 同理。
          Logger::Options::max_log_bytes：**0 表示不轮转**（测试与需要完整日志的场景）；
          默认 kDefaultMaxLogBytes = 5 * 1024 * 1024。
前置条件  日志**每行开-写-关**（append_line()），不再长期持有 ofstream。
          两个进程（CLI 与服务）共用同一个文件：长期开着的句柄既会**挡住改名**，
          也会让另一个进程继续往**已改名**的文件里写。没有这一步，轮转做不成
          ——所以这是轮转的前置条件，不是顺手改的（14.2）。
检查节奏  每写 **64 行**检查一次大小（kRotationCheckInterval）。不用时间节流：
          写入频率差异大，按行计数既便宜又确定，**测试也能预期**。
          改名失败（另一个进程正好在写）**不报错**，下一次检查再试。
轮转之后  在**新文件**里写一行说明：「日志超过 N 字节，已轮转：fmt.log -> fmt.log.1」
          ——用户翻日志能看到断点。
打开时    仍然验一次可写（写空串）✓：日志写不了要**立刻报错**，
          而不是等第一条日志静默丢掉。
```

**口径对齐**：`error.log` **仅 ERROR 级**（14.3、开发文档第 65 节）——`logger.hpp` 的头注释
原来写着「WARN 也进 `error.log`」，**与这条口径冲突**：作者一度照注释改了代码，
**被既有用例当场抓住**（`Logger.写入两个文件且ERROR单独成文件` 断言 `error.log` 只有 1 行），
于是改回代码、修掉注释。同时修掉的还有「只有 Service 打开日志文件」那句旧话
（CLI 早就往同一个 `fmt.log` 追加了，见 14.2）。提交 `821aba3`，用例见 18.36。

---

## 15. 并发与事务

### 15.1 线程模型

**只有 Service 一个进程写业务数据。** CLI 不写任何业务数据内容（它是管道客户端），
因此不存在跨进程并发——**两个例外，都不参与业务事务**：
其一，CLI 与 Service 追加同一个 `<数据根>/log/fmt.log`（见 11.12、14.2），
它是追加写、每行一次写入，不需要跨进程锁；
其二，CLI 双击时对自己数据根做一次幂等体检与补齐（11.13），它**只创建缺失的目录与默认 JSON**，
不修改任何已存在的内容，因此不会与 Service 的加载冲突（Service 侧看到的是「已存在 → 保持原样」）。

Service 内部（**阶段 4 实现为准**）：

```text
主线程              ServiceMain / SCM 回调（状态机，见 13.7）
管道运行线程         1 个：ServerRuntime::run() 是一条 accept 循环，
                    每次 accept(500) 建一条连接，然后 serve() 在这条连接上
                    「读一个请求 → handle() → 写一个响应」直到客户端断开，
                    才回到 accept —— 管道是**连接级严格串行**
HTTP 线程池          cpp-httplib 自带线程池（默认 max(8, hardware_concurrency - 1) 个线程），
                    请求并发进入：/api/ping、/api/status 不碰业务锁，是真并发
```

**没有「管道连接线程」这种东西**——实现里没有为连接起线程。由此得到四条准确说法：

> **① 命名管道是连接级严格串行。**
> `ServerRuntime::run()` 是唯一的 accept 循环：`PipeConnection::accept(500)` 一次只建立一条
> 连接，`serve()` 在这条连接上循环收发，**客户端断开前不回 accept**。因此
> **同一时刻只有一条 CLI 连接**（配套「只留一个 CLI 窗口」，13.11）；第二个客户端的
> `CreateFileW` 会拿到 `ERROR_PIPE_BUSY`，`PipeClient::connect()` 用
> `WaitNamedPipeW` 等 **3 秒**再来一次，仍拿不到就归为 `FMT-601`
> （`ServiceNotInstalled`），外层 `connect_waiting()` 再以 100 ms 步进重试到 **5 秒**总超时
> （13.9.4）。管道虽然用 `PIPE_UNLIMITED_INSTANCES` 创建，但服务端一次只 accept 一条。
>
> **② HTTP 是线程池并发。** cpp-httplib 默认 `max(8, hardware_concurrency - 1)` 个线程，
> 请求并发进入；`/api/ping`、`/api/status` 不碰业务锁，因此是真并发、不会被业务命令拖住。
>
> **③ 业务命令共用运行体的同一把互斥锁。** `ServerRuntime::handle()`（管道）与
> HTTP 的业务处理器都在 `execute_business(...)` **之前** `lock(mutex_)`，所以管道与 HTTP 的
> bucket 命令**互斥串行**。**这是互斥（mutex），不是队列（queue）**：不保证先来先服务、
> 没有优先级、没有排队长度上限、没有排队超时——抢不到锁的请求只是阻塞在 `lock()` 上。
>
> **④ 后果（阶段 5 曾经必须处理，已在提交 `188e85d` 落地）。** 一个慢业务命令会同时卡住
> **两条入口的所有业务命令**。阶段 5 的上传/下载可能持续几十秒到几分钟，如果那时仍持这把
> 锁，`bucket list`、`bucket get` 与浏览器的 bucket 请求都会一起等。两条路二选一：
> **收细锁粒度**（按 JSON 文件 / 按 `file_id` 分锁），或**把长任务移出锁**
> （登记任务 + 后台线程执行 + 轮询状态）。
>
> **实现选了后者的简化形态：「长任务不持锁」**——不加任务登记、不加进度查询 op
> （V1 只有一个 CLI 窗口、管道本身串行，登记任务没有收益），只把上传拆成两段，
> 锁只保护快的那一段：
>
> ```text
> ServerRuntime::run_upload(args)      ← 管道与 HTTP 共用（10.2、12.3.2）
>   ① 锁下取快照    paths / logger / size_limit，随即放锁
>   ② 锁外 prepare  prepare_upload()：下载或复制到 temp/、边写边算 MD5、边判大小上限
>   ③ 锁内 commit   commit_upload()：去重 → 重名 → file_id → 搬到仓库 → 写 file.json
> ```
>
> 第 ② 段几十秒到几分钟、**全程不持锁**，所以「上传时 `bucket list` 卡住」不再是事实；
> 第 ③ 段是毫秒级的一次 `lock_guard`。`file.upload` 是**唯一**走这条特殊路径的 op：
> 管道侧在 `handle()` 里、HTTP 侧在 `BusinessHandler` 里都在取锁**之前**把它拦下来
> （`commands.cpp` 的 `file_command()` 里没有 `file.upload` 分支）。
> **换根不会插进第 ② 段**：换根只由 `hello` 触发，而管道 accept/serve 是串行的（①），
> 上传期间不会再处理第二个请求，所以第 ① 段的快照在整段下载期间都成立；
> HTTP 侧每个请求各自取当前 `context_`，真正的写只在第 ③ 段的锁内发生。
> 见 15.2、15.3、18.16、18.19。

因此并发来自管道请求与 HTTP 请求两条入口，锁必须是**进程内**的。
**两条入口进的是同一个 service 层**（11.1），所以锁只需要在业务层存在一次。

**阶段 4 的实际做法：业务命令在运行体的同一把锁下串行执行。**

```text
ServerRuntime::mutex_   一把 std::mutex，持有「当前数据根 + AppContext」

管道   ServerRuntime::handle(request)
         file.upload → run_upload()（15.1 ④：① 锁下取快照 → ② 锁外 prepare → ③ 锁内 commit）
         其它已知 op → lock_guard(mutex_) → execute_business(context, op, args)
         （unknown op 在取锁之前就被 FMT-001 挡掉，不必占锁）
HTTP   BusinessHandler lambda（13.10 注册进 HttpServer）
         file.upload → run_upload()（同一份两段式实现）
         其它已知 op → lock_guard(mutex_) → execute_business(context, op, args)

效果   同一时刻只有服务在写数据根；两条入口、五条 bucket 命令之间不会互相踩
       「取当前根 + 改配置 + 搬目录」因此是一个整体，不会与并发的 list 撕裂
       file.upload 的下载段（可能几十秒到几分钟）**不在锁内**，因此不挡任何其它命令
```

V1 只有一个 CLI 窗口，串行足够；15.3 的细粒度锁（每 JSON 一把）是**这把锁里面**的事，
不改变「两条入口进同一个 service 层、共用同一批锁」的结构。
提交 `188e85d` 也没有新增第二把锁：上传的并发保护是靠「把长任务挪到锁外」实现的，
而不是靠加锁粒度——`file_id` 分配、去重、重名、写 `file.json` 仍然共用这把锁（8.3）。

**HTTP 监听器的 stop / start 必须在锁外做**（`ServerRuntime::restart_http()` 与
`request_stop()`）：

```text
错：持 mutex_ 去 http->stop() → stop() 会 join HTTP 工作线程
    而 HTTP 工作线程正在等 mutex_（它的 handler 要拿锁）
    → 互相等待，直接死锁

对：持锁把旧 HttpServer 摘出来（std::move）→ 放锁 → stop() → 起新实例 →
    再持锁装回成员；启动/换根时也必须在锁外调用 restart_http()
```

实现里这两处都写了注释（`restart_http` / `request_stop` / `handle`），改动顺序时别把
`stop()` 挪回锁里。

### 15.2 必须保护的数据

```text
file_id 分配
JSON 写入（file / share / trash / user / config）
File metadata
Share download_count
current_bucket
文件移动
当前数据根（含根切换，见 13.10）
```

**阶段 4 的现实**：这些数据由 `ServerRuntime::mutex_` **一把锁**统一保护（15.1 ③）——
好处是不可能出现「两个命令同时改 config.json」这类撕裂，代价是**一个慢命令会挡住所有命令**。
阶段 5 的上传/下载必须在 15.1 ④ 的两条路里选一条，否则「上传时 bucket list 也卡住」
会成为用户可见的故障（18.16）。

**提交 `188e85d` 的选择与效果**：选了「把长任务移出锁」，没有收细锁粒度。
于是上表里的每一项仍然由**同一把** `ServerRuntime::mutex_` 保护：

```text
file_id 分配     commit_upload() 内、锁下的同一份 file.json 快照（8.3）→ 不会重复
JSON 写入        锁内整段「读 → 改 → 写」，file.json / trash.json / config.json 都如此
File metadata    file.delete 的「搬文件 + 写 trash.json + 写 file.json」整段在锁内（10.2.4）
文件移动         同上；file.get 的 resolve_path() 也在锁内取快照
current_bucket   仍由这把锁保护
只有「下载/复制到 temp/」那一段被移出锁 —— 它不碰任何共享元数据
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
- 锁只在 Service 进程内，**不需要跨进程文件锁**（CLI 是管道客户端，不碰业务文件内容；
  它双击时的幂等补齐只创建缺失文件，不改已有内容，因此也不需要参与这些锁）；
  管道服务端与 HTTP 端共用同一批锁，因为它们调的是同一个 service 层
- 根切换（13.10）持一把独立的「根切换锁」，切换期间新请求被拒（返 `FMT-602`），
  不与业务锁嵌套获取，避免死锁

**阶段 4 的实际锁：`ServerRuntime::mutex_` 一把串行锁**（见 15.1）——上表的
`file_mutex` / `share_mutex` / `trash_mutex` / `config_mutex` / `id_mutex` 是**目标形态**，
阶段 5 落地 File 时在这把串行锁内部按需细分；`current_bucket` 与 `config.json`
（`bucket use` / `bucket create` / `bucket delete` 都会改它）现在由那把锁保护，
已经不会再出现「两个命令同时改配置」。**HTTP 的 stop / start 必须在锁外**，理由见 15.1。

**提交 `188e85d` 之后的锁策略（照实记录）**：`file` 落地**没有**引入上表那些细粒度锁，
`file_mutex` / `id_mutex` 都还没出现。理由与代价都写清楚：

```text
为什么不用细分   V1 只有一个 CLI 窗口，管道本身连接级串行（15.1 ①）；
                 真正会拖住别人的只有「下载/复制」这一段，
                 把它移出锁（两段式）就解决了 90% 的问题，比拆锁简单得多、也不易死锁
现状             一把 ServerRuntime::mutex_：file 的读与写、file_id 分配、
                 file.json / trash.json / config.json 的「读 → 改 → 写」都在它下面
上表何时兑现     如果将来放开多 CLI 窗口、或出现「上传与下载并发」的真实需求
                 （Download 属阶段 6），再按 JSON / 按 file_id 细分
```

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
- 默认路径（`<数据根>/temp/` → `repository/`）通常同卷，走第一行即可；
  只有 `temp/` 退回 `%TEMP%`（4.2）且系统临时目录在另一个盘时才是跨盘，
  这时必须走第二行的复制 → 校验 → 删除

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

`main.cpp` 捕获所有异常（完整的分发骨架见 13.2.1）：

```cpp
int wmain(int argc, wchar_t** argv) {
    try {
        return fmt::core::run(argc, argv);   // 内部分发：提权副本 / Service / CLI
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        return static_cast<int>(fmt::core::ExitCode::GeneralError);
    } catch (...) {
        std::cerr << "Fatal: unknown error\n";
        return static_cast<int>(fmt::core::ExitCode::GeneralError);
    }
}
```

**Service 形态下不能把异常抛给 SCM**：`ServiceMain` 内部自己兜住异常，
把它转成 `SetServiceStatus(SERVICE_STOPPED, dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR,
dwServiceSpecificExitCode = FMT 编号的数字部分)`（见 13.2.3 / 13.7.1），
否则服务会「无状态地消失」，Recovery 拿不到明确的失败记录，CLI 也读不到失败原因。

`std::filesystem::filesystem_error` 必须转换成可理解的错误信息，
并**指出具体路径**，而不是只输出 `error`。

---

## 17. 验证方式

### 17.1 流程

**每完成一段可运行的链路**即做一次真实运行验证，不允许所有模块做完才验证。

```text
开发 → 构建出 fmt.exe → service install + service start → 真实 CLI 跨进程跑一遍
     → 看磁盘上的实际结果
```

> 本分支从骨架重新开始（见 18.1），因此验证的第一步是**服务能被 SCM 装起来、
> 起得来、停得掉**，然后才是业务链路。提权路径（`service install` 等四条动作命令）
> 需要一次真实的管理员确认，属于必测项；`service status` 与双击时的数据根体检
> **不需要提权**，可以先用它们验证「非管理员也能看到服务状态、也能拿到完整数据根」。
> 服务起不来时要确认 CLI 能打印 `服务启动失败：FMT-xxx …` 而不是只说「启动失败」（13.2.3）。

### 17.2 为什么不是自动化单元测试

早期有过一套 Catch2 单元测试（291 个用例，全绿）。它被移除，理由不是「测试无用」，
而是这份工程的取舍变了：

- **它测的不是主线。** 用例覆盖的是各模块的孤立行为，而真正出问题的地方
  （悬垂引用、`res.status` 默认值、日志自锁死）全在**跨模块的接缝**上，
  恰恰是单元测试覆盖不到的部分。
- **它阻碍亲手构建。** Catch2 需要联网拉取，构建目录里的 `_deps` 一旦损坏
  就会阻塞配置，而工程的目标是「clone 下来就能构建」。
- **调试期反复被 `/RTC1` 弹窗挂住**，时间成本远超收益。

取代它的是**端到端实测**：起服务、用真实 `fmt.exe` 发命令、然后去看
`data/*.json` 与 `repository/` 里到底落了什么。见 18.10 的实测记录。

> **口径补充（阶段 2～4 的现状）**：上面这段说的是**上一次实现**的取舍，**Catch2 确实没有被
> 带回来**；但本分支在阶段 2 起补了一套**自研的最小测试运行器**（`tests/fmt_test.hpp` +
> `fmt_test.cpp`，`FMT_TEST(suite, 名称)` / `FMT_CHECK_EQ` 宏，**零第三方依赖、可完全离线构建**，
> 断言失败继续跑下一个用例），并在 `tests/CMakeLists.txt` 里用 `add_test(NAME fmt_tests …)`
> 接进 CTest——所以「不写自动化测试」这条**只对 Catch2 那种形态成立**：不引外部框架，
> 但模块行为仍有单元测试兜底。当前 **156 个用例、全绿**（上一轮 118 + 提交 `a2b6cd1` 新增 6 条
`HttpClient.*` + 提交 `0ad9efc` 新增 2 条 `File.*` + 提交 `5bf2c1f` 新增 2 条 `File.*`
与 1 条 `Bucket.*` + **提交 `9c3d2cb` 新增 5 条** + **提交 `0fc242b` 新增 4 条** +
**提交 `a9af276` 新增 2 条** + **提交 `2c841c8` / `5b316b3` 各新增 1 条** +
**提交 `d108c80` / `bb7a40f` / `c573f14` 各新增 1 条** +
**提交 `a340d1e` 新增 3 条** + **提交 `821aba3` 新增 2 条** +
**提交 `674d0b0` / `d5779db` 各新增 1 条**：
`Trash.文件级条目能列出并回退`、`Trash.回退遇同名冲突要拦住`、
`Trash.随桶删除的文件不能单独回退`、`Trash.永久删除文件级条目`、
`String.清理粘贴带进来的路径污染`、`File.粘贴路径里的不可见字符会被清掉`、
`Cli.位置参数的信封形状`、`Storage.两个写者同时写同一个文件不会互相踩`、
`Cli.版本文本只有一个来源`、`Cli.数据根切换提示要把两个根都说清楚`、
`Cli.service子命令集合`、`CliE2e.核心链路走真实exe与真实管道`、
`CliE2e.交互式确认答n不删答y才删`、`Ipc.管道名可被FMT_PIPE覆盖`、
`Logger.超过上限会轮转出一代`、`Logger.上限为零时不轮转`、
`Service.列表排序配置与清空回收站`、`Service.分享的创建查看列出撤销与计数`、
`Server.管理接口要token且桶路由已下线`、`App.初始化数据根会建默认账号与token`、
`App.已有空users文件时也要补建默认账号`
（另有既有用例追加断言或改口径：`File.列表与查询` 的大写 file_id、`File.软删除进回收站`
改断言 `file.json` 且断言 `trash.json` 保持空、`Service.管道能执行Bucket命令`、
`Service.破坏性操作先预检再确认`、`Service.管道能上传与操作文件`、
`Service.启动时清理temp里的遗留临时文件`（改成「新的留着、旧的清掉」）、
`Server.Bucket路由与状态码`、`Server.File路由与上传`；
见 17.2.1 与 18.19、18.21、18.22、18.23、18.24、18.25、18.27、18.28、18.29、18.31、18.32、18.33、18.34、18.35、18.36、18.37、18.38），
桶级回收站的用例见 18.17。**计数走过的台阶：118 → 124 → 126 → 129（`5bf2c1f`）
→ 134（`9c3d2cb`）→ 136（`711da4c`）→ 140（`0fc242b`）→ 142（`a9af276`）
→ 143（`2c841c8`）→ 144（`5b316b3`，`8f0fd5c` 只补断言不变）→ 145（`d108c80`）
→ 146（`bb7a40f`）→ 147（`c573f14`）→ 150（`a340d1e`）→ 152（`821aba3`）
→ 153（`674d0b0`）→ 154（`d5779db`）→ **156（`bfd89f7` / `4b812b5`：账号存储 +
认证与路由；`ff237d5` 只修测试隔离，不新增用例）**。

**测试隔离的硬教训：只隔离管道不够（提交 `ff237d5`，2026-10-09 真事故）**

```text
事故    端到端交互测试用**无参数**跑真的 fmt.exe → 那是**双击引导** → 引导会装 / 启动 /
        **重装真实的 Windows 服务**。FMT_PIPE 只隔离管道，**管不到 SCM** ✗：
        测试因此把服务注册指向了自己的临时目录，测试结束目录被清理 →
        **线上服务指向不存在的文件、部署的 fmt.exe 也没了**（已修复并 reinstall 指回真路径）。
防护    新增 **FMT_NO_SERVICE=1**（测试专用）：引导函数开头看到它就**完全不碰服务管理**；
        端到端夹具为每个子进程都设上它。
规则    凡是要跑「用户双击也会走的入口」，**必须显式切断它对 SCM / 注册表 / ProgramData
        的写入能力**——只把管道名换掉是不够的（13.9.1、18.35）。
```
> 端到端实测仍然是「真的对了」的最终判据（17.4 的硬标准不变）。
>
> **运行器的两条可观测性设计（提交 `a2b6cd1`，逐行照源码）**：
>
> ```cpp
> // tests/fmt_test.cpp：run_all()
> // 逐行刷出去：某条用例卡住（死锁、网络等待）时，最后一行就是它的名字。
> // 缓冲的话进程被强杀时什么都看不到，排查得靠猜。
> std::ios::sync_with_stdio(false);
> std::cout.setf(std::ios::unitbuf);
>
> // 每条用例**开始前**先报名字，再执行：
> std::cout << "[开始] " << full_name << "\n";
> ```
>
> 即「**每条用例开始前打印 `[开始] <套件>.<名称>`**」+「**`unitbuf` 逐行刷新**」：
> 某条卡住时，屏幕上的最后一行就是它（它的 `[通过]` / `[失败]` 还没打出来）。
> 这对 `HttpClient.https会真的做TLS握手` 这种会等网络的用例尤其重要。

> **17.2.1 新增的下载客户端用例（`tests/http_client_test.cpp`，提交 `a2b6cd1`，6 条）**：
>
> | 用例 | 覆盖 |
> |---|---|
> | `HttpClient.本地HTTP下载` | 256 KB（比读取块大，真正跑多轮读取循环）；断言 `status == 200`、`bytes` 与 `content_length` 都等于载荷长度，再**逐字节核对**内容 |
> | `HttpClient.非2xx不交给调用方` | 本地桩返回 404：`status == 404` 且 **sink 一次都没被调用**（错误页不落进调用方） |
> | `HttpClient.中止下载` | sink 第一块就返回 false：结果**成功**且 `aborted == true`、`bytes == 0`（被拒绝的那块不算收到） |
> | `HttpClient.连接失败与协议校验` | 不可路由地址 + 2 秒超时 → `DownloadFailed`/`DownloadTimeout`；`ftp://` 与越界端口 → `UrlInvalid`；`is_remote_url()` 对 http/https 为真、对本地路径为假 |
> | `HttpClient.https会真的做TLS握手` | 对**明文** HTTP 端口发 `https://` 请求 → 握手必失败，证明 https 走的是真 TLS 而不是被当成 http 处理；**用例里把三个超时都调到 2 秒**，否则会挂到默认 300 秒的接收超时 |
> | `HttpClient.真实https下载可选` | 读环境变量 `FMT_TEST_HTTPS_URL`，**没设就跳过**（不算失败）；设了就真下，并打印状态码 / 字节数 / `Content-Type` |
>
> **实测证据（已验证）**：
>
> ```text
> > $env:FMT_TEST_HTTPS_URL='https://example.com/'; .\fmt_tests.exe HttpClient
>     真实 https：https://example.com/ -> HTTP 200，577 字节，Content-Type: text/html; charset=utf-8
> 共 6 项：通过 6，失败 0
> ```
>
> 本地桩用的是 cpp-httplib 的 `httplib::Server`（**测试**里仍用它起服务器；
> 业务代码的下载侧已经不用 `httplib::Client` 了，12.1）。

> 若后续要补自动化测试，**优先补「上传 → 列表 → 下载 → 分享」这一条链路**，
> 而不是回头做各模块的孤立单元测试。

### 17.3 手工验证清单

**第 0 步：服务生命周期（本分支的第一条必测链路）**

```powershell
cd D:\FMT
.\fmt.exe service install          # 弹 UAC → 确认 → 「执行成功...」「错误码：0」
.\fmt.exe service start            # 已运行也应报成功（幂等）
.\fmt.exe                          # 双击等价：横幅 + Service Running... + 空行 + fmt> 
.\fmt.exe service stop             # 停止，退出码 0
.\fmt.exe service uninstall        # 卸载；数据目录内容不变（含 temp/）
```

预期补充检查：

```text
sc query FMT                        应能看到 FMT / FMT File Management Service
sc qc FMT                           启动类型 AUTO_START，账户 LocalSystem，binPath=当前 exe
sc qfailure FMT                     Recovery：5s / 10s / 30s 重启，重置周期 86400 秒
%ProgramData%\FMT\service.json      含 current_root / binary_path / installed_at
数据根\temp\                        提权后结果文件已读回并删除；Service 启动会清掉遗留的 fmt-* 文件
提权过程                             屏幕上**不应**出现闪过的控制台黑框（nShow = SW_HIDE）
```

**第 1 步：业务链路**（服务已启动）

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

**第 2 步：管道与数据根**

```text
服务未运行时 fmt.exe file list       → 「无法连接 FMT Service，请先执行 service install」
                                       + FMT-601 + 退出码 8
把 fmt.exe 复制到另一个目录再运行      → 声明新根，服务初始化新根并写 INFO 日志；
                                       旧根数据仍在（不删）；hello 回执 switched=true，CLI **在 stderr 打印换根提示**（提交 `8f2fbc5`）并记一行日志「数据根切换：旧 -> 新」
同一目录再双击第二次                  → 激活已有窗口，不新建控制台；
                                        日志里只多一行 [Cli] 数据根完整，控制台不变
service stop 期间发一条 file list     → 要么等完成，要么 FMT-602，不出现半写状态
                                     （FMT-602 是「服务正在停止 / 读响应超时」，
                                      与「file 命令没实现」无关——file.* 已在 188e85d 落地）
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
4. **服务不退出。** 一轮命令跑完，`sc query FMT` 必须是 `RUNNING`；
   **不是**看某个前台进程还活着（CLI 与 Service 现在是两个进程）。

---

## 18. 开发阶段与状态

### 18.1 阶段划分（本次重构后）

规范见 `FMT 开发文档.md`；本分支从**骨架重新开始**，阶段顺序按第 14 条冻结决策重排：

**Service 从旧的阶段 9 提前到阶段 3。** 理由是通道形态变了：CLI 现在是管道客户端，
而管道服务端就在 Service 进程里。如果把 Service 留到最后，阶段 3～6 的所有业务模块
都只能挂在「临时的本地调用」上验证，最后整体搬迁一次——这正是上一次踩过的返工。

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 项目骨架：CMake、Ninja、MSVC、`fmt.exe`、manifest(`asInvoker`)、`--help`/`--version`、`wmain` 入口分发骨架 | ✅ 完成 |
| 2 | 基础层：`common`（Error/Result/Time/String/Path/Logger）+ `config` + `storage` + `core` 根解析与幂等初始化（**`ensure_root`/`check_root` 由 Service 与 CLI 共用**；损坏 JSON 只报告不重置） | ✅ 完成 |
| 3 | **通道与服务**：`service`（**六条子命令** `install`/`uninstall`/`start`/`stop`/**`reinstall`**/`status`（`reinstall` 提交 `c573f14` 起正式化） + 查询接口 `query_status()`/`query_state()`/`last_start_failure()`（13.4.2）+ 按 `dwWaitHint` 自适应的落定等待（13.4.3）+ Recovery + `ServiceMain` 状态机 + 失败编号上报 `dwServiceSpecificExitCode` + `service.json`）+ `ipc`（命名管道帧、DACL、MIC、hello 的 `switched`/`previous_root` 回执）+ `cli`（管道客户端、单实例、UAC 提权引导、双击幂等体检与补齐、落定判定与 reinstall 兜底、交互循环） | ✅ 完成 |
| 4 | Bucket：create / list / get / use / delete，`current_bucket` 逻辑；`common/validation` 名称校验；两条入口（管道 + HTTP）打通；**Bucket 删除/回退的回收站形状**（目录名一律带时间戳、`trash/<user>/.original` 是桶级身份唯一权威、回退整单判定 `FMT-401`、`file.json` 增加 `trash_reason`）与桶级 `trash list` / `get` / `restore` / `delete`（`get` / `delete` 提交 `4fee290`，永久删除要 `force` 确认且只清 `trash_reason="bucket"` 的记录）；文件级条目落点定为 `trash/<user>/.files/<bucket>/YYYY/MM/DD/` | ✅ 完成（commit 32249ea；回收站形状 c2d545d；`.files` 落点与 `trash get` / `delete` 见 `4fee290`，明细见 18.15、**18.17**） |
| 5 | File / Upload / Trash / Share：`file.json`、`file_id`、上传（本机 + URL）、**文件级** trash（桶级四条已在阶段 4 全部落地）、share 下载计数；阶段 7 补**文件级** `trash get` / 永久删除与**文件级**部分恢复 | 🟡 **`file` 部分已完成，且 https 可用**（提交 `188e85d` + `a2b6cd1` + `0ad9efc`），`share` 未开始：四条命令 `file upload <来源> [文件名]` / `file list` / `file get <file_id\|文件名>` / `file delete <file_id\|文件名>` 全部落地（上传两段式、MD5 去重、重名拒绝、软删除写文件级 trash 条目；`file delete` 的参数在 `0ad9efc` 扩成两种，定位与 `file get` 一致）；**上传来源 `http://` 与 `https://` 都支持**——下载走 `common/http_client`（WinHTTP + Schannel，不用 OpenSSL、不分发 DLL，`a2b6cd1`），明细见 **18.19 与 18.20**；**文件级 trash 的读取侧（list/get/restore/delete）与 `share` 整组仍未实现**，前者属阶段 7、后者仍在阶段 5 待做。「https 不支持」**不再是已知限制** |

| 6 | `server`：HTTP 浏览器侧（下载/预览路由 + `/api/*`）与 Preview | ⏳ 未开始（阶段 4 已把 `/api/bucket` 五条 + `/api/trash` 四条路由落地，阶段 5 又补了 `/api/file` 四条，见 12.3.2；浏览器页面与下载/预览路由仍未开始） |
| 7 | 收尾：**永久删除时清理相关 Share**（`share.json`）+ 两级回收站口径的真机实测 | ⏳ 未开始（**当前只剩一个缺口**：share 模块属阶段 6，现在永久删除**没有**清理 `share.json` 的动作。原缺口「`file delete` 只写不读、`trash list`/`get`/`restore` 只认桶级条目」**已在 `0fc242b` 关闭**，见 10.4、18.25） |

沿用上一次的**做法调整**（结论仍然有效，见 18.8）：阶段 5 不按「先 File 元数据 →
再 Upload → 再 Share」逐层收口，而是**先把「上传 → 列表 → 下载 → 分享」这条链路
一次打通**，中间层粗糙一点也先让它跑起来。

**与旧版阶段表的对应关系**：

```text
旧 阶段 1～2   →  新 阶段 1～2      内容基本不变（新增 manifest 与入口分发）
旧 阶段 3      →  新 阶段 3 + 6     HTTP 通道拆开：服务侧通道提前，浏览器侧挪到阶段 6
旧 阶段 4～8   →  新 阶段 4～5      业务模块合并推进
旧 阶段 9～12  →  新 阶段 3 + 5    Service 提前；Trash / file delete / Preview 顺延
```

阶段 2～8 的历史完成记录与实测、踩坑清单保留在 18.2～18.12，
它们是**上一次实现的证据**，本次重构沿用其中的结论（尤其是 UTF-8 路径转换、
悬垂引用那几条），但**代码不作为起点**。

### 18.2 当前实现状态

> **本分支（`arch-restart`）从骨架重新开始。**
> 下面的「已实现」是**上一次实现的记录**（架构重构前，CLI 走 HTTP 的那一版），
> 现在**不再代表本分支的代码状态**——那套代码里的通道层（`common/net` 的 HTTP 客户端、
> `cli` 的 HTTP 执行、`--service run` / `--server` 主循环）已被本次重构取代。
> 保留它们的目的是：模块划分、踩坑结论、实测手法可以复用，**代码不作为起点**。
> 本分支的实际进度看 18.1 的阶段表。

上一次实现（历史记录）：**阶段 1 至阶段 8 的主链路已打通并端到端实测。**
不再维护 Catch2 单元测试套件（见 18.8），验证方式改为**真实 `fmt.exe` 跨进程实测**。
（本次重构**没有**把 Catch2 带回来，但补了一套自研的最小运行器，见 17.2 与 18.18。）

阶段 5～8（File + Upload + Share）实测记录见 18.10。


阶段 1（历史记录）：

| 已实现 | 位置 |
|---|---|
| 程序入口与异常兜底 | `src/main.cpp` |
| 运行目录计算（`FMT_ROOT`） | `src/core/path.cpp` |
| `--help` / `--version` / 未知参数 | `src/cli/cli.cpp` |

> 本次重构对阶段 1 的增量：`main` → **`wmain` + 入口分发**（13.2.1）、
> manifest `asInvoker`（1.6）、运行目录从「服务决定」改为「**CLI 声明 + 服务持有**」（4.1）。

阶段 2：

| 模块 | 已实现 | 位置 |
|---|---|---|
| 第三方库 | nlohmann/json 3.11.3、cpp-httplib 0.18.3（vendored）；静态链接 MSVC 运行库 | `third_party/`、根 `CMakeLists.txt` |
| `common/error` | `ErrorCode`（44 条）、`Error`、`Result<T>`、`Status`、`ExitCode`；`error_code_from_string` 支持跨进程还原 | `include/fmt/common/error.hpp` |
| `common/time` | 日志/ISO/日期/路径/日键 五种格式，严格解析，`is_expired` | `include/fmt/common/time.hpp` |
| `common/string` | `trim`、大小写、前后缀、`split`/`split_trimmed`、`join`、`replace_all` | `include/fmt/common/string.hpp` |
| `common/size` | 自动/固定单位的大小显示，`size_unit` 解析 | `include/fmt/common/size.hpp` |
| `common/logger` | `log/fmt.log` + `error.log`，三级，`[模块]`，线程安全 | `include/fmt/common/logger.hpp` |
| `common/validation` | 文件名与 URL 校验（含保留设备名、路径穿越；**提交 `9c3d2cb` 追加 `looks_like_file_id()` 与 `FMT-106`**，9.1/9.3） | `include/fmt/common/validation.hpp` |
| `storage` | 目录/文件操作，JSON 原子读写（`MoveFileEx` 替换），版本校验 | `include/fmt/storage/storage.hpp` |
| `config` | `config.json` / `server.json` 加载、校验、保存 | `include/fmt/config/config.hpp` |
| `core/PathManager` | 所有路径的唯一出口 | `include/fmt/core/path_manager.hpp` |
| `core/App` | 启动链路：目录 → 默认配置 → `server.json` → 日志 | `src/core/app.cpp` |

阶段 3（历史记录）：

| 模块 | 已实现 | 位置 |
|---|---|---|
| `common/api` | 响应信封：`{"ok":true,"data":…}` / `{"ok":false,"error":{"code":"FMT-002",…}}` | `include/fmt/common/api.hpp` |
| `common/net` | HTTP 客户端（`api_get`/`api_post`/`api_delete`）、URL 编码 `url_encode`/`fill_path_param`、错误码映射 | `include/fmt/common/net.hpp` |
| `api` | CLI 与 Service 的共同契约：Bucket/File/Share/Trash/Config 结构 + 路由常量 + JSON 编解码 | `include/fmt/api/api.hpp` |
| `server` | HTTP 服务端：路由注册、信封编码、`status_for_error` 映射、`make_default_handlers` 业务装配点 | `include/fmt/server/server.hpp` |
| `service` | SCM 生命周期：install（自动启动 + Recovery）/ start / stop / delete / status，绕过 HTTP | `include/fmt/service/service.hpp` |
| `cli` | 命令解析（纯函数）+ HTTP 客户端执行；`service` 命令直连 SCM | `include/fmt/cli/cli.hpp` |

> **这一批的具体形态已被本次重构取代**，可复用的是「分层」与「信封」两个结论：
>
> | 旧模块 | 新形态 |
> |---|---|
> | `common/api` 响应信封 | **保留**，管道与 HTTP 共用（12.3.2） |
> | `common/net` HTTP 客户端 | **删除** CLI 用途；只保留 URL 下载（12.1） |
> | `api` 路由常量 + JSON 编解码 | 保留（浏览器侧），管道另有 `ipc` 帧与 `op` 表（13.9） |
> | `server` 路由注册 + 信封编码 | 保留，但服务对象改为浏览器（12.3.2） |
> | `service` install/start/stop/delete/status | **重写**：命令改为**六条子命令** `install` / `uninstall` / `start` / `stop` / `reinstall` / `status`（旧 `delete` 更名 `uninstall`；`reinstall` 提交 `c573f14` 起从引导内部用法变成正式命令，**除 `status` 外每条都提权**）；`status` 是**查询命令、不提权**（13.4.1），不是「双击流程内部调用」；新增 `ServiceMain` 状态机与失败编号上报 `dwServiceSpecificExitCode`（13.2.3）、`service.json`、Recovery 细节（13.3～13.7） |
> | `cli` 命令解析 + HTTP 执行 | **重写**：解析保留，执行改为管道客户端 + UAC 提权引导 + 交互循环（第 11 节） |

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
service 状态查询（未安装）        → 提示服务不存在，退出码 0
                                    ← 旧行为，已被本次重构取代：新口径打印
                                      「服务状态：未安装」+ FMT-601 / 退出码 8（13.4.1）
service install（非管理员）       → 提权引导；取消 UAC → FMT-004 PermissionDenied + 退出码 5
```


端到端验证（真实运行 `fmt.exe`）：

```text
首次运行 → 创建 repository/ trash/ config/ data/ log/ temp/
         → 写出 config/config.json（缺失时）
         → 退出码 0
修改配置后再运行 → 用户修改保持不变
数据目录探针文件 → 多次运行后依然存在
配置损坏时运行   → 退出码 7，原文件字节未变
```

其余模块（`trash`、`preview`）**尚无实现**，handler 返回「本版本尚未实现」。

### 18.3 阶段 2 完成明细（历史记录）

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

### 18.4 阶段 3 完成明细（历史记录）

```text
3.1  common/net    HTTP 客户端 + 错误码往返（error_code_from_string）  ✅
3.2  common/api + api   响应信封 + CLI/Service 共同契约（结构 + 路由）  ✅
3.3  server        路由注册、信封编码、状态码映射、业务装配点          ✅
3.4  service       SCM 生命周期：install/start/stop/delete/status      ✅
3.5  cli           命令解析（纯函数）+ HTTP 客户端执行                  ✅
```

> 这五步里，**只有 3.2 的信封与错误码往返原样进入新版**；
> 3.1 的 HTTP 客户端、3.4/3.5 的 service/cli 形态都已被 13.3～13.12 与第 11 节取代。
> 当时每步配套单元测试（测试名用 ASCII），该套件后来整体移除，见 18.8。

### 18.5 阶段 3 踩到的坑（历史记录，已修，记录以免重犯）

| 现象 | 根因 | 处理 |
|---|---|---|
| `bucket create --name 新桶` 抛 `invalid UTF-8 byte 0xC2` | Windows 的 `argv` 是控制台代码页（GBK），而 JSON 要求 UTF-8 | `paths::utf8_arguments()` 用 `GetCommandLineW` 转换后交给命令层 |
| `bucket get 工作` 返回 HTTP 500 | 路径参数直接拼进 URL，中文原始字节让服务端路径匹配失败 | `net::fill_path_param` 做百分号编码，CLI 全部 10 处改用它 |
| 测试挂死无输出 | 夹具先 `join()` 再 `stop()`，而 `listen()` 阻塞到 `stop()` | 析构里先 `stop()` 再 `join()` |
| 参数化路由全部 404 | cpp-httplib 用 `std::regex_match` 全匹配，`/api/bucket/` 只能匹配自身 | 改用 `:name` 语法 + `req.path_params` |
| 先探测空闲端口再绑定后客户端连不上 | Windows 上释放与重绑之间端口可能被占用，而 `is_running()` 仍为 true | 改为一次 `bind_any_port()`，之后 `register_routes()` |
| 测试里 `AppContext` 析构 SIGSEGV | MSVC `/RTC1` 的 `_RTC_CheckStackVars` 与隐式生成析构函数组合下误报 | `AppContext` 显式声明并在 `.cpp` 定义构造/析构/移动 |
| 配置阶段 `Unknown CMake command FetchContent_Declare` | `include(FetchContent)` 被前一次编辑粘到注释行末尾 | 恢复为独立行 |

### 18.6 阶段 4 完成明细（历史记录，Bucket）

> **本节与 18.7 记的是「上一次实现」（CLI 走 HTTP 的那一版，命令形如
> `bucket create --name 工作`）。** 本次重构（`arch-restart`）的阶段 4 是**重新实现**的，
> 记录见 18.15：命令是位置参数（`bucket create 工作`）、入口是命名管道 +
> `/api/bucket` 两条、`BucketService::remove` 返回 `BucketRemoval`、
> 删除时**确实**会把该桶 `file.json` 记录的 `is_trash` 置 true。
> 本节的价值在于「Bucket 就是目录、没有独立 ID」这个结论继续有效。

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

> 注意：12.7 那张表（原 `FMT 重构设计.md` 第 9 节）的例子把「未设置当前 Bucket」写成 `FMT-305`。
> 两者编号不同（`FMT-604` = 无当前用户，`FMT-305` = 无当前 Bucket），
> **正式编号以 `FMT 项目架构.md` 附录 A 为准**；本文档只引用，不定义。

> 4.4 的 `--service run` / `--server` **已作废**：服务形态由 SCM 启动，
> 入口分发只看 `StartServiceCtrlDispatcherW` 的结果（13.2.1），不再有这两个参数。

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

### 18.7 阶段 4 踩到的坑（历史记录）

| 现象 | 根因 | 处理 |
|---|---|---|
| 中文用户名让 `path / "小谷"` 抛 `No mapping for the Unicode character` | MSVC 的 `fs::path` 从 `std::string` 构造按 **ANSI 代码页**解释，不是 UTF-8 | `common/text` 统一两个方向的转换；`PathManager` 所有用户输入片段都经 `path_from_utf8` |
| 删除 Bucket 后 Trash 里出现乱码目录 `宸ヤ綔`，看起来数据丢了 | `user_trash(user) / std::string{name}` 同一陷阱，漏改 | 新增 `text::join`；`bucket::remove` 改用它 |
| 上面这个 bug 单元测试**没抓到** | 测试只断言 `is_directory(目标)`，而乱码目录同样是目录 | 回归测试改为**读取磁盘上真实目录名**并比对；夹具的期望值也改走 `join` |
| 测试期望值与实现不一致（`[灏忚胺]` vs `[小谷]`） | 测试自己也用 `path / "小谷"` 构造期望值 | 测试期望值同样经 `path_from_utf8` |
| Release 下两个 server 测试失败、Debug 通过 | 插入新错误码后枚举值移位，而两套构建的产物来自不同版本头文件 | 重新完整构建；并记录「不要依赖枚举整数编号」 |
| 删除运行时 `--server` 无法验证 | 之前 `make_default_handlers` 从未被调用，服务主循环是空的 | 接入 `--service run` / `--server` 主循环（新版已改为 SCM 启动，见 13.2.1） |

### 18.8 阶段 5～8 的做法调整（历史记录，结论仍然有效）

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
- 只有 Service 写业务数据：现在 CLI 走命名管道（11.1），这条纪律反而更硬了——
  CLI 连数据文件都不再打开。**日志除外**：CLI 与 Service 追加同一个
  `<数据根>/log/fmt.log`（见 11.12、14.2）。

### 18.9 阶段 5～8 完成明细（历史记录）

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

> **本次重构（`arch-restart`）改了这条**：占位用户名是 **`user`**（不是 `default`），
> 且**不再自动创建 Bucket「工作」**——`current_bucket` 由用户的第一条 `bucket create`
> 决定（第一个桶自动成为当前）。见 4.4.1 第 5.5 步与 11.10。

### 18.10 阶段 5～8 端到端实测（历史记录）

真实 `fmt.exe --server` + 真实 CLI，跨进程；**从零开始**（先删掉
`config/config.json`、`data/*.json`、`repository/`）。

> 下面的命令形式是**重构前**的（当时服务用 `--server` 前台运行、CLI 走 HTTP）。
> 新版对应形式是 `fmt.exe service install` + `service start` + CLI 走管道（17.3）。
> 记录的价值在于**业务结果本身**与磁盘核对手法，命令形式不要照抄。

```text
1) config get                      → 当前用户/当前 Bucket 均为空
2) file upload --file .smoke-中文名.txt
                                   → 自动初始化：用户=default，Bucket=工作
                                   → 上传完成：.smoke-中文名.txt
                                     file_id=fmt-20261007-0
3) file list                       → fmt-20261007-0  .smoke-中文名.txt  text
4) share create --file fmt-20261007-0
                                   → share_id=64c17f38464b567e
                                     链接 http://localhost:4122/share/download/64c17f38464b567e
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

### 18.11 阶段 5～8 踩到的坑（历史记录）

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

### 18.12 阶段 5～8 之后仍未做的事（历史记录）

| 事项 | 现状 |
|---|---|
| `file delete` | CLI 与路由都在，handler 返回「本版本尚未实现」。落地方案已定：标记 `is_trash=true` + `trash_reason="file"` + 把物理文件移入 **`trash/<user>/.files/<bucket>/YYYY/MM/DD/`**（提交 `4fee290` 定下的落点；原口径写成 `trash/<user>/<bucket>/YYYY/MM/DD/` **已作废**）。**（这一行是上一次实现的记录；本次重构的提交 `188e85d` 已经把 `file delete` 做完，见 10.2.4、18.19）** |
| Bucket 删除的元数据标记 | 旧实现的 `bucket::remove` 只做了目录搬迁。第 30 节要求同时把该 Bucket 下所有文件的 `is_trash` 置 true——等 `file delete` 落地时一并做，两处必须同一套逻辑 |
| Trash 全部命令 | `trash list/get/restore/delete` 均未实现（**上一次实现的记录**） |
| Preview | `file preview` / `share preview` 返回 FMT-701 ——**提交 `d3aeb3d` 起 HTTP 侧已有预览**（`GET /api/file/<id>/preview`，只有策略允许的类型可预览，其余仍 701）；CLI 仍没有 `preview` 子命令 |
| `common/md5` 的测试覆盖 | 无自动化测试；实测与 `md5sum` 对比一致（上传后 `file.json` 里的 md5 与外部工具一致） |

> **上面第一、二行说的是上一次实现，本次重构已经修掉**：`BucketService::remove()`
> **自己就把**该桶 `file.json` 记录的 `is_trash` 置为 true（只动 `user` + `bucket`
> 都匹配、原本为 false 的记录），并在 `c2d545d` 起同时写 `trash_reason="bucket"`；
> `file delete` 落地时沿用同一套逻辑即可（10.1、18.15、18.17）。
> 第三行「Trash 全部命令均未实现」**已作废两轮**：**桶级 `trash list` / `get` / `restore` /
> `delete` 四条都已在阶段 4 落地**（`list` / `restore` 见 `c2d545d`，`get` / `delete` 见
> `4fee290`），现在未实现的只剩**文件级** list/restore/get/delete 与永久删除时的
> `share.json` 清理（阶段 5/7）。

### 18.13 本次重构后仍未做的事（`arch-restart` 分支）

按 18.1 的新阶段表倒推，阶段 1～6 之外还欠这些；**其中前三条必须实测**，
否则「服务能装、能起、能停」这条底线没有证据。

| 事项 | 现状 / 说明 |
|---|---|
| 提权路径端到端实测 | `service install/uninstall/start/stop/reinstall` 都需要一次真实的 UAC 确认；**`service status` 不需要**（它不提权，要专门验证非管理员账户下也能成功、且全程不弹 UAC）。要验证：成功路径、**取消 UAC（`ERROR_CANCELLED` 1223 → `FMT-004` / 5）**、等待超时（`WAIT_TIMEOUT` → `FMT-602` / 8）、重复 install（`FMT-600` / 8）、服务不存在（`FMT-601` / 8）、提权期间不出现控制台闪窗、结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 正确生成并在父进程读完后删除、数据根不可写时退回 `%TEMP%` 并写 WARN |
| 管道连接权限实测 | DACL（`D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`）与 MIC 标签（`S:(ML;;NW;;;ME)`）都要在**非提权 CLI** 上跑通；只测提权 CLI 会掩盖 13.9.2 的两个坑 |
| 根切换实测 | CLI 换目录运行 → 服务切根、幂等初始化新根、旧根数据不删、`service.json` 的 `current_root` 更新，且 hello 回执 `switched=true` + `previous_root`，CLI **在 stderr 打印换根提示**（提交 `8f2fbc5`）并记一行日志「数据根切换：旧 -> 新」；横幅另常驻一行「数据根：…」（11.9） |
| `service status` 实测 | 三种状态各跑一次：未安装（`服务状态：未安装` / `FMT-601` / 退出码 8）、已安装未运行（`已停止` / 退出码 0）、运行中（`运行中` + 宿主 + 数据根 / 退出码 0）；并确认它在**非管理员账户**下同样成功 |
| `dwServiceSpecificExitCode` 实测 | 人为让服务初始化失败（例如把 `data/file.json` 改成非法 JSON）→ 确认 `sc query` 能看到 `ERROR_SERVICE_SPECIFIC_ERROR`、CLI 打印「服务启动失败：FMT-006 JSON 格式错误」，并且**不触发重装** |
| `help bucket` / `help trash` 实测 | `fmt.exe --help` = 横幅 + 用法 + 命令总览 + 退出码表；`fmt.exe help` 只列命令、不加描述；`help service` 列出五条命令并注明 `status` 不需要管理员权限；`help bucket` 写明五条子命令的真实行为（**已同步，提交 `8a5e554`**），`help trash` 写明桶级 `list` / `get` / `restore` / `delete` 四条的真实行为（**已同步，提交 `c2d545d` + `4fee290`**），命令总览里 `(trash) list get restore delete` 全在「可用命令」组、「尚未实现」组只剩 `file` / `share`，`help file/share` 带「服务端尚未实现，现在返回 FMT-602」；`help 未知组` → stderr 一行 + 退出码 2；`help` 全程不提权、不连服务、不写日志。**提交 `188e85d` 之后**：`(file) upload list get delete` 也进了「可用命令」组、「尚未实现」组**只剩 `share` 一行**，`help file` 的正文已同步（11.4），这一项仍需**真机实测**（本节整体是行为记录，不是实测） |
| CLI 双击体检实测 | 空目录首次双击 → **控制台干净**（只有横幅与提示符），日志里有 `[Cli] 数据根新建目录：…` / `数据根新建文件：…`；再双击 → 日志里只有 `[Cli] 数据根完整`；预置一个损坏的 `data/file.json` → **stderr** 出现 `数据根文件损坏（未自动修复）：data/file.json` 且文件**未被改动**（比对时间戳与内容）；另测「数据根只读」→ stderr 出现 `数据根无法补齐：FMT-013 …` 且不阻断后续流程 |
| 横幅与 `--version` 实测 | 两者输出同一串 `File Manager Tool  v1.0  ( build  <日期> )`；重新配置（重跑 CMake）后日期随之变化；源码里搜不到硬编码的版本号或日期 |
| SCM 30 秒限制 | 需要一次「初始化故意变慢」的验证：确认 `START_PENDING` + `dwCheckPoint` 上报真的消除了 1053 |
| 服务停止中的在途操作 | 上传中途 `service stop` 的收尾行为（13.7.3 第 2 步）需要实测。**提交 `188e85d` 之后多了一个要看的点**：停止请求可能与锁外的 `prepare_upload()` 第 ② 段并行发生（第 ② 段不持锁），此时服务端进程退出会不会留下 `temp/fmt-upload-*.tmp` 的半成品——按设计不会进 `repository`，且启动时的 temp 清理会收走它（第 34 节），但仍需实测确认 |
| Recovery 实测 | 人为 `TerminateProcess` 服务进程，观察 5s / 10s / 30s 的重启节奏与 1 天计数重置 |
| Trash / `file delete` | **桶级四条已完成**（`trash list` / `get` / `restore` / `delete` + `trash/<user>/.original`，提交 `c2d545d` 与 `4fee290`，18.17）；**提交 `188e85d` 又做掉了写入侧**：`file delete` 会写文件级条目（`trash_reason="file"` + 移入 `trash/<user>/.files/<bucket>/YYYY/MM/DD/`）。**剩下的**是文件级条目**读取侧**的 list/get/restore/delete 与文件级永久删除、以及永久删除时的 share 清理——属阶段 7，见 10.4、18.19 |
| Preview | `file preview` / `share preview` 返回 `FMT-701`，属于阶段 6 |
| 交互式输入的 UTF-8 | 输出与参数已是 UTF-8；`std::getline(std::cin, ...)` 在 GBK 控制台下读中文的方案待冻结（`ReadConsoleW` 是候选） |
| 临时文件位置 | **新结论（已改口径）**：临时文件**统一放** `<数据根>/temp/`，跟着 exe 走——用户一眼能找到、随时可清；提权结果文件用固定名 `temp/fmt-elev-<父进程 pid>.json`（13.8.3），便于父进程直接拼出路径。唯一的退路是**数据根不可写**（例如 exe 放在只读位置）时退回系统临时目录 `%TEMP%`，并记一行 WARN 说明原因与改用后的路径。**历史脉络**：第 10.2 节旧文写「系统临时目录或 `FMT_ROOT/temp/`」，4.2 曾改为 `%TEMP%\fmt\<pid>\`，理由是「临时文件不放在数据根，避免污染被切换的目录」；**这条理由已被推翻**——`temp/` 是**按定义可清空**的目录，且它与 `repository/`、`data/`、`config/` 分属不同职责，把一个短暂的提权结果或上传暂存放进 `temp/`，不会污染业务数据 |

### 18.14 上一次实现遗留、本次仍需保留的纪律

以下四条与代码形态无关，重构后仍然适用（来自 18.5 / 18.7 / 18.11 的实测教训）：

```text
1. 所有用户输入片段拼路径，必须经 common/text 的 UTF-8 ↔ fs::path 转换
   → 否则静默写出乱码目录名（宸ヤ綔 这类），且 is_directory 检查不出来

2. 拼路径走 PathManager / text::join，不要自己用 path / std::string 拼
   → MSVC 的 fs::path 从 std::string 构造按 ANSI 代码页解释，不是 UTF-8

3. 长生命周期对象（Service、PathManager、Logger）不存值成员、不存会悬垂的引用
   → 值成员被移动后引用悬垂；Windows 上表现为访问违例而不是可读的异常

4. ShellExecuteExW 的 lpFile / lpParameters 必须指向具名变量，不能直接挂 .c_str()
   → info.lpFile = path_from_utf8(self).c_str(); 会指向语句结束即析构的临时 std::wstring
   → 悬垂指针的实测表现是 Win32 1155 ERROR_NO_ASSOCIATION
     （「没有应用程序与此操作的指定文件有关联」），完全看不出是提权问题，极难排查
   → 先 const std::wstring self = executable_path(); 再取 self.c_str()（见 13.8.2）
```

### 18.15 本次重构（`arch-restart`）阶段 4 完成明细（Bucket，commit 32249ea）

**这是本分支自己实现的记录**，不是 18.6 那份历史记录（那一次是「CLI 走 HTTP」的旧版）。
阶段 4 之后的两笔收尾：`8a5e554`（feat(cli): move bucket out of the not-implemented group in
help）与 `acc90a3`（feat(service): keep current_bucket honest at startup and on a root switch，
即下表最后一行的接线）。**第三笔收尾是 `4fee290`（feat(trash): finish the bucket level trash
commands）**：桶级 `trash get` / `trash delete`、永久删除的强制确认、文件级条目改落
`trash/<user>/.files/`、桶级扫描的形状检查与 `PathManager::build()` 的 `inner` 参数——
这些记在新增的 **18.18**。

| 交付物 | 位置 | 说明 |
|---|---|---|
| 名称校验 | `include/fmt/common/validation.hpp`、`src/common/validation.cpp` | `is_windows_reserved_name` / `kMaxNameBytes` / `validate_bucket_name` / `validate_file_name`（9.3 的错误码分工） |
| Bucket 业务 | `include/fmt/bucket/bucket.hpp`、`src/bucket/bucket.cpp` | `create` / `list` / `get` / `use` / `remove` / `directory_of` / `refresh_current_bucket`（10.1）；**桶级回收站随后在 `c2d545d` 补齐 `list_trashed` / `restore` + `.original` 索引（18.17），并在 `4fee290` 补齐 `get_trashed` / `purge` 与私有 `find_trashed` / `remove_bucket_file_records`（18.18）** |
| 占位用户名 | `src/core/app.cpp` | `initialize_root`：`current_user` 为空 → `user` 并保存（4.4.1 第 5.5 步） |
| 业务分发 | `src/service/commands.cpp` | `bucket.create/list/get/use/delete`，参数取 `args.argv[0]`；**`c2d545d` 追加 `trash.list` / `trash.restore` 的桶级实现，`4fee290` 追加 `trash.get` / `trash.delete`**（`trash.delete` 先检查 `args.force == true`，否则 `FMT-001`），未知组仍是 `FMT-602`（10.1、18.18） |
| 锁与响应 | `src/service/runtime.cpp` | 业务命令在 `mutex_` 下串行；HTTP 处理器取同一把锁；`restart_http`/`request_stop` 在锁外（15.1） |
| 当前桶校验接线 | `src/service/runtime.cpp`、`include/fmt/service/runtime.hpp` | `refresh_current_bucket_locked()`：启动（`start()`）与换根（`apply_root()`）时在锁内调用，失效置空并落盘、有效不动，失败只记 WARN（10.1、开发文档第 61 节） |
| HTTP 两入口 | `src/server/server.cpp` | `/api/bucket` 五条路由 + `http_status_for()` + `url_decode()`；**`c2d545d` 追加 `GET /api/trash` 与 `POST /api/trash/<名字>/restore`，`4fee290` 再追加 `GET /api/trash/<名字>` 与 `DELETE /api/trash/<名字>`**（后者从 `?force=1` / `force=true` / 请求体 `{"force":true}` 解析确认，缺了就不放 `force`，由 service 层统一拒绝；12.3.2、18.18） |
| 工具函数 | `src/common/string.cpp` | `url_encode` / `url_decode` |
| CLI 展示 | `src/cli/cli.cpp` | `print_business_data()`：列表 / 单条 / message 三种形状（11.14）；**`c2d545d` 追加 `deleted_buckets` 列表形状**（`original` 为空印「原名称未记录，无法回退」、`present=false` 印「目录已不存在」）；**`4fee290` 再追加条目详情形状**（`trashed` + `files` → 条目名 / 原 Bucket / 删除时间 / 目录 / 状态 / 文件数 / 占用），并在 `run_business_command()` 里实现 `trash delete` 的 `--yes` / `-y` 与交互确认（不发请求的取消保持退出码 0） |
| 路径落点 | `src/core/path_manager.cpp`、`include/fmt/core/path_manager.hpp` | **`4fee290`**：`build()` 增加可选 `inner`（插在 user 与 bucket 之间），`trash_file()` 传 `L".files"`，文件级条目落 `trash/<user>/.files/<bucket>/YYYY/MM/DD/`；`repository_file()` 不变（4.3、18.18） |
| 单元测试 | `tests/bucket_test.cpp`（**17 个** Bucket 行为用例，含回收站形状 6 个与 `4fee290` 新增的 4 个：`条目详情与永久删除` / `永久删除只清桶级记录` / `回收站扫描只认桶级条目` / `有索引没目录的条目可以永久删掉`）、`tests/validation_test.cpp`（6 个名称校验用例）、`tests/path_manager_test.cpp`（含 `回收站保持原层级`，断言新落点 `trash/小谷/.files/工作/…`）、`tests/service_test.cpp`（管道端到端：`bucket.create` / `bucket.list` / 缺参 `FMT-001` / **`管道能执行回收站命令`（含 `trash get`、未确认被拒 `FMT-001`/2、带 `force` 成功）** / `file.list` 仍 `FMT-602`（**这一条记的是提交 `4fee290` 时的状态**；提交 `188e85d` 之后
`file.list` 已经可用，`Service.管道能上传与操作文件` 就是它的端到端用例，见 18.19））、`tests/server_test.cpp`（`Bucket路由与状态码`：`GET /api/trash`、`POST …/restore`、`GET /api/trash/<名字>`、`DELETE` 缺 `force` → 400 + `FMT-001`、`?force=1` → 200）；全仓 **104 个用例**（**提交 `188e85d` 之后为 118 个**：新增
`tests/hash_test.cpp` 2 个、`tests/file_test.cpp` 10 个、`Service.管道能上传与操作文件`
1 个、`Server.File路由与上传` 1 个；`Service.管道能执行Bucket命令` 与
`Service.未实现的操作与未知操作被明确拒绝` 里原来拿 `file.list` 当「未实现」的例子
已改成 `share.list` / `share.create`，见 18.19） |

端到端验收清单（真实 `fmt.exe` + 真实服务 + 真实管道，跨进程；**按实现推演，需要在真机上逐条确认**）：

```text
bucket create 工作        → Bucket 已创建：工作（已设为当前 Bucket），退出码 0
                            磁盘上出现 repository/user/工作/
bucket create 工作（重复） → 执行失败：FMT-201 Bucket 已存在：工作，错误码 4
bucket create a/b         → 执行失败：FMT-202 Bucket 名称不能包含路径分隔符，错误码 2
bucket create 生活        → Bucket 已创建：生活（不抢「当前」），退出码 0
bucket list               → * 工作  (当前) / 生活 / 共 2 个 Bucket，退出码 0
bucket get 工作           → Bucket：工作 / 当前：是 / 路径：repository/user/工作，退出码 0
bucket get 不存在         → 执行失败：FMT-200 Bucket 不存在：不存在，错误码 3
bucket use 生活           → 已切换到 Bucket：生活，退出码 0（config.json 已落盘）
bucket delete 生活        → Bucket 已删除（移入回收站）：生活  ->  生活_<时间戳>，退出码 0
                            → trash/user/生活_<时间戳>/ 存在；trash/user/.original 一条记录
                              （trashed / original / deleted_at，无 file_id）；
                              data/trash.json 里**没有**桶级条目
trash list                → 每行「<trashed>  ->  <original>」+「共 N 个已删除的 Bucket」；
                            original 为空印「(原名称未记录，无法回退)」；
                            present=false 追加「(目录已不存在)」，退出码 0
trash restore <trashed>   → Bucket 已回退：<原名>，退出码 0；.original 少一条
trash restore <原名>      → 目标已存在时：执行失败：FMT-401 回退失败：Bucket 已存在：<原名>，
                            错误码 4；同名多条时 FMT-001 并列出候选 trashed 名，错误码 2
trash get <trashed>       → 回收站条目：<trashed> / 原 Bucket：<原名> / 删除时间：… /
                            目录：trash/user/<trashed> / 状态：在 / 文件数：N / 占用：…，
                            退出码 0；索引有目录没了 → 状态：目录已不存在（present=false），
                            退出码仍是 0；找不到条目 → FMT-400，错误码 3
trash delete <trashed>    → 交互窗口先问「永久删除回收站条目 <trashed> ？此操作不可恢复 (y/N)」：
                            答 y → 已永久删除：<trashed>（N 个文件，M 条记录），退出码 0；
                            答 n → 已取消，退出码 0，**不发请求**；
                            一次性命令不带 --yes → stderr「永久删除不可恢复：请加 --yes …」，
                            退出码 2，**不连服务**；
                            管道/HTTP 不带 force → FMT-001「永久删除不可恢复，需要确认
                            （force = true）」，退出码 2 / HTTP 400
bucket delete 工作（当前）→ current_bucket 置空，不自动切换到别的桶
HTTP（浏览器/调试）        → GET /api/bucket 返回信封 {ok:true,data:{buckets:…}}；
                            GET /api/trash 返回 {ok:true,data:{deleted_buckets:[…],count}}；
                            POST /api/trash/lazy-fox_<时间戳>/restore 正确解码并回退；
                            GET /api/trash/<trashed> 返回条目详情；
                            DELETE /api/trash/<trashed> 不带 force → 400 + FMT-001，
                            带 ?force=1 → 200 并真的删掉（目录、记录、索引一起）；
                            GET /api/bucket/%E5%B7%A5%E4%BD%9C 正确解码为「工作」；
                            错误码按 12.5 映射成状态码（FMT-200 → 404、FMT-201 → 409、
                            FMT-401 → 409、FMT-400 → 404、FMT-001 → 400）
```

> 上面每一行的**行为**都能在源码里对上（`src/bucket/bucket.cpp`、`src/service/commands.cpp`、
> `src/server/server.cpp`、`src/cli/cli.cpp` 的 `print_business_data()`），但**不是**已经跑过的
> 实测记录——沿用 17.1 的口径：要在真实服务上逐条确认后才升格为「实测」，
> 与 18.6 / 18.10 那种带日期的历史记录区分开。

**阶段 4 的收尾与留给后面的事**（前两条是收尾记录，其余不阻塞阶段 5）：

| 事项 | 现状 |
|---|---|
| `refresh_current_bucket()` 接线 | **已接线（运行时生效）**：`ServerRuntime` 在服务启动（`start()`）与数据根切换（`apply_root()`，hello 触发的那条路径）时于业务锁下调用，失效置空、有效不动，失败只记 WARN。单测覆盖两种情况（10.1、开发文档第 61 节） |
| `FMT-203 BucketInUse` | **保留（V1 未使用）**——提交 `674d0b0` 起口径写死：`bucket delete` 不会返回它（V1 允许删除仍有文件的 Bucket，数据一并进回收站），**没有任何代码会产生它**；原本设想的场景被 `FMT-401` / `FMT-402` / `FMT-016` 覆盖 |
| `help bucket` / `help trash` 文案 | **已同步（提交 `8a5e554`；`c2d545d` 追加 trash；`4fee290` 补 `get` / `delete`）**：命令总览把 `(bucket)` 与 `(trash) list get restore delete` 移进「可用命令」组、「尚未实现」组只剩 `(file)` / `(share)`（原来补的那行 `(trash) get delete` **已删掉**），`help bucket` / `help trash` 写明各子命令的真实行为（第一个自动成为当前 / `use` 只改 `current_bucket` / `delete` 移入回收站且当前置空不自动切换 / `restore` 整单判定 / `get` 报条目详情 / `delete` 永久删除且要 `--yes`），不再出现与实际不符的「尚未实现」（11.4、11.14）。**提交 `188e85d` 之后再进一格**：`(file) upload list get delete` 也进了「可用命令」组、该组**只剩 `(share)`**，`help file` 的正文同步（11.4、18.19）。**提交 `674d0b0` / `d5779db` 收官**：`(trash)` 多 `empty`、新增 `(config) list set`、**`(share)` 也进「可用命令」组**——「尚未实现」组只剩 `(server)` 这个空壳，`help share` 换成数据面正文（11.4、18.37、18.38） |
| Bucket 级回收站的恢复 | **已在阶段 4 落地（提交 `c2d545d`）**：`trash list` / `trash restore` + `trash/<user>/.original`，回退为整单判定（见 18.17）。旧记录里「属于阶段 5、只保证数据躺在 `trash/<user>/<bucket>/` 且 `trash.json` 有据可查」的说法已作废 |
| Bucket 级回收站的详情与永久删除 | **已在阶段 4 收尾落地（提交 `4fee290`）**：`trash get`（`{trashed, original, deleted_at, present, path, files, bytes}`，索引有目录没了只报 `present=false`，孤儿目录也能按目录名查到）与 `trash delete`（永久删除，必须显式确认；顺序为「目录 → `trash_reason="bucket"` 的 file.json 记录 → `.original`」，文件级记录绝不动）。详见 18.18 |
| 文件级 Trash | **两级都已落地**（桶级 `4fee290`、文件级 `0fc242b`）：`TrashService` 把 `file.json`（文件级，含 `deleted_at`）与 `.original`（桶级）合成一份 `TrashEntry` 视图，`trash list` 标出 `[文件]` / `[桶]`，`get` / `restore` / `delete` 两级通用（10.4、18.25）。**原口径「写入侧已落地、读取侧属阶段 7、软删除的文件看不到也恢复不了」「`file delete` 会写 `trash.json` 文件级条目」都已作废**：`trash.json` 不再写入，只做只读兼容（7.3.1） |
| 永久删除的 Share 清理 | **尚未实现**：开发文档第 60 节要求永久删除时「清理相关 Share」，但 share 模块属阶段 6，现在**没有**任何清理 `share.json` 的动作（阶段 6/7 待办，10.1 的 `purge()`、18.18） |
| 多窗口并发 | 仍依赖「只有一个 CLI 窗口」（15.1、13.11） |

### 18.16 阶段 5 开工前必须处理的并发限制（已知限制）

阶段 4 的并发模型是「一把锁串行跑业务命令」（15.1 的 ①②③④）。它对 bucket 这类
毫秒级命令完全够用，但**对阶段 5 的上传/下载是一个已知短板**，必须在写 `file` 之前定下来：

```text
现状（阶段 4 冻结）
  管道  连接级严格串行（一次一条连接、一条连接上一问一答）
  HTTP  线程池并发（默认 max(8, hardware_concurrency-1)）
  业务  两条入口共用 ServerRuntime::mutex_，且是**互斥**不是队列：
        不保证先来先服务、无优先级、无排队上限、无排队超时

问题
  上传/下载可能几十秒到几分钟（受 max_upload_size 与网络速度支配），
  持锁执行期间，管道与 HTTP 的**所有**业务命令（含 bucket list / bucket get）
  都被阻塞在 lock() 上；用户看到的是「服务像卡死了」，而且没有任何超时或进度提示

阶段 5 必须二选一（写在实现里，不要只写在文档里）
  A. 收细锁粒度：按 JSON 文件（file.json / trash.json / share.json / config.json）
     或按 file_id 分锁，让 bucket 命令与上传不再互相挡
  B. 把长任务移出锁：登记任务（返回 task_id / 进度查询 op）+ 后台线程执行 + 轮询状态，
     锁只在「读写元数据的那一小段」持有

顺带要定的两件事
  · 长耗时命令的超时值（13.9.4 现在只有 30 秒的普通命令超时，上传要单独声明）
  · 排队行为是否要显式化：如果保持互斥，至少要能告诉用户「前面有一个上传在跑」
```

在 A 或 B 落地之前，**「服务临时不响应其他命令」是阶段 5 的既定限制**，
不要把它当成 bug 去查。

**这条限制已经解决（提交 `188e85d`，选 B 的简化形态「长任务不持锁」）**：

```text
落地形态
  ServerRuntime::run_upload()  = ① 锁下取快照 → ② 锁外 prepare_upload() → ③ 锁内 commit_upload()
  ② 是唯一的长任务（下载/复制 + 算 MD5 + 判大小），**全程不持 ServerRuntime::mutex_**
  ③ 只做「去重 → 重名 → file_id → 搬文件 → 写 file.json」，毫秒级，一次 lock_guard
  file.upload 是唯一走这条路径的 op；commands.cpp 的 file_command() 里没有它的分支

效果（对照上面的「问题」）
  上传期间 bucket list / bucket get / trash * / 浏览器业务请求 **不再一起等**：
  第 ② 段期间那把锁是空的，谁都能进来
  没有引入任务登记、task_id、进度查询 op —— V1 只有一个 CLI 窗口、管道串行，
  登记任务的收益为零（真正需要进度提示的场景留给浏览器侧）

没有动的部分
  其它业务命令仍是「取锁 → execute_business()」，仍是互斥不是队列：
  两条同时到达的短命令依旧可能一个等另一个，但等的时间是毫秒级，
  不再有「几十秒到几分钟」那种用户可见的卡死

顺带要定的两件事，现在的状态
  · 长耗时命令的超时值：仍是 13.9.4 那个 30 秒的普通命令超时——
    `file.upload` 可能超过 30 秒（按 max_upload_size 与网速），这一项**仍未单独定**。
    而且不是理论问题：CLI 的 `client.receive()` 每 30 秒读不到数据就报
    `FMT-602 ServiceOperationFailed`「读取响应超时」，而第 ② 段整段不写响应——
    所以慢来源会让用户看到 FMT-602，尽管服务端还在正常下载（19.1、18.19 ⑦）
  · 排队行为是否显式化：不加——`file.upload` 不再占锁，所以「前面有一个上传在跑」
    这句话对业务命令不再成立
```

> **上面那条「长耗时命令的超时值」已在提交 `a2b6cd1` 修掉（13.9.4 与 19.1 同步更新）**：
>
> ```text
> include/fmt/ipc/protocol.hpp
>   inline constexpr int kCommandTimeoutMs = 30000;              // 普通命令，仍是 30 秒
>   inline constexpr int kUploadTimeoutMs  = 30 * 60 * 1000;     // file.upload 专用，30 分钟
>
> src/cli/cli.cpp:651
>   const int timeout_ms =
>       operation == "file.upload" ? ipc::kUploadTimeoutMs : ipc::kCommandTimeoutMs;
> ```
>
> **这是缓解，不是彻底解决**：30 分钟上限到了仍可能「用户看到超时、服务端还在下载甚至已经入库」，
> 残余风险与 CLI 的提示文案见 19.1（已知边界）。

**为什么快照是安全的（换根不会插进下载期间）**：`run_upload()` 第 ① 步从运行体取
`paths` / `logger` / `size_limit` 之后就把锁放掉，随后第 ② 段整段在锁外。
这期间数据根**不可能被换掉**——换根只有一条触发路径：CLI 的 `hello` 帧
（13.10 `apply_root()`），而管道的 accept/serve 是**连接级严格串行**的（15.1 ①）：
服务端正处在 `handle()` 里面处理这条上传请求，客户端不读走响应就不会发下一个请求，
也就没有第二个请求可处理。所以「上传期间换根」在协议层就不可达，
第 ① 步的快照在整个第 ② 段都成立，不需要额外的锁或版本号去保护它。
（HTTP 侧没有这条保证，但 HTTP 的业务处理器每次调用都自己现取 `context_`，
真正的写只在第 ③ 段的锁内发生，因此也不受影响。）

### 18.17 本次重构阶段 4 追加：Bucket 回收站形状（commit c2d545d）

**这一节记录一次口径推翻**：用户否掉了「回收站用原名 + 重名才加时间戳」。触发场景是
「同一个桶删两次、两次都可能要回到 `repository/<user>/<桶名>`」，旧做法下两条回收站条目
会抢同一个回退位置。改法共五条，全部已实现并提交：

```text
① 目录名一律带时间戳    删除后目录名 = <原桶名>_<YYYYMMDDHHMMSS>（无论有没有重名）；
                        同一秒内删两次或目录恰好同名 → 再加序号 _2、_3……
                        实现：BucketService::unique_trashed_name() + compact_stamp()
② .original 是唯一权威  桶级记录写在 trash/<user>/.original，形状见 7.3.2；
                        data/trash.json 不再记桶级条目（旧 type:"bucket" 作废）；
                        绝不剥离时间戳猜原名；.original 损坏 → 删除直接拒绝（FMT-006）
                        不搬目录；索引写不进 → 目录搬回原位（回退方向对称）
③ 回退是整单判定        目标 repository/<user>/<原名> 已存在 → FMT-401 整单拒绝
                        （不覆盖、不改名、不部分恢复）；不存在 → 整个目录一次 rename；
                        因为是整棵树搬走，不存在文件级冲突
                        —— 这一条推翻开发文档第 56 节旧口径；「部分恢复」归属
                        文件级恢复（trash restore <file_id>，阶段 7，第 54/55 节仍适用）
④ file.json 新增字段    is_trash=true 时同时写 trash_reason="bucket"（桶被删）或
                        "file"（文件自己删的，阶段 5 起）；桶回退只翻回 "bucket" 的并清空
⑤ 桶级 trash 落地       trash.list → {deleted_buckets:[{trashed,original,deleted_at,present}],count}
                        trash.restore <名称> → {trashed, original, restored_to, message}
                        HTTP：GET /api/trash、POST /api/trash/<名字>/restore（百分号解码）
                        trash get / trash delete（永久删除）当时仍未实现（FMT-602，阶段 7）
                        —— **该口径已作废：两条已在收尾提交 `4fee290` 落地，见 18.18**
```

接口形状（`include/fmt/bucket/bucket.hpp`，10.1）：

```cpp
inline constexpr const char* kOriginalIndexName = ".original";
struct BucketRemoval { std::filesystem::path moved_to; std::string trashed_name;
                       std::size_t files_affected; bool was_current; };
struct TrashBucket { std::string trashed_name; std::string original_name;
                     std::string deleted_at; bool directory_present; };
Result<BucketRemoval>            BucketService::remove(std::string_view name);
Result<std::vector<TrashBucket>> BucketService::list_trashed();
Result<TrashBucket>              BucketService::restore(std::string_view identifier);
Result<TrashBucketDetail>        BucketService::get_trashed(std::string_view identifier);  // 4fee290
Result<TrashPurge>               BucketService::purge(std::string_view identifier);        // 4fee290
```

`bucket delete` 的 `data` 增加 `trashed_name`，`message` 变成
`Bucket 已删除（移入回收站）：<原名>  ->  <回收站名>`。

**回退定位与两类异常**（`BucketService::restore`）：

```text
定位      1. 回收站里的名字（精确匹配 trashed）
          2. 原桶名（匹配 original），同名多条时要求唯一，否则 FMT-001 并列出候选 trashed 名
异常      目录在、.original 无记录 → list 照实列出（original 为空、CLI 显示
          「原名称未记录，无法回退」），回退报 FMT-001「回收站条目缺少原桶名记录
          （.original），无法回退」——不猜名字
          索引有、目录没了 → list 标 present=false 如实报告，不擅自清理；
          回退报 FMT-400「回收站目录已不存在：<trashed>」
```

单元测试（`tests/bucket_test.cpp`，`FMT_TEST(Bucket, …)`）：

```text
删除移入回收站名字带时间戳        目录名 = 原名 + 时间戳；file.json 置 is_trash/trash_reason
同一秒删两次也不覆盖              第二条加 _2，两份数据都在
索引损坏时拒绝删除                .original 损坏 → FMT-006，目录一个字没动
回退成功与已存在拒绝              目标不存在 → 整棵树搬回；已存在 → FMT-401，两边的数据都不动
同名多条回退要指定回收站名字        FMT-001 并列出候选
没有身份记录的目录只报告不回退      original 为空 → 拒绝回退
删空桶重建再删然后回退不会互相覆盖  用户提的完整场景（空桶/带文件的桶各删一次再回退）
条目详情与永久删除（4fee290）      trash get 的六个字段 + trash delete 的三步顺序
永久删除只清桶级记录（4fee290）    trash_reason="file" 的记录一条都不能少
回收站扫描只认桶级条目（4fee290）  点开头跳过 + <名字>_<14 位时间戳> 形状检查
有索引没目录的条目可以永久删掉      幽灵条目也要能删掉（否则永远清不掉）
```

另有 `tests/path_manager_test.cpp` 的 `回收站保持原层级`（断言
`D:/FMT/trash/小谷/.files/工作/2026/10/05/小谷姐姐麻辣烫.jpg`）、
`tests/service_test.cpp` 的 `管道能执行回收站命令`（`trash list` / `restore` / `get` /
`delete` 端到端；不带 `force` 被拒为 `FMT-001` / 退出码 2，带 `force` 成功）、
`tests/server_test.cpp` 的 `Bucket路由与状态码`（`DELETE /api/trash/<名字>` 不带 `force`
→ 400 + `FMT-001`，带 `?force=1` → 200）。

> **上面两行的 `FMT-001` 是当时（`4fee290`）的实况**：提交 `711da4c` 起「缺确认」
> 改成独立错误码 **`FMT-016 ConfirmRequired`**（退出码 2 / HTTP 400），用例断言也一并改了；
> 这两行保留为历史记录，**别再当成现状引用**（18.24）。

**与旧口径对照**（自查用；四份文档里不应再出现左列说法）：

| 旧说法（作废） | 现在的口径 |
|---|---|
| 回收站用原桶名，重名才加时间戳后缀 | **一律**带删除时间戳，同秒冲突加 `_2` |
| `trash.json` 里记 `type:"bucket"` 桶级条目 | 桶级只认 `trash/<user>/.original`；`trash.json` 只服务文件级 |
| 桶回退逐个文件判断、允许「部分恢复」 | 桶级**整单判定**；「部分恢复」属文件级恢复（阶段 7） |
| 按名字剥离时间戳反推原桶名 | 一律查 `.original`；查不到就拒绝回退 |
| 桶级 `trash list` / `trash restore` 属阶段 5 | 桶级一半随**阶段 4**落地；阶段 7 只留文件级与永久删除 |
| `trash get` / `trash delete` 未实现、返回 `FMT-602` | 桶级两条已在 `4fee290` 落地；只有**文件级** trash 属阶段 7（18.18） |
| 文件级条目落 `trash/<user>/<bucket>/YYYY/MM/DD/` | 落 **`trash/<user>/.files/<bucket>/YYYY/MM/DD/`**（`4fee290`）；顶层留给桶级条目 |
| 桶级扫描只跳过 `.` 开头的条目 | 跳点开头 **+** 只认 `<名字>_<14 位时间戳>`（可带 `_<1-3 位序号>`）形状（`4fee290`） |
| 永久删除直接执行、无需确认 | 必须显式确认：服务端 `force == true`（否则 `FMT-001`/2）、CLI 交互问一次或 `--yes`、HTTP `?force=1`（`4fee290`） |
| 永久删除会连文件级记录一起清掉 | **不会**：只清 `trash_reason == "bucket"` 的记录，`"file"` 的一条都不动（`4fee290`） |
| 永久删除会「清理相关 Share」 | **尚未实现**：share 模块属阶段 6，现在没有清理 `share.json` 的动作（阶段 6/7 待办） |

未跑过的实测：本节与 18.15 一样，是**对着源码写的行为记录**，不是真机实测；升格为
「实测」仍需在真实服务上逐条确认（17.1）。

### 18.18 本次重构阶段 4 收尾：桶级 `trash get` / `trash delete` 与 `.files` 落点（commit 4fee290）

`4fee290`「feat(trash): finish the bucket level trash commands」把桶级回收站补完，
同时改了一处目录布局。四件事：

```text
① 文件级条目换落点   trash/<user>/<bucket>/YYYY/MM/DD/<file>
                     → trash/<user>/.files/<bucket>/YYYY/MM/DD/<file>
                     实现：PathManager::build() 增加可选 inner（插在 user 与 bucket 之间），
                     trash_file() 传 L".files"；repository_file() 不传，仓库路径不变（4.3）
                     原因：trash/<user>/ 的**顶层留给桶级条目**
                     （trash/<user>/<桶名>_<时间戳>/）。不分开的话，阶段 5 落地文件级删除后，
                     桶级扫描会把 trash/<user>/<bucket>/ 误当成「孤儿桶条目」列出来。
                     .original（桶级索引）同样是点开头。
                     用例：PathManager.回收站保持原层级 → D:/FMT/trash/小谷/.files/工作/2026/10/05/…
② trash get <名称>   定位与 restore 完全一致（共用私有 find_trashed()：回收站名字优先，
                     原桶名必须唯一，多条 → FMT-001 并列候选）
                     返回 {trashed, original, deleted_at, present, path, files, bytes}
                     · path 相对数据根、正斜杠；files 是目录里的实际文件数；bytes 是总字节数
                     · **只有单条查询遍历目录**，list_trashed 不做（列表可能很长）
  —— **该口径已被提交 18f16ca 修改**：`TrashService::list()` 现在对每个 present 的
  桶级条目遍历一次目录填 files/bytes（10.4 有代价说明）；`BucketService::list_trashed()`
  这层索引仍然不遍历
                     · 索引里有、目录没了 → **不报错**，present=false（如实报告，不擅自清理）
                     · 孤儿目录（目录在、索引里没记录）按目录名也能查到（original 空、present=true）
                     · 两者都没有 → FMT-400 TrashEntryNotFound（退出码 3）
③ trash delete <名称> 永久删除、不可恢复。**顺序刻意如此**：
                     ① remove_all 回收站目录 → 失败则什么都没变（索引还在，可重来）
                     ② 清 file.json 里 user+bucket 匹配、is_trash==true 且
                        trash_reason=="bucket" 的记录（"file" 的**绝不动**——那些文件的数据
                        不在这个目录里）
                     ③ 最后从 trash/<user>/.original 摘掉对应那条
                     · 幽灵条目（索引有、目录没了）也能删——否则永远清不掉
                     · removed_files = 真正删掉的磁盘文件数（删前统计）；removed_records = 清掉的记录数
                     · 返回 {trashed, original, removed_files, removed_records, message}
                     · 日志记在 `Trash` 模块：条目名 + 文件数 + 记录数
④ 强制确认           服务端：请求里没有 force == true → FMT-001（退出码 2）
                     「永久删除不可恢复，需要确认（force = true）」；**任何入口都一样**，
                     且该检查在取位置参数之前（src/service/commands.cpp）
                     CLI 交互窗口：先问「永久删除回收站条目 X ？此操作不可恢复 (y/N)」，
                     答 n（或直接回车）→ 打印「已取消」、退出码 0、**不发请求**
                     CLI 一次性命令：必须 --yes（或 -y），否则本地拒绝（**不连服务**），
                     stderr 打印「该操作需要确认（FMT-016）：请加 --yes，或在交互窗口里执行」，
                     退出码 2（**提交 711da4c 起为 exit_code(ConfirmRequired)**，
                     文案也换了）；--yes/-y 是本地开关，
                     **不作为位置参数发给服务端**，只据此置 args.force = true
                     HTTP：DELETE /api/trash/<标识> 需要 ?force=1（或 force=true，大小写不敏感）
                     或请求体 {"force":true}，否则 400 + **FMT-016**（提交 711da4c 起；
                     本节记的是 `4fee290` 时的 FMT-001，见 12.5、18.24）
                     另外提交 711da4c/0fc242b 起：CLI 先发一次只读预检（args.dry_run），
                     再打印情况并问「确认执行？(y/N)」（11.15）
```

**桶级扫描的形状检查（同一提交）**：`list_trashed()` 扫 `trash/<user>/` 时跳过点开头的条目
（`.files` / `.original`），并且**只认 `<名字>_<14 位时间戳>` 或
`<名字>_<14 位时间戳>_<1-3 位序号>` 形状的目录**——手工拷进来的、别的东西都不会被误当成
桶级条目（`is_dot_entry()` + `looks_like_trashed_bucket()`）。

**接口与分发的增量**（10.1、12.3.2.1）：

```cpp
struct TrashBucketDetail { TrashBucket bucket; std::filesystem::path directory;
                           std::size_t file_count; std::uintmax_t byte_count; };
struct TrashPurge { std::string trashed_name; std::string original_name;
                    std::size_t removed_files; std::size_t removed_records; };
Result<TrashBucketDetail> BucketService::get_trashed(std::string_view identifier);
Result<TrashPurge>        BucketService::purge(std::string_view identifier);
// 私有新增：find_trashed()（restore / get_trashed / purge 共用同一套定位，找不到不算错）、
//           remove_bucket_file_records()（只清 trash_reason == "bucket" 的记录）
```

**命令集状态**：`trash` 组的**桶级部分全部完成**（`list` / `get` / `restore` / `delete`）。
CLI 命令总览里 `(trash) list get restore delete` 全在「可用命令」组，
「业务命令（服务端尚未实现，现在会返回 FMT-602）」组里**只剩 `file` / `share`**，
**不再有 trash 行**（**这是提交 `4fee290` 收尾时的状态**；提交 `188e85d` 把 `file` 四条
也做掉了，现在**只剩 `share` 一行**，见 11.4）。仍未实现的是**文件级**条目
（`file delete` 产生的）与它们对应的 list / get / restore / delete，属阶段 5/7
——注意 `file delete` 本身**已经可用**，它只是「写得进、读不出」（10.4、18.19）。

**已知缺口（阶段 6/7 待办，如实记录）**：开发文档第 60 节要求永久删除时「清理相关 Share」，
但 share 模块（阶段 6）还没实现，所以 `purge()` 现在**没有**任何清理 `share.json` 的动作——
一个被永久删除的桶如果曾有过分享记录，那些记录会留在 `share.json` 里。等阶段 6 落地
`share` 之后，永久删除要补上「按 `bucket` 清掉对应 share 记录」这一步。

**新增/更新的用例**（`fmt_tests` 共 **104 个**，全绿；**本节的 104 是提交 `4fee290` 时的数字，
提交 `188e85d` 之后是 118 个**，之后 `a2b6cd1` 追加 6 条 `HttpClient.*` 到 **124 个**、
`0ad9efc` 再追加 2 条 `File.*` 到 **126 个**、`5bf2c1f` 再追加 2 条 `File.*` + 1 条
`Bucket.*` 到 **129 个**、`9c3d2cb` 再追加 5 条（`Validation.*` / `File.*` ×2 /
`Bucket.*` ×2，并把两条端到端用例各加一段断言）到 **134 个**、`0fc242b` 再新增 4 条
（`Trash.*`）并把若干既有用例改成新形状，到 **140 个**，
新增的 14 + 6 + 2 + 3 + 5 个见 18.19、17.2.1、18.21、18.22 与 18.23）：

```text
tests/bucket_test.cpp    条目详情与永久删除 / 永久删除只清桶级记录 /
                         回收站扫描只认桶级条目 / 有索引没目录的条目可以永久删掉
tests/path_manager_test.cpp   回收站保持原层级（断言 .files 新落点）
tests/service_test.cpp   管道能执行回收站命令（list/restore/get/delete；
                         不带 force 被拒 FMT-001/2，带 force 成功，索引一起清）
tests/server_test.cpp    Bucket路由与状态码（GET /api/trash/<名字>、
                         DELETE 缺 force → 400 + FMT-001、?force=1 → 200）
```

> **本节是 `4fee290` 时的历史记录**：`FMT-001`（缺确认）后来改成 **`FMT-016`**（`711da4c`），
> `deleted_buckets` 形状后来改成 **`entries`**（`0fc242b`）；用例断言也都跟着改了
> （18.24、18.25）。引用现状请看 10.4、12.3.2.1 与 12.5。

**本轮仍未做的事**：18.15 末尾那句「本节是行为记录、不是真机实测」同样适用于本节；
文件级条目、`file` / `share` 模块、永久删除的 share 清理都还没开始。
（**提交 `188e85d` 已把其中的 `file` 模块做掉，见 18.19**；文件级条目的读取侧、
永久删除的 share 清理、`share` 模块仍未开始。）

---

### 18.19 本次重构阶段 5（上）：`file` 四条命令与上传两段式（commit 188e85d）

`188e85d`「feat(file): upload, list, get and soft delete」把阶段 5 的 `file` 部分做完。
**这一轮最重要的设计决定是上传拆成两段**，它同时兑现了 18.16 那条「阶段 5 开工前
必须处理的并发限制」。

```text
① 两段式上传（并发口径的落地）
   include/fmt/file/file.hpp：
     Result<PreparedUpload> prepare_upload(const PathManager&, const std::string& source,
                                           const std::string& name, std::uintmax_t size_limit,
                                           Logger*);                 // **锁外**，长耗时
     class FileService { Result<FileRecord> commit_upload(PreparedUpload&); … };  // **锁内**，快
   编排：ServerRuntime::run_upload(args) = ① 锁下取快照 → ② 锁外 prepare → ③ 锁内 commit
   管道：handle() 在取锁之前拦下 file.upload → run_upload()
   HTTP：BusinessHandler 同样先拦 file.upload → 同一个 run_upload()（**一份实现两条入口**）
   为什么：下载可能几十秒到几分钟，持业务锁去下载会把 bucket list / trash * /
           浏览器请求一起卡住；长任务不持锁，锁只保护元数据提交那一下
   快照为什么安全：换根只由 hello 触发，而管道 accept/serve 串行（15.1 ①）——
           上传期间不会再处理第二个请求

② 来源与文件名（**来源部分已在提交 a2b6cd1 改口径，见 18.20**）
   来源：http:// URL 或 https:// URL 都支持（走上文 18.20 的 common/http_client，
        WinHTTP + Schannel），或本机文件路径（流式 64 KiB 复制）
   【188e85d 当时的实况，已作废，留档对照】
        http:// URL（httplib::Client 下载）或本机文件路径
        https:// → FMT-300（10.2.2），消息「V1 不支持 https（需要 OpenSSL）；请改用 http:// 或本地路径」
   现在：含 "://" 但不是 http/https → FMT-300「只支持 http:// 与 https:// 的来源：<来源>」
   文件名：[文件名] 可省略 → file_name_from_source()（10.2.1）：
           砍 scheme://host → 去查询串/锚点 → 去结尾斜杠 → 取最后一段 → 百分号解码
           http://example.com/ → 空串 → FMT-100「无法从来源推断文件名，请显式给出文件名」
           **绝不拿主机名当文件名**
   临时文件：<数据根>/temp/fmt-upload-<随机>-<序号>.tmp（fmt- 前缀让服务启动时顺手清）
            TempGuard（RAII）+ commit_upload 内的第二个 TempGuard：每条失败路径都删掉它
   大小上限：边写边判（sink 里 written + length > size_limit → FMT-303），不是下完再看
   完整性：  服务器给了 Content-Length 就必须对上，否则 FMT-301
   MD5：     边写边算（10.3），Windows CNG / bcrypt

③ 提交（锁内）与作用域
   顺序：MD5 去重 → 文件名冲突 → 分配 file_id → 移到仓库 → 写 file.json
   去重： 同用户 + **任何 Bucket** + 正常文件命中 → FMT-304
          「该文件已经存在：<名字>（<file_id>）」；不新建 id、不复制文件、不改旧名字
   重名： 同用户 + **任何 Bucket** + 正常文件 + 同名（**不区分大小写**，5bf2c1f）
          → FMT-105，拒绝、**不自动改名**
   两条都是「按用户跨 Bucket」，不是桶内（与 9.2 的命名空间一致）
   file_id：fmt-<今天>-<当天已有 id 的最大序号 + 1>（8.3，不是「条数」）
   日期：  存储日期由 file_id 推出（8.5）；resolve_path() 先推，
           推不出来在桶的日期树里兜底找同名文件，唯一命中才用，多命中 → FMT-015
   回滚：  file.json 写不进去 → 删掉刚提交的仓库文件 + ERROR 日志，绝不报「上传成功」

④ 四条命令（管道 op 与 HTTP 路由都落地）
   file.upload  args.argv = [来源] 或 [来源, 文件名]          POST   /api/file
   file.list    无参数                                        GET    /api/file
   file.get     args.argv = [file_id 或 文件名]               GET    /api/file/<id_or_name>
   file.delete  args.argv = [file_id 或 文件名]               DELETE /api/file/<file_id_or_name>
                （**不需要 force**；提交 0ad9efc 起参数与 file.get 同形）
   list：当前用户 + 当前 Bucket + 正常文件，按 file_id 排序（10.2.4）
   get ：先当 file_id 查（全局唯一、不限用户/Bucket/is_trash），查不到再当文件名查
         （当前用户 + 正常文件、同用户跨 Bucket）
   delete：定位与 get 同一套规则（提交 0ad9efc，locate_record()）——先当 file_id、
         再当文件名；名字在但已软删除 → FMT-001 并给出 file_id（**不报 FMT-002**）、
         都没有 → FMT-002 FileNotFound（退出码 3）
         软删除，file_id 不变；顺序「搬文件 → 写 trash.json（失败搬回）→
         写 file.json（失败撤掉 trash 记录并搬回）」（10.2.4）
         回收站落点用**记录自己的 bucket**（trash_path_of()），不是当前 Bucket
   data 形状见 12.3.2.1；CLI 展示规则见 11.14

⑤ CLI 与 help
   命令总览：(file) upload list get delete 进「可用命令」组，
            「尚未实现」组**只剩 (share)**；help file 的正文照源码（11.4）
   输出：有 files 数组按列表打印（每行「文件名  大小」、末行「共 N 个文件」）；
        有 file_id + size 按单条打印（文件 / file_id / Bucket / 类型 / 大小 / MD5 /
        路径 / 状态）；file.upload 与 file.delete 走 message 那一行
   用例 `Service.管道能执行Bucket命令` 与 `Service.未实现的操作与未知操作被明确拒绝`
        里原来拿 `file.list` 当「未实现」的例子，已改成 `share.list` / `share.create`

⑥ 已知缺口（**这是本轮最容易被误读的一条**）
   file delete **会创建**文件级回收站条目（trash.json 里 type="file" +
   trash/<用户>/.files/<桶>/YYYY/MM/DD/），但 **trash list / get / restore 只处理桶级条目**
   （BucketService::list_trashed() 只扫 trash/<用户>/ 顶层并跳过点开头的条目）——
   **软删除的文件暂时无法从 CLI 看到或恢复**。文件级 trash 的 list/get/restore/delete
   与永久删除仍属阶段 7；数据本身完好（file get <file_id> 仍能查到 is_trash/trash_reason）

⑦ 另一处缺口（**已在提交 a2b6cd1 缓解，见 18.20 与 19.1，仍有残余风险**）
   CLI 对 file.upload 用的还是 ipc::kCommandTimeoutMs = 30 秒（13.9.4），
   而 client.receive() 的每次 read_some(30000) 一旦 30 秒读不到数据就返回
   FMT-602「读取响应超时」。第 ② 段期间服务端不写任何响应，
   所以来源慢到 30 秒没下完时 CLI 会报 FMT-602，而服务端其实还在下载、
   甚至可能已经把文件提交入库（下一轮把超时值单独定下来，见 19.1）
   → 【a2b6cd1】下一轮已经落地：file.upload 改用 ipc::kUploadTimeoutMs = 30 分钟，
     CLI 超时时额外打印「服务端可能仍在处理」的提示（19.1）；30 分钟上限本身仍是残余风险
```

**新增/更新的用例**（`tests/`，全仓 **118 个**：上一轮 104 个 + 本轮 14 个；
**提交 `a2b6cd1` 之后是 124 个**，新增的 6 条见 17.2.1；
**提交 `0ad9efc` 之后是 126 个**，新增的 2 条见下；
**提交 `5bf2c1f` 之后是 129 个**，新增的 3 条见 18.22；
**提交 `9c3d2cb` 之后是 134 个**，新增的 5 条与两条追加断言见 18.23；
**提交 `711da4c` 之后是 136 个**、**`0fc242b` 之后是 140 个**，见 18.24 与 18.25）：

```text
tests/hash_test.cpp      Hash.MD5已有向量 / Hash.分块与一次算结果一致
tests/file_test.cpp      File.从来源推断文件名 / File.扩展名与类型 / File.本地文件暂存 /
                         File.暂存失败会清理临时文件 / File.入库写记录并分配file_id /
                         File.重复内容与重名都被拒绝 / File.没有当前Bucket时拒绝上传 /
                         File.列表与查询 / File.软删除进回收站 / File.从HTTP下载入库
                         【0ad9efc】File.按文件名也能软删除、
                         File.按名字删除用的是记录自己的Bucket
                         【5bf2c1f】File.大小写不同的同名必须被当成重名、
                         File.按名字查询不区分大小写
                         【9c3d2cb】File.与file_id同形的名字不能上传、
                         File.标识与名字同时命中时报歧义
                         【a2b6cd1】`File.暂存失败会清理临时文件` 里的协议用例
                         从 https 改成 ftp://（https 现在是合法来源）
tests/validation_test.cpp 【9c3d2cb】Validation.与file_id同形的文件名被拒
tests/bucket_test.cpp    【5bf2c1f】Bucket.大小写不同也认得同一个桶
                         【9c3d2cb】Bucket.创建时大写会转成小写、
                         Bucket.use大写规范化到磁盘上的名字
tests/service_test.cpp   Service.管道能上传与操作文件（upload → list → get×2 →
                         MD5 去重被拒 → delete → get 仍可查到 is_trash/trash_reason；
                         【9c3d2cb】再断言 get 的回收站记录带 trash_path）
tests/server_test.cpp    Server.File路由与上传（空请求体 → 400 + FMT-001；
                         {"path":…,"file_name":…} 上传成功；GET/DELETE 走通）
```

**本轮仍未做的事**：`share` 整组（阶段 5 剩下它）、文件级 trash 的读取侧与永久删除
（阶段 7）、Download 与 Preview（阶段 6）、永久删除时的 `share.json` 清理。
「实测」这一栏同样适用 18.15 末尾那句话——本节是**对着源码写的行为记录**，
升格为「实测」仍需在真实服务上逐条确认（17.1）。

---

### 18.20 本次重构阶段 5（下）：下载改走 WinHTTP，`https` 支持（commit a2b6cd1）

`a2b6cd1`「feat(http): download over WinHTTP instead of OpenSSL, so https works」
**推翻了 `188e85d` 的「https 不支持」口径**。这一节记录改法与理由。

```text
① 为什么换掉 cpp-httplib 的 Client（本轮最重要的决策）
   · cpp-httplib 的 Client 要走 https 必须 OpenSSL。自己编 OpenSSL 需要 Perl + NASM，
     破坏本项目「构建只依赖 vendored 单头文件、离线可构建」的前提
   · 本项目用 /MT 静态 CRT。静态链接 OpenSSL 确实能做到不引 DLL，
     但 /MT 与市面上常见的 /MD 静态包混用 CRT 会出问题，自己编又回到上一条
   · WinHTTP 是**系统组件**：链的是系统导入库 target_link_libraries(fmt_core PRIVATE winhttp)
   · TLS 走 **Schannel**（系统证书库）：证书更新跟着系统走，**不分发任何 DLL**，
     仓库里也不需要塞 CA bundle
   · **自动使用系统代理**：WinHttpOpen 首选 WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY（Win8.1+），
     失败退回 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY——访问 https 站点基本都要走代理，这是刚需

② 新增文件与构建改动
   include/fmt/common/http_client.hpp + src/common/http_client.cpp（流式 GET 客户端）
   src/common/CMakeLists.txt：加 http_client.cpp，WIN32 下链 winhttp
   src/file/CMakeLists.txt：**不再链 cpp-httplib**（浏览器服务端仍用它）

③ 接口与行为（细则见 10.2.2.1）
   Result<HttpDownloadResult> http_download(const HttpDownloadRequest&,
                                            const std::function<bool(const char*, std::size_t)>& sink);
   请求：GET、跟随重定向（WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS）、
        连接/发送/接收超时默认 10s / 30s / 300s
        ——**接收超时是单次读取的空闲超时，不是总时长**
   响应：**只有 2xx 的响应体交给 sink**；非 2xx 的响应体直接丢弃、状态码照实返回
        （错误页不该落进用户的文件）
   中止：sink 返回 false = 调用方要求中止 → 返回**成功且 aborted = true**，
        被拒绝的那一块**不算收到**（bytes 不含它）
   压缩：**明确要求 Accept-Encoding: identity**。压缩会让 Content-Length 与实际落盘
        字节对不上（10.2 第 7 步的完整性检查会误报），也可能把压缩内容当文件存下来；
        若服务器仍返回非 identity 的 Content-Encoding → FMT-301
        「服务器返回了 xxx 压缩内容，暂不支持」
   错误：超时（ERROR_WINHTTP_TIMEOUT）→ FMT-302 DownloadTimeout；
        域名/连接/TLS/响应异常 → FMT-301 DownloadFailed，消息里带 Win32 原因；
        **证书类失败**额外读 WINHTTP_OPTION_SECURITY_FLAGS，给出更准的提示
        （根证书不受信任 / 证书主机名不符 / 证书已过期或尚未生效）
   协议：只支持 http:// 与 https://（is_remote_url()）；**URL 里带用户名密码不接受**

④ 来源判定顺序（prepare_upload()，与 10.2.2 一致）
   http:// 或 https://        -> 走网络下载
   含 "://" 但不是 http/https  -> FMT-300 UrlInvalid（「只支持 http:// 与 https:// 的来源」）
   其余                       -> 本地路径，存在性检查不过报 FMT-002
   **注意 `ftp://` 之类报的是 FMT-300，不是 FMT-002**——别让人以为是被当成
   「本地文件不存在」去排查磁盘

⑤ 命令超时（上一轮的未决项，本轮修掉）
   include/fmt/ipc/protocol.hpp：
     kCommandTimeoutMs = 30000                        // 其余命令不变
     kUploadTimeoutMs  = 30 * 60 * 1000               // file.upload 专用，30 分钟
   src/cli/cli.cpp:651：
     operation == "file.upload" ? ipc::kUploadTimeoutMs : ipc::kCommandTimeoutMs
   超时时 CLI 额外打印：
     提示：等待服务响应超时。服务端可能仍在处理，稍后用 file list 确认；
           也可以查看 log/fmt.log。
   **这是「已知边界 + 现有缓解」，不是彻底解决**：30 分钟上限到了仍可能出现
   「用户看到超时、服务端还在下载甚至已经入库」（19.1）

⑥ 测试
   tests/fmt_test.cpp：每条用例**开始前**打印 [开始] <套件>.<名称>，
        并 std::ios::sync_with_stdio(false) + std::cout.setf(std::ios::unitbuf) 逐行刷新
        ——某条卡住时，最后一行就是它（17.2）
   新增 tests/http_client_test.cpp 6 条（17.2.1 有逐条说明）：
        HttpClient.本地HTTP下载 / 非2xx不交给调用方 / 中止下载 /
        连接失败与协议校验 / https会真的做TLS握手 / 真实https下载可选
   实测：FMT_TEST_HTTPS_URL=https://example.com/ 时输出
        「真实 https：https://example.com/ -> HTTP 200，577 字节，Content-Type: text/html; charset=utf-8」
        全套 **124 项全绿**（提交 `a2b6cd1` 当时的数字；`0ad9efc` 之后是 126、
        `5bf2c1f` 之后是 129、`9c3d2cb` 之后是 134、`711da4c` 之后是 136、`0fc242b` 之后是 140，
见 18.21、18.22、18.23、18.24 与 18.25）

⑦ 与 188e85d 的差异（口径推翻清单）
   · 「https 不支持」→ **支持**，且不引 OpenSSL、不分发 DLL
   · 「https 需要 OpenSSL，会引入 DLL」→ **作废**（Schannel + 系统证书库）
   · 「http:// 下载交 httplib::Client」→ 交 **WinHTTP**；file 模块不再链 cpp-httplib
   · 「file.upload 与其它命令共用 30 秒超时」→ **30 分钟专用超时** + 超时提示
   · `File.暂存失败会清理临时文件` 的协议用例 **https → ftp://**

⑧ 仍未清理的一处（**源码里的陈旧文案——本条已结清**）
   src/cli/cli.cpp 的 `help file` 正文原先写着
   「v1 不支持 https（需要 OpenSSL）；同时只支持 http。」
   ——提交 a2b6cd1 **没有**改这句（它只动了 common/http_client、两个 CMakeLists 与测试）。
   **后续的提交已经把它改掉**：现在源码里这两行是
   「来源可以是 http:// 或 https:// 的 URL，也可以是本机路径。」+
   「网络下载走系统组件（WinHTTP + Schannel），支持 https，不需要 OpenSSL。」
   ——11.4 处的帮助正文与标注已按现状同步，18.20 的这条缺口到此关闭。
```

---

### 18.21 本次重构阶段 5（续）：`file delete` 也收文件名（commit 0ad9efc）

`0ad9efc`「feat(file): let file delete take a file name too」把 `file delete` 的参数从
「只收 `file_id`」改成「与 `file get` 一致的 `<file_id|文件名>`」。这一节记录改法与理由。

```text
① 为什么改（这一节的重点，别只记住「参数变宽了」）
   · 对称性缺失：file get 一直两种参数都收，file delete 只收 file_id，而开发文档
     **通篇没有写出理由**——第 43 节原先只有一句 `file delete <file_id>`。
     用户看文档找不到解释，这不是「设计如此」，是漏了
   · 更要紧的是诊断错误：按 file_id 单向查找时，`file delete <文件名.txt>` 必然落空，
     报出来的是 FMT-002「文件不存在」——可文件明明存在，只是用名字称呼它。
     正确的诊断要么说「已经在回收站里」，要么确实没这个文件
   · 改完的收益：用户不必先 file get 查出 file_id 再回来删

② 实现：把定位抽成一份，两处共用
   include/fmt/file/file.hpp 新增私有
     Result<std::size_t> locate_record(const std::vector<FileRecord>& records,
                                       std::string_view key) const;
   签名变化：Result<FileRecord> remove(std::string_view file_id_or_name)
     （参数名由 file_id 改成 file_id_or_name；get 侧不变）
   **落地方式：规则共用、代码两套**——get 的「命令层」仍走 get_by_id() + get_by_name()
   （10.2.4），remove() 走 locate_record()；两者**参数语义与失败语义一致**，
   locate_record() 多出的第 ③ 步只服务删除
   三步（10.2.4 有完整版）：
     ① 先当 file_id：record.file_id == key 即命中（全局唯一；形状 fmt-YYYYMMDD-N 固定，
        先查不会误伤名字——与 get 同一套理由）
        【9c3d2cb 更正：这一处实际用的是 iequals(record.file_id, key)，且「名字不会
         长得像 id」现在由上传侧的 FMT-106 保证，见 9.1、18.23】
     ② 再当文件名：record.user == current_user && !is_trash && file_name == key
        （同用户跨 Bucket；名字在同用户范围内唯一，重名上传被 FMT-105 拒绝，无歧义）
     ②.5【9c3d2cb 追加】两个索引命中**不同**记录 → FMT-001「有歧义：…请直接用
        file_id 指定要删哪一个」（只服务 delete；file get 不加，10.2.4、18.23）
     ③ 名字在、但记录 is_trash → FMT-001「该文件已经在回收站里：<名字>（file_id <id>）」
        **不能报 FMT-002**：文件还在，只是不在正常区
     都没有 → FMT-002 FileNotFound（退出码 3）
   命中后原有两道校验不变：不属于当前用户 → FMT-004；按 id 命中的 is_trash → FMT-001
     （与定位第 ③ 步是两个入口、同一个错误码，消息都带 file_id）

③ 回收站落点用记录自己的 bucket（trash_path_of()），不是当前 Bucket
   在桶 B 里按名字删掉桶 A 的文件 → 进 trash/<user>/.files/<A 的桶名>/YYYY/MM/DD/
   这样落点与「阶段 7 搬回 repository/<user>/<A>/…」才对得上（8.5、10.2.4）

④ 命令与入口（形状不变）
   CLI           file delete <file_id|文件名>    → op file.delete，args.argv = [<那一个参数>]
   help file     那两行照源码改过（11.4）：「delete <file_id|文件名>  软删除进回收站，
                 file_id 不变 / （文件级回收站目前只能写、还不能从 trash 查回，待阶段 7）」
                 ——原「（可用 trash 查回）」是跑在实现前面的说法，一并删掉
   HTTP          DELETE /api/file/<file_id_or_name>；**路由正则一个字没改**
                 （server.cpp 两条都还是 R"(/api/file/([^/]+))" + args_with_encoded_name()），
                 只是语义放宽；仍然不需要 force
   响应 data    {file_id, file_name, moved_to, message} —— **形状不变**（12.3.2.1）

⑤ 测试（新增 2 条，全套 124 → **126 项全绿**；随后 `5bf2c1f` 又加到 129、
   `9c3d2cb` 加到 134，见 18.22 与 18.23）
   File.按文件名也能软删除           名字不存在 → FMT-002「文件不存在」；
                                     按名字删与按 id 删同一条记录（file_id 相同、is_trash）；
                                     再按名字删一次 → FMT-001，消息里带 file_id
                                     （**不是** FMT-002）
   File.按名字删除用的是记录自己的Bucket
                                     在「工作」里上传，切到「生活」后按名字删掉它；
                                     落点路径里有 /工作/、没有 /生活/
   顺带把第 109 节那张表里「不同 Bucket 同名——用例尚未单列」从 ⏳ 改成 🟡：
   跨 Bucket 的那条已由第二条用例覆盖（钉的是落点，顺带证明切桶后仍按名字命中原桶的文件）；
   「不同用户同名」仍无用例

⑥ 口径推翻清单
   · 「file delete 只接受 file_id」→ **作废**，现在收 <file_id|文件名>
   · 「file delete 按 id 单向查找，找不到就 FMT-002」→ **作废**，先 file_id 再文件名
   · 「名字存在但已在回收站报 FMT-002」→ **作废**，报 FMT-001 并给出 file_id
   · 「（可用 trash 查回）」这条帮助文案 → **作废**，改成「只能写、还不能从 trash 查回」
```

> **本节与下一节的分工**：上面记的是**提交 `0ad9efc`**（`file delete` 收两种参数）。
> 紧随其后的 **`5bf2c1f`「fix(file): compare names the way Windows does, or uploads
> overwrite data」** 把比较改成不区分大小写，那是**数据损坏修复**、不是体验优化，
> 单列在 18.22。**读 10.2.4 与本节时都要连带读 18.22**：10.2.4 里三步定位的比较、
> 本节 ② 步的「名字在同用户范围内唯一」，最终口径都是 `iequals()`（ASCII 折叠）。
> 各处的用例计数也已按 `5bf2c1f` 更新为 **129**；随后提交 `9c3d2cb` 又把它推到
> **134**（见 18.23，本节保留的是 `5bf2c1f` 当时的历史数字）。

---

### 18.22 本次重构阶段 5（续）：比较一律不区分大小写（commit 5bf2c1f）

`5bf2c1f`「fix(file): compare names the way Windows does, or uploads overwrite data」
**修的是一个真实的数据损坏路径**，不是「大小写体验」问题。这一节记录损坏怎么发生、
修了什么、以及由此定下的口径。

```text
① 损坏路径（回归用例 File.大小写不同的同名必须被当成重名 先复现、后修复）
   ① 上传 doc.txt（"hello world"，11 字节）→ 记录 fmt-<今天>-0，size = 11
   ② 再传同名不同大小写的 DOC.TXT（"different content here"，22 字节）
      旧行为：重名判定是 == 精确比较 → 放过 → **产生两条记录**
              而两者在 Windows 上落到**同一个磁盘路径**
              → 第二次上传把第一个文件的字节**覆盖**了，
                而 file.json 里**第一条记录还写着 size = 11 / 旧的 md5**
      后果：**同一个磁盘文件被两条记录指向 + 字节被覆盖 + 元数据失真**
            （断言证据就是它：仓库里那个文件的 file_size 期望 11、实际 22）
   为什么严重：这不是显示或易用性问题——两份「不同文件」的历史记录里有一份是假的，
   而按第一条记录去下载/校验会拿到与元数据不符的字节（15.4 的一致性异常被人为制造出来）

② 改了什么（iequals()，ASCII 大小写折叠）
   src/file/file.cpp
     commit_upload()  重名判定：record.file_name 与 prepared.file_name 比较不再区分大小写
                      （同用户 + 任何 Bucket + 正常文件 + 同名即 FMT-105，第 38 节）
     get_by_name()    按名字查（file get <名字>）不区分大小写
     locate_record()  ① file_id ② 文件名 ③ 已在回收站的名字，**三处比较都不区分大小写**
   src/bucket/bucket.cpp
     list()           is_current = iequals(info.name, config_.current_bucket)
                      （current_bucket 存的是用户敲的拼写，可能是 "WORK" 而目录是 "work"）
     remove()         was_current = iequals(config_.current_bucket, name)
     find_trashed()   回收站里的名字与「原桶名」两轮定位都不区分大小写

③ 由此定下的口径（三条，别只当实现细节）
   1. **凡按名字/标识定位，一律不区分大小写**：文件名、file_id、桶名、回收站条目名。
      理由是 Windows 的文件系统与路径本身就不区分大小写，用户敲 `REPORT.TXT`
      不该得到「文件不存在」
   2. **名字在同用户范围内唯一（不区分大小写）**：`doc.txt` 与 `DOC.TXT` 不能共存——
      这不是额外限制，而是上面那起覆盖事故的**根因防线**（第 38 节的 FMT-105）
   3. `bucket use WORK`（目录实际叫 `work`）时：**current_bucket 保留用户敲的拼写**，
      但 `bucket list` 的「当前」标记按不区分大小写匹配，`bucket remove` 的
      `was_current` 同理
      【**该口径已被提交 `9c3d2cb` 改写**：落盘时规范化成磁盘上的实际名字
        （`canonical_name()`），`use WORK` 存进 `current_bucket` 的就是 `work`；
        `create` 也统一转小写。见 18.23——19.1 里那条待决项由此定稿】

④ 新增用例（126 → **129 项全绿**）
   File.大小写不同的同名必须被当成重名   先断言数据完整性：第一次上传的文件必须还在、
                                        大小 == 记录里的 size、内容仍是 "hello world"，
                                        然后才断言第二次上传被拒（证据顺序刻意如此：
                                        万一磁盘被覆盖，先炸的就是完整性断言）
   File.按名字查询不区分大小写           file get report.txt 找得到 Report.txt；
                                        file delete REPORT.TXT 可用
   Bucket.大小写不同也认得同一个桶       create("work") 后 use("WORK")，
                                        list() 里「当前」标记落在实际的 work 目录上
                                        （本节的数字是 5bf2c1f 当时的：current_bucket 里存的仍是 "WORK"，10.1；
                                          **9c3d2cb 之后存的是 "work"**，见 18.23）
```

**`5bf2c1f` 暴露出来的三条：全部定稿（提交 `9c3d2cb` 与 `711da4c`）**：

```text
① 名字长得像 file_id 时会被「先查 id」遮住 —— **已定稿（9c3d2cb）**
   【提交 9c3d2cb】两条可选做法**都做了**：
   a) 上传时按保留形状拒绝形如 `fmt-YYYYMMDD-N` 的名字 → 新错误码
      `FMT-106 FileNameLikeFileId`（退出码 2，属 FMT-1xx 文件名校验；
      判定 looks_like_file_id()，见 9.1 第 6 条与 9.3）
   b) 旧数据里已经存在的这种名字不猜：`locate_record()` 分两步各自记下命中
      （by_id / by_name），两者命中**不同**记录 → `FMT-001`「有歧义：…请直接用
      file_id 指定要删哪一个」（消息点名两条记录）。**只给 file delete 加**
      （locate_record() 只被 remove() 用）；file get 的两种查询范围保持不变
      ——这是有意的差异：get 只读，最坏是把 id 命中的那条给你看；delete 不能猜。
      file get 命中回收站记录时另回 trash_path（10.2.4）
② 按名字删除可能删到别的 Bucket 的文件 —— **已定稿，移出待决清单**（提交 `711da4c`）
   名字的作用域是「同用户跨 Bucket」（9.2、10.2.4），所以人在「生活」桶里敲
   `file delete a.txt` 可能删到「工作」桶里的 a.txt。**定的做法是「跨桶要确认」**：
   ① CLI 先发只读预检（`file.delete` + `dry_run`），把「a.txt 属于 Bucket 工作、
      当前 Bucket 是生活」原样打印；② 交互窗口问一次、一次性命令要 `--yes`；
   ③ 服务端缺 `force` 时返回 **`FMT-016 ConfirmRequired`**（不是静默执行，也不是
      `FMT-001`）；④ 成功后 `message` 带归属「文件已移入回收站：a.txt（Bucket：工作）」，
      `data` 也带 `bucket`。
   **为什么不改成「按名字只在当前桶里找」**：那会让跨桶同名重新变成
   「文件不存在」——正是前面两轮（`0ad9efc` 与 `5bf2c1f`）修掉的误导性诊断；
   而且 file.json 里 `bucket` 只是记录字段，用户按名字称呼文件时不该被当前站位左右的
   是**结果**，而该是**提示**。见 10.2.4、11.15、18.24
③ `bucket use` 的拼写规范化 —— **已定稿，移出待决清单**
   【提交 9c3d2cb】落盘时规范化到**磁盘上的实际名字**（`canonical_name()`：
   不区分大小写地扫桶目录），`use WORK` 存进 current_bucket 的是 "work"；
   get 显示的、delete 写进回收站目录名与 .original 的 original 也都是实际名字；
   create 先 to_lower() 再建目录（WORK → work，renamed 为真时回 note）。见 10.1、18.23
```

---

### 18.23 本次重构阶段 5（续）：Bucket 名称统一小写、保留 file_id 形状、回收站路径（commit 9c3d2cb）

`9c3d2cb`「feat: lowercase bucket names, reserve the file_id shape, report the trash path」
一次收口四个由「按名字查找」打开的口子。**134 个用例全绿**（由 129 增 5）。

```text
① 新错误码 FMT-106 FileNameLikeFileId（include/fmt/common/error.hpp、
   src/common/error.cpp）：退出码 2，默认消息「文件名与文件标识同形（fmt-YYYYMMDD-N），
   会与 file_id 混淆」；属 FMT-1xx 文件名校验一组。HTTP 语义上属 400 一类，
   但 http_status_for() 尚未显式登记 → 现在落 default: 500（12.5 有注，属待办）

② validate_file_name() 在「Windows 保留设备名」之后加这一条（9.1 第 6 条、9.3）：
   新判定函数 bool looks_like_file_id(std::string_view)（validation.hpp）
   形状：fmt-YYYYMMDD-N，最短 14 个字符（4 + 8 + 1 + 1），"fmt-" 前缀按 ASCII 折叠比较，
         日期段恰好 8 位数字，序号段全是数字且非空
   拒绝：fmt-20261008-0 / FMT-20261008-0 / fmt-20261008-123
   放行：fmt-20261008-0.txt / my-fmt-20261008-0 / fmt-20261008（没有序号）/
         fmt-2026100-0（日期 7 位）
   理由：定位是「先按 file_id 查、查不到再按名字查」（10.2.4）。若一个文件就叫
         fmt-20261008-0，而另一个文件的 file_id 恰好是它，按名字提交的删除就会删错对象
         ——所以这个形状是**保留形状**，与 Windows 保留设备名同类。
   来源：用户明确要求「上传时拒绝这种名字」，另外要求同时实现歧义检测（③）

③ FileService::locate_record() 现在分两步各自查找，若两个索引不同（一个按其 file_id
   命中、另一个按其文件名命中）→ FMT-001 InvalidArgument，消息形如
   「有歧义：fmt-20261008-0 既是 fmt-20261008-0 的文件标识，又是另一个文件的文件名
   （file_id fmt-20261008-1）。这种名字现在不允许上传；请直接用 file_id 指定要删哪一个」。
   **只给 delete 加**（locate_record() 只被 remove() 用）；**file get 的两种查询范围
   保持不变**（10.2.4 的「两种查询范围是有意不同的」一段）

④ Bucket 名称统一小写（bucket.hpp 新增 struct BucketCreation，create 由 Status 改为
   Result<BucketCreation>）：
   create   名称先 to_lower()（**只折叠 ASCII**，中文不受影响）再校验、再建目录；
            WORK 建成 work，renamed = true，调用方据此提示用户；再敲 create WORK
            会被当成同一个桶 → FMT-201
   use      落盘时规范化成磁盘上的实际名字（canonical_name()：不区分大小写地扫桶目录），
            所以 use WORK 存进 current_bucket 的是 "work"
   get      返回的 name 也是磁盘上的实际名字（get WORK 显示 work）
   delete   回收站目录名与 .original 里的 original 同样用磁盘实际名字，
            以后 restore 出来的目录拼写才一致
   为什么：Windows 目录不区分大小写，WORK 与 work 本来就是同一个目录；不统一拼写，
           current_bucket、file.json 的 bucket、.original 的 original 会各留一份，
           日后比对与恢复都会踩坑
   服务端：bucket.create 的 data 增加 note（仅在发生转换时出现），形如
           "note": "Bucket 名称统一使用小写：已把 WORK 转为 work"；
           bucket.use 的 bucket 字段改为**规范化后的名字**，值被改过时也给 note；
           CLI 把 note 打成单独一行「提示：…」（12.3.2.1、11.14）

⑤ file get 对回收站里的记录返回 trash_path（相对数据根、正斜杠，形如
   trash/user/.files/工作/2026/10/08/test.txt）。仓库里没有该文件时**不返回 path**
   ——这是设计，不是缺失；CLI 多打一行「回收站路径：…」（10.2.4、11.14、12.3.2.1）

⑥ iequals() 改为只折叠 ASCII（src/common/string.cpp）：>= 0x80 的字节直接原样比较
   （原来交给 std::tolower，C locale 下虽是恒等，但一旦有人调 setlocale 就会把 UTF-8
   名字改坏）。这是「比较一律不区分大小写」那条口径的**实现约束**（9.4）

⑦ 帮助文案（src/cli/cli.cpp，照源码抄进 11.4）：help bucket 新增
   「名称统一使用小写：create WORK 会建成 work（会提示你）；/ 其余命令按名找桶时
   不区分大小写，找得到就按磁盘上的实际名字处理。」两行；help file 的 upload 追加
   保留形状两行、get 改成「按文件名只查正常文件；按 file_id 连回收站里的也查得到
   （带 is_trash 与 trash_path）」、末尾补「名字与 file_id 的比较都不区分大小写
   （Windows 习惯）」

⑧ 新增用例（129 → **134 项全绿**）
   Validation.与file_id同形的文件名被拒   判定函数 + validate_file_name 的错误码与
                                        退出码；放行反例（.txt 后缀 / my- 前缀 /
                                        无序号 / 日期 7 位）都在断言里
   File.与file_id同形的名字不能上传       显式名与从来源推断的名字**两条路都拦**
   File.标识与名字同时命中时报歧义        手工造旧数据（一个 file_id 恰是另一个的
                                        file_name）后 remove 必须报 FMT-001，
                                        消息里出现「歧义」与另一条的 file_id
   Bucket.创建时大写会转成小写           BucketCreation{requested="WORK", name="work",
                                        renamed, became_current}、config 里是 "work"、
                                        **磁盘上的实际条目名是小写**（用目录遍历比对
                                        entry 名，不能用 directory_exists 判断大小写
                                        ——那在 Windows 上恒为真）、再敲一次 → FMT-201
   Bucket.use大写规范化到磁盘上的名字     use("WORK") 后 current_bucket == "work"、
                                        get("WORK").name == "work"
   另两条既有端到端用例追加断言：
   Service.管道能执行Bucket命令           create WORK → data.bucket == "work" 且有 note
   Service.管道能上传与操作文件           file get 回收站记录带 trash_path
                                        （以 "trash/user/.files/" 开头）
```

**自查（本节引入的口径，别再写反）**：`current_bucket` 不再保留用户敲的拼写；
Bucket 名称不会大小写各存一份；文件名不能是 `fmt-YYYYMMDD-N`；`file get` 按名字
**查不到**回收站里的记录（按 `file_id` 才查得到），并且回收站记录带 `trash_path`。

---

### 18.24 本次重构阶段 5（续）：先检查 → 说清楚 → 再确认（commit `711da4c`，缺口收尾 `6a40742`）

`711da4c`「feat: check first, say what conflicts, then confirm」+ `6a40742`「fix: close the
three gaps the document review found」。**136 个用例全绿**（134 + 新增 2 条）。

```text
① 新错误码 FMT-016 ConfirmRequired（include/fmt/common/error.hpp、src/common/error.cpp）：
   退出码 2，默认消息「该操作需要显式确认（force）」，HTTP **400**。
   **口径变更**：trash.delete 缺 force 原来 FMT-001，现在 FMT-016；file.delete 跨桶
   未确认、bucket.delete 非空桶未确认同样是它。通用规则见开发文档第 82 节

② 预检 dry_run（args.dry_run = true；HTTP 由 src/server/server.cpp 的 delete_args()
   解析 ?dry_run=1，同时保留 ?force=1 与请求体 {"force":true}）：
   file.delete    FileService::check_remove()（FileDeleteCheck）→ ambiguous / other_bucket /
                  blocked / needs_confirm / current_bucket / file_id / file_name / bucket /
                  path / candidates[] / message。
                  **歧义是 blocked 而不是 needs_confirm**：y/N 表达不了「删哪一个」，
                  只能让用户改用 file_id（候选两条都摊开）。这是刻意的，不是漏了确认
   trash.delete   711da4c 时由 BucketService::get_trashed() 给出条目详情，0fc242b 起
                  统一为 TrashService::check_purge()：needs_confirm **恒为 true**，
                  把要毁掉的东西摊开（类型 / 标识 / 名称 / 删除时间 / 文件数 / 占用）
   bucket.delete  BucketService::check_remove()（0fc242b 补）：有内容才 needs_confirm

③ CLI 统一流程（src/cli/cli.cpp 的 confirm_before_acting() / print_precheck()，11.15）：
   ① 连上服务 → ② 发只读预检 → ③ 打印情况（目标属于哪个桶 / 两条歧义候选 /
   永久删除会毁掉什么）→ ④ 交互问「确认执行？(y/N)」或一次性 --yes → ⑤ 同意后才带 force
   发真实请求。**用户确认之前一个破坏性请求都不发**；预检失败就直接报预检的错；
   --yes 是本地开关、不进 argv；答 n → 「已取消」+ 退出码 0；一次性缺 --yes → 退出码 2；
   服务端**仍独立校验 force**（预检负责「说清楚」，force 负责「兜底」）

④ 顺带的行为变化：file.delete 的成功响应增加 bucket 字段，跨 Bucket 时 message 变成
   「文件已移入回收站：a.txt（Bucket：工作）」

⑤ 三个已修好的缺口（6a40742）：
   · http_status_for() 登记 FileNameLikeFileId → 400（原口径「未登记会落 500」作废）
   · FileService::get_by_id() 改用 iequals——标识比较一律不区分大小写（9.4）
   · bucket.get 的 path 改用规范化后的名字（bucket get WORK → bucket 与 path 都是 work）

⑥ 新增/改动用例（134 → **136**）
   File.删除预检会把情况说清楚        同桶不打扰；跨桶说清两个桶名；预检不改数据
   Service.破坏性操作先预检再确认      file.delete 跨桶的 dry_run 形状；无 force → FMT-016；
                                     带 force 成功且 message 带 Bucket；trash.delete 的
                                     dry_run 带 files/bytes/original；无 force → FMT-016
   File.标识与名字同时命中时报歧义     （追加 check_remove 断言：ambiguous + 两条候选）
   File.列表与查询                     （追加大写 file_id 也查得到）
   Service.管道能执行Bucket命令        （追加 bucket get WORK → bucket/path 都是 work）
   Server.Bucket路由与状态码           （DELETE ?dry_run=1 断言；未确认的码改 FMT-016）
   Server.File路由与上传               （?dry_run=1 断言；同形名 → 400 + FMT-106）
   Service.管道能执行回收站命令        （未确认的码改 FMT-016）
```

---

### 18.25 本次重构阶段 5（续）：回收站读侧 + 删桶前提醒（commit `0fc242b`）

`0fc242b`「feat(trash): make the file level readable, and warn before a bucket goes」。
**140 个用例全绿**（136 + 新增 4 条）。一句话：**`file delete` 以前只写不读，
现在文件级条目能在 `trash` 里看到、恢复、永久删除了。**

```text
① 新模块 src/trash/（include/fmt/trash/trash.hpp + src/trash/trash.cpp）：
   TrashService **组合** FileService 与 BucketService，把两级合成一份视图，
   并负责跨命名空间的标识解析；自己不存状态。原口径「src/trash/ 是计划位置」作废

② 权威来源：**文件级 = file.json**（is_trash / trash_reason / **新增 deleted_at**），
   路径由 file_id 与记录推出；**桶级 = trash/<用户>/.original**（不变）。
   **data/trash.json 不再写入**（以前只是第二份副本）：保留为**只读兼容**
   （legacy_deleted_at() 补老数据的 deleted_at；remove_trash_record() 在回退/
   永久删除时顺手清掉）。原口径「trash.json 记文件级条目」作废

③ 统一条目形状 TrashEntry：type（file/bucket）/ id（文件=file_id；桶=回收站目录名）/
   name / bucket / deleted_at / bytes / files / present / restorable / trash_path / message。
   响应：trash.list → {entries, count, files, buckets}；get/restore/delete → {entry, message?}；
   预检 → {needs_confirm, blocked, entry, message?}。
   **旧的 deleted_buckets 与顶层 trashed/original/files/bytes 形状已不存在**。
   桶级条目的 files/bytes 在 list() 里对每个 present 条目遍历一次目录算出来
   （提交 18f16ca；list 因此不是纯索引查询，见 10.4）

④ trash list 标出是文件还是桶（CLI；**提交 18f16ca 起桶级条目也带文件数与占用**）：
     [文件]  a.txt（Bucket 工作，1.2KB）
     [桶]    工作（3 个文件，5.0KB）  ->  工作_20261008151538
   共 2 项（1 个文件、1 个桶）

⑤ 标识解析（TrashService::resolve()，不猜）：① 桶的回收站目录名（精确）→ ② file_id
   （**查所有回收站记录**，含随桶删除的）→ ③ 桶的原名 → ④ 文件名；③④ 命中多条 →
   blocked + 候选，让用户用 file_id 或完整回收站名指定

⑥ 回退的三种「确认解决不了」（都是 blocked，不是 needs_confirm）：
   · 目标位置已有同名正常文件（删掉后又传了一个）→ FMT-401 RestoreConflict，
     消息点明冲突那条的 file_id；不覆盖、不改名
   · 随桶一起删除的文件（trash_reason == "bucket"，数据在桶的回收站目录里）→
     FMT-402 RestoreBucketMissing，让用户整体恢复那个桶；这类文件**不出现在 trash list
     的文件区**（整棵树已计入桶级条目的文件数），但按 file_id 查得到、restorable = false
   · 回收站里数据缺失 → FMT-002，消息给出路径

⑦ bucket.delete 新增预检（BucketService::check_remove()）：有内容时报告文件数与占用，
   并写明「删除后只能整体恢复这个桶、无法只恢复其中某个文件」（是当前桶再补一句
   「当前 Bucket 会被置空」）→ needs_confirm = true、缺 force 返回 FMT-016；
   **空桶不打扰用户**。bucket.delete 也进了 CLI 的「检查→提示→确认」名单

⑧ 帮助文案（src/cli/cli.cpp，照源码抄进 11.4）：trash 的标题改成「两类条目：文件级
   [文件] 与桶级 [桶]，都会标出来」，list/get/restore/delete 说明按新行为写；
   bucket delete 加了「桶里有文件时会先提醒…一次性命令要加 --yes」

⑨ 新增/改动用例（136 → **140**）
   Trash.文件级条目能列出并回退        list 里标出 type=file / id / name / bucket /
                                     deleted_at；按 file_id 取到同一条；回退后
                                     trash_reason 与 deleted_at 清空；列表变空
   Trash.回退遇同名冲突要拦住          删掉后又上传同名 → 预检 conflict（不是靠确认解决），
                                     restore 返回 FMT-401（退出码 4），两份数据都还在
   Trash.随桶删除的文件不能单独回退     列表里只有那个桶（文件不单独列）；按 file_id
                                     查得到但 restorable=false；restore → FMT-402（退出码 3）
   Trash.永久删除文件级条目            预检 needs_confirm 恒真 + 条目详情；预检不改数据；
                                     执行后记录一并清掉
   File.软删除进回收站                 （改为断言 file.json 里的 is_trash / trash_reason /
                                     deleted_at，并断言 trash.json 保持空）
   Service.破坏性操作先预检再确认       （新增 bucket.delete 非空桶预检 + 缺 force → FMT-016
                                     + 带 force 成功）
   Service.管道能执行回收站命令         （entries/entry 统一形状：type / id / name）
   Server.Bucket路由与状态码            （entries / entry 新形状 + ?dry_run=1）
   Server.File路由与上传                （?dry_run=1 零副作用）
```

---

### 18.26 文档复核引出的两处修正（commit `18f16ca`）

`18f16ca`「fix(cli): report the precheck's own exit code, and count files in the list」
修的是**文档与代码不一致的两处**（140 个用例仍全绿）：

```text
① 预检失败时的退出码（10.4 / 11.15 / 开发文档第 127.7 节）
   原实况：一次性命令的预检失败时，stderr 打真实错误码（FMT-002 → 3），
           **进程退出码却统一走 2**（「需要确认」的码），脚本会误读
   现在：confirm_before_acting() 返回 ConfirmOutcome{proceed, exit_code}，
         调用点直接用 outcome.exit_code —— 预检的错误码原样透出
     预检自身失败（FMT-002 / 已在回收站等） → 预检的那个错误码（FMT-002 → 3）
     预检通信失败（FMT-601 等）             → 通信错误的码（8）
     blocked（歧义 / 同名冲突 / 随桶删除 / 数据缺失） → 2（FMT-001）
     一次性命令缺 --yes                     → 2（FMT-016）
     交互窗口答 n（用户主动取消）            → 0
   顺带：缺 --yes 的提示语改成
     「该操作需要确认（FMT-016）：请加 --yes，或在交互窗口里执行」
   用例 Service.破坏性操作先预检再确认 覆盖（18.25 的同一套用例，断言更新）

② trash list 里桶级条目的 files / bytes（10.4 / 12.3.2.1 / 11.14）
   原实况：列表里恒为 0（那是 0fc242b 的 `to_entry()` 没填），
          与「用户批准的格式」不符
   现在：TrashService::list() 对每个 present 的桶级条目调一次 get_trashed()
         遍历目录，把文件数与占用填进条目；CLI 那一行变成
         「  [桶]    工作（3 个文件，5.0KB）  ->  工作_20261008151538」
   代价（保留说明）：每个桶条目多遍历一次目录，trash list 因此不是纯索引查询
         —— 显式命令，可以接受；BucketService::list_trashed() 那层索引仍不遍历
```

---

### 18.27 用户报的「文件明明存在却说不存在」：粘贴污染（commit `a9af276`）

`a9af276`「fix(upload): strip what paste adds, and name it when it hurts」。
**142 个用例全绿**（140 + 新增 2 条）。

```text
用户报的原始现象（照实记录）：
  fmt> file upload ‪C:\Users\lenovo\Pictures\pet-food-store\头像\asdva.jpg
  执行失败：FMT-002 本地文件不存在：‪C:\...\头像\asdva.jpg
  错误码：3
文件确实存在（130184 字节）。原因是路径首尾各夹了一个**不可见的格式字符**：
开头 U+202A（LEFT-TO-RIGHT EMBEDDING）、结尾 U+202C（POP DIRECTIONAL FORMATTING）
——从聊天窗口、网页、终端复制路径时常见，屏幕上完全看不出来。
CLI 用的是 wmain（宽字符），中文路径本身没问题；是这两个字符被当成了路径的一部分。

① 新函数（include/fmt/common/string.hpp / src/common/string.cpp）：
   clean_user_path(text)          清掉不可见格式字符 + **成对**引号 + 首尾空白
   invisible_characters(text)     按出现顺序去重返回码位名（{"U+202A","U+202C"}）
   清掉的码位：U+00A0、U+00AD、U+200B–U+200F、U+202A–U+202E、U+2060–U+2064、
   U+2066–U+2069、U+FEFF；引号 `"` / `'` / `“”` **成对**才去（不成对不动，
   免得改掉名字里真的带引号的情况）；中文等多字节序列原样保留

② 生效位置（一处收口 + 一处纵深防御）：
   service/commands.cpp 的 argument()  —— **所有**位置参数读取的唯一入口，
   CLI 与 HTTP 共用（12.3.2.1）；
   file/file.cpp 的 prepare_upload()  —— 上传来源与显式文件名再清一次（10.2.3）

③ 报错口径（9.5、10.2.3）：清掉后仍找不到 → FMT-002，消息点名码位；只去了引号/空白
   → 「（已去掉粘贴带进来的引号或空白）」；什么都没清 → 原来那句

④ 名字规则：不可见字符**不允许**进名字——validate_file_name() → FMT-101、
   validate_bucket_name() → FMT-202（9.1、9.3）。理由：名字要长期存下来、
   还要被用户再敲一遍，屏幕上看不出来的字符没法重敲

⑤ 新增用例（140 → **142**）
   String.清理粘贴带进来的路径污染   U+202A/U+202C、成对与不成对引号、首尾空白、
                                   U+00A0、中文路径不受影响、invisible_characters()
                                   的去重与码位名
   File.粘贴路径里的不可见字符会被清掉  **复刻用户场景**：中文目录 头像/ 下的文件 +
                                   U+202A/U+202C 包裹 → 上传预检成功；Explorer 引号
                                   包裹 → 成功；仍然找不到时消息里出现 U+202A 与
                                   U+202C；名字里的不可见字符被 validate_file_name 拒绝
```

**自查（本轮新增的口径）**：不能再写「上传路径按原样使用」——路径会被 `clean_user_path()`
清过（`argument()` 一处收口，`prepare_upload()` 再清一次）；而 `FMT-101`（文件名）与
`FMT-202`（Bucket 名）的适用条件里要写上「含不可见格式字符」。

---

### 18.28 CLI 崩溃：预检的开关挂到了位置参数数组上（commit `2c841c8`）

`2c841c8`「fix(cli): build the precheck arguments as an envelope, not on the array」。
**143 个用例全绿**（142 + 新增 1 条）。

```text
用户实测：fmt> file delete a7.jpg → 弹出「Debug Error! abort() has been called」
原因    确认流程里写成：
          check.args = arguments;          // arguments 是位置参数**数组** ["a7.jpg"]
          check.args["dry_run"] = true;    // nlohmann 对数组用字符串下标 → type_error.305
        异常没人接 → abort()（Debug 版就是那个框）。
        服务端期望的形状是开关与 argv **同级**：{"argv":["a7.jpg"],"dry_run":true}

修复    include/fmt/cli/cli.hpp / src/cli/cli.cpp 新增
          nlohmann::json argument_envelope(const nlohmann::json& positional,
                                           bool dry_run = false, bool force = false);
        **预检与真实请求都用它**，两者因此不可能各错一处（11.15、13.9.3）

教训    单元测试原来**自己照着服务端期望的形状拼请求**，而 CLI 拼的是另一种形状：
        **测试全绿、CLI 一敲就崩**。测试复制了契约，却没有共用产出契约的那段代码。
        规则定成「形状必须由同一个构造函数产出，测试不许自己拼」
用例    Cli.位置参数的信封形状（tests/cli_test.cpp）：旧写法必须抛 type_error.305，
        新写法必须是**带 argv 且开关同级**的对象——钉住的是机制本身
```

---

### 18.29 服务重装暴露的两处进程间竞争 + 回收站结果的误导字段（commit `5b316b3`；桶级由 `8f0fd5c` 补齐）

`5b316b3`「fix: stop the state file and the temp sweep from fighting each other」。
**144 个用例全绿**（143 + 新增 1 条，另改 1 条既有用例）。这一批都是**用户在自己机器上
重装服务 + 跑真命令**时暴露的。

```text
① 提权结果文件被 temp 清理误删 → 假失败 FMT-602（4.2、13.8.3、13.8.4）
   实测日志：
     [Elevated] 提权副本执行成功：install（错误码 0）
     [Elevated] 结果文件写入失败：无法打开文件：D:\...\temp\fmt-elev-2296.json.tmp
     [Service]  清理 temp/ 中 1 个遗留临时文件
     [Cli]      service install 提权失败：FMT-602 提权副本没有返回结果（退出码 1）
   原因：service install 会在同一次操作里**启动服务**，而服务启动时清理 temp/ 里所有
        fmt-* 文件；提权副本回传结果的临时文件正好叫 fmt-elev-<pid>.json(.tmp)。
        **服务其实装好并启动了，用户看到的却是失败。**
   修复：src/service/runtime.cpp 的 clean_temp_directory() **只清十分钟以前**的
        （结果文件寿命只有几十毫秒），读不到时间戳的也不动

② 原子写的临时文件重名 → service.json 静默不更新（6.6）
   原因：write_text_file_atomic() 固定用 <目标>.tmp，而同一目标有两个写者——
        安装器在启动服务后写 service.json，服务启动时也写它。先完成的一方把 .tmp
        rename 走，另一方读回校验时报「无法打开文件 …tmp」；安装器那处是
        (void)save_state(updated)（**忽略返回值**），于是 service.json 的时间戳
        一直停在旧值（实测 18:19，多次重装都不更新）
   修复：临时名带 pid + 序号（<目标>.<pid>.<n>.tmp）、进程内对原子写加互斥、
        MoveFileExW 对 ACCESS_DENIED / SHARING_VIOLATION / LOCK_VIOLATION
        （及 FILE_NOT_FOUND）短暂重试（最多 40 次 × 5 ms）
   用例：Storage.两个写者同时写同一个文件不会互相踩（8 线程 × 40 轮写同一个文件，
        断言零失败、内容必须是某一次**完整**写入、且不留 .tmp）

③ 回收站恢复结果里的误导字段（10.4；文件级 `5b316b3`、桶级 `8f0fd5c`）
   现象：trash restore 成功后打印「状态：数据已不存在」，而数据刚刚被搬回仓库
   原因：TrashService::restore() / purge() 把返回条目的 present 改成 false（本意是
        「已经不在回收站里了」），但 present 的语义是**数据在不在磁盘上**
   修复：5b316b3 改掉文件级两处，**8f0fd5c 再删掉桶级两处**（`src/trash/trash.cpp`
        的 restore / purge 里的 `done.present = false`）——两级、restore / purge
        四种结果都不再翻转它，结果由 message 说明；语义写死在 10.4 与开发文档
        53.1、53.3 的字段表里。
        测试补断言：Service.管道能执行回收站命令（桶级）、Trash.文件级条目能列出
        并回退 / Trash.永久删除文件级条目（文件级），**计数仍是 144**
        （只加断言，没有新增用例）

④ 既有用例改口径：Service.启动时清理temp里的遗留临时文件
   （现在是「**新的留着**、把时间拨回一小时后的**旧的清掉**、用户手放的其它文件始终不动」）
```

这一批修完之后，作者又在**已安装的服务**上跑了一遍真流程：上传、软删除、
`trash list` / `restore`、交互窗口答 n 的取消路径、以及一次重装。

---

### 18.30 桶级 `present` 补齐 + 真机核实（commit `8f0fd5c`）

`8f0fd5c`「fix(trash): stop the bucket results from claiming the data is gone」。
**144 个用例仍全绿**（只补断言，没有新增用例）。

```text
修什么    src/trash/trash.cpp 里**桶级**的 restore 与 purge 也把 `done.present = false`
          删掉了（文件级在 5b316b3 已经改过）——现在两级、restore/purge 四种结果
          **都不再翻转 present**：它只表示「数据在不在磁盘上」，操作结果由 message 说明
补断言    Service.管道能执行回收站命令：桶级 trash.restore 与 trash.delete 的结果条目
          各断言 present == true
          Trash.文件级条目能列出并回退 / Trash.永久删除文件级条目：文件级同样各一条
          （这正是该挡住这条误导信息的地方）
```

**对着已安装的服务实测（已核实，`8f0fd5c` 之后跑过一遍）**：

```text
bucket create tmp-verify
bucket delete tmp-verify --yes
  Bucket：tmp-verify / 当前：否 / 文件数：0 / 占用：0B
  Bucket 已删除（移入回收站）：tmp-verify -> tmp-verify_20261008192822（之后只能整体恢复这个桶）
trash list
  [桶]    tmp-verify  ->  tmp-verify_20261008192822
共 1 项（0 个文件、1 个桶）
trash restore tmp-verify_20261008192822
  [桶] tmp-verify
    标识：tmp-verify_20261008192822
    删除时间：2026-10-08T19:28:22
  Bucket 已回退：tmp-verify          ← 没有「状态：数据已不存在」
trash delete tmp-verify_20261008192822 --yes
  永久删除后不可恢复：tmp-verify_20261008192822（原桶 tmp-verify，0 个文件，0B）
  已永久删除：tmp-verify
trash list → 共 0 项
```

真机顺带确认了两件之前只写在文档里的行为：**空桶删除不打扰用户**
（`needs_confirm = false`，`--yes` 多余但无害），以及 `bucket delete` 成功后消息里
带「**（之后只能整体恢复这个桶）**」（10.1、11.15）。

---

### 18.31 全功能真机测试：兜底路由的 404/501 + 一个待决缺口（commit `4ddb515`）

**2026-10-09 对已安装的服务**跑了 68 项检查（用户提供素材目录），暴露出**一个已修问题 +
一个真缺口**。`4ddb515`「fix(server): answer unknown routes with 404, unimplemented ones
with 501」，**144 个用例全绿**（只改实现与既有用例，没有新增用例）。

```text
① 已修：未知 / 未实现的 /api 路径原来一律 500（12.3、12.5）
   实测     GET /api/nosuch → HTTP 500 + FMT-602「操作尚未实现：/api/nosuch」
   原因     兜底路由 `Get(R"(/api/.*)")` 里**硬编码 response.status = 500**
   现在     已知模块（**file / trash / share / config / server / preview——`bucket` 不在其中**，
            提交 `4b812b5` 把桶的接口整个删了）下没有这个接口
            → **501 + FMT-602**「接口尚未实现：<path>」（如分享下载端点）
            完全打错的 /api/... → **404 + 新错误码 FMT-017 RouteNotFound**「没有这个接口」
            **`/api/bucket*` → 404 + FMT-017**（故意不要，不是「还没做」）
            已知路由但业务找不到对象（GET /api/file/nope.bin）→ 404 + FMT-002（不变）
           `FMT-602` 的映射也从「落 default 500」改成**显式 501**
   新错误码 FMT-017 RouteNotFound（FMT-0xx 通用一组，退出码 3）**只由 HTTP 兜底路由产生**
            ——管道入口没有「路由」概念

② 缺口收窄（**提交 22c3c3e 之后**；18.39 有完结记录）
   当时     服务装好、在跑，但 localhost:4122 **没有监听**
   原因     ServerConfig::enabled 默认 false，而全仓库没有代码把它置为 true
            （安装流程不碰 config/server.json，CLI 也没有命令能开）；
            5.2 里那句「Service 场景下由安装流程置为 true」**没实现**
   手动验证 改成 true 并重启服务后，HTTP 入口完全正常（下表那些 HTTP 检查就是这么跑通的）
   **现在**：代码默认 host 从 127.0.0.1 改成 **localhost**，线上 server.json 已
            enabled: true 并真机验收通过（18.39）
   **仍是缺口**：**全新数据根**装完服务后 enabled 仍默认 false（没人自动打开）
   待决     ① 安装流程按文档置 true，还是 ② 加一条 CLI 命令（如 config http on）
```

**真机测试通过的部分（2026-10-09，对已安装服务实测；用户提供素材目录）**：

| 类别 | 已核实 |
|---|---|
| Bucket | `create PROBE` → 提示转小写 `probe` ✓；重名 `FMT-201` ✓；中文桶名 ✓；`list` / `get` / `use` 大小写不敏感 ✓；`use` 不存在 `FMT-200` ✓；名字非法（`a:b` / `.` / `CON`）`FMT-202` ✓；**删空桶不打扰用户** ✓；删非空桶提醒「只能整体恢复这个桶」并要 `force`（`FMT-016`）✓ |
| 上传 | 本地路径（含中文目录）✓；显式中文名 ✓；同内容 `FMT-304` ✓；同名 `FMT-105` ✓；`fmt-YYYYMMDD-N` 形状 `FMT-106` ✓；`CON.txt` `FMT-103` ✓；路径不存在 `FMT-002` ✓；名字含分隔符 `FMT-102` ✓；**HTTPS 真实下载成功**（github raw）✓；URL 推不出名 `FMT-100` ✓；域名解析失败 `FMT-301`（带「域名解析失败」）✓；`ftp://` `FMT-300` ✓；超 50MB `FMT-303` ✓；**不可见字符包裹的路径自动清掉并上传成功** ✓ |
| 查询 / 删除 | `file get` 按名 / 按 id ✓；不存在 `FMT-002` ✓；缺参 `FMT-001` ✓；同桶删除不问 ✓；重复删除提示「已经在回收站里：<file_id>」✓；跨桶删除无 `--yes` → `FMT-016` 且**确实没删** ✓，带 `--yes` → 成功且消息带「（Bucket：probe）」✓ |
| 回收站 | `list` 标出 `[文件]` / `[桶]`（含桶的文件数与占用）✓；`get` 按 `file_id` / 按名字 ✓；不存在 `FMT-400`（带「随桶删除请用 trash list 找桶」提示）✓；回退 ✓；同名冲突 `FMT-401` ✓；永久删除缺 `--yes` → `FMT-016` ✓、带 `--yes` ✓；随桶删除的文件单独回退 → `FMT-402`（带桶名与「`trash restore <桶的回收站名>`」提示）✓；整体恢复桶 ✓ |
| HTTP（`enabled` 手动打开后） | `GET /api/ping`、`/api/status`（含 `channel` / `pid` / `root` / `version`）、`/api/bucket`、`/api/file` ✓；`POST /api/file`（`{"path":…}`）✓、缺 `path` → 400 ✓；`POST /api/bucket`（含小写规范化 + `note`）✓、重名 → 409 ✓；`DELETE /api/file?dry_run=1` ✓、跨桶无 `force` → 400 + `FMT-016` ✓、带 `force=1` → 200 ✓；`GET /api/trash` ✓；`POST /api/trash/<名>/restore` ✓ |
| 行为确认 | ① 删除**当前** Bucket 后 `current_bucket` 被置空，之后 `file list` 报 `FMT-305 未设置当前 Bucket`（退出码 3）；`trash restore` 把桶恢复回来**不会**自动设回当前桶，需要 `bucket use`（设计如此，见 10.1、开发文档第 30/61 节）。② `FMT-301 DownloadFailed` 的退出码是 **1**（附录 A 与实现一致） |

> **实测风险 → 已被提交 `8f2fbc5` 处理（2026-10-09）**：做 `version` 测试时在**构建目录**里
> 跑了交互模式，CLI 按设计把**自己所在目录**声明为数据根，于是服务的数据根被切到了构建目录
> （日志：`[Main] 数据根切换: D:/Data/CLionProjects/FMT/cmake-build-debug/bin
> -> D:/Data/Temp/JMT/fmt`；随后又用部署目录的 exe 发了一条命令切回来）。
> 当时这件事**只进日志、控制台上看不见**，用户随后选了这一项并实现——**现在两者都上控制台**：
> 切换那一刻 stderr 打印 `root_switch_notice()`，交互窗口横幅永远显示「数据根：…」
> （11.9、13.10、18.33）。**原口径「只进日志、不刷控制台」已作废。**

---

### 18.32 `version` 命令（commit `d108c80`）

`d108c80`「feat(cli): add the version command」。**145 个用例全绿**（144 + 新增 1 条）。

```text
现象    窗口里敲 version 回答「未知命令」，而 fmt.exe --version 是可用的
        （原来只有旗标实现了）
修复    新增命令 version —— 与横幅、--version / -v **共用同一份文本**
        `cli::version_text()`（实现就是 return banner_text();），三处不可能各说一套
        （原来 only 旗标走 banner_text，命令侧根本没接）
三种用法，同一份输出（11.6）：
        fmt.exe version            一次性执行
        fmt.exe --version / -v     一次性执行（原有）
        fmt> version               窗口里执行（也接受 --version / -v）
        输出一行：File Manager Tool  v1.0  ( build  2026.10.09 )
                  （版本取 version::MAJOR.MINOR，构建日期由 CMake 配置时生成）
两个性质 ① **不需要服务在运行**（不连管道、不查服务状态、不弹 UAC）
        ② **不写任何磁盘内容**——一次性分支放在「开日志器」之前，log/fmt.log 不会
           因为敲 version 多出记录（与 --help / --version 同一条口径：
           「从这里开始都是真的干活，才值得写日志」，见 11.13）
命令总览 print_command_list() 多一行（顺序 service → bucket → file → trash → help →
        version → exit）：
          (version)  version                打印版本与构建日期
帮助   help <组> 的支持列表多了 version，且 help version 有正文（11.4，照源码抄）
用例   Cli.版本文本只有一个来源（tests/cli_test.cpp）：断言 version_text() 里含
        `File Manager Tool` / `v1.0` / `build`——**不钉具体日期**，因为构建日期是
        CMake 配置时生成的；钉住的是「三处同源」这件事
```

**实现上的两处细节（读源码时值得注意）**：① `version_text()` 必须定义在**匿名命名空间
之外**（`banner_text()` 在里面），否则声明在 `cli.hpp` 里的符号链接期找不到；
② 一次性分支紧跟在 `--version` 分支之后、**在「开日志器」之前**——这正是「不写磁盘」的
实现方式，不是巧合。

---

### 18.33 数据根被搬走时要说出来（commit `8f2fbc5` + `bb7a40f`）

**2026-10-09**，用户从上一批实测里选了这一项：「数据根被别的 `fmt.exe` 抢走时控制台上看不见」。
`8f2fbc5`「feat(cli): say out loud when the service data root moves」实现，
`bb7a40f`「test(cli): give the root notice a home and a test」把文本抽出来并加测试。
**146 个用例全绿**（145 + 新增 1 条）。

```text
背景    「数据根由 CLI 声明」意味着**任何位置的 fmt.exe 一连上就会把服务的数据根搬到自己
        目录下**——桶、文件、回收站整体换成另一个目录的内容；而这件事原来**只写一行日志**。

① 切换发生时（hello 回执 switched=true）
   位置      src/cli/cli.cpp 的 ensure_connected()，拿到 session.root_switched 之后
   输出      **stderr**（不是 stdout）打印 root_switch_notice(previous, current)：
               注意：服务的数据根已切换
                 原来：D:/Data/Temp/JMT/fmt
                 现在：D:/Data/CLionProjects/FMT/cmake-build-debug/bin
               原因是「数据根由 CLI 声明」：谁连上服务，服务就用谁的目录。
               如果这不是你想要的，请用数据根正确的那个 fmt.exe 再执行一条命令切回去。
             正斜杠（to_forward_slashes），不带反斜杠转义；同时照旧记一行日志
② 交互窗口横幅区永远多一行数据根（run_interactive，数据来自 service::load_state()）
             File Manager Tool  v1.0  ( build  2026.10.09 )
             Service Running...
             数据根：D:/Data/Temp/JMT/fmt
   不一致时   与本程序所在目录（iequals 比较）不同 → 紧跟一行
             「注意：本程序所在目录是 …，连上之后服务会切到本目录（数据根由 CLI 声明）。」
   **触发条件（如实说明）**：双击引导会**先连上服务并声明本目录**，所以服务在跑时
             两边通常已经一致；这一行主要在**服务不可用**（引导未能连上）时出现，
             用来说明「服务记录的数据根与你所在目录不同」。服务记的根为空时不打「数据根：」那一行。
③ 为什么「连接那一刻报警」就够（**设计论据**）
   服务**一次只接受一条连接**（15.1 ① 严格串行）：交互窗口握着管道时别的 CLI 连不上
   （ERROR_PIPE_BUSY → 重试 → FMT-601）。所以**「会话中途被别人把数据根搬走」不可能发生**，
   切换只可能发生在某个 CLI **连上来的那一刻**——在那一刻报警即可完全覆盖（13.10 末尾同注）。

④ 文本为什么要收进 root_switch_notice()（bb7a40f）
   原来这段文本内联在连接路径里，**没法断言**；抽成 cli.hpp 里的
   std::string root_switch_notice(const std::string& previous, const std::string& current)
   之后就能测了——**这是控制台输出第一次有测试覆盖**。别把文本再写回连接路径里。
   用例 Cli.数据根切换提示要把两个根都说清楚：断言旧根、新根、「数据根」、「切回去」都在；
   并检查旧根为空（服务首次启动）时也包含新根。

⑤ 实测（2026-10-09，对已安装服务）
   从构建目录执行 bucket list → stderr 出现
     「注意：服务的数据根已切换 / 原来：D:/Data/Temp/JMT/fmt / 现在：…/cmake-build-debug/bin」
   再用部署目录的 exe 执行    → stderr 出现反向的那一条（…/bin -> D:/Data/Temp/JMT/fmt）
   交互窗口横幅              → 多出「数据根：D:/Data/Temp/JMT/fmt」
```

---

### 18.34 `service reinstall` 正式化（commit `c573f14`）

`c573f14`「feat(cli): expose service reinstall」。**147 个用例全绿**（146 + 新增 1 条）。

```text
事实    reinstall **本来就有**——提权副本的 operation（13.8.2），双击引导内部用它
        （13.12「宿主 exe 已丢失」的修复路径）。本轮**只做接线**：
        一次 UAC 内「卸载（先停服务、解开旧 exe 的文件占用）→ 按**当前这个 exe**
        重新注册 → 启动」。

接线    src/cli/cli.cpp + include/fmt/cli/cli.hpp：
        ① is_user_service_command() 加入 reinstall —— 这个谓词原本在匿名命名空间里
           （注释还写着「reinstall 是引导流程内部使用的，不在命令集里，文档冻结的是四条」），
           现在**移到 cli.hpp 声明**，便于用例钉住；
        ② 两处用法提示都改成
             用法：service install | uninstall | start | stop | reinstall | status
           （交互循环一处、一次性 dispatch 一处）；
        ③ print_command_list() 的 service 那一行加上 reinstall；
        ④ help service 正文新增 reinstall 一整段（11.4 照源码抄）。

口径    **service 子命令是六条** install / uninstall / start / stop / reinstall / status，
        其中**除 status 外每条都提权**。
        原口径「service 只有四条命令」「文档冻结的是四条」「reinstall 只在引导流程内部
        使用」**均已作废**（保留在注记里）。

三个「不变」（13.8.4 有完整版）：
        ① 业务数据不变 —— uninstall() 只 DeleteService，不删
           repository / trash / config / data / log / temp；
        ② C:\ProgramData\FMT\service.json 不变 —— 卸载不删状态目录，current_root 保留，
           重装后数据根仍是原来那个，直到某个 CLI 连上来重新声明；
        ③ 数据根仍由 CLI 声明，与「宿主 exe 是谁」无关（11.9 的横幅、13.10 的换根提示照旧）。

为什么是「更新 exe」的正确路径：
        服务在跑时旧宿主 exe 被占用、无法覆盖；reinstall 先停旧宿主解开占用，再把注册
        指向**你运行的那一份**新 exe——**不需要先复制，也只要一次 UAC**。
        宿主 exe 被移动或删除时同理（4.8 节表格第三行 / 13.8.4）。

用例    Cli.service子命令集合（tests/cli_test.cpp）：断言 is_user_service_command() 对
        install / uninstall / start / stop / reinstall 为真；对 status、空串、Install
        （大小写不同）、乱写的词为假。**命令集合第一次有断言**——它决定「敲了什么会被
        当成什么」，所以把它从匿名命名空间搬到头文件里专门钉住。

实测（2026-10-09，对已安装服务）
        服务已停止时：service reinstall → exit 0；服务 RUNNING；宿主 = 执行它的那份 exe；
                      数据根仍是 D:/Data/Temp/JMT/fmt；桶 / 文件 / 回收站全部未变
        服务正在运行时：service reinstall → exit 0；宿主 PID 2088 → 9212（换成新进程）；
                      STATE 仍 RUNNING；业务命令正常
```

---

### 18.35 端到端冒烟测试：真实 exe 走真实管道（commit `a340d1e`）

`a340d1e`「test(cli): drive the real exe over the real pipe」。**150 个用例全绿，全量约 17 秒**
（端到端部分约 5 秒）。这是本工程**第一次让真实 `fmt.exe` 走用户走的路**。

```text
为什么要它（与 18.28 同源）
  `file delete a7.jpg` 弹「Debug Error! abort() has been called」那次崩溃，是从
  **全绿的单元测试**里漏出去的：单元测试直接调业务层、**按服务端期望的形状拼请求**，
  而 CLI 拼的是另一种形状（把 dry_run 挂在位置参数数组上 → type_error.305 → abort()）。
  让 CLI 自己造请求，是唯一能让这类 bug 现形的方法。
  而且那次崩溃的代码路径对**所有破坏性操作都生效**——所以本套件里第一个
  `file delete` 用例就会当场失败（不是靠运气覆盖到的）。

做法（tests/cli_e2e_test.cpp，套件名 CliE2e）
  ① 进程内起 ServerRuntime（用自己的管道名，见下）
  ② 把 fmt.exe **复制到临时数据根**——因为「数据根 = CLI 所在目录」，
     不复制的话真实 exe 会把服务的数据根指到自己所在目录
  ③ CreateProcessW 拉起**真实 exe**、喂 stdin、把 stdout+stderr 合并收下来
  ④ 断言**退出码**与**用户看到的文字**（而不是内部结构）

隔离（本套件能在这台机器上跑的前提）
  FMT_PIPE 覆盖管道名（13.9.1）：测试进程设好它，自己起的服务与拉起的 CLI 子进程
  都用同一个私有名字 → 与**正在运行的真服务**完全隔离，真服务的桶、文件、
  回收站一个都不动。

覆盖（用例表见 17.2 的「自动化回归测试」一节）
  version（本地命令）/ bucket create WORK（小写归一 + 提示）/ bucket list /
  file upload（本地路径）/ 同内容 FMT-304 / 同名不同内容 FMT-105 /
  file get 按名与按 id / 不存在 FMT-002 / file delete → trash list（含 [文件]）→
  trash get（含「回收站路径」）→ trash restore /
  跨桶删除无 --yes → FMT-016 且随后确认文件仍在、带 --yes → 成功且消息带「Bucket：work」/
  永久删除无 --yes → FMT-016（含「不可恢复」）、带 --yes → 成功 /
  删桶：空桶直接成功（仍打印「只能整体恢复」）、非空桶无 --yes → FMT-016 /
  **交互式确认**：喂 n → 「确认执行？」+「已取消」且文件仍在；喂 y → 不再出现「已取消」
  且条目进了回收站（此前完全没有自动化覆盖）

实测（2026-10-09，对已安装且**正在运行**的真服务）
  跑 CliE2e 前：服务运行中，数据根 D:/Data/Temp/JMT/fmt
  跑 CliE2e  ：2/2 通过
  跑 CliE2e 后：服务仍运行中、数据根不变；桶 lazy、2 个文件、回收站里 a7.jpg —— **完全未变**
  全量        ：150 项通过，17.3 秒，无残留进程与临时目录
```

---

### 18.36 日志轮转（commit `821aba3`）

`821aba3`「feat(logger): rotate the log file instead of growing for ever」。
**152 个用例全绿**（150 + 新增 2 条）。

```text
背景    fmt.log 原来**无上限追加**：服务跑几周就一直长。现在超过 5 MB 轮转成 fmt.log.1
        （**只留一代**），error.log 同理。

为什么现在才可能做到轮转
  日志原来长期持有 ofstream **✗**——CLI 与服务共用同一个文件，句柄一直开着既会挡住
  改名，也会让另一个进程继续往已改名的文件里写。现在改成**每行开-写-关**
  （append_line()），改名才有可能成功。**这是轮转的前置条件，不是顺手改的。**

检查节奏
  每写 **64 行**检查一次大小（kRotationCheckInterval）。不用时间节流：写入频率差异大，
  按行计数既便宜又确定，**测试也能预期**。改名失败（另一个进程正好在写）**不报错**，
  下一次检查再试。

轮转后
  在**新文件**里写一行说明：「日志超过 N 字节，已轮转：fmt.log -> fmt.log.1」——
  用户翻日志能看到断点。

配置    Logger::Options::max_log_bytes：**0 表示不轮转**（测试与需要完整日志的场景）；
        默认 kDefaultMaxLogBytes = 5 * 1024 * 1024（5 MB）。
打开时  仍然验一次可写（写空串）✓：日志写不了要**立刻报错**，而不是等第一条日志静默丢掉。

顺带修掉头文件两处过时说法（与 §65 / 本文档 14.2、14.3 对齐）
  ① include/fmt/common/logger.hpp 原写「**只有 Service 打开日志文件**；CLI 用
     console_only()」——**早已不是这样**：CLI 也往同一个 <数据根>/log/fmt.log 追加
     （这正是「日志跟着用户敲的命令走」那条口径）。已改。
  ② 原写「WARN 也进 error.log」——**与 §65 冲突**：§65 写的是 error.log **仅 ERROR 级**。
     作者一开始照头文件改了代码，**被既有测试当场抓住**
     （Logger.写入两个文件且ERROR单独成文件 断言 error.log 只有 1 行），
     于是改回代码、修掉头文件那张表。**口径以「仅 ERROR 级」为准**（14.3、开发文档 §65）。

用例    Logger.超过上限会轮转出一代（上限设 1 KB，写 400 行 → 断言 fmt.log.1 存在、
        两代都非空、新文件里有「轮转」说明行）；
        Logger.上限为零时不轮转（写 200 行 → 没有 .1，200 行全在一个文件里）。
```

---

### 18.37 零碎三项：`trash empty`、`file list --sort`、`config list`/`set`（commit `674d0b0`）

`674d0b0`「feat: empty the trash, sort the file list, and manage config from the window」。
**153 个用例全绿**（152 + 新增 1 条）。三件事原来都**没有 CLI 入口**。

```text
① trash empty —— 一次清空回收站的两级条目
   语义     文件级 + 桶级一次全删，永久删除、不可恢复
   形状     与其它破坏性操作一致：dry_run 预检回
            {files, buckets, bytes, needs_confirm, blocked:false, message}
            缺 force → FMT-016；CLI 侧 --yes 或窗口里答 y
   空回收站 needs_confirm = false，**直接成功返回（0 项）**，不打扰用户
   消息     预检「永久删除回收站里的全部 N 项（x 个文件、y 个桶，共 SIZE），不可恢复」
            执行「已清空回收站：x 个文件、y 个桶，共释放 SIZE」
   实现要点 ① **每删一项都重新 list 一遍**——删掉一项后其余条目的索引/路径会变，
               用旧列表接着删会大面积失败
            ② **先删文件级、再删桶级**（避免「条目没了、数据还在」的孤儿）
            ③ 单条失败**跳过并记日志**，不卡死整个清空；kMaxRounds 兜底防死循环
   HTTP     **路由没有加**（DELETE /api/trash）——HTTP 入口现在按用户决定是关闭的，
            先不加，等那批「简单接口」一起做（12.3.2）

② file list --sort name|size|id
   默认     **name**（不区分大小写；同名用 file_id 保证稳定）
   size     size 大的在前
   id       入库顺序（file_id 里的日期+序号天然递增）
   乱写     → **FMT-001**（「排序方式只能是 name / size / id，收到：<key>」），
            **不静默按默认排**
   响应     data 里新增 sort 字段，回显实际用的排序方式
   CLI      --sort 是**本地开关**（不进 argv），单独放进 args.sort
   顺带修   help file 里 delete 那行原写「文件级回收站目前只能写、还不能从 trash 查回，
            待阶段 7」——早就不是了，已改成「之后用 trash list / trash restore 找回来」

③ config list / config set max_upload_size <大小>
   list     返回 current_user / current_bucket / max_upload_size / size_unit /
            language / path；CLI 打成标签行（不吐 JSON）
   set      **只让改 max_upload_size**：其余只读 → FMT-001
            「V1 只能改 max_upload_size（其余只读）：<key>」
            理由：它是唯一需要按机器/网络调整的值；size_unit / language 目前没有
            对应行为，current_bucket 走 bucket use
   大小写法 纯字节 10485760，或 10MB / 512KB / 1GB（1KB = 1024 字节）；
            下限 1KB、上限 100GB（兜溢出）；非法值一律 FMT-001
   落盘     走 save_config；**落盘失败回滚内存里的值**并返回错误
   帮助     新增 help config（11.4 照源码抄）；命令总览加
            (trash) … empty 与 (config) list set 两行

④ FMT-203 BucketInUse：**保留（V1 未使用）**
   原本设想用于「桶仍被引用」，但实际路径分别由 FMT-401（回退冲突）、
   FMT-402（原桶已删）、FMT-016（删非空桶要确认）覆盖，**没有产生它的代码**。
   编号语义冻结、**不删行**，但也不要假装它会被返回（附录 A / 12.5 已注明）。

实测（2026-10-09，对已安装服务）
  file list --sort size        → 3.71MB 的在前、127.13KB 在后
  file list --sort nonsense    → FMT-001「排序方式只能是 name / size / id，收到：nonsense」
  config list                  → 标签行（配置文件 / 当前用户 / 当前 Bucket / 上传上限 /
                                 大小单位 / 语言）
  config set max_upload_size 50MB → 「已改为 52428800 字节（50MB）」（与原值相同，等于不改）
  config set current_user other   → FMT-001「V1 只能改 max_upload_size」
  trash empty（不带 --yes）      → FMT-016 + 「永久删除回收站里的全部 1 项
                                 （1 个文件、0 个桶，共 112.04KB），不可恢复」
  trash list                   → a7.jpg 仍在（**故意只跑预检，不动真数据**）
```

---

### 18.38 share 数据面（commit `d5779db`）

`d5779db`「feat(share): the share data plane」。**154 个用例全绿**（153 + 新增 1 条）。
Share 是最后一个「背后什么都没有」的业务模块，这一轮做的是**数据那一半**——
**HTTP 下载端点按用户决定推迟**。实现跟着开发文档第 16、46～51 节走。

```text
模块 / 存储   src/share/（share.hpp / share.cpp + CMake）；
              data/share.json = {version:1, shares:[{share_id, file_id,
              max_download_count, download_count, expire_time, is_valid}]}
              expire_time 为空 = JSON 里写 null = 不过期
share_id      **12 位随机十六进制**，来自 BCryptGenRandom —— **id 就是凭证**，
              可预测的 id 会让任何人猜出链接（不是 std::mt19937 ✗）
              撞号重摇（最多 16 次）；生成失败当错误返回
默认值        max_download_count = 20、expire_time = 现在 + 7 天（两个条件相互独立）
share get     如实报状态：未知 id 才是真错误 FMT-500（退出码 3）；
              「已过期 / 次数用尽 / 已撤销 / 关联文件在回收站」都是成功 + 状态
              （state / available / message），不伪装成不存在
              检查顺序：有效性 → 文件存在 → 不在回收站 → 过期 → 次数（按开发文档 §49；
              §50 的流程图把次数写在过期前面，以 §49 为准）
center 约束   create 要求文件属当前用户且不在回收站（否则 FMT-503）；
              文件进回收站 → 它的 share 立刻不可用（FMT-503）+ 阻断新的 create（§50）
share.download**新 op**（管道可用；将来的 HTTP 下载端点也走它）：
              register_download() 在**业务锁内**一次完成「全部检查 + 计数 +1 + 落盘」，
              所以 §51 的「19 + 两次 = 21」不可能发生；计数写不进去就**拒绝这次下载**
CLI           四条命令可用；打印分支**排在「单条文件信息」之前**（分享数据里也带
              file_id 与 size，否则会被当成文件详情）；命令总览里 share 从
              「服务端尚未实现」移进「可用命令」；help share 换成 11.4 那份
HTTP          **一个路由都没加**（用户决定）——等用户定开放哪几个接口（12.3.2）
另：两个原本拿 share.list / share.create 当「尚未实现」例子的旧用例，改成用
    **server.nosuch**（server.* 是唯一还剩的空壳）

实测（2026-10-09，对已安装服务）
  file get asdva.jpg          → fmt-20261008-0
  share create fmt-20261008-0 → share_id eaaecc6869ab，0/20，到期 2026-10-16T20:24:52
  share get eaaecc6869ab      → 状态：可用
  share list fmt-20261008-0   → 「eaaecc6869ab  0/20  可用  到期 …」共 1 条
  share delete eaaecc6869ab   → 已撤销
  share get eaaecc6869ab      → FMT-500 分享不存在（退出码 3）
  file list / trash list      → 与测试前一致（真实文件与回收站未受影响）
```

---

### 18.39 HTTP 接口（第 1、2 步）+ 账号/令牌 + localhost（提交 `bfd89f7` / `4b812b5` / `ff237d5` / `22c3c3e`）

四个提交：`bfd89f7` 账号存储、`4b812b5` 认证与路由、`ff237d5` 测试隔离修复、
`22c3c3e` localhost + 开启 HTTP。**156 个用例全绿**（154 + 新增 3 条，另一条改期望值）。

```text
① HTTP 已开启，监听 localhost:4122（提交 22c3c3e）
   ServerConfig::host 的**代码默认值从 127.0.0.1 改成 localhost**（用户明确要求写
   localhost）；线上 config/server.json 改成 enabled: true, host: "localhost"，
   真机日志「HTTP 监听 localhost:4122」。
   **仍是缺口**：安装流程不会自动打开 enabled（那句「由安装流程置为 true」一直没实现），
   全新数据根装完服务后 HTTP 仍是关的，要手改配置（12.1、12.2、19.1）。

② 认证（提交 4b812b5）：FMT-018 Unauthorized → 401、退出码 5
   范围      **所有 /api/* 都要 token**；唯一例外是 **/api/ping**（健康检查）与
             **/api/share/<id>/download**（别人拿分享链接下载——**分享链接本身就是凭证**，
             这是用户选的方案 B）
   放哪      **只在一处**：httplib 的 **pre-routing 钩子**（set_pre_routing_handler）。
             **为什么**：「漏给某条路由加认证」是这类代码最典型的事故，所以不逐个路由判断；
             一处集中检查，新增路由不可能忘
   fail-closed  **没有注入校验器时一律 401**（默认关着）——宁可全拒，也不放行
   请求头    X-FMT-Token: <token> 与 Authorization: Bearer <token> 都接受
   比较      常量时间（token 与密码哈希都是），避免时序侧信道
   token 从哪拿  **config list** 打印「用户 ID / 访问 token / 建议的请求头」——
             机主唯一方便拿到它的地方（V1 没有登录接口）

③ 桶的 HTTP 接口已全部删除（提交 4b812b5，用户明确「桶不要」）
   /api/bucket* → **404 + FMT-017**（**不是 501**：故意不要，不是还没做）
   「已知模块」列表因此是 **file / trash / share / config / server / preview**，
   **bucket 不在其中**；桶继续由 CLI 管，HTTP 客户端操作的是**当前桶**。

④ 分享路由（管理接口都要 token）
   POST   /api/share            请求体 {"file_id": "fmt-20261009-0"}
   GET    /api/share/<share_id>
   GET    /api/share?file_id=…
   DELETE /api/share/<share_id>
   GET    /api/share/<id>/download   公开（不要 token）；**流式下载第 3 步才做**，
                                     现在 501 + FMT-602

⑤ data/user.json 的最终 schema（**没有桶列表**，提交 bfd89f7）
   { "version": 1, "users": [ { "user_id": "u-b1e7c28f", "username": "user",
     "password_hash": "<PBKDF2-SHA256，64 位十六进制>", "password_salt": "<16 字节随机盐>",
     "token": "<32 位十六进制，永久有效>", "current_bucket": "lazy",
     "created_at": "…", "updated_at": "…", "last_login_at": "" } ] }
   **buckets 字段已删除**：桶以磁盘上的 repository/<user>/<bucket>/ 为准；
   读的时候忽略老字段，**下一次初始化会把老文件里的它清掉**（线上已验证清掉了）。
   current_bucket 保留（以后可能有用），但只是**快照**——运行时权威仍是 config.json。
   密码只存哈希（PBKDF2-SHA256 **10 万轮** + 每用户盐），初始密码不落地明文；
   V1 没有登录接口，**真正的凭证是 token**。
   默认账号在**数据根初始化**时创建（initialize_root）：新根会建；
   **已经是 {"users":[],"version":1} 的老根也会补建**（线上就是这种情况）；
   已存在则**绝不覆盖**。

⑥ ⚠ 真事故与防护：FMT_NO_SERVICE（提交 ff237d5）
   端到端交互测试用**无参数**跑真的 fmt.exe → 那是**双击引导** → 引导会装/启动/
   **重装真实的 Windows 服务**。**FMT_PIPE 只隔离管道，管不到 SCM ✗**：测试把服务注册
   指向了自己的临时目录，测试结束目录被清理 → **线上服务指向不存在的文件、
   部署的 fmt.exe 也没了**（已修复，reinstall 指回真路径）。
   防护：新增 **FMT_NO_SERVICE=1**（测试专用），引导函数开头看到它就**完全不碰服务管理**；
   端到端夹具为每个子进程都设上它。
   **规则**：凡是要跑「用户双击也会走的入口」，必须显式切断它对 **SCM / 注册表 /
   ProgramData** 的写入能力——只换管道名不够（13.9.1、18.35）。

⑦ 另一条工程教训：httplib::Client 不能按值返回
   测试辅助函数里 `return client;` 让第一条 Server 测试**栈溢出**
   （0xC00000FD）；改成就地构造即好（13.9.2 的陷阱清单同注）。

⑧ 真机验收记录（2026-10-09 21:29，localhost:4122）
   日志：HTTP 监听 localhost:4122
   /api/ping            无 token → 200
   /api/file            无 token → 401 FMT-018；带 token → 200
                        （files 里只有文件信息，**已无 current_bucket**）
   /api/bucket          带 token → 404 FMT-017
   /api/trash           带 token → 200；无 token → 401
   POST /api/share      → 200，share_id 3d92277fc240，0/20，到期 7 天后
   GET  /api/share/<id> → 200，state「可用」
   GET  /api/share?file_id=… → 200，count 1
   GET  /api/share/<id>/download → 501 FMT-602（公开但第 3 步才做）
   DELETE /api/share/<id> → 200；再 GET → 404 FMT-500
   user.json → 已无 buckets 字段，current_bucket 保留
```

---

### 18.40 HTTP 第 3 步：流式 upload / download / preview + 公开分享下载（commit `d3aeb3d`）

`d3aeb3d`「feat(http): stream uploads, downloads and previews」。**HTTP 接口的最后一部分**，
**156 个用例仍全绿**（流式断言加进既有 `Server.File路由与上传`，用例数不变）。

```text
① 路由表（最终形态，见 12.3.2）
   POST   /api/file/upload            请求体**就是文件内容**（流式）；
                                      文件名来自 ?name= 或 Content-Disposition
   GET    /api/file                   列出文件（**只有文件信息**）
   GET    /api/file/<id|名字>         get 单个信息（响应含相对数据根的 path）
   GET    /api/file/<id|名字>/download  下载（attachment，流式）
   GET    /api/file/<id|名字>/preview   预览（inline，流式）
   DELETE /api/file/<id|名字>         软删除（?dry_run=1 / ?force=1）
   GET    /api/trash                  列表
   GET    /api/trash/<标识>           get
   DELETE /api/trash/<标识>           **永久删除**（?dry_run=1 / ?force=1）
   POST   /api/trash/<标识>/restore   回退
   POST   /api/share                  {"file_id": "…"}
   GET    /api/share/<share_id>
   GET    /api/share?file_id=…
   DELETE /api/share/<share_id>
   公开（唯一不要 token）：GET /api/share/<share_id>/download
     —— 分享链接本身就是凭证；**必须先记账再放行**（第 50/51 节在这里生效，
        计数写不进去就不下载，避免超发）

② 流式上传的要点
   请求体就是文件内容（Java / Python 客户端直接推字节流）。
   **旧的「请求体给服务端本地路径」那套已删除**——对远端客户端没有意义。
   边收边写、边判上限：超过 max_upload_size **立刻中止接收并删掉暂存文件**
     （FMT-303 → 400），不是「写完再看」。
   暂存文件名 fmt-upload-<pid>-<序号>.tmp 放在 temp/ 下：**沿用 fmt- 前缀**，
     所以服务启动时的清理会收走中断留下的碎片（有 10 分钟年龄保护）。
   落盘后交给业务层补算大小与 MD5 并入库：**全程只有一次移动，不二次拷贝**
     （这正是大文件走流式上传的意义）；失败路径一律删暂存文件。
   文件名：?name=xxx，或 Content-Disposition: attachment; filename="x.jar"
     （也支持 filename*=UTF-8''… 百分号编码）；**没给文件名 → FMT-100（400）**。
   链路多了一个业务 op：**file.upload_stream**（argv = 暂存路径 + 文件名）；
     管道也能调，但它是给 HTTP 流式上传用的。
   新增函数：prepare_staged_upload(paths, staged, name, size_limit, logger)、
     content_type_of(file_name)、preview_content_type(file_type, file_name)。

③ 下载 / 预览
   都用 httplib 的 set_content_provider 流式回，不把整个文件读进内存。
   **下载不受预览策略限制**：任何类型都能下载；Content-Type 猜不出来就给
     application/octet-stream；Content-Disposition: attachment;
     filename*=UTF-8''<百分号编码>（中文名任何客户端都能正确落地）。
   **预览策略只有一份**，在文件模块 preview_content_type() 里：
     file_type == "image" → 按扩展名给 image/*；
     文本类（.txt/.md/.json/.csv/.log/.xml）→ 对应 MIME；
     **其余一律 FMT-701（400，可下载但不可预览）**——HTTP 与将来的其它入口共用。

④ 数据字段
   file.get 与 share.download 的响应新增 **path**（**相对数据根**，例如
     repository/user/lazy/2026/10/09/x.txt），HTTP 层按它定位文件；
     **file.list 不加**（用户要求列表只输出文件信息）。

⑤ 两个真实 bug（都进「陷阱」清单）
   ① **ContentReader 型处理器早退不读请求体 → httplib 直接断开连接**：
      客户端拿到的是「没有响应」，而不是我们精心写的错误码
      （没给文件名本该回 FMT-100）。修法：早退前先 `drain_reader()` 把体读干净。
   ② **下载误用了预览策略**：一开始下载与预览共用一个分支，结果 `.bin` 的**下载**
      被 FMT-701 挡掉。下载与预览是两件事，**下载不受预览策略限制**。

⑥ 测试（仍 156 项，全绿）
   流式断言加在既有 `Server.File路由与上传` 里（该用例上限传 4096）：
     没给文件名 → FMT-100；与 file_id 同形的名字 → FMT-106；
     **10KB 上传 → FMT-303 且 temp/ 里不留 fmt-upload-* 碎片**；
     100 字节上传成功；**下载字节与原文件逐字节相同**且 Content-Disposition 是
     attachment；`.bin` 预览 → FMT-701。

⑦ 真机验收（localhost:4122）
   POST /api/file/upload?name=live-probe.txt（体 525 字节）→ 200，md5 3f39d8bc…，size 525
   GET  /api/file/fmt-20261009-0/download                 → 525 字节，逐字节一致
   GET  /api/file/fmt-20261009-0/preview                  → 200，text/plain; charset=utf-8，inline
   POST /api/share {"file_id":"fmt-20261009-0"}           → share_id 0d7b3cf4bc2e
   GET  /api/share/0d7b3cf4bc2e/download  **不带 token**  → 200，且 download_count 变成 1
   清理：撤销分享 + 软删除 + 永久删除；file list 仍 2 个文件、trash list 仍 1 项、
        temp/ 无残留
```

---

## 19. 待决事项

### 19.1 本次重构引入的待决事项

| 事项 | 说明 |
|---|---|
| `init` 操作是否需要显式命令 | 当前设计是「CLI 双击时先自己跑一次 `check_root`（11.13），连上后再声明 root，服务用同一套规则幂等初始化」（13.10），没有独立的 `init` 命令。是否需要一条显式的「初始化这个数据根」命令待定；不加也不影响主链路 |
| 数据根由 CLI 声明 vs 架构文档 | 已对齐：`FMT 项目架构.md` 与本文档都写 **CLI 声明、服务持有**，并明确 CLI 双击时对自身数据根做同一套幂等体检与补齐（第 3.1 / 3.2 节 ↔ 4.4 / 11.13）。后续若再出现「`FMT_ROOT` = `fmt.exe` 所在目录」的旧表述，以冻结决策为准 |
| 两个不同的数据根 | CLI 所在目录（声明值）与服务宿主 exe 所在目录（无 CLI 连接时的回退值）可能不同。是否需要一条「服务启动后马上规范化为记录值」的规则，避免「服务自启 → 用宿主目录 → 第一个 CLI 连上 → 又切一次」的抖动，待定。注意这种切换现在会**显式可见**：hello 回执 `switched=true` 且 CLI **在 stderr 打印换根提示**（提交 `8f2fbc5`）并记一行日志「数据根切换：旧 -> 新」；横幅另常驻一行「数据根：…」（11.9） |
| 多 CLI 窗口 | 单实例（11.11）保证同时只有一个数据根，因此根切换不需要按连接隔离。若将来放开多窗口，需要重新设计「一个当前根」的语义 |
| `service.json` 的字段集 | 现定 `version` / `current_root` / `binary_path` / `installed_at`。是否需要记录「上次正常停止时间」等诊断字段待定 |
| `--elevated` 的参数形状 | **已定稿**：`fmt.exe --elevated <operation> --result "<结果文件绝对路径>"`，`operation ∈ install / uninstall / start / stop / reinstall`（13.8.2），**`status` 不在其中**（它不提权，见 13.4.1）。**提交 `c573f14` 起这五个 op 里除 `status` 外全部对用户可见**（`reinstall` 从「引导流程内部用法」提升为正式命令）：用户能敲的 service 子命令 = 这五个 + `status` = **六条**（13.8.2、13.8.4）。是否再为某个 op 携带附加参数（如 install 时指定 root）仍待定 |
| 管道 `op` 的完整清单 | **阶段 4 已冻结 `bucket.*`**：`bucket.create` / `bucket.list` / `bucket.get` / `bucket.use` / `bucket.delete`，位置参数统一放 `args.argv`（13.9.3、12.3.2.1）。**桶级 `trash.*` 也已冻结**：`trash.list` / `trash.get` / `trash.restore` / `trash.delete`（`get` / `delete` 提交 `4fee290`；`trash.delete` 额外要求 `args.force == true`，18.18）。**提交 `188e85d` 冻结 `file.*` 四种**：`file.upload`（`args.argv = [来源]` 或 `[来源, 文件名]`）、`file.list`（无参数）、`file.get`（`[file_id 或 文件名]`）、`file.delete`（`[file_id 或 文件名]`——提交 `0ad9efc` 起与 `get` 同形，**不需要 `force`**），18.19、18.21。仍待定的只剩 `share.*` 与 `config.*`。`hello` 的**响应**字段已定：`root` + `switched` +（切换时）`previous_root` |
| 长耗时命令的超时值 | **已定并落地（提交 `a2b6cd1`，从「未决」改为「已知边界 + 现有缓解」）**：普通命令仍是 `ipc::kCommandTimeoutMs = 30000`；**`file.upload` 单独用 `ipc::kUploadTimeoutMs = 30 * 60 * 1000`（30 分钟）**，CLI 侧 `run_business_command()` 按 `operation == "file.upload"` 选值（`src/cli/cli.cpp:651`）。CLI 等待超时时会额外打印：「提示：等待服务响应超时。服务端可能仍在处理，稍后用 file list 确认；也可以查看 log/fmt.log。」**残余风险（如实保留）**：30 分钟上限本身仍会出现「用户看到超时、服务端其实还在下载甚至已经提交入库」，只是窗口从 30 秒拉长到 30 分钟且**这一次用户被告知了**。彻底的解法仍是进度/长任务语义（18.16 B 的完整形态），V1 不做 |

| 阶段 5 的并发模型（锁粒度） | **已定，从待决清单移出**（提交 `188e85d`）：选的是**「把长任务移出锁」的简化形态——长任务不持锁**，没有收细锁粒度、也没有任务登记。上传拆两段：`prepare_upload()` 在锁外下载/复制到 `temp/`（边写边算 MD5、边判大小上限），`commit_upload()` 在锁内完成去重/重名/file_id/搬文件/写 `file.json`；运行体 `ServerRuntime::run_upload()` 编排，管道与 HTTP 共用。因此「上传时 `bucket list` 卡住」不再是事实（15.1 ④、15.2、15.3、18.16、18.19）。**仍然只有一把 `ServerRuntime::mutex_`**：`file_mutex` / `id_mutex` 那些细分锁仍未引入，等真的需要「上传与下载并发」时再说 |
| 文件级 Trash 的读取侧 | **已关闭（提交 `0fc242b`，见 18.25）**：`src/trash/` + `TrashService` 把两级合成一份视图，**文件级 = `file.json` 是权威**（新增 `deleted_at`），`data/trash.json` 改成只读兼容。`trash list` / `get` / `restore` / `delete` 两级都能用，`list` 标出 `[文件]` / `[桶]`；回退的三种「确认解决不了」情况（同名冲突 / 随桶删除 / 数据缺失）是 `blocked`。原口径「文件级 list/get/restore/delete 属阶段 7」**已作废** |
| 名字长得像 `file_id` 时会被「先查 id」遮住 | **已定稿，从待决清单移出（提交 `9c3d2cb`，见 18.23）**：两条可选做法**都做了**——① 上传时按保留形状拒绝形如 `fmt-YYYYMMDD-N` 的文件名（新错误码 `FMT-106 FileNameLikeFileId`，退出码 2，属 `FMT-1xx` 文件名校验；判定 `looks_like_file_id()` 见 9.1 第 6 条与 9.3；**显式名与从来源推断的名字走同一道校验**）；② 旧数据里已经存在的这种名字，`file delete` 在两个索引命中**不同**记录时报 `FMT-001`「有歧义：… 请直接用 file_id 指定要删哪一个」并点名两条记录。**只给 `file delete` 加**（`locate_record()` 只被 `remove()` 用）；`file get` 的两种查询范围保持不变（按 `file_id` 全局含回收站、按文件名只查当前用户的正常文件），这是**有意的差异**——get 只读，最坏是把 id 命中的那条给用户看；delete 破坏性，不能猜。另：`file get` 命中回收站记录时增加 `trash_path`（10.2.4、12.3.2.1） |
| 按名字删除可能删到别的 Bucket 的文件 | **已定稿，从待决清单移出（提交 `711da4c`，见 18.24）**：**跨桶要确认**，不是静默执行，也不改成「按名字只在当前桶里找」（那会把跨桶同名重新变成「文件不存在」——正是 `0ad9efc` / `5bf2c1f` 修掉的误导诊断）。做法：CLI 先发只读预检（`file.delete` + `dry_run`）打印「属于哪个桶、当前是哪个桶」→ 交互 `y/N` 或一次性 `--yes` → 服务端缺 `force` 返回 **`FMT-016 ConfirmRequired`**（退出码 2）→ 成功后 `message` 与 `data.bucket` 都带归属 |
| `bucket use` 的拼写要不要规范化 | **已定稿，从待决清单移出（提交 `9c3d2cb`，见 10.1、18.23）**：**落盘时规范化成磁盘上的实际名字**。新增私有 `BucketService::canonical_name()`——不区分大小写地扫 `repository/<user>/` 下的目录、返回磁盘上的实际拼写，找不到才退回 `to_lower()`；`use` 存进 `current_bucket` 的就是它（`use WORK` 存 `work`），`get` 返回的 `name`、`delete` 的回收站目录名与 `.original` 里的 `original`、`file.json` 里按桶名匹配的记录同样用它——`restore` 拼出来的目录名因此与原来一致。`create` 另在**建目录前**先 `to_lower()`（`WORK` 建成 `work`，`renamed` 为真时回 `note` 让 CLI 提示用户）。`iequals()` 的实现同时收紧成**只折叠 ASCII**（9.4） |
| 停止等待与 Recovery 的相互作用 | 停止过程中若超时（13.7.3），是否返回非零退出码让 SCM 记录一次失败、甚至触发 Recovery，待定——不处理好会出现「停止失败 → 自动重启」的循环 |
| 管道实例数量与线程模型 | **阶段 4 的实况已改**：管道用 `PIPE_UNLIMITED_INSTANCES` 创建，但服务端**一次只 accept 一条连接**、连接内一问答（严格串行，15.1 ①）——**没有「每连接一线程」**，所以不存在「被本机进程耗尽线程」的问题。真正要定的是**第二个客户端的行为**：现在它拿到 `ERROR_PIPE_BUSY`、等 3 秒重试、最终 `FMT-601`（「同一时刻只有一个 CLI 窗口」是有意为之，决策 10）。是否放开多连接（改成线程池或固定工作线程数）待定，与 18.16 的锁粒度一起考虑 |
| 提权副本的结果通道 | **已定稿，从待决清单移出**：结果经结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 回传（13.8.3），数据根不可写时退回 `%TEMP%` 同名文件。命名管道方案作废，原因是 MIC「禁止向上写」（13.9.2 坑 2），不存在「管道 + 临时文件降级」两条路 |
| `version` 字段与兼容 | `service.json` 沿用「未知版本直接拒绝」的策略，还是允许忽略未知字段待定 |
| 双击引导「等待落定」的具体时长 | **已定稿，从待决清单移出**：不写死时长，改为**按 SCM 的 `dwWaitHint` 自适应**——每轮查询把 `dwWaitHint` 夹在 100 ms – 2000 ms 之间作为下次间隔，兜底上限 30 秒，状态一旦不是等待类就立即结束（13.4.3）。理由是写死 8 秒在慢机器上会把「还在启动」误判成「启动失败」，白弹一次解决不了问题的 UAC 重装。判定用的错误码集合也已冻结（13.2.3）。「仍没起」时是否再多试一次 `reinstall` 仍待实测后定 |
| `service status` 输出是否要机器可读格式 | **已定稿，从待决清单移出**：**V1 不做 `--json`，也不预留参数名**。机器可读通道是**命令退出码**（`0` 成功；未安装 `FMT-601` → `8`；查询失败 → `8`），人类可读通道是那几行文本（11.6、13.4.1）。需要结构化字段（如 `wait_hint_ms`）时再加 |
| **`server.json` 的 `enabled` 仍由安装流程负责（缺口收窄，2026-10-09）** | **提交 `22c3c3e` 起**：代码默认 `host` 从 `127.0.0.1` 改成 **`localhost`**，线上 `config/server.json` 已 `enabled: true` 并真机验收（「HTTP 监听 localhost:4122」，接口全通；18.39）。**仍是缺口**：`ServerConfig::enabled` 代码默认仍是 `false`，而**全仓库没有任何代码把它置为 `true`**（`git grep 'enabled = true'` 在 `src/` 零命中；唯一一次是 `tests/config_test.cpp`），安装流程不碰 `config/server.json`，CLI 也没有命令能开 → **全新数据根**装完服务后不会监听，要手改配置。**可选做法（未定，等用户拍）**：① 让安装流程按 5.2 的原文把 `enabled` 置为 `true`；② 加一条 CLI 命令（如 `config http on` / `config set enabled true`）显式打开——**提交 `674d0b0` 起 `config list/set` 已实现**（但 `set` 只让改 `max_upload_size`），做法 ② 已经近在手边。见 5.2、12.1、12.2、18.37、18.39 |
| **HTTP 第 3 步已完成，接口层面没有待决项** | 提交 `d3aeb3d` 落地了上传（流式）、下载、预览与**公开分享下载**（`GET /api/share/<id>/download`，先记账再放行）——12.3.2 那张表就是最终形态（18.40）。**仍未开**：`/api/config` 两条、`DELETE /api/trash`（清空）；CLI 仍没有 `preview` 子命令。见 12.3.2、18.40 |

### 19.2 上一次实现遗留的待决事项（仍然有效）

| 事项 | 说明 |
|---|---|
| 业务处理函数装配 | 业务模块在装配点注册（旧为 `server::make_default_handlers`）。新版需同时给**管道 op 表**与 **HTTP 路由**注册，两处必须指向同一实现。**阶段 4 已按此落地**：`service::execute_business()` 是唯一的 `op → 实现` 分发点（`src/service/commands.cpp`），管道路由在 `ServerRuntime::handle()`、HTTP 在 `register_business_routes()`，两者都调它 |
| 首次使用的用户与 Bucket | 开发文档第 93 节旧文要求「提示用户先设置」；**阶段 4 已按需求原文落地**：`current_user` 为空时由 `initialize_root` 自动补占位名 **`user`**（不是旧记录的 `default`），随即写回 `config.json`，用户第一条命令就能成功。**Bucket 不再自动创建**：`current_bucket` 为空时由用户的 `bucket create` 决定（第一个桶自动成为当前，第 28 节），文件类命令才返回 `FMT-305`。`FMT-604` 只在用户被显式清空时出现 |
| `file delete` 与 Bucket 删除的元数据标记 | 两处必须共用同一套「移入 Trash + 置 `is_trash` + 写 `trash_reason`」逻辑。**Bucket 侧已实现**（`BucketService::set_bucket_files_trash_flag`，只动 `user`+`bucket` 匹配、`is_trash` 原本为 false 的记录，写 `trash_reason="bucket"`；回退时只翻回 `"bucket"` 的那些）；`file delete` 落地时写 `trash_reason="file"` 并复用同一判定（7.1、18.17） |
| 大小显示 | `size_unit` 默认 `MB`，于是 27 字节显示成 `0MB`。是否改成默认 `AUTO` 待定（`common/size` 已支持 `AUTO`） |
| 日志轮转 | **已实现，从待决清单移出（提交 `821aba3`，见 14.6、18.36）**：`fmt.log` 超过 **5 MB** 轮转成 `fmt.log.1`（**只留一代**），`error.log` 同理；**0 表示不轮转**（`Logger::Options::max_log_bytes`）。原口径「V1 不做轮转；单文件上限与轮转规则留待后续」**已作废** |
| Bucket Trash 元数据结构 | **阶段 4 已定稿（形状随后在 `c2d545d` 改为 `.original`），从待决清单移出**：权威是 `trash/<user>/.original` 的 `{version:1,buckets:[{trashed,original,deleted_at}]}`，**没有 `file_id`**；回收站目录名一律带删除时间戳（同秒冲突加 `_2`）；`data/trash.json` 不再记桶级条目（旧的 `type:"bucket"` 作废），它只服务阶段 5 起的文件级条目（7.3.2、18.17） |
| 最大上传大小默认值 | 现取 50 MB，硬编码在 `file/service.cpp` 的 `kMaxUploadSize`，尚未接到 `config.max_upload_size` |
| HTTPS（**本行已作废，见下方更正**） | ~~需要 OpenSSL，会引入 DLL，V1 不支持；`file upload <https://...>` 返回 `UrlInvalid`~~ |
| HTTPS 更正（提交 `a2b6cd1`） | **https 支持，且不用 OpenSSL、不分发任何 DLL**：下载走 `common/http_client` 的 **WinHTTP + Schannel**（TLS 用系统证书库，自动使用系统代理），`https://…` 是合法上传来源。原文「会引入 DLL」的说法**不准确**——WinHTTP 是系统组件，`/MT` 静态 CRT 下也不需要额外分发任何东西。**已从各处限制清单删除**（1.2、10.2.2、18.20） |
| 旧 `paths::` 自由函数 | 仍保留 `executable_path` / `root` 等自由函数供启动阶段使用，与 `PathManager` 并存；后续可考虑收敛，但 `root()` 必须保留（它要先于 PathManager 存在） |
| `text::join` 的使用纪律 | 凡是用用户输入拼接路径，必须经 `common/text`。这条纪律目前靠代码审查保证，没有编译期强制 |
| `AppContext` 的指针持有纪律 | `paths` 与 `logger` 都必须 `unique_ptr` 持有，因为业务服务保存它们的引用。这条约束靠注释说明，没有编译期强制——改动 `AppContext` 成员时容易踩回去 |
| 自动化回归测试 | **口径已变**：本分支阶段 2 起有了一套**自研最小运行器**（`tests/fmt_test.hpp`，`FMT_TEST` / `FMT_CHECK_EQ`，零第三方依赖、可离线构建，`add_test(NAME fmt_tests …)` 接进 CTest），当前 **152 个用例、全绿**（上一轮 118 个，
提交 `a2b6cd1` 又新增 6 条 `HttpClient.*`，见 17.2、17.2.1、18.18、18.19、18.20；
提交 `0ad9efc` 再新增 2 条 `File.*`，见 18.21；提交 `5bf2c1f` 再新增 2 条 `File.*`
与 1 条 `Bucket.*`，见 18.22；提交 `9c3d2cb` 再新增 5 条，见 18.23；
提交 `711da4c` + `6a40742` 再新增 2 条并把若干既有用例改成新形状，见 18.24；
提交 `0fc242b` 再新增 4 条 `Trash.*`，见 18.25；提交 `18f16ca` 只修代码、
不新增用例（140 不变），见 18.26；提交 `a9af276` 再新增 2 条
（`String.清理粘贴带进来的路径污染`、`File.粘贴路径里的不可见字符会被清掉`），见 18.27；
提交 `2c841c8` 新增 `Cli.位置参数的信封形状`（143），见 18.28；
提交 `5b316b3` 新增 `Storage.两个写者同时写同一个文件不会互相踩` 并把
`Service.启动时清理temp里的遗留临时文件` 改成「新的留着、旧的清掉」（144），见 18.29；
提交 `8f0fd5c` 只补断言（144 不变），见 18.30；提交 `4ddb515` 只改实现与既有用例
（144 不变），见 18.31；提交 `d108c80` 新增 `Cli.版本文本只有一个来源`（145），见 18.32；
提交 `bb7a40f` 新增 `Cli.数据根切换提示要把两个根都说清楚`（146），见 18.33；
提交 `c573f14` 新增 `Cli.service子命令集合`（147），见 18.34；
提交 `a340d1e` 新增 3 条（**150**：`CliE2e.核心链路走真实exe与真实管道`、
`CliE2e.交互式确认答n不删答y才删`〔新文件 `tests/cli_e2e_test.cpp`〕、
`Ipc.管道名可被FMT_PIPE覆盖`），见 18.35；
提交 `821aba3` 新增 2 条 `Logger.*`（152），见 18.36；
提交 `674d0b0` 新增 `Service.列表排序配置与清空回收站`（**153**），见 18.37；
提交 `d5779db` 新增 `Service.分享的创建查看列出撤销与计数`（154），见 18.38；
提交 `bfd89f7` / `4b812b5` 新增 3 条（**156**：`Server.管理接口要token且桶路由已下线`、
`App.初始化数据根会建默认账号与token`、`App.已有空users文件时也要补建默认账号`；
`Config.服务配置默认值` 的期望值改成 `localhost`），见 18.39；
提交 `d3aeb3d` 落地流式 upload / download / preview 与公开分享下载（**156 项不变**，
流式断言加进既有 `Server.File路由与上传`），见 18.40）。
**端到端夹具的隔离要求（提交 `ff237d5`，真事故）**：除 `FMT_PIPE` 外，每个子进程还必须设
**`FMT_NO_SERVICE=1`**——无参数跑真 `fmt.exe` 会走**双击引导**，而引导会装/启动/重装
**真实服务**，`FMT_PIPE` 管不到 SCM（详见下面的「测试隔离的硬教训」与 18.39）。
**端到端冒烟测试（提交 `a340d1e`，`tests/cli_e2e_test.cpp`，套件名 `CliE2e`）**：
进程内起 `ServerRuntime`（用 `FMT_PIPE` 私有管道名，13.9.1）→ 把 `fmt.exe` **复制到临时数据根**
（「数据根 = CLI 所在目录」）→ `CreateProcessW` 拉起**真实 exe**、喂 stdin、合并收
stdout+stderr → 断言**退出码与用户看到的文字**。两条用例的覆盖面：
`CliE2e.核心链路走真实exe与真实管道`（version / `bucket create WORK` 小写归一 + 提示 / list /
upload / `FMT-304` / `FMT-105` / `file get` 按名与按 id / `FMT-002` / `file delete` →
`trash list`（含 `[文件]`）→ `trash get`（含「回收站路径」）→ `trash restore` /
跨桶删除无 `--yes` → `FMT-016` 且**文件仍在**、带 `--yes` → 成功且消息带「Bucket：work」/
永久删除无 `--yes` → `FMT-016`（含「不可恢复」）、带 `--yes` → 成功 /
删桶：空桶直接成功（仍打印「只能整体恢复」）、非空桶无 `--yes` → `FMT-016`）、
`CliE2e.交互式确认答n不删答y才删`（喂 `n` → 「确认执行？」+「已取消」且文件仍在；
喂 `y` → 不再出现「已取消」且条目进了回收站）。**为什么必须有它**：`file delete` 弹
`abort()` 那次崩溃，**单元测试全绿却没挡住**——测试直接调业务层、自己拼请求形状，
而 CLI 拼的是另一种形状；**只有真实 exe 走一遍用户走的路，这类回归才会当场露出来**
（那次崩溃对**所有破坏性操作都生效**，所以本套件第一个 `file delete` 用例就会失败）。
**耗时**：全量约 **17 秒**（端到端部分约 5 秒）——它会拉起子进程，比纯单元测试慢，
但仍在十几秒量级。
**仍然没有的**是「上传 →
列表 → 下载 → 分享」这条**跨模块链路**的完整自动化：下载与分享两段仍靠端到端实测
（`a340d1e` 已把「上传 → 列表 → 查询 → 软删除 → 回收站 → 确认」这段补上了；
`188e85d` 那两条端到端用例是**进程内**调用的，不是真实 exe） |

