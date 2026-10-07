# FMT 技术文档

> 项目：FMT（Windows 文件管理系统）
> 版本：V1
> 文档状态：**已按 2026-10 架构重构同步**（单一 `fmt.exe` 三形态、CLI 走命名管道、
> service 五命令 + 四条动作命令 UAC 提权（`status` 查询不提权）、数据根由 CLI 声明、
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
> | `FMT 重构设计.md` | 本次重构的冻结决策、通道设计、阶段拆分、错误码补充表 |
> | **本文档** | **技术实现**：技术栈、接口设计、实现方式、类与函数、协议细节、落地步骤 |
>
> 与开发文档或架构文档冲突时以它们为准；本文档只补实现细节，不另立规范。
> 本次重构涉及的决策以 `FMT 重构设计.md` 第 2 节「冻结决策一览」为准。
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
| cpp-httplib | 0.18.3 | HTTP 服务端（浏览器侧）**与** URL 下载客户端 | `third_party/cpp-httplib/httplib.h` |

引用形式（包含路径以 `third_party/` 为根）：

```cpp
#include <nlohmann/json.hpp>
#include <cpp-httplib/httplib.h>
```

`cpp-httplib` 提供 `httplib::Server`（Service 侧监听 `127.0.0.1:4122`）与
`httplib::Client`（`file upload <url>` 时的服务端下载）。Windows 下需链接
`ws2_32`（已在 `third_party/CMakeLists.txt` 中处理）。

> **CLI 不走 HTTP。** CLI 与 Service 之间的命令通道是**命名管道** `\\.\pipe\fmt.control`
> （见 13.9 / 11.1），`httplib::Client` 只用于下载远程 URL。
> 早期设计的「CLI 是 HTTP 客户端」已作废。

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
> `trash` 尚未实现，同样不建空目录。
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
| 链接库 | `advapi32`（SCM、DACL）、`shell32`（提权）、`bcrypt`（随机数）、`ws2_32`（httplib）；全部系统库，不引入 DLL |
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
     CLI 记一行日志「数据根切换：旧 -> 新」（只进日志，不刷控制台）；旧根数据不删除，留在原地
```

数据根切换与初始化的完整流程见 13.10；服务自身状态文件见 13.6。

### 4.2 目录布局

```text
FMT_ROOT/                      CLI 声明的数据根
├── fmt.exe                    服务宿主 exe（安装时登记的路径，通常就在这里）
├── repository/                正式文件
│   └── <user>/<bucket>/YYYY/MM/DD/<file_name>
├── trash/                     回收站，保持原层级
│   └── <user>/<bucket>/YYYY/MM/DD/<file_name>
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
    └── fmt-elev-<父进程 pid>.json   提权结果文件（13.8.3），父进程读完立刻删除
```

**不在数据根内的位置：**

```text
%ProgramData%\FMT\service.json    服务自身状态（当前数据根 + 安装信息），不是业务数据
```

**`temp/` 的定位与规则**：

```text
定位      临时文件目录：不是业务数据、也不是日志，内容随时可以清空
放什么    ① 提权结果文件 temp/fmt-elev-<父进程 pid>.json（父进程读完立刻删除，13.8.3）
          ② 以后上传时的暂存文件（10.2 旧文写「temp/ 或者系统临时目录」，现在明确为 temp/）
谁创建    CLI 在执行提权类命令前会尝试创建 <数据根>/temp；创建不出来（例如 exe 放在只读位置）
          就退回系统临时目录 %TEMP%，并写一行 WARN 说明原因与改用后的路径。
          提权副本在写入结果文件前也会确保目录存在——它自己有权限，
          所以调用方数据根只读时它仍然能建出来
清理      Service 启动时删除 temp/ 下以 fmt- 开头的遗留文件（上次异常退出留下的提权结果等），
          用户手放进去的其它文件一律不动；删除数量记一行 INFO（时机见 13.7.1 第 4 步）
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
                 —— switched=true 时记一行日志「数据根切换：旧 -> 新」（只进日志）
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

| 字段 | 类型 | 说明 |
|---|---|---|
| `current_user` | string | 当前用户。用户系统未实现前为空，由 CLI 引导设置 |
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
| `enabled` | 是否启用 HTTP。默认 `false`，Service 场景下由安装流程置为 `true` |
| `host` | 监听地址，`0.0.0.0` 允许局域网访问 |
| `port` | 监听端口，默认 4122 |

V1 使用 HTTP，后续可扩展 HTTPS。

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
| 字段类型错误 | 报配置错误，不猜测 |

**不得删除原配置，不得重新生成空配置。**

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

### 7.6 数据模型与数据根的关系

`data/*.json` 里的记录**不保存绝对路径**，只保存相对数据根的层级信息：

```text
file.json     只记 user / bucket / file_name / …（层级由规则推出）
trash.json    original_path 与 trash_path 都是相对数据根的相对路径
              （形如 repository/小谷/工作/2026/10/05/test.txt）
```

这是数据根动态化（4.1）能成立的前提：切换根之后，同一个 `data/file.json`
放到另一个根下，相对路径仍然指向该根内部的正确位置。

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
4.  创建临时文件（位置见下：<数据根>/temp/）
5.  下载或复制数据（流式，不整文件入内存）
6.  检查下载/复制是否完整
7.  检查大小 ≤ max_upload_size（超限则删除临时文件）
8.  计算 MD5（32 位小写十六进制）
9.  MD5 去重检查（已存在 → 提示「该文件已经存在」，终止）
10. 文件名合法性检查
11. 文件名冲突检查（同用户 + 正常文件 + 同名 → 冲突）
12. 生成 file_id
13. 移动临时文件到 repository/<user>/<bucket>/YYYY/MM/DD/（跨目录移动，见 15.5）
14. 写入 file.json
15. 完成
```

**临时文件位置**（**已改口径**）：

```text
<数据根>\temp\<name>.tmp                   上传暂存文件的默认位置（本节的下载/复制阶段用）
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
                                              └──→ HTTP 127.0.0.1:4122（仅浏览器）
```

| 通道 | 用于 | 说明 |
|---|---|---|
| 命名管道 `\\.\pipe\fmt.control` | 全部业务命令 | 一请求一响应，帧见 13.9 |
| SCM API（经 UAC 提权的短命副本） | `service install/uninstall/start/stop` | 见 13.8 |
| SCM API（**不提权**，本地直连） | `service status` | 只读查询，见 11.2 / 13.4.1 |
| 本地处理 | `--help` / `--version` / `help` / `exit` | 不依赖 Service |
| HTTP `127.0.0.1:4122` | **仅浏览器** | CLI 不再使用 |

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
| `--help` / `--version` | 本地 | 不依赖 Service |
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
├── --version
├── help    [组]                     ← 命令总览 / 某一组的详细说明
├── exit | quit                      ← 交互循环内退出
├── bucket  create <name> | list | get <name> | use <name> | delete <name>
├── file    upload <url|path> | list | get <file_id> | get <filename> | delete <file_id>
├── share   create <file_id> | get <share_id> | list <file_id> | delete <share_id>
├── trash   list | get <id> | restore <id> | delete <id>
├── config  get | set --user <name> | set --bucket <name>
└── service install | uninstall | start | stop | status
```

`file delete` / `share delete` / `bucket delete` 是**业务命令**（走管道，删除语义是移入
Trash），与 `service uninstall` 完全不同层次，不要混用「删除」二字。
`service status` 与它们都不冲突：它是 `service` 子命令，读的是 SCM 状态。

### 11.4 命令帮助

帮助有**两个入口**，内容来源是同一份命令总览与同一份分组详情：

```text
fmt.exe --help        一次性：横幅 + 用法 + 命令总览 + 退出码表 + 「详细说明」一行
fmt.exe help [组]     一次性：不带参数 = 命令总览；带组名 = 该组详情
help [组]             交互循环内同上（交互里也接受 --help 这个别名）
```

**`help`（不带参数）只列命令、不加描述**：

```text
可用命令：
  (service)  install  uninstall  start  stop  status
  (help)     help [命令]
  (exit)     exit  quit

业务命令（服务端尚未实现，现在会返回 FMT-602）：
  (bucket)   create  list  get  use  delete
  (file)     upload  list  get  delete
  (share)    create  get  list  delete
  (trash)    list  get  restore  delete
```

**`help <组>` 打印该组详情。** 当前支持 `service` / `bucket` / `file` / `share` /
`trash` / `help` / `exit`（`quit` 等同 `exit`）。`help service` 的输出：

```text
service —— Windows 服务管理
  install    安装并启动服务；需要管理员权限，弹一次 UAC
  uninstall  停止并删除服务；需要管理员权限
             不删除 repository / trash / config / data / log / temp
  start      启动服务；需要管理员权限
  stop       停止服务；需要管理员权限
  status     查询服务状态；不需要管理员权限

说明：启动类型为自动启动，运行账户为 LocalSystem；异常退出由 Windows
      服务恢复策略自动重启（第一次 5 秒、第二次 10 秒、之后 30 秒）。
```

四组业务命令（`bucket` / `file` / `share` / `trash`）的详情里**都要注明**
「服务端尚未实现，现在返回 FMT-602」——因为它们的管道 op 还没落地，敲下去拿到的是 `FMT-602`，
不注明会让人以为是参数写错了。

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
| 未知命令 | 报错 + 提示帮助 + 非 0 退出码 |
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

本次重构涉及的映射（与 `FMT 重构设计.md` 第 9 节的冻结表一致）：

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
这两个函数名是实现细节，`FMT 重构设计.md` 里把它表述为「同一个字符串还原函数」。

### 11.9 界面与交互模式

**横幅**（进入交互循环前打印）：

```text
File Manager Tool  v1.0  ( build  2026.10.08 )
Service Running...

fmt >
```

服务未运行时第二行改为：

```text
File Manager Tool  v1.0  ( build  2026.10.08 )
Service Stopped...

fmt >
```

横幅文本来自 `banner_text()`（`src/cli/cli.cpp`），它拼的是 `fmt/version.hpp` 的
`MAJOR` / `MINOR` / `BUILD_DATE`（见 3.4），**不得硬编码**；`--version` 调的是同一个函数。

**提示符：`fmt> `**（`fmt` + `>` + 一个空格，无换行）。

**提示符前先空一行**：交互循环在打印 `fmt> ` 之前先输出一个空行——横幅之后一次，
以及**每条命令执行完之后**一次，这样输出不会和提示符挤在一起。
**用户只敲回车（空命令）时不再重复空行**，不会叠出连续两个空行。

- 命令集与命令行参数形式一致
- 空行忽略
- `help` 打印命令总览，`help <组>` 看详情（`--help` 在交互里是别名）；`exit` / `quit` 退出
- 命令失败时输出错误并**继续**循环
- **同时只允许一个 CLI 窗口**（命名互斥体，见 11.11）
- **控制台只留交互与异常**：数据根体检结果、服务当前状态、换根通知都只进日志（见下）

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

换到另一个目录双击（hello 回执 `switched=true`）时，**换根通知同样只在日志里**，
控制台不刷这一行（想看就问 `service status`，它会打印「服务数据根」）：

```text
2026-10-08 09:31:07 [INFO] [Service] 数据根切换：D:/FMT -> D:/FMT2
```

**只有异常才走 stderr**（两条）：数据根无法补齐、以及数据根里的文件损坏。

```text
数据根无法补齐：FMT-013 目录创建失败 ...
数据根文件损坏（未自动修复）：data/file.json
```

> 归位理由：本工程原本就有「**控制台负责用户交互与重要异常，日志文件负责完整运行记录**」
> 这条原则（见 14.2）。数据根体检的「建了什么 / 完不完整」、服务的当前状态、以及换根通知
> 都属于**例行运行记录**，放在控制台只会把提示符淹掉——尤其是双击时用户只想知道
> 「能不能开始敲命令」。因此把这些行按原则归位到日志，控制台只保留横幅、提示符、
> 命令结果与异常。

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
current_user 为空   → 引导用户设置当前用户
current_bucket 为空 → 执行 file upload / file list 时提示先创建或选择 Bucket
```

CLI 只是**提示**（见 18.9 的历史决策：当前实现自动落到 `default` 与默认 Bucket
「工作」，由**服务**写回 `config.json`）；CLI 自己不写配置文件，它只在双击时补齐
**缺失的**目录与默认 JSON（见 11.13），不修改任何已有内容。

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
2. `--help` / `--version` / `help` / `exit` 不写日志、不创建任何目录（它们不该在磁盘上留下东西）。
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

---

## 12. HTTP Server

### 12.1 实现方式

使用 **cpp-httplib**（`third_party/cpp-httplib/httplib.h`），它同时提供
`httplib::Server` 与 `httplib::Client`，服务端与下载客户端共用一套代码，
无需自研 HTTP 栈。

```text
Service 侧：httplib::Server    监听 127.0.0.1:4122，注册路由（浏览器用）
下载侧    ：httplib::Client    file upload <url> 时由 Service 下载远程文件
CLI 侧    ：不使用 HTTP        CLI 走命名管道（第 11.1 节 / 第 13.9 节）
```

**冻结决策：CLI 不再走 HTTP。** 早期设计的「CLI 是 HTTP 客户端、
Service 是 HTTP 服务端，两者通过 127.0.0.1:4122 通信」已作废：

| 对比 | 旧（作废） | 新（冻结） |
|---|---|---|
| CLI ↔ Service 通道 | `httplib::Client` → `127.0.0.1:4122` | 命名管道 `\\.\pipe\fmt.control` |
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
src/common/net/
├── http_client.hpp/.cpp    对 httplib::Client 的封装（仅供 URL 下载使用）
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

> **端口占用不再是致命问题。** 旧的 CLI 依赖 4122，端口被占时整条命令链路失效；
> 现在 CLI 走管道（13.9），HTTP 只服务浏览器，起不来只是「浏览器访问不了」。
> 服务必须继续工作，并在日志里写明端口与原因。

Service 启动顺序：先初始化业务与存储，再启动 HTTP，避免请求到达时数据层未就绪。

根切换（13.10）时 HTTP **不需要重启**——`server.json` 随根改变后，最迟下一次
请求生效；`disabled` 时停止监听，`enabled` 时按新 `host`/`port` 重新 `listen`。

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
| GET | `/api/config` | `config get` |
| PUT | `/api/config` | `config set --user/--bucket` |
| GET | `/api/share` | `share list <file_id>` |
| POST | `/api/share` | `share create <file_id>` |
| GET | `/api/share/<share_id>` | `share get <share_id>` |
| DELETE | `/api/share/<share_id>` | `share delete <share_id>` |
| GET | `/api/trash` | `trash list` |
| GET | `/api/trash/<id>` | `trash get <id>` |
| POST | `/api/trash/<id>/restore` | `trash restore <id>` |
| DELETE | `/api/trash/<id>` | `trash delete <id>`（永久删除） |

**`service *` 没有、也不会有 HTTP 路由**：它操作的是 SCM，与服务进程内的业务无关，
且服务可能尚未安装/运行。见 13.8。

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
管道侧的 `file upload` 用的是同一份语义。

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
       4.3 清理 <root>/temp 下以 fmt- 开头的遗留文件（上次异常退出留下的提权结果等；
           用户手放的其它文件不动），删除数量记一行 INFO（见 4.2、13.8.3）
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

- 结果文件**读完即删**，不留在 `temp/` 里（退路情形下也不留在 `%TEMP%`）；
  也避免下次同 pid 复用时把旧结果当成新结果。上次异常退出留下的 `fmt-` 前缀文件，
  由 Service 启动时统一清理（见 4.2 与 13.7.1 第 4 步）。
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
| 提权副本没有返回结果 | 结果文件不存在 | `FMT-602 ServiceOperationFailed` / 退出码 **8**，提示「提权副本没有返回结果」 |
| 当前用户不是管理员 / 策略禁止提权 | `ShellExecuteExW` 失败，`ERROR_ACCESS_DENIED`（5） | `FMT-603 AdminRequired` / 退出码 5，提示「需要管理员账户」 |
| 服务已经装过（install 时） | `OpenServiceW` 成功 | `FMT-600 ServiceAlreadyInstalled` / 退出码 8 |
| 服务不存在（start / stop / uninstall） | `OpenServiceW` 失败，`ERROR_SERVICE_DOES_NOT_EXIST` | `FMT-601 ServiceNotInstalled` / 退出码 8 |
| `reinstall` 时服务不存在 | `OpenServiceW` 失败，`ERROR_SERVICE_DOES_NOT_EXIST` | **忽略**卸载阶段的这个错误，继续安装并启动（不是失败） |

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

`hello` 的响应 `data` 带两个字段，用来告诉 CLI「服务现在在哪个根、这次有没有跟着换」：

```json
{ "id": 1, "ok": true, "data": { "root": "D:/FMT2", "switched": true, "previous_root": "D:/FMT" } }
```

| 字段 | 说明 |
|---|---|
| `root` | 服务处理后确认的当前数据根（等于 CLI 声明的 root） |
| `switched` | 这次声明是否**导致服务切换了数据根**；未切换时该字段不出现（缺省视为 false） |
| `previous_root` | 切换前的旧数据根；**只在 `switched` 为真时出现** |

CLI 侧：`switched == true` → 记一行日志 `[Service] 数据根切换：旧 -> 新`（只进日志）；否则静默。
未切换时响应就是 `{ "id":1, "ok":true, "data":{ "root":"D:/FMT2" } }`。

其余业务参数按 `op` 决定，就放在同一层或 `args` 子对象里；
**字段细节随各命令实现确定**（见 19.1）。

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
| 普通命令（list / get / config 等） | **30 秒** | 超时 → `FMT-602` / 退出码 8 |
| 长耗时命令（upload / delete 事务） | 由 `op` 单独声明（如 10 分钟） | 同上，但要在 CLI 侧显示进度而不是静默等待 |
| 服务端写响应 | 30 秒 | 写失败 → 记 WARN，断开该实例，**不影响其他连接** |

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
10. hello 响应回填 switched = true 与 previous_root = 旧根；CLI 收到后记一行日志
    「数据根切换：旧 -> 新」（模块 Service），**控制台不打印这一行**
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
· 管道仍允许多实例（13.9.1），但业务上只有那一个 CLI 窗口在发命令
· 根切换锁是进程内的一把独占锁，保证「切换」这段临界区不被并发的业务请求穿插
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
 │   switched=true → 记一行日志「数据根切换：旧 -> 新」（只进日志）
 ├─ file.list ───────────→│  在 D:\A 下执行
```

规则是**同一套、幂等的**：Service 在启动/换根时执行 `bootstrap_root`，CLI 在双击时执行
`check_root`，两者是同一份实现、同一份清单，都只补缺失、都不碰业务数据内容。
CLI 仍然**不写任何业务 JSON 的内容**、不删文件、不改名；它连「这个根有没有初始化过」
都不需要问——自己先补齐，服务再用同样的规则确认一遍。

> 旧口径「初始化只由服务执行、CLI 只读不建业务目录」已作废：
> 双击时应立刻得到可用的数据根，而不是等一个可能启动失败的服务。
> 变的是**谁有权调用那套规则**，规则本身没变。

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
       └─ 成功 → 继续；响应 switched=true 时记一行日志「数据根切换：旧 -> 新」（只进日志）
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
├── fmt.log        全部日志
└── error.log      仅 ERROR 级

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

---

## 15. 并发与事务

### 15.1 线程模型

**只有 Service 一个进程写业务数据。** CLI 不写任何业务数据内容（它是管道客户端），
因此不存在跨进程并发——**两个例外，都不参与业务事务**：
其一，CLI 与 Service 追加同一个 `<数据根>/log/fmt.log`（见 11.12、14.2），
它是追加写、每行一次写入，不需要跨进程锁；
其二，CLI 双击时对自己数据根做一次幂等体检与补齐（11.13），它**只创建缺失的目录与默认 JSON**，
不修改任何已存在的内容，因此不会与 Service 的加载冲突（Service 侧看到的是「已存在 → 保持原样」）。

Service 内部：

```text
主线程              ServiceMain / SCM 回调（状态机，见 13.7）
管道监听线程         1 个，接受连接 → 每连接 1 个工作线程
管道工作线程         每个 CLI 连接 1 个：读帧 → 派发到 service 层 → 写响应帧
HTTP 处理线程        cpp-httplib 为每个连接起一个线程
```

因此并发来自管道请求与 HTTP 请求两条入口，锁必须是**进程内**的。
**两条入口进的是同一个 service 层**（11.1），所以锁只需要在业务层存在一次。

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
                                       旧根数据仍在（不删）；hello 回执 switched=true，CLI 记一行日志「数据根切换：旧 -> 新」（只进日志，不刷控制台）
同一目录再双击第二次                  → 激活已有窗口，不新建控制台；
                                        日志里只多一行 [Cli] 数据根完整，控制台不变
service stop 期间发一条 file list     → 要么等完成，要么 FMT-602，不出现半写状态
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
| 1 | 项目骨架：CMake、Ninja、MSVC、`fmt.exe`、manifest(`asInvoker`)、`--help`/`--version`、`wmain` 入口分发骨架 | ⏳ 进行中 |
| 2 | 基础层：`common`（Error/Result/Time/String/Path/Logger）+ `config` + `storage` + `core` 根解析与幂等初始化（**`ensure_root`/`check_root` 由 Service 与 CLI 共用**；损坏 JSON 只报告不重置） | ⏳ 未开始 |
| 3 | **通道与服务**：`service`（SCM 五命令 `install`/`uninstall`/`start`/`stop`/`status` + 查询接口 `query_status()`/`query_state()`/`last_start_failure()`（13.4.2）+ 按 `dwWaitHint` 自适应的落定等待（13.4.3）+ Recovery + `ServiceMain` 状态机 + 失败编号上报 `dwServiceSpecificExitCode` + `service.json`）+ `ipc`（命名管道帧、DACL、MIC、hello 的 `switched`/`previous_root` 回执）+ `cli`（管道客户端、单实例、UAC 提权引导、双击幂等体检与补齐、落定判定与 reinstall 兜底、交互循环） | ⏳ 未开始 |
| 4 | Bucket：create / list / get / use / delete，`current_bucket` 逻辑 | ⏳ 未开始 |
| 5 | File / Upload / Trash / Share：`file.json`、`file_id`、上传（本机 + URL）、trash、share 下载计数 | ⏳ 未开始 |
| 6 | `server`：HTTP 浏览器侧（下载/预览路由 + `/api/*`）与 Preview | ⏳ 未开始 |

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
| `common/validation` | 文件名与 URL 校验（含保留设备名、路径穿越） | `include/fmt/common/validation.hpp` |
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
> | `service` install/start/stop/delete/status | **重写**：命令改为五条 `install` / `uninstall` / `start` / `stop` / `status`（旧 `delete` 更名 `uninstall`）；`status` 是**用户可见的第五条命令、不提权**（13.4.1），不是「双击流程内部调用」；新增 `ServiceMain` 状态机与失败编号上报 `dwServiceSpecificExitCode`（13.2.3）、`service.json`、Recovery 细节（13.3～13.7） |
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

> 注意：`FMT 重构设计.md` 第 9 节的例子把「未设置当前 Bucket」写成 `FMT-305`。
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
| `file delete` | CLI 与路由都在，handler 返回「本版本尚未实现」。落地方案已定：标记 `is_trash=true` + 把物理文件移入 `trash/<user>/<bucket>/YYYY/MM/DD/` |
| Bucket 删除的元数据标记 | `bucket::remove` 只做了目录搬迁。第 30 节要求同时把该 Bucket 下所有文件的 `is_trash` 置 true——等 `file delete` 落地时一并做，两处必须同一套逻辑 |
| Trash 全部命令 | `trash list/get/restore/delete` 均未实现 |
| Preview | `file preview` / `share preview` 返回 FMT-701 |
| `common/md5` 的测试覆盖 | 无自动化测试；实测与 `md5sum` 对比一致（上传后 `file.json` 里的 md5 与外部工具一致） |

### 18.13 本次重构后仍未做的事（`arch-restart` 分支）

按 18.1 的新阶段表倒推，阶段 1～6 之外还欠这些；**其中前三条必须实测**，
否则「服务能装、能起、能停」这条底线没有证据。

| 事项 | 现状 / 说明 |
|---|---|
| 提权路径端到端实测 | `service install/uninstall/start/stop/reinstall` 都需要一次真实的 UAC 确认；**`service status` 不需要**（它不提权，要专门验证非管理员账户下也能成功、且全程不弹 UAC）。要验证：成功路径、**取消 UAC（`ERROR_CANCELLED` 1223 → `FMT-004` / 5）**、等待超时（`WAIT_TIMEOUT` → `FMT-602` / 8）、重复 install（`FMT-600` / 8）、服务不存在（`FMT-601` / 8）、提权期间不出现控制台闪窗、结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 正确生成并在父进程读完后删除、数据根不可写时退回 `%TEMP%` 并写 WARN |
| 管道连接权限实测 | DACL（`D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`）与 MIC 标签（`S:(ML;;NW;;;ME)`）都要在**非提权 CLI** 上跑通；只测提权 CLI 会掩盖 13.9.2 的两个坑 |
| 根切换实测 | CLI 换目录运行 → 服务切根、幂等初始化新根、旧根数据不删、`service.json` 的 `current_root` 更新，且 hello 回执 `switched=true` + `previous_root`，CLI 记一行日志「数据根切换：旧 -> 新」（只进日志，不刷控制台） |
| `service status` 实测 | 三种状态各跑一次：未安装（`服务状态：未安装` / `FMT-601` / 退出码 8）、已安装未运行（`已停止` / 退出码 0）、运行中（`运行中` + 宿主 + 数据根 / 退出码 0）；并确认它在**非管理员账户**下同样成功 |
| `dwServiceSpecificExitCode` 实测 | 人为让服务初始化失败（例如把 `data/file.json` 改成非法 JSON）→ 确认 `sc query` 能看到 `ERROR_SERVICE_SPECIFIC_ERROR`、CLI 打印「服务启动失败：FMT-006 JSON 格式错误」，并且**不触发重装** |
| `--help` 与 `help` 实测 | `fmt.exe --help` = 横幅 + 用法 + 命令总览 + 退出码表；`fmt.exe help` 只列命令、不加描述；`help service` 列出五条命令并注明 `status` 不需要管理员权限；`help bucket/file/share/trash` 都带「服务端尚未实现，现在返回 FMT-602」；`help 未知组` → stderr 一行 + 退出码 2；`help` 全程不提权、不连服务、不写日志 |
| CLI 双击体检实测 | 空目录首次双击 → **控制台干净**（只有横幅与提示符），日志里有 `[Cli] 数据根新建目录：…` / `数据根新建文件：…`；再双击 → 日志里只有 `[Cli] 数据根完整`；预置一个损坏的 `data/file.json` → **stderr** 出现 `数据根文件损坏（未自动修复）：data/file.json` 且文件**未被改动**（比对时间戳与内容）；另测「数据根只读」→ stderr 出现 `数据根无法补齐：FMT-013 …` 且不阻断后续流程 |
| 横幅与 `--version` 实测 | 两者输出同一串 `File Manager Tool  v1.0  ( build  <日期> )`；重新配置（重跑 CMake）后日期随之变化；源码里搜不到硬编码的版本号或日期 |
| SCM 30 秒限制 | 需要一次「初始化故意变慢」的验证：确认 `START_PENDING` + `dwCheckPoint` 上报真的消除了 1053 |
| 服务停止中的在途操作 | 上传中途 `service stop` 的收尾行为（13.7.3 第 2 步）需要实测 |
| Recovery 实测 | 人为 `TerminateProcess` 服务进程，观察 5s / 10s / 30s 的重启节奏与 1 天计数重置 |
| Trash / `file delete` | 与 18.12 相同，尚未实现；落地方案已定（标记 `is_trash` + 移入 `trash/<user>/<bucket>/YYYY/MM/DD/`） |
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

---

## 19. 待决事项

### 19.1 本次重构引入的待决事项

| 事项 | 说明 |
|---|---|
| `init` 操作是否需要显式命令 | 当前设计是「CLI 双击时先自己跑一次 `check_root`（11.13），连上后再声明 root，服务用同一套规则幂等初始化」（13.10），没有独立的 `init` 命令。是否需要一条显式的「初始化这个数据根」命令待定；不加也不影响主链路 |
| 数据根由 CLI 声明 vs 架构文档 | 已对齐：`FMT 项目架构.md` 与本文档都写 **CLI 声明、服务持有**，并明确 CLI 双击时对自身数据根做同一套幂等体检与补齐（第 3.1 / 3.2 节 ↔ 4.4 / 11.13）。后续若再出现「`FMT_ROOT` = `fmt.exe` 所在目录」的旧表述，以冻结决策为准 |
| 两个不同的数据根 | CLI 所在目录（声明值）与服务宿主 exe 所在目录（无 CLI 连接时的回退值）可能不同。是否需要一条「服务启动后马上规范化为记录值」的规则，避免「服务自启 → 用宿主目录 → 第一个 CLI 连上 → 又切一次」的抖动，待定。注意这种切换现在会**显式可见**：hello 回执 `switched=true` 且 CLI 记一行日志「数据根切换：旧 -> 新」（只进日志，不刷控制台） |
| 多 CLI 窗口 | 单实例（11.11）保证同时只有一个数据根，因此根切换不需要按连接隔离。若将来放开多窗口，需要重新设计「一个当前根」的语义 |
| `service.json` 的字段集 | 现定 `version` / `current_root` / `binary_path` / `installed_at`。是否需要记录「上次正常停止时间」等诊断字段待定 |
| `--elevated` 的参数形状 | **已定稿**：`fmt.exe --elevated <operation> --result "<结果文件绝对路径>"`，`operation ∈ install / uninstall / start / stop / reinstall`（13.8.2），**`status` 不在其中**（它不提权，见 13.4.1）。是否再为某个 op 携带附加参数（如 install 时指定 root）仍待定 |
| 管道 `op` 的完整清单 | `bucket.*` / `file.*` / `share.*` / `trash.*` / `config.*` / `hello` 的**精确名字与参数**随各命令实现确定（13.9.3）。`hello` 的**响应**字段已定：`root` + `switched` +（切换时）`previous_root` |
| 长耗时命令的超时值 | 普通命令定为 30 秒（13.9.4）；`file.upload` 这类的最长等待时间需按最大上传大小估算后冻结 |
| 停止等待与 Recovery 的相互作用 | 停止过程中若超时（13.7.3），是否返回非零退出码让 SCM 记录一次失败、甚至触发 Recovery，待定——不处理好会出现「停止失败 → 自动重启」的循环 |
| 管道实例数量与线程模型 | `PIPE_UNLIMITED_INSTANCES` + 每连接一线程，理论上可被本机进程耗尽。是否改成固定工作线程池待定 |
| 提权副本的结果通道 | **已定稿，从待决清单移出**：结果经结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 回传（13.8.3），数据根不可写时退回 `%TEMP%` 同名文件。命名管道方案作废，原因是 MIC「禁止向上写」（13.9.2 坑 2），不存在「管道 + 临时文件降级」两条路 |
| `version` 字段与兼容 | `service.json` 沿用「未知版本直接拒绝」的策略，还是允许忽略未知字段待定 |
| 双击引导「等待落定」的具体时长 | **已定稿，从待决清单移出**：不写死时长，改为**按 SCM 的 `dwWaitHint` 自适应**——每轮查询把 `dwWaitHint` 夹在 100 ms – 2000 ms 之间作为下次间隔，兜底上限 30 秒，状态一旦不是等待类就立即结束（13.4.3）。理由是写死 8 秒在慢机器上会把「还在启动」误判成「启动失败」，白弹一次解决不了问题的 UAC 重装。判定用的错误码集合也已冻结（13.2.3）。「仍没起」时是否再多试一次 `reinstall` 仍待实测后定 |
| `service status` 输出是否要机器可读格式 | **已定稿，从待决清单移出**：**V1 不做 `--json`，也不预留参数名**。机器可读通道是**命令退出码**（`0` 成功；未安装 `FMT-601` → `8`；查询失败 → `8`），人类可读通道是那几行文本（11.6、13.4.1）。需要结构化字段（如 `wait_hint_ms`）时再加 |

### 19.2 上一次实现遗留的待决事项（仍然有效）

| 事项 | 说明 |
|---|---|
| 业务处理函数装配 | 业务模块在装配点注册（旧为 `server::make_default_handlers`）。新版需同时给**管道 op 表**与 **HTTP 路由**注册，两处必须指向同一实现 |
| 首次使用的用户与 Bucket | 开发文档要求「提示用户先设置」。上一次实现改为**自动落到 `default` 与 Bucket「工作」**并写回 `config.json`，否则用户第一条命令就是报错。本次沿用，仍然是有意放宽 |
| `file delete` 与 Bucket 删除的元数据标记 | 两处必须共用同一套「移入 Trash + 置 `is_trash`」逻辑 |
| 大小显示 | `size_unit` 默认 `MB`，于是 27 字节显示成 `0MB`。是否改成默认 `AUTO` 待定（`common/size` 已支持 `AUTO`） |
| 日志轮转 | V1 不做轮转；单文件上限与轮转规则留待后续 |
| Bucket Trash 元数据结构 | 开发文档标注为暂定，Bucket 级 Trash 记录的字段需细化 |
| 最大上传大小默认值 | 现取 50 MB，硬编码在 `file/service.cpp` 的 `kMaxUploadSize`，尚未接到 `config.max_upload_size` |
| HTTPS | 需要 OpenSSL，会引入 DLL，V1 不支持；`file upload <https://...>` 返回 `UrlInvalid` |
| 旧 `paths::` 自由函数 | 仍保留 `executable_path` / `root` 等自由函数供启动阶段使用，与 `PathManager` 并存；后续可考虑收敛，但 `root()` 必须保留（它要先于 PathManager 存在） |
| `text::join` 的使用纪律 | 凡是用用户输入拼接路径，必须经 `common/text`。这条纪律目前靠代码审查保证，没有编译期强制 |
| `AppContext` 的指针持有纪律 | `paths` 与 `logger` 都必须 `unique_ptr` 持有，因为业务服务保存它们的引用。这条约束靠注释说明，没有编译期强制——改动 `AppContext` 成员时容易踩回去 |
| 自动化回归测试 | 当前没有。若后续要补，优先补「上传 → 列表 → 下载 → 分享」这一条链路，而不是各模块的孤立单元测试 |

