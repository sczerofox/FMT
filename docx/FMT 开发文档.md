# FMT 开发文档

> 项目名称：FMT
> 项目类型：Windows 文件管理系统
> 当前版本：V1
> 对应架构文档：`FMT 项目架构.md`
> 开发平台：Windows
> 构建工具：CMake + Ninja + Visual Studio Build Tools
> 主程序：`fmt.exe`（单一可执行文件，三种形态：CLI 形态 / Service 形态 / 提权短命副本）
> 文档状态：V1 开发规范（`arch-restart` 重构版）
> 本次重构差异与决策索引：第 122 节（冻结规则）与第 128 节（相对旧设计的差异与理由）

---

# 1. 文档目的

本文档用于规定 FMT V1 的具体开发方式。

《FMT 项目架构.md》负责定义：

* 系统整体架构
* 模块关系
* 数据关系
* 核心设计原则

本文档负责定义：

* 项目代码结构
* 模块职责
* 数据结构
* CLI 命令
* 文件操作流程
* JSON 数据操作
* File ID 生成
* MD5 去重
* Trash
* Share
* 运行目录与数据根
* Windows Service
* Service 提权与命名管道 IPC
* CLI 界面约定
* HTTP Server
* 错误处理
* 一致性处理
* 测试要求

开发过程中：

> 如果实现细节与本开发文档冲突，应优先修改文档并确认后再修改代码，不允许直接在代码中形成新的隐藏规则。

---

# 2. V1 开发原则

FMT V1 遵循：

```text
简单
稳定
快速
明确
可维护
```

优先级：

```text
数据安全
>
数据一致性
>
功能正确
>
性能
>
代码简洁
```

不为了追求“高级架构”而增加没有实际需求的复杂组件。

V1 不使用：

* 数据库
* Redis
* MQ
* 微服务
* 独立守护进程
* 复杂任务调度器
* 复杂 ORM
* 复杂权限系统

JSON + 文件系统满足 V1 需求。

---

# 3. 开发环境

V1 使用：

```text
Windows
C++
CMake
Ninja
Visual Studio Build Tools
```

推荐编译方式：

```text
CMake
    ↓
Ninja
    ↓
MSVC
    ↓
fmt.exe
```

依赖（vendor 到 `third_party/`，不再自研）：

```text
nlohmann/json       JSON 解析与序列化
cpp-httplib         HTTP Server（Service 侧；**URL 下载自提交 a2b6cd1 起不再用它**，
                    改走系统的 WinHTTP + Schannel，见第 33.2 节）
```

CRT 采用 `/MT` 静态链接，最终产物只有一个 `fmt.exe`。

---

# 4. 项目源码目录

运行目录与源码目录分离。

建议源码：

```text
FMT/
├── CMakeLists.txt
│
├── include/
│   └── fmt/
│       ├── cli/
│       ├── config/
│       ├── core/
│       ├── bucket/
│       ├── file/
│       ├── share/
│       ├── trash/
│       ├── storage/
│       ├── service/
│       ├── ipc/
│       ├── server/
│       └── common/
│
├── src/
│   ├── main.cpp
│   │
│   ├── cli/
│   ├── config/
│   ├── core/
│   ├── bucket/
│   ├── file/
│   ├── share/
│   ├── trash/
│   ├── storage/
│   ├── service/
│   ├── ipc/
│   ├── server/
│   └── common/
│
├── tests/
│
├── resources/
│
└── docs/
```

> **实况（提交 `0fc242b` 收口）**：`bucket/` 已落地（含**桶级回收站**：
> `list_trashed` / `restore` / `get_trashed` / `purge` + `trash/<user>/.original`，
> 提交 c2d545d 与 4fee290），`file/` 已落地（含**文件级**回收站的写与读），
> **`src/trash/` 也已经存在**（提交 `0fc242b`：`TrashService` 把文件级与桶级合成一份
> 视图 + 跨命名空间的标识解析，见第 52 节）——不再是「计划位置」。
> 仍未创建的是 `share/`（阶段 5 剩下的部分）。

---

# 5. 运行目录

编译完成后：

```text
<数据根>/
├── fmt.exe
├── repository/               repository/<user>/<bucket>/YYYY/MM/DD/<file_name>
├── trash/                    回收站：桶级条目 + 文件级条目，两层分开（提交 4fee290）
│                             trash/<user>/<bucket>_<YYYYMMDDHHMMSS>/  ← 桶删除的落点（顶层）
│                             trash/<user>/.original                 ← 桶级身份唯一权威
│                             trash/<user>/.files/<bucket>/YYYY/MM/DD/<file_name>
│                                                                     ← 文件级条目（阶段 5 起）
├── config/
│   ├── config.json
│   └── server.json
├── data/
│   ├── user.json
│   ├── file.json
│   ├── share.json
│   └── trash.json
├── log/
│   ├── fmt.log
│   └── error.log
└── temp/
    └── fmt-elev-<父进程 pid>.json    提权结果文件，父进程读完立刻删除
```

> **`trash/<用户>/` 的顶层只放桶级条目（提交 `4fee290`）**：桶级条目是
> `trash/<user>/<桶名>_<时间戳>/` 这样的目录，文件级条目收在点开头的
> `trash/<user>/.files/<bucket>/YYYY/MM/DD/` 下（第 21 节）。
> 分成两层是**必须的**：阶段 5 落地 `file delete` 之后，文件级条目如果还直接落在
> `trash/<user>/<bucket>/`，桶级扫描就会把这个目录误当成「孤儿桶条目」列出来
> （阶段 5 尚未落地，这里先把落点定死）。点开头的 `.files` 与 `.original` 都会被
> 桶级扫描跳过，扫描另有一道 `<名字>_<14 位时间戳>` 形状检查（第 53.1 节）。

数据根（`FMT_ROOT`）**由 CLI 声明**：CLI 连接服务时用 `hello` 帧带上自己 exe 所在目录
（`GetModuleFileNameW` 取父目录），服务把该目录作为「当前数据根」。切换数据根时**不删除旧根数据**，
只对新根做幂等初始化（见第 91～94 节）。服务的 `hello` 响应会回填本次是否发生了切换：

```json
{ "id":1, "ok":true, "data":{ "root":"D:/FMT2", "switched":true, "previous_root":"D:/FMT" } }
```

`switched` 为真表示这次声明**导致服务切换了数据根**，此时才带 `previous_root`；CLI 据此
**在 stderr 打印一段提示**（提交 `8f2fbc5`，文本来自 `cli::root_switch_notice(previous, current)`：
「注意：服务的数据根已切换 / 原来：… / 现在：… / 原因是「数据根由 CLI 声明」… /
如果这不是你想要的，请用数据根正确的那个 fmt.exe 再执行一条命令切回去。」），
**同时**仍记一行日志 `数据根切换：旧 -> 新`。
未切换时不含这两个字段（`switched` 缺省视为 false）。**原口径「只记日志、不刷控制台」已作废**
（第 127.1 节）。

CLI 在双击时还会**先对自己所在数据根做一次体检与补齐**（六个目录 + 六个默认 JSON，只补缺失，
见第 91～92 节）；这一步在打开日志器之前完成，所以 `log/` 也在它创建之列。

`log/` 与业务数据分离（日志不是业务数据），见第 65 节。

`temp/` 是**临时文件目录**：**既不是业务数据、也不是日志**，内容随时可以清空。它放两类东西：

```text
1. 提权结果文件 temp/fmt-elev-<父进程 pid>.json（父进程读完立刻删除，见第 126 节）
2. 以后上传时的暂存文件（见第 34 节：之前写「temp/ 或者系统临时目录」，现在明确为 temp/）
```

`temp/` 的创建与清理规则：

```text
谁创建  CLI 双击时会对自己的数据根执行与服务共用的幂等体检与补齐（ensure_root/check_root，
        见第 91～92 节），其中就包括 temp/；这一步在打开日志器之前完成，
        所以 log/ 与 temp/ 都在同一条「新建目录」清单里。
        CLI 在执行提权类命令前也会确保 <数据根>/temp 存在；
        创建不出来（例如 exe 放在只读位置）就退回系统临时目录 %TEMP%，
        并写一行 WARN 说明原因与改用后的路径。
        提权副本在写入结果文件前也会确保目录存在——它自己有权限，
        所以调用方数据根只读时它仍然能建出来。
清理    Service 启动时删除 temp/ 下**十分钟以前**的、以 fmt- 开头的遗留文件
        （上次异常退出留下的提权结果等），用户手放进去的其它文件一律不动；
        删除数量记一行 INFO。
        **为什么按年龄过滤（提交 `5b316b3`）**：提权副本回传结果的临时文件也叫
        `fmt-elev-<pid>.json(.tmp)`，而 `service install` 会在同一次操作里**启动服务**
        ——服务启动就来清 temp/。原来只看前缀 `fmt-`，于是把父进程**正在收**的结果文件
        一起删掉，父进程只好报「FMT-602 提权副本没有返回结果」：服务其实已经装好并启动，
        **用户看到的是假失败**（实测日志见第 126 节）。正在回传的结果文件寿命只有几十毫秒，
        所以按年龄放过新的、只清旧的；读不到时间戳的文件也不动。
```

服务自身状态不属于业务数据，单独存放，不放进任何数据根：

```text
%ProgramData%\FMT\service.json
```

该文件只记录「当前数据根路径」与安装信息。服务开机自启且没有 CLI 连接时，
数据根取该文件的记录值；从未记录过则取服务宿主 exe 所在目录。

---

# 6. 模块划分

核心模块：

```text
common
config
storage
core
bucket
file
share
trash
cli
ipc
service
server
```

依赖关系原则：

```text
CLI
 │
 ▼
Service / Business
 │
 ▼
Core
 │
 ├── Storage
 └── File System
```

CLI 与 Service 之间经 `ipc` 模块的命名管道通信；HTTP Server 与 CLI 把请求交给同一个
service 层（见第 76 节）。

业务模块之间不得直接大量互相调用底层实现。

---

# 7. common 模块

`common` 提供公共类型和工具。

建议包含：

```text
Error
Result
Time
String
Path
Hash
Validation
Logger
```

例如：

```text
common/
├── error
├── result
├── time
├── string
├── path
├── hash
├── validation
└── logger
```

公共模块不得依赖具体业务模块。

---

# 8. config 模块

负责：

```text
config.json
server.json
```

的加载、校验、保存。

主要职责：

```text
读取配置
验证配置
提供配置
修改配置
保存配置
```

---

# 9. config.json

V1 暂定：

```json
{
  "current_user": "",
  "current_bucket": "",
  "max_upload_size": 52428800,
  "size_unit": "MB",
  "language": "zh-CN"
}
```

字段：

```text
current_user
current_bucket
max_upload_size
size_unit
language
```

**阶段 4 已落地的行为**：文件里写出来的 `current_user` **不会长期为空**。数据根初始化
（`initialize_root`）读到空值时自动置为占位名 `user` 并写回，所以 `config.json` 里
`current_user` 为空只代表「这一次初始化之前的状态」，不代表运行时状态：

```json
{
  "current_user": "user",
  "current_bucket": "工作",
  "max_upload_size": 52428800,
  "size_unit": "MB",
  "language": "zh-CN"
}
```

`current_bucket` 由 `bucket use` / `bucket create`（第一个桶）写入，同样是名称而不是路径或
ID（第 27 节）。加载规则见第 10 节：**字段缺失或类型不符时用默认值补齐并回写**；
`FMT-008 ConfigError` 只在「文件不是合法 JSON / 版本不受支持 / 写不回去」时出现，
不会因为某个字段写错类型就拒绝整个数据根（`FMT 技术文档.md` 第 5.3 节）。

---

# 10. 配置加载规则

程序启动：

```text
启动
 ↓
检查 config/
 ↓
加载 config.json
 ↓
校验 JSON
 ↓
加载 server.json
 ↓
校验配置
 ↓
进入程序
```

如果配置文件不存在：

```text
允许创建默认配置
```

如果配置文件存在但 JSON 损坏：

```text
停止相关初始化
报告配置错误
```

不能：

```text
删除原配置
重新生成空配置
```

上述「检查 → 加载 → 校验」流程在 **Service 形态**中执行；CLI 形态不加载配置、不创建配置目录、
也不写配置文件，只把命令经命名管道交给服务（见第 76 节、第 91～94 节）。

**`config list` / `config set`（提交 `674d0b0` 新增；server_* 三项是本次改动（未提交））**：

```text
config.list   返回 current_user / current_bucket / max_upload_size / size_unit /
              language / path，**外加 config/server.json 的公共字段**（统一加
              `server_` 前缀，免得与 config.json 的字段撞名）：
                server_enabled / server_host / server_port / server_path
              **不含任何凭证**：口令哈希从不出现；token 虽然在 data 里，
              但 **CLI 默认不打印**（见下）——要看加 `--show-token`。
              CLI 打成标签行（不吐 JSON），**两个文件按顺序分开、一段一个文件、
              一行一个键值对**（用户的四条版式意见）：
                配置文件：config/config.json
                当前用户：user
                当前 Bucket：test
                上传上限：50MB（52428800 字节）
                大小单位：MB
                语言：zh-CN
                网络服务配置：config/server.json
                网络服务 enabled：true
                服务地址 host：localhost
                服务端口 port：4122
                用户 ID：u-f4485f27
                访问 token：（已隐藏；加 --show-token 显示，或读数据根下的 data/user.json）
              `config list --show-token` 才把最后一行换成
              「访问 token：<32 位>」+「HTTP 请求头：X-FMT-Token: <32 位>」
config.set    **可改四项**：
                max_upload_size <大小>      → config.json（其余 key 仍只读 → FMT-001）
                server_enabled <布尔>       → config/server.json
                server_host <地址>          → config/server.json
                server_port <1-65535>       → config/server.json
              其它 key → FMT-001「只能改 max_upload_size / server_enabled / server_host /
              server_port（其余只读）：<key>」
布尔写法      true / false（也接受 1 / 0、yes / no、on / off，大小写不敏感）
大小写法      纯字节 10485760，或 10MB / 512KB / 1GB（1KB = 1024 字节）；
              下限 1KB、上限 100GB（兜溢出）；非法值一律 FMT-001
落盘          走 save_config / save_server_config（第 11 节那套 .tmp 原子替换）；
              **落盘失败回滚内存里的值**并返回错误
server_* 的特例  **改完必须重启服务才生效**（server.json 只在服务启动时读一次），
              所以响应里带 `restart_required = true` + `server_path` + 一句
              「需要重启服务才生效」；**服务侧不做 reinstall**（服务进程去提权重装
              自己是在踩自己的脚），由 CLI 在命令成功后按 `restart_required` 执行：
              ① 服务没安装 → 只提示「下次安装/启动时生效」；② 已提权 → 直接 reinstall；
              ③ 普通权限 → 弹一次 UAC；用户取消就**如实提示**手动执行
              `fmt.exe service reinstall`（绝不假装已生效）
op            config.* 两种：config.list / config.set（管道与将来的 HTTP 路由共用）
```

**`config/server.json` 的默认值（本次改动（未提交））**：`enabled` 默认 **true**
（原来 false）。理由：新数据根装完就该能连上 `http://localhost:4122` 做集成测试，
默认关着会让人以为是功能坏了（真机上真发生过）；它只监听 `host`（默认 localhost），
除 `/api/ping` 与分享下载外都要 token，所以默认打开是可控的。

---

# 11. 配置保存

配置修改采用：

```text
读取
 ↓
修改内存对象
 ↓
写入临时文件
 ↓
验证临时文件
 ↓
替换正式文件
```

例如：

```text
config.json
config.json.tmp
```

只有临时文件写入成功并验证通过后，才替换正式文件。

**临时文件名必须唯一（提交 `5b316b3`，`src/storage/storage.cpp`）**：

```text
临时名    <目标>.<pid>.<序号>.tmp（原来是固定的 <目标>.tmp）
为什么    **同一个目标会有两个写者**：`service install` 会在启动服务之后由安装器写
          service.json（记 host_path / installed_at），而服务启动时也写它。
          固定名 `<目标>.tmp` 时，先完成的一方把它 rename 走，另一方紧接着做
          **读回校验**就找不到自己的临时文件 → 报「无法打开文件 …tmp」；
          更糟的是安装器那处是 `(void)save_state(updated);`（**忽略了返回值**），
          于是 service.json 静默地一直不更新（实测时间戳停在 18:19，重装多次也不变）。
          带 pid + 序号之后，两个写者各写各的，rename 是「后完成者胜」，
          正常结果就是最后那份内容
进程内    对原子写加互斥（同一进程里的多个线程不必去抢替换那一步）
替换重试  MoveFileExW 带 MOVEFILE_REPLACE_EXISTING 时，若遇到
          ERROR_ACCESS_DENIED / ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION
          （还有 ERROR_FILE_NOT_FOUND），**短暂重试**（最多 40 次 × 5 ms）——
          这类失败是短暂的，不该当成永久错误报给用户；其它错误码立即失败
用例      Storage.两个写者同时写同一个文件不会互相踩（8 线程 × 40 轮写同一个文件，
          零失败、内容必须是某一次**完整**写入、且不留 .tmp）
```

例如：

```text
config.json
config.json.2296.7.tmp        ← 进程 2296 的第 7 次原子写
```

---

# 12. Storage 模块

Storage 负责底层数据访问。

主要内容：

```text
JSON Storage
File Storage
Trash Storage
```

负责：

```text
读取 JSON
保存 JSON
创建目录
移动文件
删除文件
检查文件
```

Storage 不负责业务规则。

例如：

```text
Storage
```

可以执行：

```text
move(A, B)
```

但不应该自己决定：

```text
文件是否允许删除
```

删除规则由 File/Trash Service 决定。

---

# 13. JSON Storage

管理：

```text
data/user.json
data/file.json
data/share.json
data/trash.json
```

## 13.1 文件格式（已冻结）

按「单例 / 集合」分两类，**都带 `version` 字段**。

单例数据（`config.json`、`server.json`、`user.json`——`user.json` 是**单用户文件**：
一个数据根只有一个账号，用户字段平铺在根上，见第 18 节）：

```json
{
    "version": 1,
    ...
}
```

集合数据（`file.json`、`share.json`、`trash.json`）：

```json
{
    "version": 1,
    "<集合名>": [ ... ]
}
```

集合字段名固定：

```text
file.json   -> files
share.json  -> shares
trash.json  -> trash
```

（`user.json` **不再是集合**：只有一个账号，所以它是单例、字段平铺，没有 `users` 数组。）

采用「带集合字段的对象」而非裸数组，便于以后给集合增加元数据、演进 Schema。

## 13.2 版本策略（已冻结）

> **只接受明确支持的版本，未知版本直接拒绝。**

程序只支持 `version: 1`；遇到 `version: 2` 返回 `FMT-011 JsonUnsupportedVersion`。

**不降级、不猜测、不尝试兼容、不自动修改文件。** 以后支持 v2 时，由代码明确实现 v2
的读取逻辑。

## 13.3 解析器边界（已冻结）

支持：

```text
Object { }
Array [ ]
String
\uXXXX
\" 与常规 JSON 转义
Number
true / false / null
```

明确不支持：

```text
// 注释
/* */ 注释
尾随逗号
任何非标准 JSON
「尽量解析」的容错行为
```

遇到 `{ "file_id": "xxx", }` 直接报 JSON 格式错误，**不自动修复**。

原则上：

> **JSON 是程序数据，不是给用户写的配置语言。解析失败就失败，不猜、不修、不吞错误。**

解析器与业务结构分离：

```text
common/json/
├── JsonValue
├── JsonParser
├── JsonWriter
└── JsonError
```

业务层负责：

```text
file.json
    ↓
JsonParser
    ↓
JsonValue
    ↓
FileRecord
```

这样 `file.json` 字段变化不需要修改底层解析器。

## 13.4 更新要求

开发时必须保证：

```text
读取
修改
保存
```

不会破坏其他记录。写入采用 `.tmp` 后替换的原子方式（见第 11 节）。

---

# 14. file.json 数据结构

V1 暂定：

```json
{
  "file_id": "fmt-20261005-0",
  "user": "小谷",
  "bucket": "工作",
  "file_name": "test.txt",
  "extension": ".txt",
  "file_type": "text",
  "size": 1024,
  "md5": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
  "is_trash": false,
  "trash_reason": ""
}
```

字段：

| 字段          | 类型      | 说明         |
| ----------- | ------- | ---------- |
| `file_id`   | string  | 文件唯一 ID    |
| `user`      | string  | 所属用户       |
| `bucket`    | string  | Bucket 名称  |
| `file_name` | string  | 文件名        |
| `extension` | string  | 文件扩展名      |
| `file_type` | string  | 文件类型       |
| `size`      | integer | 文件大小，字节    |
| `md5`       | string  | MD5        |
| `is_trash`  | boolean | 是否处于 Trash |
| `trash_reason` | string | **为什么在回收站**：`"bucket"`（桶被删）/ `"file"`（文件自己删的，阶段 5 起）；不在回收站时为空串 |

**`trash_reason` 的阶段 4 口径（commit c2d545d，已实现）**：`is_trash = true` 时**必须同时写**
`trash_reason`。两条规则对着写：

```text
桶删除  只给 is_trash 为 false 的记录置位，同时写 trash_reason = "bucket"
        （已经因为自己删过而在回收站里的文件不动，也不算进 files_affected）
桶回退  只翻回 trash_reason == "bucket" 的那些，并把 trash_reason 清空；
        用户单独删过的文件（"file"）保持不动
```

**为什么必须区分**：没有它，回退一个桶就会把用户自己删过的文件一起放出来——桶级删除与
文件级删除都只写一个 `is_trash` 布尔值，回退时无法判断「这条记录该不该跟着桶一起回去」。
阶段 5 落地 `file delete`（`trash_reason = "file"`）时复用同一套判定，不许另起一套。

**已落地（提交 `188e85d`）**：`file delete` 就是「把 `is_trash` 置 true + 写
`trash_reason = "file"`」，判定与桶级删除共用同一套（`FileService::remove()`，第 43 节）；
桶回退只翻回 `"bucket"` 的那些，`"file"` 的一条都不动。

**记录里既不存路径也不存时间**（`FMT 技术文档.md` 第 7.6 节的纪律）：`file.json` 只有上面
这些字段，文件的实际落点由 `file_id` 里的日期片段推出（第 20、21 节），所以换数据根之后
同一条记录依然指向新根内部的正确位置。

---

# 15. file.json 禁止字段

以下字段不放入 `file.json`：

```text
max_download_count
download_count
expire_time
```

这些属于 Share。

File 只描述：

> 文件本身。

---

# 16. Share 数据结构

V1 暂定：

```json
{
  "share_id": "xxxxxxxx",
  "file_id": "fmt-20261005-0",
  "max_download_count": 20,
  "download_count": 0,
  "expire_time": null,
  "is_valid": true
}
```

字段：

| 字段                   | 说明         |
| -------------------- | ---------- |
| `share_id`           | Share 唯一标识 |
| `file_id`            | 对应文件       |
| `max_download_count` | 最大下载次数     |
| `download_count`     | 当前已完成下载次数  |
| `expire_time`        | 过期时间       |
| `is_valid`           | Share 是否有效 |

默认：

```text
max_download_count = 20
expire_time        = 现在 + 7 天（提交 d5779db 定下；本文档原来只写了 20，
                     7 天来自 FMT 技术文档.md 第 7.2 节「20 次 + 7 天 = 7 天内最多下载 20 次」）
```

**提交 `d5779db` 起本模块已落地（数据面）**：`src/share/` 真实存在，
`data/share.json` 的形状就是上面那张表（`version:1` + `shares[]`）；
`share_id` = **12 位随机十六进制**（`BCryptGenRandom`，见第 48 节）；
`expire_time` 为空 = JSON 里写 `null` = **不过期**；
**两个条件相互独立**（7 天内**或者** 20 次以内，谁先到谁生效）；
一个文件可以有**多个** share，各自独立计数。
**公开下载端点已实现（提交 `d3aeb3d`）**：`GET /api/share/<share_id>/download`，
不要 token（链接本身就是凭证）、先记账再放行（见 `FMT 技术文档.md` 第 12.3.2、18.40 节）。

---

# 17. Trash 数据结构

> **口径（提交 `0fc242b` 之后）**：**文件级**条目的权威是 `file.json`
> （`is_trash` / `trash_reason` / `deleted_at`，见 17.1），**桶级**条目的权威是
> `trash/<用户>/.original`（见 17.2）。**`data/trash.json` 不再写入任何条目**
> ——它保留为**只读兼容**（老数据里的 `deleted_at` 与老的 `type = "bucket"` 记录），
> 回退/永久删除时顺手清掉。原口径「`trash.json` 只服务文件级条目」（commit c2d545d）
> 已被 `0fc242b` 取代。

## 17.1 文件级条目（**权威已改为 `file.json`**，提交 `0fc242b`）

> **口径变更（提交 `0fc242b`）：文件级回收站不再写 `data/trash.json`。**
> 权威是 `file.json` 那一条记录本身——`is_trash` / `trash_reason` / **`deleted_at`**
> （提交 `0fc242b` 新增字段）都在里面，路径由 `file_id` 与记录推出，
> 所以**不需要第二份索引**。`trash.json` 从此是「**保留、只读兼容**」：
> 老数据里没有 `deleted_at` 时从它的旧记录里补上（`legacy_deleted_at()`），
> 回退/永久删除时顺手把那条老记录清掉（`remove_trash_record()`），
> **新数据一行都不写**。

文件级记录住在 `file.json` 里（第 14 节），进回收站后新增/变化的就是这三个字段：

```json
{
  "file_id": "fmt-20261005-0",
  "user": "小谷",
  "bucket": "工作",
  "file_name": "test.txt",
  "extension": ".txt",
  "file_type": "text",
  "size": 1024,
  "md5": "d41d8cd98f00b204e9800998ecf8427e",
  "is_trash": true,
  "trash_reason": "file",
  "deleted_at": "2026-10-05T20:00:00"
}
```

```text
deleted_at   进回收站的时间（ISO 8601，本地时间，local_datetime_iso()）；
             回退时清空；老数据没有这个字段 → 从 trash.json 的老记录里补（只读兼容）
trash_path   不存：数据在 trash/<用户>/.files/<桶>/YYYY/MM/DD/<文件>，
             由 file_id 定日期、记录定用户/桶/文件名，trash_path_of() 现推（第 21 节）
```

`type` / `original_path` 这些字段随 `trash.json` 一起**不再写入**（老记录仍可读）。
原来那句「记录字段就是上面这六个、`type` 恒为 `"file"`」的口径**已作废**。

**读取侧已落地（提交 `0fc242b`）**：`trash list` / `get` / `restore` / `delete` 现在
**同时处理文件级与桶级**条目（统一形状 `TrashEntry`，第 52、53 节）；
`file delete` 搬进 `trash/<用户>/.files/…` 的数据可以列出来、回退回原位置、永久删除。
**不再有「只写得进、读不出」这个缺口。**

## 17.2 桶级条目：`trash/<用户>/.original`（阶段 4 已落地并冻结）

Bucket 不生成 `file_id`；桶级记录**没有 `file_id`**，用原桶名标识。**唯一权威**是回收站
目录下的 `.original`（实现细节见 `FMT 技术文档.md` 第 7.3 节，布局见
`FMT 项目架构.md` 第 5.4.1 节，代码常量 `kOriginalIndexName = ".original"`）：

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

| 字段           | 类型     | 说明                        |
| ------------ | ------ | ------------------------- |
| `version`    | integer | 固定 `1`；不认识就拒绝（第 13.2 节）   |
| `buckets`    | array  | 桶级条目的唯一权威表                |
| `trashed`    | string | 回收站里的目录名（**带删除时间戳**）       |
| `original`   | string | 原桶名；为空表示没有身份记录，此时拒绝回退。**提交 `9c3d2cb` 起写的是磁盘上的实际桶名**（`canonical_name()` 的结果，`delete WORK` 记的是 `work`），这样 `restore` 拼出来的目录名与原来完全一致 |
| `deleted_at` | string | 本地时间 ISO 8601（无时区）        |

规则：

- 删除 Bucket 时目录名**一律**是 `<原桶名>_<YYYYMMDDHHMMSS>`（例如
  `lazy-fox_20261008012233`），**无论有没有重名**；同一秒内删两次、或目录恰好同名时
  再加序号 `<原名>_<时间戳>_2`。因此同一个名字删多少次都不会互相覆盖
  （旧口径「用原名、重名才加时间戳」**作废**：那样两条条目会抢同一个回退位置，
  回退时必然撞车——见第 30 节）。
- 目录搬成功后，在 `buckets` 数组**末尾追加一条**这样的记录；
- **不靠剥离时间戳反推原名**（`x_2026...` 也可能本来就叫这个），一律查这张表；
- **`.original` 损坏时 `bucket delete` 直接拒绝**（`FMT-006 JsonParseError`），不搬动目录——
  否则会留下一个「没有身份记录、原名永久丢失」的条目；索引**写不进去**时目录**搬回原位**，
  回退方向同理（索引没减掉就把目录退回去）。**磁盘与索引不允许不一致。**
- 记录里**不放** `file_id`，也不放绝对路径。

---

# 18. User 数据

**单用户文件**：**一个数据根只有一个账号**，所以 `data/user.json` 就是「那个用户」——
用户字段平铺在根上，没有 `users` 数组（字段本身与提交 `bfd89f7` 定的**完全一致**，
只是换了容器；以后加字段直接平铺进根对象）。

```json
{ "version": 1,
  "user_id": "u-b1e7c28f", "username": "user",
  "password_hash": "<PBKDF2-SHA256，64 位十六进制>", "password_salt": "<16 字节十六进制盐>",
  "token": "<32 位十六进制>", "token_issued_at": "…", "token_expires_at": "…", "current_bucket": "lazy",
  "created_at": "…", "updated_at": "…", "last_login_at": "" }
```

```text
**没有 buckets 字段**（用户要求删除）：桶以磁盘上真实存在的
        repository/<user>/<bucket>/ 为准。读的时候忽略老字段，
        **下一次初始化会把老文件里的它清掉**（线上已验证清掉了）。
current_bucket 保留（以后可能有用），但它只是**快照**——运行时权威仍是
        config.json 的 current_bucket。
密码    只存哈希（PBKDF2-SHA256 **10 万轮**）；口令明文从不落盘、也从不跨进程
        （见第 18.4 节）。
盐      **新账号**用 `directory_salt(username)` = 固定串 `fmt:user-salt:v1` 与用户名
        的派生值（确定性）；**v1.0 建的老账号**是 16 字节**随机盐**，照读照用。
        `password_salt` 字段一律保留：它是**口令派生的权威来源**，
        验密与改密都用它——这样老账号升级后口令不会被锁死（见第 18.5 节）。
token   32 位十六进制、**有时效**（`config.token_ttl_seconds`，默认 7 天）；HTTP 的 `/api/*` 就靠它认证；过期回 FMT-019，用 `user token renew`（凭密码）换新
        （第 82 节、`FMT 技术文档.md` 第 12.3.2 节）。
        **CLI 日常用登录（`login` / `-u -p`），token 是「临时备用钥匙」**——
        Java/HTTP 直连仍然只用 token（见第 18.3 节）。
还没账号 文件里只有 {"version":1}：补建缺失文件时写的就是它。
        **账号不再自动创建**：必须由机主执行 `user create <用户名>`（第 18.2 节）。
默认账号 **已取消**：`initialize_root` 不再建账号，也不再调用
        `UserStore::ensure_default()`（该函数已删除）。老版本建出来的随机口令账号
        本来就是"谁也不知道口令"的死账号，用它当凭证并不安全。
老形状   `{"version":1,"users":[{…}]}` 照读（取第一条）；下一次初始化会把它改写成
        单用户形状。空的老数组等价于「还没有账号」。
```

> **原口径「V1 不实现完整用户系统，只保留 `data/user.json` 作为未来扩展入口、
> 当前用户由 `config.json` 的 `current_user` 确定」已作废**（提交 `bfd89f7`）：
> 文件现在有账号、密码哈希与 token，HTTP 认证就靠它。
> 当前用户**仍然**由 `config.json` 的 `current_user` 确定。

**阶段 4 实现口径（已落地）**：V1 **不要求用户先设置当前用户**。数据根初始化
（`initialize_root`）时若 `current_user` 为空，自动置为占位名：

```text
user
```

并立刻保存配置（需求原文：「不做用户先用 user 代替」）。因此磁盘上的实际路径形如
`repository/user/<bucket>/…`，CLI 敲 `bucket create 工作` 不需要任何前置设置。

`current_user` 只会在被**显式清空**（外部改配置等异常路径）时才为空，此时 Bucket 服务
返回 `FMT-604 NoCurrentUser`——这个错误码保留给这种情况，不再是正常首启的必经之路
（见第 93 节）。

## 18.1 `user` 命令组（已落地）

一个数据根一个账号，所以命令组只有四条（外加登录用的一条内部操作）：

| 命令 | 服务端 op | 说明 |
|---|---|---|
| `user create <用户名>` | `user.create` | 建账号；提示两次口令（**回显 `*`**）、写 `data/user.json`、把该文件的 MD5 记进 `config.json` 的 `uuid` |
| `user get` | `user.get` | 打印账号字段（用户名 / ID / 当前 Bucket / 创建时间 / 更新时间 / 最后登录 / 密码状态 / **当前 token 与有效期**）。**要登录才能用**；仍然不打印口令哈希与盐 |
| `user update password` | `user.update` | 当前密码 → 新密码 ×2；**只能改一次**（第 18.6 节） |
| `user delete` | `user.delete` | 先验密码，再**逐字输入 `delete`** 才动手；永久删除 `repository/ trash/ config/ data/ temp/`（只留 `fmt.exe` 与 `log/`），**不进回收站、不可撤销** |
| （内部）取口令盐 | `user.salt` | 供登录/改密/删除前算摘要用（第 18.4 节） |
| （内部）验口令 | `user.verify` | 登录用：常量时间比对摘要 |

**用户名规则**：`[A-Za-z0-9_.-]`、≤32 字节、不能是 `.` / `..`（它同时是
`repository/<用户名>/` 的目录名）。已有账号时 `user create` **拒绝重建**，
并且**在问口令之前就拒绝**（真机反馈：先让人输两遍密码、最后才说"已经有账号"体验很差）。

**首启引导**：没有账号时交互模式打印「请先创建账号：`user create <用户名>`」，
并拦住其它命令（`help` / `service` / `log` / `ping` 类不受影响）。

**关键文件缺失保护**（用户要求 6）：`data/` 下 `user.json`、`file.json`、`share.json`、
`trash.json` **缺任何一个**时——**一个字节都不写**，报 `FMT-002` 并提示先恢复文件；
`user create` 同样被拦住（否则"丢文件"的事故会被一次建号抹平）。

## 18.2 口令怎么走（明文不出 CLI 进程）

口令**从不跨进程**，也不落盘、不进日志：

```text
CLI                                  服务
 │  ① user.salt / user.get            │
 │ ─────────────────────────────────► │  给出该账号的 password_salt
 │  ② 本地算摘要                       │
 │     password_digest(口令, 盐)        │  （PBKDF2-SHA256，10 万轮）
 │  ③ user.verify / user.update /      │
 │     user.delete 带摘要 ───────────►  │  常量时间比对比对已存哈希
```

**为什么不用「挑战-应答」**：服务只存**单向摘要**，而 PBKDF2 不可组合
（`PBKDF2(P)` 与 `PBKDF2(PBKDF2(P))` 不相等），所以服务端**无法**对随机 nonce
算出同一个应答值——除非它存明文或"口令等价值"，那更糟。因此方案是
「CLI 用服务给出的盐算摘要、只发摘要」。盐必须由服务给：**老账号是随机盐**（第 18.5 节）。

## 18.3 登录与门禁（已落地）

**登录方式**（两种等价）：

```text
fmt.exe -u <用户名> -p        # 启动参数：回车后隐藏输入密码，然后进交互窗口
fmt> login <用户名>           # 窗口内：同上；不带用户名就用账号里那个
fmt> logout                   # 退出登录
```

**登录态只有内存**（`Session::logged_in` / `Session::username`）：**窗口一关就没了**，
下次必须重新登录。刻意不落盘——落盘就不叫"关掉重开要重新登录"了。

**门禁**：业务命令必须先登录，其余不受限。

| 需要登录 | 不需要登录 |
|---|---|
| `file *`、`bucket *`、`trash *`、`share *`、`config *` | `service *`（装/卸/启/停/重装/状态） |
| `user get`、`user update password`、`user token renew`、`user delete` | `log clean`、`help`、`version`、`exit` |
| | `user create`（还没有账号时得先能建） |

不受限的那几类是有意为之：服务坏了、账号进不去的时候，得靠它们把机器救回来。
拦下时的提示必须**能照着做**：`本窗口内登录：login <用户名>`。
`user get` 之所以也要登录：它会打印**当前 token**（用户要求），token 是凭证，不该不登录就给。
`user token renew` 同样要登录（用户明确要求「换 token 得先登录才能改」）——登录时已经算过口令
摘要，修完这条之后 renew 不会重复问密码。

**登录会更新时间戳**：`user.verify` 验过口令之后写 `last_login_at` / `updated_at`
（用户要求「登录之后最后登录时间要变」）。登录痕迹**不在**改密指纹里（第 18.5 节），
所以怎么写都不会影响「密码只能改一次」。

**门禁只做在 CLI 层（方案 A）**：服务端认证协议**一个字没改** —— 管道与 HTTP 仍然
每请求认 `data/user.json` 里的 token。所以 Java/REST 直连照旧可用，
**token 是「临时备用钥匙」**；将来 jar 走登录接口也只是再加一层，不动现有协议。

## 18.4 盐与老账号兼容（**升级必须遵守**）

| 账号来源 | `password_salt` | 口令派生用的盐 |
|---|---|---|
| **新账号**（2.x 起 `user create`） | `directory_salt(用户名)`（确定性） | 同左 |
| **v1.0 老账号** | 16 字节**随机盐**（历史遗留） | **记录里那个随机盐** |

**口令派生一律取自记录里的 `password_salt`**（服务通过 `user.salt` 给出）。
曾经写成"一律按用户名派生"——那会让**老账号的口令当场验不过，等于把机主锁在门外**
（真机上就是随机盐账号，被 `App.老账号的随机盐依然能验密与改密` 这条用例钉住）。

新账号为什么用确定性盐：`uuid` 规则（第 18.6 节）依赖"同一用户名重建账号后文件内容
一致"，随机盐会让每次重建的 MD5 都不同。

## 18.5 `config.json` 的 `uuid` 与「密码只能改一次」

`config.json` 的 `uuid` 存的是「**口令状态的指纹**」。口径按用户原话定：

> 创建用户时：将已经存入的**密码、盐和用户 id、创建时间**的内容进行哈希运算，
> 将计算值存入 `config.json` 的 `uuid`

```text
指纹 = md5( { user_id, password_hash, password_salt, created_at } 的规范化 JSON )

建号 / 初始化    uuid := 指纹
user update     ① 先验旧口令；② uuid 必须等于**当前记录**的指纹（= 还没改过）；
                ③ 改完之后，uuid := **改动之前**那份记录的指纹
下一次再来       uuid 与当前记录的指纹不再一致 → 拒绝，提示「先 user delete 再 user create」
```

**只算这四项的理由**：其余字段都会自己动 —— `token` / `token_issued_at` /
`token_expires_at`（`user token renew` 会重写）、`last_login_at` / `updated_at`
（每次登录都会写）。把它们算进来，一次登录或一次换 token 就会被判成"密码已经改过"，
机主被永久锁死；而如果为了绕开去重写 uuid，又会把"已改过"的状态抹掉、重新放开一次改密。

**兼容**：升级前两代的口径（整条记录只排 token 字段、以及更早的整份文件 MD5）**都认**
（`never_changed_password()`），否则一个明明没改过密码的老账号会被判成"已经改过"。
任一次成功的改密 / 换 token 会把 uuid 迁移成新口径。

**③ 的时点极关键**（真踩过）：曾经写成"保存之后再取指纹"，那等于把 uuid 设成
**改完之后**那份记录的指纹，而下一次进来加载到的正是它，于是「改前指纹 == uuid」
永远成立 —— **可以无限次改密**。被 `App.初始化数据根不再自动建账号` 这条用例钉住。

## 18.6 `data/user.json` 不会被日常操作改写

`uuid` 规则成立的前提是：**指纹里的那四个字段在正常使用期间不变**。

`user.json` 本身**会**被改写（两处，都是有意的）：

| 时机 | 写什么 | 影响指纹吗 |
|---|---|---|
| 登录成功（`user.verify` 之后） | `last_login_at`、`updated_at` | **不**（不在四项里） |
| `user token renew` | `token` / `token_issued_at` / `token_expires_at` | **不**（不在四项里） |

**这条约束不能松**：如果哪天有人把 `last_login_at` 或 token 加进指纹，
登录一次或换一次 token 就会让「密码只能改一次」误判——要么把机主锁死
（判成已改过），要么把状态洗回"没改过"（重新放开一次）。
对应地，HTTP 认证仍然是**每请求现读、不写回**，所以登录时间只在真正登录时更新。

---

## 18.7 token 的时效（**过期一律拒绝**）

用户要求「service 端实现 token 的时效性，过时不能用」。token 不再永久有效。

| 项 | 口径 |
|---|---|
| 字段 | `data/user.json` 的 `token_issued_at` / `token_expires_at`（本地时间 `YYYY-MM-DDTHH:MM:SS`） |
| 有效期 | `config.json` 的 `token_ttl_seconds`，默认 **7 天**；`config set token_ttl 7d` / `12h` / `3600` 可改，**拒绝 0** |
| 生效范围 | 只影响**下一次签发**；已签发的 token 有效期写死在 user.json 里，不会被配置改动影响 |
| 过期判定 | `UserStore::find_by_token()`：先常量时间比对 token，再判时效；解析不了/没有记录也按过期 |
| 错误码 | **`FMT-019 TokenExpired`**（与 `FMT-018` 区分：过期该续期，无效是凭证抄错） |
| 续期 | `user token renew`（**凭密码摘要**，服务 op `user.token.renew`）；旧 token 立即失效 |
| 老 token | **没有 `token_expires_at` 的按已过期处理**：升级后必须换一次（说不清有效期的凭证不放行） |

**为什么不给「永不过期」**：`token_ttl` 为 0 被明确拒绝。留这么一条后门，等于这次改动白做。

**为什么续期凭密码而不是凭旧 token**：凭旧 token 续期等于没有时效；而不给续期路径的话，
token 一过期就只剩 `user delete`（数据全没）这一条路。密码是唯一「不依赖 token 自身」的凭证。

### 与「密码只能改一次」的关系（**必须守住**）

`config.json` 的 `uuid` 是「账号内容」的指纹，语义是**密码改没改过**。token 会轮换，
轮换不该改变这个语义，所以：

* 指纹**排除** `token` / `token_issued_at` / `token_expires_at`（`record_md5`）；
* `user token renew` **不碰** `updated_at`，也不消耗那一次改密机会；
* `update_password` 的「还没改过」判定额外接受**老口径**（整份文件 MD5），
  这样升级前建的老账号不会被判成「已经改过」而锁死机主；
  老账号续期时会把 uuid 迁移成新指纹，状态保持不变。

---
# 19. Path 模块

所有实际文件路径必须由 FMT 统一生成。

禁止直接使用用户输入拼接路径。

例如：

```text
用户输入：
../../test.txt
```

必须拒绝。

---

# 20. Repository 路径

正常文件：

```text
repository/
└── <user>/
    └── <bucket>/
        └── YYYY/
            └── MM/
                └── DD/
                    └── <file_name>
```

例如：

```text
repository/
└── 小谷/
    └── 工作/
        └── 2026/
            └── 10/
                └── 05/
                    └── test.txt
```

**`YYYY/MM/DD` 由 `file_id` 推出，不是另存一份（提交 `188e85d`）**：`file.json` 里没有
路径、也没有入库时间，日期就是 `file_id` 的 `fmt-YYYYMMDD-N` 片段：

```text
fmt-20261008-0  ->  repository/<user>/<bucket>/2026/10/08/<file_name>
```

`FileService::resolve_path()` 先按这条规则推路径（`date_from_file_id()` → `PathManager::repository_file()`）；
推出来的路径**不存在**时，再在`repository/<user>/<bucket>/` 的日期树里（往下三层）
找同名文件**兜底**：**唯一命中**才用，命中多个说明有歧义，报 `FMT-015 ConsistencyError`
（退出码 6），交给人处理——绝不替用户猜是哪一份。`file_id` 形状不对（不是
`fmt-` + 8 位数字 + `-`）同样报 `FMT-015`。

> 这样做的前提是 `file_id` **全程不变**（第 23、43 节：进回收站、恢复都不换 ID），
> 所以日期片段永远等于最初的入库日期；日期目录也只是**存放约定**，不是身份的一部分。

---

# 21. Trash 路径

Trash 保持原有层级（**桶以下**的 `YYYY/MM/DD/<文件名>` 一层不少，方便恢复）。

例如（**文件级条目**，阶段 5 起；`PathManager::trash_file`）：

```text
trash/
└── 小谷/
    └── .files/                  ← 文件级条目的容器，点开头（提交 4fee290）
        └── 工作/
            └── 2026/
                └── 10/
                    └── 05/
                        └── test.txt
```

> **落点在提交 `4fee290` 挪了一层**：原口径是 `trash/<user>/<bucket>/YYYY/MM/DD/<file>`，
> 现在是 **`trash/<user>/.files/<bucket>/YYYY/MM/DD/<file>`**（原口径已作废）。
> 原因：`trash/<user>/` 的**顶层留给桶级条目**（`trash/<user>/<桶名>_<时间戳>/`）。
> 不分开的话，阶段 5 落地文件级删除后，桶级扫描会把 `trash/<user>/<bucket>/` 这个目录
> 误当成「孤儿桶条目」列出来（第 53.1 节的扫描就是这么扫的）。`.original`（桶级索引）
> 同样是点开头，两者一起被扫描跳过。
>
> 实现：`PathManager::build()` 增加可选参数 `inner`（插在 user 与 bucket 之间，默认空），
> `trash_file()` 传 `L".files"`；`repository_file()` 不传，仓库路径（第 20 节）不变。
>
> ```cpp
> // include/fmt/core/path_manager.hpp（提交 4fee290）
> // base/user[/inner]/bucket/YYYY/MM/DD/file_name
> Result<std::filesystem::path> build(std::filesystem::path base, std::string_view user,
>                                     std::string_view bucket, const DateParts& date,
>                                     std::string_view file_name,
>                                     std::wstring_view inner = {}) const;
>
> // src/core/path_manager.cpp
> Result<std::filesystem::path> PathManager::trash_file(
>         std::string_view user, std::string_view bucket, const DateParts& date,
>         std::string_view file_name) const {
>     return build(trash(), user, bucket, date, file_name, L".files");
> }
> ```
>
> 用例 `PathManager.回收站保持原层级` 断言
> `D:/FMT/trash/小谷/.files/工作/2026/10/05/小谷姐姐麻辣烫.jpg`。

这样可以方便恢复。

**桶级条目是另一回事（阶段 4 已落地）**：删掉一个 Bucket 时整个桶目录搬成

```text
trash/<用户>/<桶名>_<YYYYMMDDHHMMSS>/      同一秒再删一次就加序号 _2、_3……
trash/<用户>/.original                     桶级身份记录（原名 / 回收站名 / 删除时间）
```

即**桶这一层就是桶级条目的落点**，目录名一律带删除时间戳（不再「重名才加」）；
文件名与原名一律查 `.original`，不靠剥离时间戳反推（第 17.2、30、56 节）。

**文件级回收站路径同样由 `file_id` 推出（提交 `188e85d`）**：`FileService::trash_path_of()`
用同一个 `date_from_file_id()` 推出 `trash/<user>/.files/<bucket>/YYYY/MM/DD/<file_name>`
（`PathManager::trash_file()`），`file delete` 的落点就是它，响应里的
`moved_to` 也由它算出来（第 43 节）。和仓库侧一样，路径不写进任何记录，
只由 `file_id` + `user` + `bucket` + `file_name` 现推。

> **`<bucket>` 取的是记录自己的 `bucket`，不是当前 Bucket（提交 `0ad9efc` 明确）**：
> `trash_path_of(const FileRecord&)` 只读传进来的那条记录（`record.bucket`），
> 从头到尾不看 `config_.current_bucket`。所以「在桶 B 里按名字删掉桶 A 的文件」时，
> 文件会进 `trash/<用户>/.files/<A 的桶名>/YYYY/MM/DD/<文件名>`，**不会**落在 B 下。
> 这样落点和恢复目标才对得上：`file.json` 里那条记录的 `bucket` 仍是 A，
> 阶段 7 的文件级恢复要搬回 `repository/<用户>/<A>/…`（第 43 节）。用例
> `File.按名字删除用的是记录自己的Bucket` 就钉这一条。

---

# 22. File ID 生成

格式：

```text
fmt-YYYYMMDD-N
```

例如：

```text
fmt-20261005-0
fmt-20261005-1
fmt-20261005-2
```

生成流程：

```text
获取当前日期
 ↓
获取当天计数器
 ↓
原子增加
 ↓
生成 file_id
 ↓
检查唯一性
```

**实现（提交 `188e85d`，`FileService::next_file_id()`）**：不是内存计数器，也不是「条数」——

```text
前缀   "fmt-" + local_date_compact() + "-"      （local_date_compact() = YYYYMMDD）
扫描   同一份 file.json 快照里所有前缀相同的记录，取**当天已有 id 的最大序号 + 1**
       非数字尾巴（手工改过的 id）直接跳过，数字大到溢出也跳过，不影响别的记录
结果   fmt-<今天>-<N>，例如今天已有 fmt-20261008-0 / -2（-1 删过），下一个就是 fmt-20261008-3
```

两条纪律对着写：

```text
用「最大序号 + 1」而不是「条数」：删除过的 id 不许复用（第 23 节），
                                  否则删掉 -1 之后下一条又发 -1，历史日志就指错了
分配整段在业务锁内、且基于同一份快照：所以不会出现两个上传拿到同一个 N（第 24 节）
```

---

# 23. File ID 持久化

不能只依赖内存计数器。

程序重启后必须能够继续生成：

```text
fmt-20261005-N
```

且不能重复。

实现方式可以采用：

```text
持久化当天计数器
```

或：

```text
从已有 metadata 计算当天最大序号
```

最终实现方案在编码前确定。

**实现选的是第二种（提交 `188e85d`）**：没有单独的计数器文件，每次分配都从
`data/file.json` 的当前内容现算「当天最大序号 + 1」（第 22 节）。
这样即使程序异常退出、或用户手工改过 `file.json`，重启后也不会发出重复 ID——
状态只有 `file.json` 一处，不存在「计数器与记录不一致」的第二种失败模式。

要求：

> 即使程序异常退出，也不能产生重复 file_id。

---

# 24. File ID 并发安全

如果未来同时出现多个上传任务：

```text
Upload A
Upload B
Upload C
```

必须保证：

```text
A != B != C
```

不能出现：

```text
fmt-20261005-5
fmt-20261005-5
```

ID 分配必须是原子的。

**实现（提交 `188e85d`）**：整段「读 `file.json` → 算最大序号 → 加记录 → 写回」都在
运行体的同一把业务锁内（`ServerRuntime::mutex_`，`commit_upload()` 是第二阶段的入口）。
所以并发上传在现实里是**串行排队**的：后一个上传拿到的快照一定包含前一个刚写下的 id，
不可能拿到同一个 `N`。上传的**下载/复制部分在锁外**，但那一段不分配 id——
id 只在 `commit_upload()` 里生成（第 32、39 节）。

```text
① prepare_upload()   锁外：下载/复制到 temp/，边写边算 MD5      —— 不碰 file_id、不碰 file.json
② commit_upload()    锁内：MD5 去重 → 重名 → next_file_id() → 搬文件 → 写 file.json
```

---

# 25. 文件名校验

上传文件名必须经过：

```text
FilenameValidator
```

检查：

```text
空文件名
非法字符
路径分隔符
路径穿越
Windows 保留名称
与 file_id 同形（保留形状，提交 9c3d2cb）
长度限制
```

例如：

```text
../test.txt
```

拒绝。

```text
test/name.txt
```

拒绝。

---

**阶段 4 已落地**：`FilenameValidator` 以 `common/validation` 的形式实现，落在
`include/fmt/common/validation.hpp` 与 `src/common/validation.cpp`。对外接口：

```cpp
bool is_windows_reserved_name(std::string_view name);   // CON/PRN/AUX/NUL/COM1-9/LPT1-9，带扩展名也算

// 提交 9c3d2cb：名字是否与 file_id 同形（fmt-YYYYMMDD-N，前缀按 ASCII 折叠比较）。
// 这种名字会让「先按 file_id 查、查不到再按名字查」的定位产生歧义，所以是保留形状。
bool looks_like_file_id(std::string_view name);

inline constexpr std::size_t kMaxNameBytes = 255;       // 单个路径分量的字节上限

Status validate_bucket_name(std::string_view name);     // 失败一律 FMT-202 BucketNameInvalid
Status validate_file_name(std::string_view name);       // 失败按原因分工，见下表
```

两类名称共用同一套规则：非空、≤ 255 字节、不含路径分隔符（`/` 与 `\`）、不是 `.` 或 `..`、
不含控制字符（`< 0x20` 与 `0x7F`）、不含 `< > : " | ? *`、
**不含不可见格式字符（提交 `a9af276`，见下）**、不是 Windows 保留设备名、
不以点或空格结尾。中文等 UTF-8 名称合法（UTF-8 多字节序列的字节都 ≥ 0x80，不受控制字符
与非法字符规则影响）。

**错误码分工（文件名 ↔ Bucket 名）**：

| 情况 | 文件名（`validate_file_name`） | Bucket 名（`validate_bucket_name`） |
|---|---|---|
| 空 | `FMT-100 FileNameEmpty` | `FMT-202 BucketNameInvalid` |
| 含 Windows 非法字符 | `FMT-101 FileNameInvalidChar` | `FMT-202 BucketNameInvalid` |
| 含路径分隔符 / 是 `.` 或 `..` | `FMT-102 FileNameSeparator` | `FMT-202 BucketNameInvalid` |
| Windows 保留设备名 | `FMT-103 FileNameReserved` | `FMT-202 BucketNameInvalid` |
| 超长（> 255 字节） | `FMT-104 FileNameTooLong` | `FMT-202 BucketNameInvalid` |
| 以点或空格结尾 | `FMT-101 FileNameInvalidChar` | `FMT-202 BucketNameInvalid` |
| **含不可见格式字符**（`U+00A0` / `U+200B`–`U+200F` / `U+202A`–`U+202E` / `U+2060`–`U+2064` / `U+2066`–`U+2069` / `U+FEFF` 等，**提交 `a9af276`**） | `FMT-101 FileNameInvalidChar` | `FMT-202 BucketNameInvalid`（消息里点名码位，见第 33 节） |
| 与 `file_id` 同形（`fmt-YYYYMMDD-N`，**提交 `9c3d2cb`**） | `FMT-106 FileNameLikeFileId`（退出码 2） | —（Bucket 名不受限） |

文件名分成 `FMT-100～104`（外加提交 `9c3d2cb` 的 `FMT-106`）是为了让上传失败时能直接
告诉用户「哪里不对」；Bucket 名
只有 `FMT-202` 一个错误码（`FMT-203 BucketInUse` 留给「仍被引用」这种业务冲突，
不是名称校验）。两者的区别只是**错误码粒度**，判定规则完全一致。

**不可见格式字符不进名字（提交 `a9af276`）**：`validate_file_name()` 里
（非法字符之后、保留设备名之前）与 `validate_bucket_name()` 里（非法字符之后、
保留设备名之前）各加一道检查，命中就拒：

```text
清理函数    bool 判定用 common/string 的 invisible_characters(text)
            （按出现顺序**去重**返回码位名，形如 {"U+202A", "U+202C"}）
文件名      FMT-101 FileNameInvalidChar
            「文件名里有不可见字符（U+202A、U+202C），请把名字重敲一遍」
Bucket 名   FMT-202 BucketNameInvalid
            「Bucket 名称里有不可见字符（…），请把名字重敲一遍」
为什么      这种名字屏幕上看不出来、**用户没法重新敲一遍**，按名查找、排序、日志
            也全对不上；而且复制粘贴会一路带着它
```

> **注意与「清理」的分工**：位置参数与上传来源会被 `clean_user_path()` **清掉**
> 这些字符（第 33 节），所以正常的粘贴路径照常能用；但只要它们**留在名字里**
> （文件名 / Bucket 名），就是上面这两条错误——名字是要长期存下来、还要被用户
> 再敲一遍的东西，不能容忍看不见的字符。

**`FMT-106 FileNameLikeFileId`（提交 `9c3d2cb`，属 `FMT-1xx` 文件名校验一组）**：
判定放在**「Windows 保留设备名」之后**（`validate_file_name()` 里的顺序是
分隔符 → 非法字符 → 保留设备名 → 本形状 → 点/空格结尾），命中即拒、退出码 2，
默认消息「文件名与文件标识同形（fmt-YYYYMMDD-N），会与 file_id 混淆」：

```text
形状    fmt-YYYYMMDD-N
最短    14 个字符（4 + 8 + 1 + 1）
前缀    "fmt-" 按 ASCII 折叠比较（file_id 的比对本身就不区分大小写）
日期段  恰好 8 位数字
序号段  全是数字且非空（不限制位数）
拒绝    fmt-20261008-0 / FMT-20261008-0 / fmt-20261008-123
放行    fmt-20261008-0.txt（带扩展名就不是 id）/ my-fmt-20261008-0 /
        fmt-20261008（没有序号）/ fmt-2026100-0（日期 7 位）
```

**为什么是保留形状**：定位规则是「**先按 `file_id` 查、查不到再按名字查**」
（第 42/43 节，`locate_record()` / `get_by_id()` + `get_by_name()`）。
如果一个文件就叫 `fmt-20261008-0`，而另一个文件的 `file_id` 恰好是它，
那么**按名字提交的删除会先命中 id 那条记录、删错对象**。所以这个形状与
「Windows 保留设备名」（第 26 节）是**同一类规则**：名字虽然语法合法，但会被
定位逻辑吃掉，必须在上传时（**显式名与从来源推断的名字走同一道校验**）就拒绝。
旧数据里已经存在的这种名字不静默处理，见第 43 节的歧义判定与 `FMT-001`。
单元测试见 `tests/validation_test.cpp` 的 `Validation.与file_id同形的文件名被拒`。

---

# 26. Windows 保留名称

必须考虑：

```text
CON
PRN
AUX
NUL
COM1
COM2
...
LPT1
LPT2
...
```

即使增加扩展名也需要按照 Windows 文件名规则处理。

---

**阶段 4 已落地**：上面这份名单由 `common/validation` 的
`is_windows_reserved_name()` 一处实现（`include/fmt/common/validation.hpp`），
`validate_bucket_name` 与 `validate_file_name` 都调用它，不在业务模块里各写一份。

```text
判断方式    取第一个 '.' 之前的部分做主干，ASCII 大小写不敏感比对
名单        con / prn / aux / nul / com1..com9 / lpt1..lpt9
带扩展名    `CON.txt`、`Com1.log` 同样判为保留名（主干命中即拒）
不误伤      `CONSOLE`、`COM0`、`工作` 都合法
```

命中时的错误码：文件名 → `FMT-103 FileNameReserved`；Bucket 名 → `FMT-202 BucketNameInvalid`
（见第 25 节的分工表）。单元测试见 `tests/validation_test.cpp`。

---

> **与「保留形状」的关系（提交 `9c3d2cb`）**：本节是**保留名字**的第一类（设备名），
> 第二类是 `fmt-YYYYMMDD-N`（`FMT-106 FileNameLikeFileId`，见第 25 节）。
> 两者同类——名字本身合法，但会被文件系统或定位逻辑吃掉，所以都在
> `validate_file_name()` 里拒绝；区别只是前者由 Windows 定义、后者由本项目的
> 「先按 `file_id` 查、再按名字查」定位规则定义。Bucket 名只有设备名这一类限制。

---

# 27. Bucket Service

Bucket Service 负责：

```text
create
list
get
use
delete
```

主要职责：

```text
Bucket 名称校验
Bucket 是否存在
当前 Bucket
Bucket 删除（移入回收站，目录名一律带时间戳）
Bucket 回收站列表 / 回退（桶级，阶段 4 已落地）
Bucket 回收站条目详情 / 永久删除（桶级，提交 4fee290 已落地）
```

---

**阶段 4 已落地**（`include/fmt/bucket/bucket.hpp`、`src/bucket/bucket.cpp`）。
Bucket **就是 `repository/<user>/<bucket>/` 这个目录本身**，没有独立 ID：存在性 = 目录存在；
`current_bucket` 存在 `config.json` 里。真实接口：

```cpp
// 回收站里桶级记录的权威文件名（位于 trash/<user>/ 下）
inline constexpr const char* kOriginalIndexName = ".original";

struct BucketInfo { std::string name; bool is_current; };

// 创建结果（提交 9c3d2cb）：名称统一转小写后再建目录，调用方据此提示用户
struct BucketCreation {
    std::string requested;       // 用户原本敲的（可能带大写）
    std::string name;            // 实际创建的名字（小写）
    bool renamed;                // requested != name
    bool became_current;         // 是不是顺手设成了当前 Bucket
};

// 删除结果：删到哪儿去了（回收站里的名字）、影响了几个文件、删的是不是当前 Bucket
struct BucketRemoval {
    std::filesystem::path moved_to;
    std::string trashed_name;   // trash/<user>/<trashed_name>
    std::size_t files_affected;
    bool was_current;
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

// 回收站里的一个桶条目：directory_present=false 表示索引里有、目录没了（只报告，不擅自清理）
struct TrashBucket {
    std::string trashed_name;
    std::string original_name;      // 索引里没有则为空，此时拒绝回退
    std::string deleted_at;
    bool directory_present;
};

// 单个回收站条目的详情（trash get 用，提交 4fee290）
struct TrashBucketDetail {
    TrashBucket bucket;
    std::filesystem::path directory;
    std::size_t file_count;          // 目录里的实际文件数
    std::uintmax_t byte_count;       // 总字节数
};

// 永久删除之后的结果（trash delete 用，提交 4fee290）
struct TrashPurge {
    std::string trashed_name;
    std::string original_name;
    std::size_t removed_files;       // 真正删掉的磁盘文件数（删前统计）
    std::size_t removed_records;     // file.json 里清掉的记录数
};

class BucketService {
public:
    BucketService(const PathManager& paths, Config& config, Logger* logger);

    Result<BucketCreation> create(std::string_view name);        // 提交 9c3d2cb：原为 Status
    Result<std::vector<BucketInfo>> list();
    Result<BucketInfo> get(std::string_view name);
    Status use(std::string_view name);
    Result<BucketRemoval> remove(std::string_view name);        // 注意：不是 Status
    Result<BucketDeleteCheck> check_remove(std::string_view name) const;  // 预检（0fc242b，只读）
    Result<std::vector<TrashBucket>> list_trashed();            // 桶级回收站列表
    Result<TrashBucket> restore(std::string_view identifier);   // 桶级回退（整单搬回 / 合并）
    Result<BucketMergeCheck> check_merge(std::string_view identifier) const;  // 合并预检
    Result<BucketMerge> merge(std::string_view identifier);     // 合并（不覆盖、不改名）
    Result<TrashBucketDetail> get_trashed(std::string_view identifier);  // 条目详情（4fee290）
    Result<TrashPurge> purge(std::string_view identifier);      // 永久删除（4fee290）
    std::filesystem::path directory_of(std::string_view name) const;
    Status refresh_current_bucket();

private:
    // 在索引里定位条目：先按回收站里的名字精确匹配，再按原桶名（必须唯一）。
    // **找不到不算错**（found=false）——孤儿目录要能走到「按目录名处理」那条路。
    struct TrashLookup { bool found; std::size_t index; };
    Result<TrashLookup> find_trashed(const std::vector<TrashBucket>& entries,
                                     std::string_view identifier) const;
    // 永久删除时清 file.json 记录：只清 trash_reason == "bucket" 的那些
    Status remove_bucket_file_records(std::string_view bucket_name, std::size_t* removed);
};
```

要点：

```text
create    名称先 to_lower()（只折叠 ASCII，中文不受影响，提交 9c3d2cb）→ 名称校验（失败 FMT-202）
          → 已存在则 FMT-201 → 建 repository/<user>/<小写名>/
          → 若 current_bucket 为空则设为它并保存配置；返回 BucketCreation
list      列出 user_root 下的目录，按名称排序，标出 current_bucket
get       不存在则 FMT-200；name 回**磁盘上的实际名字**（canonical_name()，
          `get WORK` 显示 work），is_current 用 iequals 比对
use       不存在则 FMT-200 → 落盘时把名称规范化成磁盘上的实际名字
          （canonical_name()：不区分大小写地扫桶目录），`use WORK` 存进的是 "work"
remove    不存在则 FMT-200 → 一律用磁盘上的实际名字（canonical_name()）去搬目录、
          写 .original、清 file.json 记录：整个目录移到 trash/<user>/<实际名字>_<时间戳>/
          → 在 .original 追加一条桶级记录（original 是实际名字）→ 该桶 file.json 记录
          置 is_trash=true 且 trash_reason="bucket" → 若删的是当前 Bucket 则置空、绝不自动切换。
          **提交 0fc242b**：删非空桶前由 CLI 先发 check_remove 预检（缺 force 时服务端
          自己返回 FMT-016 ConfirmRequired，见第 30 节）；成功消息末尾补
          「（之后只能整体恢复这个桶）」
check_remove  私有预检（提交 0fc242b，只读）：canonical_name 定位 → 数目录里的普通文件与
              字节数 → has_content = files > 0；有内容时 message 列出文件数与占用并写明
              「删除后整个桶移入回收站，之后只能整体恢复这个桶，无法只恢复其中某个文件」，
              是当前桶再补一句「当前 Bucket 会被置空」；空桶只有当前桶时给一句置空提示
list_trashed  索引 + 目录扫描：索引有目录没了标 present=false；目录有索引没有则 original
              留空照样列出（都不擅自清理）。**扫描形状检查（4fee290）**：跳过点开头的
              条目，且只认 <名字>_<14 位时间戳>（可带 _<1-3 位序号>）形状的目录
restore   目标 Bucket 不存在 → 整个目录一次 rename 搬回（见第 56 节）；
          **目标 Bucket 已存在 → 合并**（第 56.3 节）：把回收站目录里的文件逐个搬进
          已存在的桶，不覆盖、不改名，先要用户确认（`force`，缺则 FMT-016）；
          现有桶里有同名文件就一个文件都不搬 → FMT-403 RestoreMergeConflict
check_merge  合并的预检（只读，第 56.3 节）：数「要搬几个、目标里已有几个、占多少」，
          并列出每一个同名冲突的名字；目标不在、名字拼写不一致也在这里说清楚
merge      合并的执行体：逐个 move 文件 → 全部搬完才摘 .original → 只把真正搬过去
          的 file.json 记录翻回正常 → 收掉只剩空目录的回收站条目；
          中途搬失败的那些留在回收站、记录保持 is_trash=true（下次还能再来一次）
get_trashed  单个条目详情（4fee290）：定位与 restore 完全一致（共用 find_trashed）；
             索引有目录没了不报错，返回 present=false；孤儿目录可按目录名查到；
             两者都没有则 FMT-400 TrashEntryNotFound。**这条会遍历目录**数文件数与
             字节数（提交 18f16ca 起 `trash list` 里每个 present 的桶级条目也走它，
             见第 53.1 节）
purge     永久删除（4fee290）：① remove_all 回收站目录 ② 只清 file.json 里
          trash_reason=="bucket" 的记录 ③ 摘 .original 那条。顺序刻意（失败可重来）；
          幽灵条目也能删。服务端要求请求带 force==true（见第 59、60 节）
find_trashed  私有，restore / get_trashed / purge 共用同一套定位：回收站名字优先，
              再用原桶名且必须唯一（多条 → FMT-001 并列候选）；**找不到不算错**
canonical_name  私有（提交 9c3d2cb）：不区分大小写地扫 user_root 下的目录，
              返回磁盘上的实际条目名；找不到才退回 to_lower(name)
refresh    current_bucket 非空但目录不存在时置空并保存；绝不自动选择别的 Bucket
日志       模块名统一 `Bucket`（INFO：创建 / 切换 / 删除 / 回退；创建时名称被转换会在日志里注明「（名称统一小写，由 WORK
            转换）」；WARN：当前 Bucket 失效置空）；
           永久删除记在 `Trash` 模块下（INFO：条目名 + 文件数 + 记录数，4fee290）
```

**名称统一小写（提交 `9c3d2cb`）**：Windows 目录不区分大小写，`WORK` 与 `work` 本来就是
同一个目录。不统一拼写，`current_bucket`、`file.json` 的 `bucket`、`.original` 的 `original`
就会各留一份，日后比对与恢复都会踩坑。所以：`create` 先 `to_lower()` 再校验再建目录；
`use` / `get` / `delete` 一律经 `canonical_name()` 规范化到**磁盘上的实际名字**。
`to_lower()` 只折叠 ASCII（`>= 0x80` 的字节原样保留），中文桶名不受影响；
`iequals()` 同样只折叠 ASCII（见第 42、43 节与 `FMT 技术文档.md` 第 9.4 节）。

`remove` 返回结构化结果而不是 `Status`，是因为调用方（管道与 HTTP 响应）必须如实回报
「移到哪儿、影响了几个文件、删的是不是当前 Bucket」，而不是只说一句「成功」（见第 30 节）。

---

# 28. 创建 Bucket

命令：

```text
bucket create <name>
```

流程：

```text
输入 Bucket 名称
 ↓
名称统一转小写（to_lower()，只折叠 ASCII，提交 9c3d2cb）
 ↓
名称校验
 ↓
检查当前用户是否已经存在
 ↓
创建目录
 ↓
写入相关状态
 ↓
如果是第一个 Bucket
     ↓
current_bucket = name
```

**阶段 4 实现口径（已落地）**：

```text
0. 名称先 to_lower()（提交 9c3d2cb，只折叠 ASCII，中文不受影响）：requested = 用户敲的，
   name = 小写形式；renamed = (requested != name)；以下各步一律用 name
1. current_user 为空 → FMT-604（正常路径不会发生：initialize_root 已置占位名 user，
   只有用户被显式清空才走这里，见第 18、93 节）
2. validate_bucket_name 失败 → FMT-202 BucketNameInvalid（规则见第 25、26 节）
3. repository/<user>/<name>/ 已存在 → FMT-201 BucketAlreadyExists
   （所以再敲一次 `create WORK` 会被当成同一个桶 → FMT-201，不会建出第二个）
4. 建目录：先确保 repository/<user>/，再建 <name>/（FMT-013 由 storage 返回）
5.「第一个」的判定就是 current_bucket 为空：为空则设为新 Bucket 并保存 config.json，
   同时把 became_current 置真；非空时**不抢走**「当前」（第二个及以后的 Bucket
   不会自动成为当前）
6. 成功记一行 INFO 日志（模块 Bucket；renamed 为真时注明「（名称统一小写，由 WORK 转换）」），
   返回 BucketCreation；响应 data 见 `FMT 技术文档.md` 第 12.3.2 节——
   `bucket` 是**实际建成的名字**（work），`renamed` 为真时**增加 `note`** 让 CLI 提示用户
```

`create` 不校验「用户是否已经存在」这种用户系统语义——V1 没有用户系统，
`current_user` 非空即可（占位名 `user` 也算存在）。

---

# 29. Bucket Use

命令：

```text
bucket use <name>
```

流程：

```text
检查 Bucket
 ↓
确认存在
 ↓
current_bucket = 磁盘上的实际名字（canonical_name()，提交 9c3d2cb）
 ↓
保存 config.json
```

`bucket use` 不修改 Bucket 本身。

**阶段 4 实现口径（已落地）**：Bucket 不存在 → `FMT-200 BucketNotFound`；名称为空 →
`FMT-001 InvalidArgument`；「存在」的判定就是 `repository/<user>/<bucket>/` 目录存在。
成功路径只做两件事：改内存里的 `current_bucket`、把 `config.json` 落盘——**不动 Bucket
目录、不移动文件、不改 file.json**。日志记一行 INFO。`use` 不做名称合法性校验：不合法的
名字不可能存在，走到「不存在」分支即可，用户拿到的仍是 `FMT-200`。

**落盘前规范化（提交 `9c3d2cb`）**：`use` 存进 `current_bucket` 的是
`canonical_name(name)`——**不区分大小写地扫 `repository/<user>/` 下的目录、返回磁盘上的
实际条目名**，所以 `use WORK` 存进去的是 `work`（目录本来就叫 `work`）。找不到同名目录时
才退回 `to_lower(name)`（这条兜底在 `use` 里走不到，因为前面已经确认目录存在）。
理由与第 27 节同：不统一拼写，`current_bucket`、`file.json` 的 `bucket`、`.original` 的
`original` 会各留一份，restore 拼出来的目录名就会与原来不一致。
`bucket use` 的响应里 `bucket` 也是这个规范化后的名字；值被改过时 `data` 里**增加 `note`**
（「Bucket 名称统一使用小写：已把 WORK 规范为 work」），CLI 打成单独一行 `提示：…`。

---

# 30. Bucket Delete

命令：

```text
bucket delete <name>
```

流程：

```text
确认 Bucket
 ↓
检查当前用户
 ↓
移动 Bucket 数据到 Trash
 ↓
相关文件 is_trash = true
 ↓
保存 Trash metadata
 ↓
如果删除的是 current_bucket
     ↓
current_bucket = ""
```

不会自动切换到其他 Bucket。

**阶段 4 实现口径（已落地；回收站形状见 commit c2d545d）**。删除是**移入回收站**，
不是丢弃数据；顺序与上面的流程图一致（顺序里**索引先读、目录后搬、索引最后写**是有意的）：

```text
0. 预检（提交 0fc242b，只读）：CLI 先发一次带 dry_run 的 bucket.delete，
   BucketService::check_remove() 数出桶里的文件数与占用并给出提醒。
   有内容 → needs_confirm = true；**空桶不打扰用户**（needs_confirm = false）。
   服务端兜底：请求里没有 force == true 且桶里有内容 → FMT-016 ConfirmRequired
   （「<预检消息>；确认删除请加 force（CLI：--yes）」）。预检被绕过也一样拦得住。
1. current_user 为空 → FMT-604；名称为空 → FMT-001；Bucket 不存在 → FMT-200
1.5 名称规范化（提交 9c3d2cb）：actual = canonical_name(name)，即**磁盘上的实际桶名**；
   下面第 3～6 步与 was_current、回收站目录名、.original 的 original 一律用 actual
   （`bucket delete WORK` 时回收站目录名与 original 都是 work_<时间戳> / work，
    以后 restore 出来的目录拼写才一致）
2. 先读 trash/<user>/.original：**读不出来就到此为止**（FMT-006 JsonParseError），
   一个字都不搬——否则会留下一个「没有身份记录、原名永久丢失」的条目
3. 记下 was_current = iequals(config.current_bucket, actual)
4. 目录名**一律**带删除时间戳：<原桶名>_<YYYYMMDDHHMMSS>（例如 lazy-fox_20261008012233），
   无论有没有重名；trash/<user>/ 下已有同名就再加序号 _2、_3……，绝不覆盖已有条目。
   把整个目录 rename 过去：repository/<user>/<bucket>/ → trash/<user>/<bucket>_<时间戳>/
5. 在 .original 的 buckets 数组末尾追加一条记录（trashed / original / deleted_at，
   见第 17.2 节）；**写不进去就把目录搬回原位**并报错——磁盘与索引不允许不一致
6. 相关文件 trash_reason = "bucket"：只改 file.json 里 **user 与 bucket 都匹配**、
   且 is_trash 原本为 false 的记录（同时置 is_trash=true）；别的 Bucket 的记录、
   以及用户自己删过的文件（trash_reason="file"）一律不动
   （file.json 缺失时视为 0 个文件，不报错）
7. 若 was_current：current_bucket 置空并保存；**不自动切换到别的 Bucket**
   ——真机核实（2026-10-09）：置空之后下一条要当前桶的命令会报
   **`FMT-305 未设置当前 Bucket`**（退出码 3，例如 `file list`）
8. 返回 BucketRemoval{moved_to, trashed_name, files_affected, was_current}——调用方据此
   如实回报；`message` 末尾补一句「（之后只能整体恢复这个桶）」（提交 0fc242b）。
   管道与 HTTP 的 data 字段见 `FMT 技术文档.md` 第 12.3.2 节
```

**为什么删桶要先提醒（提交 `0fc242b`）**：桶级回退要么**整单搬回**（目标不在），
要么**合并**（目标已存在，第 56.3 节）——两条路都不做「只恢复其中一个文件」，
桶里那些文件在删除后 `trash_reason == "bucket"`，数据躺在
`trash/<用户>/<桶>_<时间戳>/` 里而不是 `.files/` 下——**没法只把其中某个文件拿出来**。
所以有内容时先告诉用户「N 个文件、多大、之后只能整体恢复这个桶」（是当前桶再说一句
「当前 Bucket 会被置空」），确认了才删；空桶没有这个代价，不打扰用户。

**为什么目录名一律带时间戳**（旧口径是「用原名、重名才加」）：同一个桶删两次时，两条回收站
条目如果都记着「原名 = lazy-fox」，回退就必然撞车。用户提的完整场景：

```text
删 lazy-fox（空桶）        → lazy-fox_20261008012233
再建 lazy-fox、放文件、再删 → lazy-fox_20261008020304
回退第一个（空桶）          → repository/user/lazy-fox 重建，往里放新文件
回退第二个（带文件的）      → 目标 lazy-fox 已存在
                             → 旧口径：FMT-401 整单拒绝，不碰第一个
                             → 现口径（第 56.3 节）：先说清「合并几个文件」，
                               用户确认后把文件合并进去（不覆盖、不改名）；
                               有同名文件则 FMT-403、一个都不搬
```

一律带时间戳 + `.original` 记原名，两件事各自独立，谁都不会被覆盖，也不会认错身份。

`FMT-203 BucketInUse`（「仍被引用，不能删除」）**保留（V1 未使用）**——提交 `674d0b0`
之后口径写死为这一句：**当前没有任何代码会产生它**。
V1 允许删除仍有文件的 Bucket（数据一并移入回收站，文件记录标记 `is_trash` 并写
`trash_reason="bucket"`）。原本设想它用于「桶仍被引用」，但实际路径分别被
**`FMT-401`（回退冲突）**、**`FMT-402`（原桶已删）**、**`FMT-016`（删非空桶要确认）**
覆盖了，所以没有产生它的代码。**编号语义冻结、不删行**，但也**不要假装它会被返回**。

---

# 31. File Service

File Service 负责：

```text
upload
list
get
download
delete
```

核心原则：

```text
File Service
```

负责业务规则。

底层：

```text
Storage
```

负责实际文件操作。

**实际接口（提交 `188e85d`，`include/fmt/file/file.hpp`）**：

```cpp
namespace fmt {

// file.json 的一条记录（第 14 节）
struct FileRecord {
    std::string file_id;
    std::string user;
    std::string bucket;
    std::string file_name;
    std::string extension;     // 小写、含点；没有扩展名时为空
    std::string file_type;     // image / text / video / archive / other
    std::uintmax_t size = 0;
    std::string md5;           // 32 位小写十六进制
    bool is_trash = false;
    std::string trash_reason;  // "bucket" / "file" / 空
    std::string deleted_at;    // 进回收站的时间（提交 0fc242b；file.json 是唯一权威）
};

// 「要删的到底是哪一个、有没有要先说清楚的情况」——**预检，零副作用**（提交 711da4c）。
// 目标唯一但在别的 Bucket → other_bucket（可确认）；名字与 file_id 撞在两条不同记录上
// → ambiguous（**y/N 解决不了**，候选摊在 candidates 里，只能改用 file_id）。
struct FileDeleteCheck {
    std::string file_id;
    std::string file_name;
    std::string bucket;          // 目标所属 Bucket
    std::string current_bucket;
    std::string path;            // 能定位到时给出（相对数据根、正斜杠）
    bool other_bucket = false;
    bool ambiguous = false;
    std::vector<FileRecord> candidates;
    std::string message;
};

// 上传第一阶段的产物：数据已经完整落在 temp/ 下，大小与 MD5 已知
struct PreparedUpload {
    std::filesystem::path temp_path;
    std::string file_name;
    std::uintmax_t size = 0;
    std::string md5;
};

// 阶段一（**锁外**，长耗时）：下载或复制到 <数据根>/temp/，边写边算 MD5、边判大小上限
Result<PreparedUpload> prepare_upload(const PathManager& paths, const std::string& source,
                                      const std::string& name, std::uintmax_t size_limit,
                                      Logger* logger);

// 从来源推断文件名（第 32 节）；推不出来返回空串
std::string file_name_from_source(std::string_view source);
// 小写扩展名（含点）；无扩展名返回空串
std::string extension_of(const std::string& file_name);

class FileService {
public:
    FileService(const PathManager& paths, Config& config, Logger* logger);

    // 阶段二（**锁内**，快）：去重、重名、file_id、移动到仓库、写 file.json。
    // 接手之后临时文件的生命周期归它管：任何失败路径都会删掉它（第 35 节）。
    Result<FileRecord> commit_upload(PreparedUpload& prepared);

    Result<std::vector<FileRecord>> list();                       // 第 41 节
    Result<FileRecord> get_by_id(std::string_view file_id);        // 第 42 节（比较不区分大小写）
    Result<FileRecord> get_by_name(std::string_view file_name);    // 第 42 节
    Result<FileRecord> remove(std::string_view file_id_or_name);   // 第 43 节，软删除
    Result<FileDeleteCheck> check_remove(std::string_view file_id_or_name) const;  // 预检（711da4c）

    // 回收站读侧（提交 0fc242b）：只列**文件级**条目（trash_reason == "file"，
    // 随桶删除的整棵树挂在桶级条目下，不在这里重复计数）
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
    Result<FileRestoreCheck> check_restore(std::string_view file_id) const;  // 只读预检
    Result<FileRecord> restore(std::string_view file_id);   // 搬回仓库 + 复位 is_trash/trash_reason/deleted_at
    Result<FileRecord> purge(std::string_view file_id);     // 永久删除：删数据 → 删记录 → 清老 trash.json

    Result<std::filesystem::path> resolve_path(const FileRecord& record) const;
    Result<std::filesystem::path> trash_path_of(const FileRecord& record) const;

private:
    Result<std::vector<FileRecord>> load_records() const;
    Status save_records(const std::vector<FileRecord>& records) const;
    std::string next_file_id(const std::vector<FileRecord>& records) const;  // 第 22 节
    // 记录**按规则推**出来的仓库目标路径（file_id 定日期，记录定用户/桶/文件名）；
    // 回退时往这里搬回去（提交 0fc242b）
    Result<std::filesystem::path> repository_path_of(const FileRecord& record) const;
    std::filesystem::path bucket_path() const;
    const PathManager& paths_;
    Config& config_;
    Logger* logger_ = nullptr;
};

}  // namespace fmt
```

> `download` 还是**设计里的一步、尚未实现**：它属于阶段 6 的 HTTP 浏览器侧
> （`GET /file/download/<文件名>`，`FMT 技术文档.md` 第 12.3.1 节）。
> 阶段 5 落地的只有 `upload` / `list` / `get` / `delete` 四条（第 32、41～43 节）。

---

# 32. File Upload

命令（**实际语法，提交 `188e85d`**：比旧文多一个可选的文件名参数）：

```text
file upload <来源> [文件名]
```

**来源**两条都支持，与「CLI 传来源、不传内容」的冻结口径一致（第 95 节、
`FMT 技术文档.md` 第 12.3.2.1 节）：

```text
http:// 或 https:// URL  由服务下载（提交 a2b6cd1 起走 common/http_client 的
                        WinHTTP + Schannel 客户端，流式写进 temp/；第 33 节）
本机文件路径            由服务直接读盘复制（流式 64 KiB 分块）
```

**文件名可省略**：省略时从来源推断（第 33 节的 `file_name_from_source()`）；
推不出来（例如 `http://example.com/`）返回 `FMT-100 FileNameEmpty`，
提示「无法从来源推断文件名，请显式给出文件名」——**必须**由用户显式给名字，
绝不拿主机名凑一个。

**上传是两段式（本轮最重要的设计决定）**：下载可能几十秒到几分钟，
**持着运行体那把业务锁去下载会把 `bucket list`、`trash *` 和浏览器请求一起卡住**，
所以长任务不持锁，锁只保护元数据提交那一下：

```cpp
// ① 锁外：长耗时。把来源流式写进 <数据根>/temp/fmt-upload-<随机>-<序号>.tmp，
//    边写边算 MD5、边写边判大小上限；任何失败都删临时文件（第 34、35 节）
Result<PreparedUpload> prepare_upload(const PathManager& paths, const std::string& source,
                                      const std::string& name, std::uintmax_t size_limit,
                                      Logger* logger);

// ② 锁内：快。去重 → 重名 → 分配 file_id → 移动到仓库 → 写 file.json
Result<FileRecord> FileService::commit_upload(PreparedUpload& prepared);
```

运行体 `ServerRuntime::run_upload()` 就是这两段的编排，**管道与 HTTP 共用这一份**
（`file.upload` 走管道时由 `handle()` 直接调它；浏览器侧的 `POST /api/file/upload`
——**提交 `d3aeb3d` 起改成流式，请求体就是文件内容**——也走它）：

```text
① 锁下取快照   paths / logger / size_limit（只拿必须的东西，随即放锁）
② 锁外 prepare 下载或复制到 temp/，算 MD5、判大小
③ 锁内 commit  登记入库；临时文件的生命周期从这一刻起归 commit_upload 管
```

**换根不会在下载期间发生**：换根只由 `hello` 触发，而管道的 accept/serve 是串行的
（`FMT 技术文档.md` 第 15.1 节①），上传期间不会再处理第二个请求——所以第 ①
步拿到的快照在整段下载期间都成立。HTTP 侧没有这个问题：每个请求的处理器
自己取当前 `context_`，而真正的写操作只在第 ③ 步的锁内发生。

> **HTTP 流式上传走的是同一个「第二段」（提交 `d3aeb3d`）**：`POST /api/file/upload`
> 的请求体**就是文件内容**，HTTP 处理器用 `ContentReader` **边收边写**到
> `temp/fmt-upload-<pid>-<序号>.tmp`（**边判上限**：超了立刻中止接收并删暂存文件，
> `FMT-303` → 400），落盘后调 `prepare_staged_upload(paths, staged, name, size_limit,
> logger)` 补算大小与 MD5，再走下面这套入库——**全程只有一次移动，不二次拷贝**
> （这正是大文件走流式上传的意义）；失败路径一律删暂存文件。
> 对应的 op 是 **`file.upload_stream`**（`args.argv = [暂存路径, 文件名]`），
> 它额外待办的一件事：**没给文件名 → `FMT-100`（400）**
> （`?name=` 与 `Content-Disposition` 都没给时；`FMT 技术文档.md` 第 12.3.2、18.40 节）。
>
> **本次改动（未提交）给这条路由加了第二种请求体：`multipart/form-data`**（Postman 的
> form-data、浏览器表单、Spring 的 `MultipartFile` 都是这个形状）——取**第一个带 `filename`
> 的 part** 的正文，普通文本字段读掉但不落盘，多个文件 part → `FMT-001`，没有文件 part →
> `FMT-100`，请求体没收完 → `FMT-701` 并丢弃暂存文件。**与裸字节形态共用同一条链路**
>（下面这套入库、`file.upload_stream` 那一段**一个字节都没改**）；顺手把「连接断在中途
> 会把半截文件当成功入库」这个真问题一起修了（两条路都修）。在它之前，multipart 打到这个
> 路由会掉进 httplib 的解析器、以**空 500 + `EXCEPTION_WHAT: bad_function_call`** 收场
>（`FMT 技术文档.md` 第 12.3.2 节）。
>
> **同一轮还把上限真正接通了（本次改动（未提交））**：`HttpServer::start` 的最后一个参数
> 从「一个数」改成 **`UploadLimitProvider`（取值器）**，运行体注入的那个**每次上传请求现读**
> `context_->config.max_upload_size`（锁内只读一个字段，不干别的）——这样
> **`config set max_upload_size` 之后不用重启服务**（拿启动时的快照会让「调大上限」当场失效：
> HTTP 层还按旧的小值中止接收）。在这之前运行体**压根没传**这个参数，HTTP 层拿到的是默认
> `0`（= 这一层不判），所以「边收边判」只存在于文档与测试里，线上所有超限都是收完之后由
> 业务层拒的。同时上限改判**收到的字节**：multipart 的普通字段也计入（不计的话 multipart
> 就成了绕过上限的口子）。新增用例 **`Server.上传上限每次请求现取`**：同一个服务实例上
> 把上限调大 → 下一次请求立刻能过，调小 → 下一次请求立刻 400（都是 HTTP 层那句消息）。

完整流程（两段式落地后的样子）：

```text
第一段（锁外，第 34、35 节）
0.  粘贴污染清理（提交 a9af276，第 33.1.1 节）：来源与显式文件名先过 clean_user_path()
    ——去掉不可见格式字符与**成对**引号；`argument()` 那层已经清过一次
1.  缺少来源 → FMT-300 UrlInvalid
2.  文件名：用参数给的，或从来源推断；推断为空 → FMT-100
3.  文件名合法性校验（第 25 节，FMT-100～104 + 提交 9c3d2cb 的 FMT-106 +
    提交 a9af276 的「不可见字符 → FMT-101」）
4.  来源分支：http:// 与 https:// 都交网络下载（`common/http_client` 的 WinHTTP 客户端）；
    含 "://" 但不是 http/https → FMT-300；本地路径不存在 → FMT-002 FileNotFound（第 33 节；
    清掉过不可见字符时消息会点名码位）
5.  创建 <数据根>/temp/fmt-upload-<随机>-<序号>.tmp
6.  流式写入（64 KiB 一块），边写边算 MD5、边判大小上限（超限即中止，不留半成品）
7.  完整性检查：服务器给了 Content-Length 就必须对上（对不上 → FMT-301 DownloadFailed）；
    超过大小上限 → FMT-303 SizeLimitExceeded（边写边判，不是下完再看）
8.  临时文件落盘、拿到 size 与 32 位小写 MD5；失败一律删临时文件

第二段（锁内，第 37～40 节）
9.  检查当前用户（FMT-604）、current_bucket（FMT-305）、桶目录是否存在（FMT-200）
10. 桶内判重（本次改动（未提交），第 37/38 节）：**只看当前用户的当前 Bucket** 里的正常文件，
    **先看内容、再看名字**（四条口径见第 37 节）——
    内容相同（名字同与不同都一样）→ **FMT-304**「文件已存在」，**不提改名**；
    内容不同 + 名字相同 → **FMT-105**「文件名重复」，**要提示修改文件名**；
    内容不同 + 名字不同 → 正常入库
11. 判重的作用域是「当前 Bucket」，不是「同用户跨 Bucket」（本次改动（未提交）；
    **同名不区分大小写**，提交 `5bf2c1f`）
12. 生成 file_id（第 22 节）→ 由它推出日期目录
13. 移动临时文件到 repository/<user>/<bucket>/YYYY/MM/DD/（第 39 节）
14. 写入 file.json；写不进去就把刚提交的仓库文件删掉（第 40 节）
15. 完成，记录 INFO 日志「入库：<file_id> <file_name> -> <路径>」
```

---

# 33. Upload URL

**实况（提交 `a2b6cd1`）：`http://` 与 `https://` 都支持，而且不引 OpenSSL、不分发任何 DLL。**
本节原先写的是「V1 只支持 `http://`，`https://` 不支持」——**该口径已被 `a2b6cd1` 推翻**
（那次提交的标题就是「feat(http): download over WinHTTP instead of OpenSSL, so https works」），
下面全部改成实况。

```text
http://        ✅ 支持（WinHTTP：10 秒连接超时、30 秒发送超时、300 秒接收超时，跟随重定向）
https://       ✅ 支持（同一客户端；TLS 走系统 Schannel，证书用系统证书库）
本机文件路径    ✅ 支持（流式读取，见第 32 节）
ftp:// 等其它  ❌ 不支持 → FMT-300 UrlInvalid（「只支持 http:// 与 https:// 的来源：<来源>」）
带用户名密码的 URL ❌ 不支持 → FMT-300 UrlInvalid（「URL 不支持带用户名密码的形式」）
```

## 33.1 判定顺序（`prepare_upload()`，逐行照源码）

```text
http:// 或 https://        -> 走网络下载（is_remote_url()）
含 "://" 但不是 http/https  -> FMT-300 UrlInvalid（"只支持 http:// 与 https:// 的来源"）
其余                       -> 本地路径，存在性检查不过报 FMT-002 FileNotFound
                              （清过不可见字符时消息点名码位，见 33.1.1）
```

## 33.1.1 粘贴路径的污染：清掉、并在报错时点名（提交 `a9af276`）

**用户报的原始现象**（照实记录）：

```text
fmt> file upload ‪C:\Users\lenovo\Pictures\pet-food-store\头像\asdva.jpg
执行失败：FMT-002 本地文件不存在：‪C:\Users\lenovo\Pictures\pet-food-store\头像\asdva.jpg
错误码：3
```

文件**确实存在**（130184 字节）。原因是路径首尾各夹了一个**不可见的方向格式字符**：
开头 `U+202A`（LEFT-TO-RIGHT EMBEDDING）、结尾 `U+202C`（POP DIRECTIONAL FORMATTING）
——从聊天窗口、网页、终端复制路径时常见，**屏幕上完全看不出来**。
CLI 走 `wmain`，中文路径本身没问题；就是这两个字符被当成了路径的一部分。

**清理函数（`include/fmt/common/string.hpp` / `src/common/string.cpp`）**：

```cpp
// 清掉粘贴污染：不可见格式字符 + **成对**引号（Explorer「复制路径」会给路径套一对引号）
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

**在哪里生效（一处收口 + 一处纵深防御）**：

```text
① src/service/commands.cpp 的 argument()   —— **所有**位置参数读取的唯一入口，
   CLI 与 HTTP 共用；粘贴污染在这里就被清掉（一处收口）
② src/file/file.cpp 的 prepare_upload()   —— 上传**来源**与**显式文件名**再清一次，
   即便将来有人绕过 argument() 直接调它也不受影响（纵深防御）
```

**清掉之后仍然找不到时的报错（点名码位，第 35 节与第 127.6 节也有样例）**：

```text
执行失败：FMT-002 本地文件不存在：C:\...\asdva.jpg（你粘贴的路径里有不可见字符
U+202A、U+202C，它会让路径对不上；已自动清掉，请检查路径是否还有别的问题）

只去掉首尾空白或引号、没有不可见字符时：
执行失败：FMT-002 本地文件不存在：<清理后的路径>（已去掉粘贴带进来的引号或空白）

什么都没清掉时：还是原来那句「本地文件不存在：<路径>」
```

> **名字里的不可见字符不允许**（提交 `a9af276`）：`validate_file_name()` →
> `FMT-101 FileNameInvalidChar`、`validate_bucket_name()` → `FMT-202 BucketNameInvalid`
> （第 25 节有完整说明）。理由：名字要长期存下来、还要被用户再敲一遍，
> 屏幕上看不出来的字符没法重敲，按名查找、排序、日志也全对不上。
> **别写成「上传路径按原样使用」**——路径会被清，名字会被拒，两者不同。

> **写给排错的人（重要）**：`ftp://` 之类报的是 **`FMT-300`，不是 `FMT-002`**。
> 实现里专门先判 `source.find("://") != std::string::npos`，就是为了不让用户
> 把「协议不支持」误读成「本地文件不存在」而去翻磁盘。
> 用例 `File.暂存失败会清理临时文件` 断言的正是 `ftp://example.com/a.bin` → `FMT-300`：
> **这里原先是 `https://`，`a2b6cd1` 换成了 `ftp://`**——因为 https 现在合法了。

`parse_url()`（`src/common/http_client.cpp`）另做几项基本合法性检查：砍掉 scheme 后
主机名为空 → `FMT-300`「URL 缺少主机名」；端口不是数字或超出 1～65535 → `FMT-300`；
`authority` 里带 `@`（即 `user:pass@host`）→ `FMT-300`；`path` 部分为空则按 `/` 处理
（例：`http://example.com` 取 `http://example.com/`）。查询串保留（下载要用），
只有文件名推断会把它砍掉。

## 33.2 为什么换掉 cpp-httplib 的 Client（本轮最重要的决策）

| 理由 | 说明 |
|---|---|
| cpp-httplib 的 `Client` 走 https **必须 OpenSSL** | 自己编 OpenSSL 需要 **Perl + NASM**，破坏本项目「构建只依赖 vendored 单头文件、离线可构建」的前提 |
| 本项目用 **`/MT` 静态 CRT** | 静态链接 OpenSSL 虽然也能做到不引 DLL，但 `/MT` 与市面上常见的 `/MD` 静态包**混用 CRT 会出问题**，自己编又回到上一条 |
| **WinHTTP 是系统组件** | `target_link_libraries(fmt_core PRIVATE winhttp)` 链的是系统导入库，Windows 在它就在 |
| TLS 走 **Schannel** | 用**系统证书库**，证书更新跟着系统走，仓库里不需要塞 CA bundle，**不分发任何 DLL** |
| **自动使用系统代理** | `WinHttpOpen` 首选 `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`（Win8.1+），失败退回 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY`——访问 https 站点基本都要走代理，这是刚需 |

**更正一处不准确的老表述**：原文说「HTTPS 需要 OpenSSL，会把 DLL 带进产物」。
在 `/MT` + vendored 单头文件的约束下，「会引入 DLL」**并不准确**（静态链接 OpenSSL 也不引 DLL，
只是 CRT 混用会炸）；真正的阻碍是**构建链**（Perl/NASM）与 **CRT 匹配**。换成 WinHTTP 后
这个问题整体不存在了：既不引 OpenSSL，也不带任何 DLL。

**构建改动**：

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

## 33.3 客户端行为（`common/http_client`，逐行照源码）

```cpp
// include/fmt/common/http_client.hpp
bool is_remote_url(std::string_view source);   // http:// 或 https://

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

Result<HttpDownloadResult> http_download(
    const HttpDownloadRequest& request,
    const std::function<bool(const char* data, std::size_t size)>& sink);
```

```text
请求     GET；跟随重定向（WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS，设置失败按系统默认）
超时     连接 10s / 发送 30s / 接收 300s
         **接收超时是单次读取的空闲超时，不是总时长**——读得慢但一直在读就不会超时
代理     AUTOMATIC_PROXY 优先，失败退回 DEFAULT_PROXY
读取     64 KiB 一块（kReadChunk），按 WinHttpQueryDataAvailable 调整
只有 2xx 的响应体交给 sink；非 2xx 的响应体直接丢弃、状态码照实返回
         （错误页不该落进用户的文件；file.cpp 看到 status != 200 才报 FMT-301）
sink 返回 false = 调用方要求中止 → 返回**成功**且 aborted = true，
         被拒绝的那一块**不算收到**（bytes 不含它）
```

**为什么明确要求 `Accept-Encoding: identity`**：

```text
压缩会让 Content-Length 与实际落盘字节对不上——第 32 节第 7 步的完整性检查会误报；
更糟的是可能把 gzip/deflate 的压缩内容当成文件原样存进仓库，用户拿到一个打不开的文件。
所以请求头里显式写 Accept-Encoding: identity。
若服务器仍返回非 identity 的 Content-Encoding → FMT-301 DownloadFailed，
消息「服务器返回了 <编码> 压缩内容，暂不支持：<URL>」。宁可失败，也不存一份坏文件。
```

**错误映射**：

```text
超时（ERROR_WINHTTP_TIMEOUT）        → FMT-302 DownloadTimeout
                                      「发送请求超时」/「等待响应超时」/
                                      「读取数据超时（已收 N 字节）」
域名解析 / 连接 / TLS / 响应异常      → FMT-301 DownloadFailed，消息里带 Win32 原因
                                      （域名解析失败 / 无法连接 / 连接被中断 /
                                       服务器响应异常 / Win32 <code> <系统文案>）
证书类失败                            → FMT-301 DownloadFailed，额外读
                                      WINHTTP_OPTION_SECURITY_FLAGS，给出更准的提示：
                                      根证书不受信任 / 证书主机名不符 / 证书已过期或尚未生效
URL 非法 / 协议不支持 / 带凭据 / 端口非法 → FMT-300 UrlInvalid
```

## 33.4 老口径留档（已作废，勿再引用）

```text
【作废，188e85d 时期的写法】
http://        ✅ 支持（httplib::Client 下载，10 秒连接超时、300 秒读超时，跟随重定向）
https://       ❌ 不支持 → prepare_upload() 直接返回 FMT-300 UrlInvalid
file:// 等其它  ❌ 不支持 → FMT-300（「只支持 http:// 开头的 URL：<来源>」）
错误消息：V1 不支持 https（需要 OpenSSL）；请改用 http:// 或本地路径
理由（已不成立）：HTTPS 需要 OpenSSL，会把 DLL 带进产物
```

`a2b6cd1` 推翻的正是这一段；其中「会把 DLL 带进产物」这个理由本身也不准确，见 33.2。

## 33.5 文件名推断规则（`file_name_from_source()`，必须写准）

省略 `[文件名]` 时按下面的顺序处理，**任何一步都不许拿主机名当文件名**：

```text
① 先砍掉 scheme://host，只看路径部分
   "://" 之后第一个 "/" 起才算路径；找不到 "/"（如 http://example.com）→ 路径视为空
② 去掉查询串与锚点：从第一个 '?' 或 '#' 起全部截掉
③ 去掉结尾的 '/' 与 '\'（可能不止一个）
④ 取最后一段（最后一个 '/' 或 '\' 之后的部分）
⑤ 百分号解码（url_decode）：URL 里的中文是 %XX 编码的
```

对照例子（`tests/file_test.cpp` 的 `File.从来源推断文件名` 逐条断言）：

```text
https://example.com/a/b/test.zip           -> test.zip
http://example.com/a.bin?token=1#x         -> a.bin      （② 砍掉 ?token=1 与 #x）
http://example.com/dir/                    -> dir         （③ 去掉结尾斜杠后取最后一段）
http://example.com/%E5%B7%A5%E4%BD%9C.txt  -> 工作.txt     （⑤ 百分号解码）
D:\Data\test\a.txt                         -> a.txt       （本地路径同样走 ③④）
/tmp/x/y.log                               -> y.log
http://example.com/                        -> （空串）    ← 推断不出名字
```

推断为空时**不猜**：`prepare_upload()` 报 `FMT-100 FileNameEmpty`
（「无法从来源推断文件名，请显式给出文件名」），要求用户显式给名字，
而不是存一个叫 `example.com` 的文件。

---

# 34. Upload 临时文件

下载阶段：

```text
repository
```

不能直接作为目标。

应该：

```text
temp/
```

**已明确：上传暂存文件统一放 `<数据根>/temp/`**（不再二选一），跟着 exe 走——用户一眼能找到、
随时可以清空。旧文写的「或者系统临时目录」只在**数据根不可写**时作为退路：
这时 CLI 退回系统临时目录 `%TEMP%`，并写一行 WARN 说明原因与改用后的路径。

例如：

```text
临时文件
    ↓
下载完成
    ↓
验证
    ↓
正式文件
```

避免部分下载文件进入正常仓库。

**实际文件名（提交 `188e85d`）**：

```text
<数据根>/temp/fmt-upload-<随机数>-<序号>.tmp
```

`fmt-` 前缀是刻意的：**服务启动时**会清掉 `temp/` 下以 `fmt-` 开头的遗留文件
（`clean_temp_directory()`，用户手放进去的其它文件一律不动，删除数量记一行 INFO），
所以崩溃留下的暂存文件不会越积越多。随机数来自 `std::random_device`、序号是进程内
`std::atomic` 自增，保证同一进程内不重名。

---

# 35. Upload 失败

以下情况：

```text
404
连接失败
超时
下载中断
响应异常
大小超限
MD5 重复
文件名冲突
```

都不能生成正常 File。

临时文件必须清理。

**实现（提交 `188e85d`）：临时文件的清理分两段、各有一个责任人，一个失败路径都不漏。**

```text
prepare_upload()   用 TempGuard（RAII）守着临时文件：函数正常返回才 guard.keep = true
                   交出所有权；任何提前 return（404、连接失败、超时、中断、超大小上限、
                   写盘失败、Content-Length 对不上、MD5 算不出来）都由析构函数删掉它
commit_upload()    从它接手那一刻起（开头就 TempGuard guard{prepared.temp_path}），
                   临时文件的生命周期归它管：参数/状态类失败、去重命中、重名命中、
                   移动失败……每一条失败路径都会删掉它
                   移动成功 → guard.keep = true（已经搬进仓库，别再按临时文件删）
                   file.json 写不进去 → 走第 40 节的回滚，删掉刚提交的**仓库文件**
```

用例 `File.暂存失败会清理临时文件` 直接断言：本地文件不存在、**协议不支持的来源**、
超过大小上限三种失败之后，`temp/` 里的常规文件数必须是 **0**（不留垃圾）。

**本地路径找不到时的报错要能自我诊断（提交 `a9af276`）**：`prepare_upload()` 清掉粘贴污染
（不可见格式字符 / 成对引号）之后**仍然**找不到时，消息会**点名被清掉的码位**：

```text
有不可见字符  FMT-002 FileNotFound（退出码 3）：
              「本地文件不存在：<清理后的路径>（你粘贴的路径里有不可见字符 U+202A、U+202C，
                它会让路径对不上；已自动清掉，请检查路径是否还有别的问题）」
只去掉引号/空白  「本地文件不存在：<清理后的路径>（已去掉粘贴带进来的引号或空白）」
什么都没清      「本地文件不存在：<路径>」（原来那句）
```

用例 `File.粘贴路径里的不可见字符会被清掉` 复刻用户场景：中文目录 `头像/` 下的文件
+ `U+202A` / `U+202C` 包裹 → 上传成功；Explorer 引号包裹 → 成功；仍然找不到时消息里
出现 `U+202A` 与 `U+202C`；名字里的不可见字符被 `validate_file_name()` 拒绝
（第 33.1.1、25 节）。

> **口径更正（提交 `a2b6cd1`）**：这条用例中间那一段原来用的是 `https://…`，现在改成了
> `ftp://example.com/a.bin`——因为 **https 已经是合法来源**，不再能当「不支持」的例子。
> 断言依旧是 `FMT-300 UrlInvalid`（不是 `FMT-002`）。

---

# 36. MD5 计算

文件下载完成后：

```text
读取文件
 ↓
计算 MD5
 ↓
得到 32 位十六进制字符串
```

比较时统一：

```text
小写
```

例如：

```text
d41d8cd98f00b204e9800998ecf8427e
```

**实现口径（提交 `188e85d`，`include/fmt/common/hash.hpp`）**：**边下载边算**，
不是「下完再读一遍文件」——所以接口是增量的，不是一次给一整块：

```cpp
// include/fmt/common/hash.hpp
namespace fmt {

class Md5 {
public:
    Md5();
    ~Md5();
    Md5(const Md5&) = delete;
    Md5& operator=(const Md5&) = delete;

    // 追加数据。失败（算法不可用等极端情况）返回 false，之后 finish() 返回空串。
    bool update(const void* data, std::size_t size);
    bool update(std::string_view text);

    // 收尾并返回 32 位小写十六进制。只能调用一次。
    std::string finish();

private:
    void* algorithm_ = nullptr;  // BCRYPT_ALG_HANDLE
    void* hash_ = nullptr;       // BCRYPT_HASH_HANDLE
    void* object_ = nullptr;     // 哈希对象缓冲（BCrypt 要求调用方提供）
    bool failed_ = false;
};

// 一次性计算（小数据与测试用）
std::string md5_hex(std::string_view text);

}  // namespace fmt
```

三条实现纪律：

```text
① 走 Windows CNG（bcrypt）：BCryptOpenAlgorithmProvider(BCRYPT_MD5_ALGORITHM) →
   BCryptCreateHash → BCryptHashData → BCryptFinishHash → BCryptDestroyHash，
   最后转 32 位小写十六进制。**不引入第三方哈希实现**
② bcrypt.lib 在 src/common/CMakeLists.txt 里链接（if(WIN32) target_link_libraries(fmt_core PRIVATE bcrypt)），
   所以产物里只多一个系统 DLL 的导入项，不新增可分发文件
③ 上传路径上就是「写盘的同时 update」：prepare_upload() 的 sink 每写一块就
   hash.update(data, length)；写完 out.close() 之后 finish()。
   finish() 返回空串视为 MD5 计算失败 → FMT-009 StorageError「MD5 计算失败」，临时文件照删
```

用例：`Hash.MD5已有向量`（对已知向量断言，含空串 `d41d8cd98f00b204e9800998ecf8427e`）、
`Hash.分块与一次算结果一致`（同一份数据分块累加与 `md5_hex()` 一次算必须同结果——
这条直接保护「边下载边算」的正确性）；上传侧由 `File.本地文件暂存` / `File.从HTTP下载入库`
断言入库记录的 `md5` 等于 `md5_hex()` 的值。

---

# 37. 内容判重（当前桶内内容相同 → FMT-304「文件已存在」）

**原口径（提交 `188e85d`）**：同用户 + 任何 Bucket + 正常文件命中相同 MD5 → 上传终止、
`FMT-304 Md5Duplicate`，不新建 file_id、不复制文件、不改旧名字。

**现口径（本次改动（未提交），用户三轮明确后的定稿）**：判重**只看当前用户的当前 Bucket**
（不跨桶——1 桶有这个文件，2 桶照样可以有同内容的另一份），而且**先看内容、再看名字**：

```text
① 内容相同 + 文件名不同 → FMT-304「文件已存在」，**不提示修改文件名**
② 内容相同 + 文件名相同 → FMT-304「文件已存在」，**不提示修改文件名**
③ 内容不同 + 文件名相同 → FMT-105「文件名重复」，**提示用户修改文件名**
④ 内容不同 + 文件名不同 → 正常上传
```

用户四条口径原话：

```text
1. 文件上传 当前桶内 内容相同 文件名不同 提示文件已存在，不提示用户修改文件名称
2. 文件上传 当前桶内 内容相同 文件名相同 提示文件已存在，不提示用户修改文件名称
3. 文件上传 当前桶内 内容不相同 文件名相同 提示文件名重复，提示用户修改文件名称
4. 文件上传 当前桶内 内容不相同 文件名不相同 正常上传
```

（更早还有一条总口径：「一个桶内不能存在相同文件名和相同内容的文件；1 桶有这个文件，
2 桶也可以有 1 桶内容相同的文件」。）

```text
实现        FileService::commit_upload()（第 38 节）：先按 md5 判内容（①② 合并成一步），
            再按 names-iequals 判名字（③），剩下的走 ④；两条都只在当前 Bucket 内生效
FMT-304     「文件已存在」，退出码 4、HTTP 409；消息带上已有文件的名字与 file_id
            （内容相同但名字不同时，用户需要知道是哪一份占了这个内容）
FMT-105     「文件名重复」，退出码 4、HTTP 409；消息里明确「请修改文件名后重新上传」
md5         仍然算、仍然记进 file.json（完整性核对仍用它，第 39 节）
```

**为什么内容相同连名字都不用提**：这是用户明确要的诊断——「这份内容在这个桶里已经有了」。
同一个桶里同一份内容只留一份（不重复占磁盘），而名字在这里无关紧要。
**要改名字的只有 ③**：内容不同却同名，两者会落到**同一个磁盘路径**
（`repository/<用户>/<Bucket>/YYYY/MM/DD/<文件名>`），不拒就是覆盖（第 38 节）。

**为什么别的桶里可以有同内容的一份**：Bucket 是命名空间，落盘路径由 `record.bucket` 推出；
共享一份字节会让「删掉 1 桶的那份」把 2 桶的也删掉，或者要靠引用计数兜住——
那是另一套设计，不在本次范围。

**内容层面的数据损坏仍有防线**：`commit_upload()` 的第 ④ 步（第 39 节）在写 `file.json`
之前会核对记录里的 `size` / `md5` 与磁盘上的一致，不一致按 FMT-015 报出来。

---

# 38. 文件名重复（当前桶内同名 → FMT-105）

**这一条只管「内容不同 + 名字相同」**（本次改动（未提交）定稿；内容相同的那两种走第 37 节）。
判重的完整四条口径见第 37 节，这里是其中的 ③：

```text
同用户
+
同一个 Bucket
+
正常文件
+
相同 filename（不区分大小写）
+
内容不同
```

则：

```text
拒绝，返回 FMT-105「文件名重复」，提示用户修改文件名
```

不能自动：

```text
test(1).txt
test_1.txt
```
**作用域是「当前用户的当前 Bucket」，不是「同用户跨 Bucket」**（本次改动（未提交），用户要求）：
不同 Bucket 各自是独立的命名空间，同一个文件名可以在两个桶里各存一份；同一份内容也可以在
两个桶里各存一份（第 37 节）。原来的口径（提交 `188e85d`「Bucket 不构成文件名命名空间」）
**已被本次推翻**。

```text
命中条件   record.user == current_user
        && !record.is_trash
        && iequals(record.bucket, current_bucket)          ← 本次改动（未提交）
        && iequals(record.file_name, prepared.file_name)   ← 提交 5bf2c1f
        && record.md5 != prepared.md5                      ← 本次改动（未提交）：内容不同的那一种

返回值     FMT-105 FileNameConflict（退出码 4）
           「文件名重复：Bucket「<桶>」里已有同名文件：<file_name>（file_id <已有 id>，
             内容不同；请修改文件名后重新上传）」
           —— 内容相同的那两种先被第 37 节拦下，报 FMT-304「文件已存在」且**不提改名**
副作用     无：不新建 id、不复制文件、不改旧名字、临时文件被清掉（第 35 节）
```

**名字比较不区分大小写（提交 `5bf2c1f`，修的是数据损坏）**：这一条原来是 `==` 精确比较，
于是先传 `doc.txt`、再传 `DOC.TXT` 会被当成两个不同的名字放过——而两者在 Windows 上
落到**同一个磁盘路径**，第二次上传把第一个文件的字节**覆盖**掉，
`file.json` 里第一条记录还写着旧的 `size` / `md5`。结果是**同一个磁盘文件被两条记录指向
+ 字节被覆盖 + 元数据失真**（回归用例 `File.大小写不同的同名必须被当成重名`：
仓库里那个文件 `file_size` 期望 11、实际 22）。所以：**名字在同一个 Bucket 内唯一
（不区分大小写）**，`doc.txt` 与 `DOC.TXT` 不能共存——这不是额外限制，
是那条覆盖路径的根因防线（`FMT 技术文档.md` 第 18.22 节）。

**重名不自动改名**：不生成 `test(1).txt` / `test_1.txt`，也不覆盖旧文件——
要用户自己换一个名字再传（`file upload <来源> <新名字>`）。
回收站里的同名/同内容文件（`is_trash = true`）**不占用**正常文件空间，可以重新上传。

**「内容相同」为什么不走这一条**：内容相同的两种（名字同与不同）先被第 37 节拦下，
报的是 FMT-304「文件已存在」，而且**不要求用户改名字**——用户要的是
「这份内容在这个桶里已经有了」，不是「你的名字撞了」。

**更早的一道闸：与 `file_id` 同形的名字（提交 `9c3d2cb`）**。`validate_file_name()` 在
「Windows 保留设备名」之后加了 `looks_like_file_id()`，命中即 `FMT-106 FileNameLikeFileId`
（退出码 2，第 25 节）——所以走到本节的冲突检查时，`prepared.file_name` **不可能**是
`fmt-YYYYMMDD-N` 这个形状。显式名（`file upload <来源> <名字>`）与从来源推断的名字
（`file_name_from_source()`）走的是**同一道校验**，两条路都拦。

---

# 39. Upload 正式提交

只有：

```text
下载成功
+
大小合法
+
MD5 检查完成
+
文件名检查通过
+
file_id 创建成功
```

后才能正式提交。

顺序：

```text
临时文件
 ↓
repository
 ↓
file.json
```

**实现（提交 `188e85d`，`FileService::commit_upload()`）**：这个顺序就是「先搬文件、
再写元数据」，中间任何一步失败都有明确回滚（第 40 节）。完整顺序是：

```text
① 检查 current_user / current_bucket / 桶目录存在 / 临时文件还在 / 文件名合法
② 读 file.json 快照 → **桶内判重**（第 37/38 节：当前 Bucket 里同文件名 → FMT-105）
③ 组 FileRecord：file_id = next_file_id(records)、extension / file_type 由文件名算、
   size / md5 来自第一段，is_trash = false
④ 由 file_id 推出日期目录 → ensure_directory() 建 repository/<user>/<bucket>/YYYY/MM/DD/
⑤ move_file(temp → repository)：同卷用 std::filesystem::rename（原子）；
   跨卷（temp/ 退到 %TEMP% 且不在同一个盘时）才 复制 → 校验大小一致 → 删源
   （大小对不上就删掉目标并报 FMT-009，绝不留半份）
⑥ 追加记录并 save_records()（file.json 原子写）
⑦ 记 INFO 日志「入库：<file_id> <file_name> -> <仓库路径>」
```

`file_id` 创建**不是**独立的第 ① 步，而是在第 ③ 步、与去重/重名共用同一份 `file.json`
快照——这也是「id 分配不会并发重复」的原因（第 24 节）。

---

# 40. Upload 回滚

如果：

```text
repository 文件移动成功
```

但是：

```text
file.json 写入失败
```

必须：

```text
删除刚刚提交的 repository 文件
```

或者进入明确的一致性恢复流程。

不能直接告诉用户：

```text
上传成功
```

**实现（提交 `188e85d`）**：已经照上面这条做了，不用「一致性恢复流程」这一支——

```cpp
records.push_back(record);
if (const Status status = save_records(records); !ok(status)) {
    // 第 40 节：file.json 写不进去就把刚提交的仓库文件删掉，绝不报「上传成功」
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
    logger_->error("File", "写 file.json 失败，已回滚仓库文件：" + path_to_utf8(destination));
    return *error_of(status);        // FMT-005 IoError（原子写失败）/ 退出码 1
}
```

删失败也只记 ERROR 日志（`std::error_code` 忽略），用户拿到的是**错误**而不是「上传成功」；
`file.json` 里没有这条记录，磁盘上最多留一个仓库文件，属一致性检查能报出来的
「文件有、metadata 没有」（`FMT 技术文档.md` 第 15.4 节），**不静默吞掉**。

---

# 41. File List

命令：

```text
file list
```

只显示：

```text
当前用户
+
当前 Bucket
+
正常文件
```

不显示：

```text
is_trash = true
```

的文件。

Trash 单独查询。

**实现（提交 `188e85d`，`FileService::list()`；**排序在提交 `674d0b0` 扩成三种**）**：

```text
作用域    record.user == current_user && record.bucket == current_bucket && !record.is_trash
排序      file list --sort name|size|id（提交 674d0b0）：
            name（**默认**）  按文件名，**不区分大小写**；同名用 file_id 保证稳定
            size             size 大的在前
            id               入库顺序（file_id 里的日期+序号天然递增）
          乱写（不是这三个）→ **FMT-001**，不静默按默认排
          响应里新增 sort 字段，回显实际用的排序方式
          CLI 侧：--sort 是**本地开关**（不进 argv），单独放进 args.sort
前置      current_user 为空 → FMT-604 NoCurrentUser；current_bucket 为空 → FMT-305 NoCurrentBucket
原口径    重排前固定「按 file_id 升序」——它现在只是 id 这一种（默认换成 name）
```

**搜索与分页（提交 `53ec4be`，CLI 与 HTTP 同一套业务实现）**：

```text
search     对**文件名**做不区分大小写的**子串**匹配，**也匹配 file_id**——
           理由是用户手里常有的就是 id；响应里回显 search
page       从 **1** 开始
page_size  **缺省或 0 = 不分页**（保持老行为一次给全，只是多回一个字段）；
           上限 **1000**，超了报 **FMT-001**
新增字段   total（**命中总数，不是本页条数**）、page、page_size、total_pages；
           有搜索时才回显 search
顺序冻结   **过滤 → 排序 → 分页**
           理由：先切片再排序、或排序不稳定，会让**同一个文件出现在两页、
           另一个一页都不出现**（测试里就是遍历三页断言「不漏不重」）
越界       超出末页返回**空页**（不是错误）
CLI        file list [--sort name|size|id] [--search 关键字] [--page N --page-size M]
           打印：「匹配「jpg」共 2 个文件（第 1/2 页，本页 1 条）」
           （--search / -s、--page、--page-size 都是**本地开关**，不进 argv，
             单独放进 args.search / args.page / args.page_size）
```

服务端 `data`（管道与 HTTP 完全相同，第 12.3.2.1 节）：

```json
{
  "files": [
    { "file_id": "fmt-20261008-0", "file_name": "test.txt", "extension": ".txt",
      "file_type": "text", "size": 1024, "md5": "d41d8cd98f00b204e9800998ecf8427e" }
  ],
  "count": 1,
  "current_bucket": "工作",
  "sort": "name",
  "total": 1, "page": 1, "page_size": 0, "total_pages": 1
}
```

**提交 `53ec4be` 追加的四个字段**：`total`（命中总数）、`page`、`page_size`、`total_pages`；
`page_size` 为 0 表示不分页（此时 `total_pages` = 1）。有搜索时另回 `search`。

注意列表里**没有 `user` / `bucket` / `is_trash`**——列表本来就限定在当前用户 + 当前 Bucket +
正常文件，这三个字段没有信息量，服务端不返回（第 127.6 节的展示规则按这个形状打印）。
`count` 与 `files` 同长，`current_bucket` 是给 CLI 显示用的当前桶名。

---

# 42. File Get

支持：

```text
file get <file_id>
```

和：

```text
file get <filename>
```

通过 `file_id` 查询时：

```text
全局唯一
```

通过文件名查询时：

```text
当前用户
+
正常文件
```

**实现（提交 `188e85d`）**：

```text
命令层   先当 file_id 查（get_by_id），查不到再当文件名查（get_by_name）——
         所以用户敲 file get <名字> 时会先白跑一次 id 查询，这是刻意的：
         file_id 形状固定（fmt-YYYYMMDD-N），先查 id 不会误伤名字
按 id     record.file_id iequals(record.file_id, 参数) 即命中；**不限用户、不限 Bucket、不限 is_trash**
         （软删除后仍能按 id 查回来，此时 is_trash = true、trash_reason = "file"、
           并多回一个 trash_path，见下）
         **提交 `6a40742` 更正**：id 侧原来是精确比较（`==`）——`5bf2c1f` 只改了 delete 侧的遗留；
                现在**标识比较一律不区分大小写**：`file get FMT-20261008-0`（大写）也查得到（用例
`File.列表与查询` 有这条断言）；原注「两处目前不一致」**已作废**。
按名字    record.user == current_user && !record.is_trash &&
         iequals(record.file_name, 参数)     ← 提交 5bf2c1f 起不区分大小写
         （本次改动（未提交） 起**只看当前 Bucket**：别的 Bucket 里的同名文件不再命中，
           见第 9.2 节；当前桶没有、别的桶里有 → FMT-002 并说清它在哪个桶）
查不到   FMT-002 FileNotFound（退出码 3）
```

**两种查询范围（提交 `9c3d2cb` 明确「保持不变」）**：按 `file_id` 查是**全局**的
（不限用户、不限 Bucket、连回收站里的也查得到）；按**文件名**查只查**当前用户的正常文件**。
这是**有意的**：`file get` 是只读查询，最坏结果是把 id 命中的那条给用户看；
`file delete` 是破坏性操作，所以在 `locate_record()` 里多了一步歧义判定（第 43 节）。
两处的范围差异**不是**漏实现。

服务端 `data`：

```json
{
  "file_id": "fmt-20261008-0", "file_name": "test.txt", "bucket": "工作",
  "extension": ".txt", "file_type": "text", "size": 1024,
  "md5": "d41d8cd98f00b204e9800998ecf8427e",
  "is_trash": false, "trash_reason": "",
  "path": "repository/user/工作/2026/10/08/test.txt"
}
```

**命中回收站记录时（提交 `9c3d2cb`）**：除了原有的 `is_trash` / `trash_reason`，
再**增加 `trash_path`**（相对数据根、正斜杠，形如
`trash/user/.files/工作/2026/10/08/test.txt`，由 `trash_path_of()` 现推）：

```json
{
  "file_id": "fmt-20261008-0", "file_name": "test.txt", "bucket": "工作",
  "extension": ".txt", "file_type": "text", "size": 1024,
  "md5": "d41d8cd98f00b204e9800998ecf8427e",
  "is_trash": true, "trash_reason": "file",
  "trash_path": "trash/user/.files/工作/2026/10/08/test.txt"
}
```

**仓库里没有该文件时，`path` 就不返回**（回收站里的记录当然不在 `repository/` 下）
——这是**设计**，不是字段缺失；`trash_path` 才是那个文件现在在哪。
CLI 会按 `trash_path` 多打一行「回收站路径：…」（第 127.6 节）。

**预览链接与下载链接（本次改动（未提交））**：`file get` 的 `data` 里再增加两条完整地址——

```text
preview_url    http://<server.json 的 host>:<port>/api/file/<file_id>/preview   要 token
download_url   http://<server.json 的 host>:<port>/api/file/<file_id>/download  要 token
```

要点三条：① **无论 HTTP 服务有没有开都给**（地址由配置算出，不是探测出来的）；
② 路径里用 **`file_id`** 而不是文件名（全局唯一、只含 `[0-9a-z-]`，不用百分号编码）；
③ 协议只给 `http`——服务端只监听明文 HTTP（`server.json` 没有 TLS 配置），
要 https 得自己在前面挂反向代理，这里不编造一个连不上的 https 地址。
预览链接对图片与文本类有效，其余类型浏览器会拿到 `FMT-701`（第 12.3.2 节）；
下载链接任何类型都能用。CLI 各多打一行「预览链接：…」「下载链接：…」（第 127.6 节）。

`path` 由 `resolve_path()` 现推（第 20 节：先按 `file_id` 的日期推，推不出来再在桶的日期树里
兜底找同名文件），用 `relative_path_text()` 写成**相对数据根、正斜杠**的文本；
路径推不出来或磁盘上没有时**不写 `path` 字段**（其余字段照常返回，CLI 少打一行「路径」），
查询本身仍然成功——「记录在、文件不在」是第 62～64 节要报的一致性异常，
不该让一次查询直接失败。

---

# 43. File Delete

命令：

```text
file delete <file_id|文件名>
```

**定位规则与第 42 节 `file get` 完全一致（提交 `0ad9efc`）**——两处参数**同名同义**，
不需要先 `file get` 把文件名换成 `file_id` 再来删（见本节末的「为什么与第 42 节对齐」）。

流程：

```text
查找 File
 ↓
确认正常状态
 ↓
确定实际路径
 ↓
移动到 Trash
 ↓
is_trash = true
 ↓
写回 file.json（顺手兼容清理老 trash.json，第 17.1 节）
```

文件：

```text
file_id
```

不改变。

**实现（提交 `188e85d` 落地，`0ad9efc` 把定位改成与 `file get` 一致；
`FileService::remove(std::string_view file_id_or_name)`）：软删除，`file_id` 不变。**

**① 定位记录（提交 `0ad9efc`，私有函数 `locate_record(records, key)`；
三处比较在提交 `5bf2c1f` 改为不区分大小写，`9c3d2cb` 再加第 ②.5 步，第 38 节）**——参数是
`<file_id|文件名>`，**四步**（① ② ②.5 ③），与第 42 节 `file get` 的定位**同一套规则**
（②.5 是 delete 独有的）：

```text
① 先当 file_id     iequals(record.file_id, 参数) 即命中（**不区分大小写**）。
                   全局唯一；file_id 形状固定（fmt-YYYYMMDD-N），先查不会误伤名字
                   ——与第 42 节同一套理由。（**名字本身曾经可以长得像 id**：
                     提交 9c3d2cb 起上传时按 FMT-106 拒掉这个保留形状，见第 25 节）
② 再当文件名       record.user == current_user && !record.is_trash &&
                   iequals(record.bucket, current_bucket) &&      ← 提交 本次改动（未提交）
                   iequals(record.file_name, 参数)
                   （**桶内命名空间**：别的 Bucket 里的同名文件**不再命中**，
                    只记下「它在哪个桶」用于错误消息；名字在同一桶内唯一——
                    **不区分大小写也算同名**，重名上传会被 FMT-105 拒绝，所以不会出现
                    「一个名字对应两条正常记录」的歧义，也不会出现「两条记录指向同一个
                    磁盘文件」的数据损坏）
②.5 两个索引命中**不同**记录（提交 9c3d2cb）
                   ① 与 ② 各自记下命中下标（by_id / by_name），**两者都命中且不是同一条**
                   → FMT-001 InvalidArgument（退出码 2），消息「有歧义：<参数> 既是
                     <file_id> 的文件标识，又是另一个文件的文件名（file_id <file_id>）。
                     这种名字现在不允许上传；请直接用 file_id 指定要删哪一个」
                   ——**不猜**。这种数据只可能来自旧版本或手工改过的 file.json
                     （新数据上传时已被 FMT-106 拦住）。
                     命中同一条记录（一个文件同时满足两路）或只命中一路，都照常继续
③ 名字存在但已在回收站
                   → FMT-001 InvalidArgument（退出码 2），
                     消息「该文件已经在回收站里：<名字>（file_id <file_id>）」
                     ——**不能报成「文件不存在」**：文件还在，只是不在正常区
                   这一步只认「当前用户 + is_trash + 同名 + **与当前 Bucket 同名**」
                   （比较同样不区分大小写；本次改动（未提交） 起别的桶里的回收站条目不算——
                   它跟当前桶不相干）；
                   按 file_id 命中时不会走到这里，
                   而是走下面「② 命中之后的原有校验」的第 ① 步
都没有             → FMT-002 FileNotFound（退出码 3），消息「文件不存在：<参数>」
```

**② 命中之后的原有校验与落地步骤（提交 `188e85d`，未变）**：

```text
① 确认状态    不属于当前用户 → FMT-004 PermissionDenied（退出码 5）
              已经在回收站（is_trash == true，按 file_id 命中时）→ FMT-001 InvalidArgument
              （退出码 2）「该文件已经在回收站里：<file_id>」
              ——这一条与新加的定位第 ③ 步是**两个入口、同一个错误码**：
                按 id 命中的在这里报，按名字命中的在定位时就报，消息都带 file_id
② 定位路径    resolve_path()（第 20 节）拿到仓库里的实际位置；
              trash_path_of() 由 file_id 的日期推出 trash/<用户>/.files/<桶>/YYYY/MM/DD/<文件名>
              （第 21 节），先 ensure_directory() 建好父目录
③ 搬文件      move_file(仓库 → 回收站)：同卷 rename，跨卷 复制 → 校验大小 → 删源
④ 改 metadata  同一份快照上把 is_trash 置 true、trash_reason 置 "file"、
              **deleted_at 置 local_datetime_iso()**，写回 file.json
⑤ 记 INFO 日志「软删除：<file_id> <file_name> -> <回收站路径>」
```

> **提交 `0fc242b` 改掉两处**：① 原来的第 ④ 步「写 trash.json 追加一条文件级记录」
> **没有了**——`file.json` 是唯一权威（`deleted_at` 就记在那条记录里，第 17.1 节），
> `trash.json` 只在读取时兼容老记录；② 顺序与回滚因此简化为「搬文件 → 写 file.json」，
> 写失败就把文件搬回仓库（原来那条「先撤掉刚追加的 trash.json 记录」的回滚动作一并作废，
> 见本节末的回滚段）。

**回收站落点用记录自己的 `bucket`（提交 `0ad9efc` 明确，第 21 节）**：第 ② 步的
`trash_path_of(records[index])` 只读**命中记录**的 `bucket`，不看当前 Bucket——
所以在桶 B 里按名字删掉桶 A 的文件时，文件进的是
`trash/<用户>/.files/<A 的桶名>/…`，而不是 B。命中之后剩下的每一步都只依赖这一条记录，
与「用户现在站在哪个 Bucket」无关。用例 `File.按名字删除用的是记录自己的Bucket` 钉这一条。

**为什么与第 42 节对齐（提交 `0ad9efc`）**：`file get` 一直两种参数都收，`file delete`
只收 `file_id`，开发文档也**从来没写出理由**——这是对称性缺失，不是有意为之。而且只按
`file_id` 单向查找时，用户用名字称呼一个确实存在的文件，落空后会报
`FMT-002 文件不存在`，**诊断是错的**（文件在，只是没用 id 称呼它）。所以 `remove()` 也
照第 42 节那两步来查——命令层与 `remove()` 都是「先当 file_id、再当文件名」，
**规则同一套、代码各走各的**：「命令层」用 `get_by_id()` + `get_by_name()`（第 42 节），
`remove()` 用 `locate_record()`（本节），`locate_record()` 多出来的第 ②.5 步（歧义判定）
与第 ③ 步只服务删除。
**参数语义与失败语义必须一致**，这才是对齐的要点。

**比较一律不区分大小写（提交 `5bf2c1f`，数据损坏修复）**：上面三步的比较
**都用 `iequals()`（ASCII 大小写折叠）**，不是 `==`。这不是体验优化——
按精确比较时，先传 `doc.txt` 再传 `DOC.TXT` 会被当作两个不同的名字放过，
而两者在 Windows 上落到**同一个磁盘路径**，第二次上传直接**覆盖**第一个文件的字节，
`file.json` 里第一条记录还写着旧的 `size`/`md5`——**同一个磁盘文件被两条记录指向 +
字节被覆盖 + 元数据失真**。回归用例 `File.大小写不同的同名必须被当成重名` 就是先复现、
后修复这条路径的。同一提交还把 `get_by_name()`、上传的重名判定、桶侧
（`is_current` / `was_current` / 回收站定位）一并改成不区分大小写，详见第 38 节与
`FMT 技术文档.md` 第 10.1、10.2.4、18.22 节。由此定下的三条口径：**凡按名字/标识定位
一律不区分大小写**；**名字在同一个 Bucket 内唯一（不区分大小写）**——`doc.txt` 与
`DOC.TXT` 不能共存，这是上面那起覆盖事故的根因防线（本次改动（未提交） 把作用域从「同用户」
收窄到「当前 Bucket」，详见第 37、38 节）；桶名不再各存一份拼写——
**提交 `9c3d2cb` 起 `bucket use WORK` 存进 `current_bucket` 的是磁盘上的实际名字 `work`**
（`canonical_name()`，第 27、29 节），`iequals()` 的实现同时收紧成**只折叠 ASCII**
（`>= 0x80` 的字节原样比较；原来交给 `std::tolower`，C locale 下虽是恒等，
但一旦有人调 `setlocale` 就会把 UTF-8 名字改坏）。

**与第 42 节的对齐与差异（提交 `9c3d2cb` 明确）**：两处的**参数语义与失败语义仍然一致**，
但 `locate_record()` 比 `file get` 多两步——②.5 的**歧义判定**（本节）与 ③ 的
「名字在回收站」。这是因为：

```text
file get     只读查询。最坏结果是「把 id 命中的那条给你看」，用户看错了还能再看一次
             ——所以按 file_id 全局（含回收站）、按文件名只查当前用户的正常文件，
               **两种查询范围保持不变**，不加歧义判定
file delete  破坏性操作。猜错的代价是删掉另一个对象，**不能猜**
             ——所以两个索引同时命中不同记录时直接报 FMT-001 并点名两条记录
```

这处不对称是**有意的差异**，不是漏实现；第 42 节的两种查询范围也没有改动。

服务端 `data`（管道与 HTTP 相同；**提交 `711da4c` 增加 `bucket` 字段**）：

```json
{ "file_id": "fmt-20261008-0", "file_name": "test.txt", "bucket": "工作",
  "moved_to": "trash/user/.files/工作/2026/10/08/test.txt",
  "message": "文件已移入回收站：test.txt" }
```

**跨 Bucket 成功时消息带归属**（提交 `711da4c`）：目标不属于当前 Bucket 时
`message` 变成 `文件已移入回收站：test.txt（Bucket：工作）`（`data.bucket` 也一并给出，
字段名不变、只是多了这一个）。同桶时不加这个后缀。

**预检（`dry_run`）与确认（提交 `711da4c`，本节新增的第一道闸）**：

```text
预检      args.dry_run = true（HTTP：?dry_run=1）→ FileService::check_remove()，
          **只读、零副作用**，返回：
            ambiguous      名字与 file_id 撞在**两条不同**记录上
            other_bucket   目标唯一，但在别的 Bucket 里
            blocked        = ambiguous（**确认解决不了**，见下）
            needs_confirm  = other_bucket（同桶为 false，CLI 不打扰用户）
            current_bucket / file_id / file_name / bucket / path / candidates[] / message
          歧义时 candidates 是两条记录（各带 file_id / file_name / bucket），message 点名
          两个 file_id 让用户改用 file_id 重发
阻断     **歧义不是「确认一下就能删」**：y/N 表达不了「删哪一个」。所以它落在
          blocked 而不是 needs_confirm——CLI 打印两条候选 + stderr 提示
          「这项操作不能靠确认解决，请按上面的提示指定具体对象」，不再发执行请求。
          这是刻意的判断，不是漏了确认
执行     用户同意后**才**带 force 发真实请求。跨桶未确认（预检被绕过、别的客户端直接发）
          → 服务端返回 **FMT-016 ConfirmRequired**（退出码 2），消息形如
          「<预检消息>；确认删除请加 force（CLI：--yes）」
同桶     没有别的要先说清楚的（两个布尔都 false）→ 不打扰用户，直接执行
```

**顺序与回滚（提交 `188e85d`；`0fc242b` 简化为两步）**：顺序是「先搬文件 →
再写 `file.json`」，每一步失败都能退回去，不留半边状态：

```text
搬文件失败            → 直接返回错误，仓库文件没动、JSON 也没动（最干净）
file.json 写失败      → 把文件**搬回仓库**（rename 回去），然后返回错误；
                        并记 ERROR 日志「写 file.json 失败，已回滚软删除」
```

> **`trash.json` 那两步作废（提交 `0fc242b`）**：原来还有「写 trash.json（失败搬回）」
> 与「写 file.json 失败先撤掉刚追加的 trash.json 记录」两步。文件级条目的权威现在是
> `file.json`（第 17.1 节），不再有第二份 JSON 要维护，所以回滚只剩「搬回去」这一下，
> 中间态的窗口也更小。

配合第 40 节（上传回滚）与本节的软删除回滚，`file` 这条链路上**没有**「报了成功但磁盘
和 JSON 不一致」的路径：能报成功的只有两种状态——要么完全没动，要么两边都改完。

> ~~**重要缺口（阶段 7）**~~ **该缺口已在提交 `0fc242b` 关闭**：`file delete` 产生的
> **文件级**条目现在能在 `trash list` / `get` / `restore` / `delete` 里看到、回退、
> 永久删除（统一形状 `TrashEntry`，第 52～54、59 节；`TrashService` 把文件级与桶级
> 合成一份视图）。数据仍然完好：文件在 `trash/<用户>/.files/…` 下、`file.json` 里那条
> `is_trash = true` / `trash_reason = "file"` / `deleted_at` 记着删除时间。

HTTP 侧对应 `DELETE /api/file/<file_id_or_name>`（提交 `0ad9efc` 起路径参数也可给文件名，
路由本身仍是原来的 `([^/]+)` + `url_decode()`，见 `FMT 技术文档.md` 第 12.3.2 节），
**不需要确认**（软删除可恢复，与 `trash delete` 的永久删除不同）。

---

# 44. Download

下载流程：

```text
查询 File
 ↓
检查 is_trash
 ↓
检查实际文件
 ↓
检查 metadata
 ↓
检查 size
 ↓
必要时检查 MD5
 ↓
打开文件
 ↓
流式传输
 ↓
完整完成
```

大文件不得一次性全部加载到内存。

---

# 45. Download Count

只有：

```text
完整成功下载
```

才：

```text
download_count + 1
```

以下情况不增加：

```text
失败
拒绝
过期
次数耗尽
文件不存在
文件处于 Trash
用户中途取消
```

---

# 46. Share Service

负责：

```text
share create <file_id|文件名>
share get <share_id>
share list [<file_id|文件名>]     ← 不带参数列**全部**分享（本次改动（未提交））
share delete <share_id>
```

**`share list` 不带参数（本次改动（未提交））**：用户要求「应该不加参数」就能看——
不然想查「哪条过期了、哪条次数用满了」得先知道是哪个文件。落地：

```text
share list              列出全部 share（最近创建的在前），每行标出文件名（当前桶的标
                        「（当前桶）」）、下载次数 n/m、状态；次数用满或已过期时在行尾
                        直接写「[下载次数已满]」「[下载已过期]」（两个原因可能同时成立，
                        所以分开标，不合并成一个「不可用」）
share list <file_id|文件名>  只看那个文件的（与 file get 同一套定位）
响应                    data.scope = "all" / "file"；每条都带 expired / limit_reached /
                        downloads_left / state / available / bucket / file_name / share_url；
                        带参数时另回 file_id
链接                    列表**一条一行，不打印分享链接**（用户实测要求）；要看链接用
                        `share get <share_id>`——第 49 节，那里「看全部信息」连失效链接都给
                        （并标注「当前不可下载，链接已失效」）。响应 data 里两种模式都带
                        `share_url`，只是 CLI 列表不打印
```

---

# 47. Share Create

命令：

```text
share create <file_id|文件名>
```

````text
【本次改动（未提交）】参数与 `file get` / `file delete` 同一套定位规则：
先当 file_id 查（全局唯一、形状固定 fmt-YYYYMMDD-N），查不到再当文件名查
（当前用户 + 当前 Bucket 的正常文件）；两处都没有 → FMT-002，当前桶没有而别的桶
有同名时消息里点名那个桶。`share list [<file_id|文件名>]` 同样两种都收，
**不带参数就列全部分享**（见第 49 节与开发文档 §46 的 `share list` 说明）。

返回值里多一条**完整分享链接**（`share_url` / `url` 同一份）：
    http://<server.json 的 host>:<port>/api/share/<share_id>/download
**无论 HTTP 服务有没有开都给**——地址是配置算出来的，不是探测出来的；
协议只给 http（服务端只监听明文 HTTP，没有 TLS 配置），要 https 自己在前挂反向代理。
`share get` / `share list` 也带同一条链接。
```

流程：

```text
检查 File
 ↓
确认 File 正常
 ↓
生成 share_id
 ↓
设置 max_download_count = 20
 ↓
设置 expire_time
 ↓
保存 share.json
 ↓
返回 Share 信息
```

---

# 48. Share ID

Share ID 必须：

```text
唯一
不可预测
```

不能使用：

```text
1
2
3
4
```

这种简单递增 ID。

Share ID 可以作为外部访问凭证。

**提交 `d5779db` 的实现（照源码）**：

```text
形状     12 位随机十六进制（例如 eaaecc6869ab）
来源     BCryptGenRandom（Windows CNG 系统调用）——不是 std::mt19937 ✗：
         伪随机可预测，当访问凭证不合格；FMT 技术文档.md 第 3.x 节库表本来
         就指定了这个系统调用
撞号     创建时撞号就重摇，最多 16 次
失败     生成失败**当错误返回**，绝不退化成可预测 id、也不退化成递增数字
```

---

# 49. Share Get

命令：

```text
share get <share_id>
```

检查：

```text
Share 是否存在
File 是否存在
File 是否处于 Trash
Share 是否过期
下载次数是否达到上限
```

过期 Share 可以返回：

```text
存在，但已过期
```

而不是伪装成从未存在。

**提交 `d5779db` 的实现（如实报状态）**：

```text
未知 share_id            → **真错误 FMT-500（退出码 3）**「分享不存在」
存在但不可用（四种）      → **成功返回 + 状态**，不伪装成「从未存在」：
   已过期 / 次数用尽 / 已撤销 / 关联文件在回收站
响应形状                 state 字段（可用 / 已过期 / 次数用尽 / 已撤销 /
                        关联文件不可用）+ available 布尔 + message
expire_time 解析失败     （被手工改坏）按**已过期**处理——安全侧默认可拒
撤销（is_valid=false）   映射到 FMT-500；**撤销是删记录**，
                        所以 share delete 之后再 get 就是 FMT-500

检查顺序（按本节 §49，提交 d5779db 就是按这个顺序写的）：
    有效性 → 文件存在 → 文件不在回收站 → **过期 → 次数**
注意：§50 下面那段流程图把「次数」写在「过期」前面，与本节的列表顺序**不一致**；
      提交 d5779db 取本节（§49）的顺序：**先过期、再次数**。
      两者只影响「同时过期且次数用尽」时报哪个状态，实现以 §49 为准。
```

**「查看全部信息」（本次改动（未提交））**：用户要求 `share get <share_id>` 要把这条分享的
**全部信息**摊开，包括「已过期」或「已经不能下载」的情况。响应在原有字段之外增加：

```text
expired          布尔：是不是**因为过期**而不能用（与 state 独立——一条分享可能既过期又用尽，
                 §49 只报先命中的那个状态，所以两个布尔都要给）
limit_reached    布尔：是不是**因为次数用尽**而不能用
downloads_left   max_download_count - download_count（下限 0，方便直接显示「还剩几次」）
file_in_trash    关联文件在不在回收站
bucket / size    关联文件的桶与大小（原来就有）
url / share_url  完整分享链接（与 create 一致）

CLI 打印（同一处口径）：文件与 Bucket、分享链接、下载次数、到期、状态、message；
   下载次数用满时那一行写「下载次数：20/20（下载次数已满）」，
   过期时到期那一行写「到期：2026-10-20T14:03:11（已过期）」，
   链接不可用时也照给，但标注「（当前不可下载，链接已失效）」——「看全部信息」而不是藏字段。
```

---

# 50. Share Access

访问 Share 时：

```text
Share
 ↓
检查有效性
 ↓
检查 File
 ↓
检查 File 状态
 ↓
检查下载次数
 ↓
检查过期时间
 ↓
开始下载
```

Share 不得绕过 File 状态。

**提交 `d5779db` 的落地（本节是 share 模块的中心约束）**：

```text
create   要求文件属于当前用户、且**不在回收站**，否则 FMT-503
回收站   文件进回收站后，它的 share **立刻不可用**（FMT-503，状态「关联文件不可用」），
         并且**阻断新的 create**
下载     register_download() 在**业务锁内**一次完成「全部检查 + 计数 +1 + 落盘」——
         所以第 51 节那个「19 + 两次 = 21」不可能发生
         计数写不进去就**拒绝这次下载**（不放行，否则会超发）
HTTP     公开下载端点已实现（提交 d3aeb3d）：GET /api/share/<share_id>/download，
         不要 token（链接本身就是凭证）、先记账再放行；管道与 HTTP 都走 share.download
```

---

# 51. Share 下载次数并发

例如：

```text
最大次数 = 20
当前次数 = 19
```

同时两个请求进入：

```text
Request A
Request B
```

必须避免：

```text
20
21
```

正确结果必须只能允许一个请求完成最后一次。

因此：

```text
检查次数
+
确认下载资格
+
增加计数
```

必须具备原子性/并发保护。

**提交 `d5779db` 的实现**：`register_download()` 在**业务锁内**一次完成
「全部检查 + 计数 +1 + 落盘」，因此「19 + 两次 = 21」不可能发生；
**计数写不进去就拒绝这次下载**（不放行——否则会超发）。

---

# 52. Trash Service

负责：

```text
trash list
trash get
trash restore
trash delete
trash empty
```

**`trash empty`（提交 `674d0b0` 新增）：一次清空回收站的两级条目**：

```text
语义       文件级 + 桶级**一次全删**，永久删除、不可恢复
形状       与其它破坏性操作一致：dry_run 预检回
           {files, buckets, bytes, needs_confirm, blocked:false, message}
           缺 force → FMT-016；CLI 侧 --yes 或窗口里答 y
空回收站   needs_confirm = false，**直接成功返回（0 项）**，不打扰用户
消息       预检「永久删除回收站里的全部 N 项（x 个文件、y 个桶，共 SIZE），不可恢复」
           执行「已清空回收站：x 个文件、y 个桶，共释放 SIZE」
实现要点   ① **每删一项都重新 list 一遍**——删掉一项后其余条目的索引/路径会变，
              用旧列表接着删会大面积失败
           ② **先删文件级、再删桶级**（避免「条目没了、数据还在」的孤儿）
           ③ 单条失败**跳过并记日志**，不卡死整个清空；kMaxRounds 兜底防死循环
HTTP       路由**没有加**（DELETE /api/trash）——HTTP 入口现在按用户决定是关闭的，
           先不加，等那批「简单接口」一起做（FMT 技术文档.md 第 12.3 节有注明）
```

**阶段状态（提交 `0fc242b` 之后：两级都可用）**：`trash` 组原本整组排在阶段 7，
桶级先落地（c2d545d、4fee290），**文件级在提交 `0fc242b` 落地**——**文件级与桶级现在
走同一份视图**：

```text
trash list      两级都列，并且**每一条都标出是 [文件] 还是 [桶]**（0fc242b）
trash get       单个条目：类型 / 标识 / 名称 / 所属 Bucket / 删除时间 / 路径 / 大小或文件数
trash restore   [文件] 按 file_id 搬回原 Bucket 的原位置；[桶] 目标不在 → 整单搬回，
                目标已存在 → 合并（先确认；有同名文件 FMT-403，第 56.3 节）
trash delete    **永久删除、不可恢复**，两级都要显式确认（第 59 节）
```

> 旧口径「`trash get` / `trash delete` 未实现 → FMT-602」「文件级条目只写得进、
> 读不出、属阶段 7」**都已作废**（前者 `4fee290`，后者 `0fc242b`）。
> 桶级与文件级**共用同一个命令名**，靠标识解析区分（第 53.4 节）。
>
> **服务端实现位置（提交 `0fc242b`）**：`src/service/commands.cpp` 的 `trash_command()`
> 现在只做参数与 JSON 形状，业务交给 **`src/trash/trash.cpp` 的 `TrashService`**，由它
> **组合** `FileService`（文件级）与 `BucketService`（桶级）——`TrashService` 自己不存
> 状态，只做合并与标识解析（见第 53.4 节）。桶级那套 `find_trashed()` 仍然服务
> `BucketService` 自己的 `restore` / `get_trashed` / `purge`（第 56.1 节）。

> 上面那张表原来的写法是「`trash get` 未实现 → FMT-602」「`trash delete` 未实现 → FMT-602
> （阶段 7）」，**该口径已作废**（提交 `4fee290`）。桶级四条命令都能成功执行，不再返回
> `FMT-602`；命令总览里 `(trash) list get restore delete` 已完整落在「可用命令」组。
> （当时「服务端尚未实现」组里就只剩 `share` 一行；**提交 `d5779db` 起 `share` 也落地了**，
> 见第 68、95、105 节。）

**提交 `0fc242b` 之后不再有「文件级待阶段 7」这个缺口**：条目权威改成
`file.json`（第 17.1 节），`TrashService` 把两级合成一份列表并负责标识解析。
原口径「`trash list` 只列桶级、文件级条目扫不到」**已作废**。

**服务端实现位置（提交 `0fc242b` 之后）**：`trash_command()` 只做参数与 JSON 形状，
业务在 **`src/trash/trash.cpp` 的 `TrashService`**（组合 `FileService` + `BucketService`，
合并两级视图并做标识解析）；桶级细节仍落在 `BucketService`
（`list_trashed()` / `restore()` / `get_trashed()` / `purge()`，定位共用私有 `find_trashed()`），
文件级细节落在 `FileService`（`list_trashed()` / `check_restore()` / `restore()` / `purge()`）。

---

# 53. Trash List

## 53.1 列表（两级统一形状，提交 0fc242b）

命令与请求：

```text
fmt> trash list
op = "trash.list"（无参数）
HTTP：GET /api/trash
```

响应 `data`（与源码一致）：

```json
{ "entries": [
    { "type": "bucket", "id": "lazy-fox_20261008012233", "name": "lazy-fox",
      "bucket": "", "deleted_at": "2026-10-08T01:22:33", "bytes": 0, "files": 0,
      "present": true, "restorable": true,
      "trash_path": "trash/user/lazy-fox_20261008012233" },
    { "type": "file", "id": "fmt-20261008-0", "name": "a.txt",
      "bucket": "工作", "deleted_at": "2026-10-08T02:10:00", "bytes": 1024, "files": 1,
      "present": true, "restorable": true,
      "trash_path": "trash/user/.files/工作/2026/10/08/a.txt" } ],
  "count": 2, "files": 1, "buckets": 1 }
```

| 字段 | 说明 |
| --- | --- |
| `type` | `"file"` / `"bucket"`——**列表里每一条都标出是哪一类**（提交 `0fc242b`，用户明确要求） |
| `id` | 文件 = `file_id`；桶 = 回收站里的目录名（`<原桶名>_<14 位时间戳>`）。**这才是回退/永久删除该传的标识** |
| `name` | 展示名：文件 = 文件名；桶 = 原桶名（`.original` 里没记录时给回收站目录名） |
| `bucket` | 文件所属 Bucket（用户 + 记录里的 `bucket`）；桶级条目为空串 |
| `deleted_at` | 删除时间（本地时间 ISO 8601）：文件取 `file.json` 的 `deleted_at`（老数据从 `trash.json` 补），桶取 `.original` |
| `bytes` | 文件 = `size`；桶级 = 目录占用总字节数（**提交 `18f16ca` 起列表里就算出来**，见下面的代价说明） |
| `files` | 文件 = 1；桶级 = 目录里的实际文件数（同上） |
| `present` | **数据在不在磁盘上**（提交 `5b316b3` 写死语义、`8f0fd5c` 补齐桶级）：文件 = `.files/` 下那个文件还在不在，桶 = 那个回收站目录还在不在。**回退/永久删除的结果条目不翻转它**（结果由 `message` 说明）——**两级、restore / purge 四种结果现在都不翻转**；`false` 一律如实报告，不擅自清理。**原口径**「回退成功时把它置 false（表示已经不在回收站里）→ CLI 打印『状态：数据已不存在』」**已作废**：那是 `5b316b3` 修掉文件级、`8f0fd5c` 修掉桶级的误导信息（数据刚被搬回仓库）。用例 `Service.管道能执行回收站命令`（桶级 restore / purge 都断言 `present == true`）与 `Trash.文件级条目能列出并回退` / `Trash.永久删除文件级条目`（文件级同样断言）钉着这条语义 |
| `restorable` | 能不能**单独**回退：随桶删除的文件是 `false`（第 58 节），`.original` 里没有原名的桶也是 `false`，此时带 `reason` 说明 |
| `trash_path` | 数据的实际落点，相对数据根、正斜杠（能推出来时才给） |
| `reason` | 只在不能回退时出现：为什么不能（随桶删除 / 缺身份记录 / 数据缺失） |

`count` = 总条数，`files` = 文件级条数，`buckets` = 桶级条数。
排序：**按 `deleted_at` 倒序**（最近删除的在前）；时间相同时先桶后文件、再按 `id`，保证顺序稳定。
**`trash.json` 里的老文件级记录不再单独列**——它的权威位置是 `file.json`（第 17.1 节）。

**桶级条目的 `files` / `bytes` 在列表里就算出来（提交 `18f16ca`）**：`TrashService::list()`
对每个 `present == true` 的桶级条目调一次 `get_trashed()`（遍历那个回收站目录），
把文件数与占用填进条目，所以列表里直接看得到「几个文件、多大」。
**代价（如实记录）**：每个桶条目多遍历一次目录，`trash list` 因此**不是纯索引查询**
——它是用户显式敲的命令，这个代价可以接受；`list_trashed()`（`BucketService` 的索引层）
仍然不遍历目录。原口径「桶级的 `files`/`bytes` 在列表里恒为 0、精确值只在
`trash get`/预检里给」**已作废**（那是 `0fc242b` 到 `18f16ca` 之间的实况）。

CLI 打印（`src/cli/cli.cpp`，提交 `0fc242b`；`18f16ca` 起桶级条目带文件数/占用）：

```text
fmt> trash list
  [文件]  a.txt（Bucket 工作，1.2KB）
  [桶]    工作（3 个文件，5.0KB）  ->  工作_20261008151538
共 2 项（1 个文件、1 个桶）
```

（桶级那一行的 `（N 个文件，X）` 只在 `files > 0` 时打印；`name` 与 `id` 相同时不打印
`  ->  <id>` 那一段。）

三条边界**如实报告、不擅自清理**：

- 索引里有、目录没了 → 该条标 `present: false`，CLI 在行尾加 `(目录已不存在)`；
- 目录在回收站里、`.original` 里没有记录（手工拷进来的、旧版本留下的）→ 照实列出，
  `original` 为空，CLI 显示 `(原名称未记录，无法回退)`；回退时按第 56.1 节拒绝，
  **不靠剥离时间戳猜原名**。
- **扫描形状检查（提交 `4fee290`）**：扫 `trash/<user>/` 时
  ① 跳过点开头的条目（`.files` / `.original`，第 21 节）；
  ② **只认符合 `<名字>_<14 位时间戳>` 或 `<名字>_<14 位时间戳>_<1-3 位序号>` 形状的目录**。
  别的目录（手工塞进来的、别的东西）不会被误当成桶级条目。用例
  `Bucket.回收站扫描只认桶级条目` 覆盖这一点，`Bucket.条目详情与永久删除` 覆盖详情与永久删除。

## 53.2 文件级条目（**读取侧提交 `0fc242b` 已落地**）

只列 `current_user` + `is_trash = true` + **`trash_reason == "file"`** 的记录：

```text
权威        file.json 那一条记录（is_trash / trash_reason / deleted_at），字段见第 17.1 节
排序        deleted_at 倒序；时间相同按 file_id（入库顺序）
数据位置    trash/<用户>/.files/<桶>/YYYY/MM/DD/<文件名>，由 file_id 与记录推出
随桶删除的  trash_reason == "bucket" 的**不在这里列**：它们的整棵树挂在桶级条目下面
            （trash/<用户>/<桶>_<时间戳>/），再列一遍就是重复计数（第 58 节）。
            按 file_id 仍查得到，但 restorable = false 并说明「只能整体恢复那个桶」
```

**老数据兼容（提交 `0fc242b`）**：`trash.json` 里的旧文件级记录不再写入，
只在记录缺 `deleted_at` 时用它补上（`legacy_deleted_at()`），并在回退/永久删除时清掉
（`remove_trash_record()`；清不掉只记一行 WARN，不算失败）。

**原口径「写入侧 ✅ / 读取侧 ❌，文件级条目属阶段 7」已作废**——文件级条目现在能在
`trash list` / `get` / `restore` / `delete` 里看到、回退、永久删除（第 52、54、59 节），
用例 `Trash.文件级条目能列出并回退`、`Trash.永久删除文件级条目` 覆盖。

## 53.3 单条条目详情：`trash get`（**两级统一形状，提交 `0fc242b`**）

命令与请求：

```text
fmt> trash get <标识>
op = "trash.get"，args.argv = ["<标识>"]
HTTP：GET /api/trash/<标识>（路径参数百分号解码）
标识：file_id（文件）/ 回收站目录名（桶）/ 原名（桶名或文件名），见 53.4
```

**定位方式：先做标识解析**（第 53.4 节：① 回收站目录名 → ② `file_id` → ③ 桶原名 →
④ 文件名；③④ 命中多条 → `blocked` + 候选）。解析出来是哪一级就取哪一级的条目，
两者都没有 → `FMT-400 TrashEntryNotFound`（退出码 3）。
**索引里有、目录没了不报错**（`present: false`，如实报告、不擅自清理）；
**孤儿目录**（目录在、`.original` 里没记录）也能按目录名查到（`restorable: false` + `reason`）。

响应 `data`（统一形状 `TrashEntry`，**不再是顶层 `trashed` / `original` / `files`**）：

```json
{ "entry": { "type": "bucket", "id": "lazy-fox_20261008012233", "name": "lazy-fox",
             "bucket": "", "deleted_at": "2026-10-08T01:22:33",
             "bytes": 1228, "files": 2, "present": true, "restorable": true,
             "trash_path": "trash/user/lazy-fox_20261008012233" } }
```

| 字段 | 说明 |
| --- | --- |
| `type` | `"file"` / `"bucket"` |
| `id` | 文件 = `file_id`；桶 = 回收站目录名 |
| `name` | 展示名：文件名 / 原桶名（缺身份记录时给目录名） |
| `bucket` | 文件所属 Bucket；桶级为空 |
| `deleted_at` | 删除时间 |
| `bytes` / `files` | 文件：`size` / 1；桶：**这一条会递归数目录** |
| `present` | **数据在不在磁盘上**（提交 `5b316b3` + `8f0fd5c` 写死语义）；`restore` / `purge` 的结果条目**不翻转它**（两级都是）——恢复成功后数据在仓库里，所以它是 `true`，别拿它当「还在不在回收站」用 |
| `restorable` | 能不能单独回退；`false` 时带 `reason` |
| `trash_path` | 相对数据根、正斜杠 |

> **遍历目录的范围（提交 `18f16ca` 起）**：`trash get`、`trash restore` / `delete` 的预检，
> **以及 `trash list` 里的每个 `present` 桶级条目**都会遍历一次目录。
> 列表因此不是纯索引查询——显式命令，代价可以接受；`BucketService::list_trashed()`
> 那层索引仍然不遍历目录（第 53.1 节有同样的说明）。

CLI 输出（`src/cli/cli.cpp` 的 `print_trash_entry()`；**文件与桶都标出来**）：

```text
fmt> trash get lazy-fox_20261008012233
[桶] lazy-fox
  标识：lazy-fox_20261008012233
  文件数：2
  删除时间：2026-10-08T01:22:33
  回收站路径：trash/user/lazy-fox_20261008012233

fmt> trash get fmt-20261008-0
[文件] a.txt
  标识：fmt-20261008-0
  Bucket：工作
  大小：1.2KB
  删除时间：2026-10-08T02:10:00
  回收站路径：trash/user/.files/工作/2026/10/08/a.txt
```

不能单独回退时多打一行 `可回退：否（<reason>）`，数据不在时多打一行 `状态：数据已不存在`；
`restorable = false` 的条目**也能查到**，只是 `trash restore` 会拒绝它。

## 53.4 标识解析（不猜，提交 0fc242b）

`trash get` / `restore` / `delete` 共用一个标识解析（`TrashService::resolve()`），
四种来源按顺序试：

```text
① 桶的回收站目录名（精确）  lazy-fox_20261008012233 —— 命中最确定，先试它
② file_id                   全局唯一；**查所有回收站记录**（含随桶删除的），
                            否则按 id 找那个文件会得到「没有这个条目」，
                            而真相是「它在那个桶里、只能整体恢复」
③ 桶的原名                  可能多条（同一个桶删过两次）
④ 文件名                    可能多条（同名文件删过多次，或跨 Bucket）
```

③④ 命中**多条** → `blocked` + 候选（`type` / `id` / `name` 各带上），消息提示
「请用 file_id（文件）或完整的回收站名（桶）指定」；一条都不命中 →
`FMT-400 TrashEntryNotFound`，消息里**补一句**「如果是随 Bucket 删除的文件，它的整棵树
挂在桶级条目下，用 `trash list` 找到那个桶再整体恢复」。
桶的回收站名一律带 `_<14 位时间戳>`，与 `file_id`（`fmt-YYYYMMDD-N`）形状不可能撞车。

---

# 54. Trash Restore

> **适用范围（提交 `0fc242b` 之后两级都适用）**：本节讲**文件级**回退
> （`trash restore <file_id>`）的逐个文件口径；**桶级回退**是整单搬回或整单合并
> （第 56 节），不走这里。两级共用一个命令名与一套标识解析（第 53.4 节），服务端由
> `TrashService` 组合 `FileService` / `BucketService` 实现。**原口径「本节属阶段 7、
> 尚未实现」已作废。**

恢复前：

```text
检查 Trash 记录          file_id 在 file.json 里、is_trash == true（否则 FMT-001 / FMT-002）
 ↓
随桶删除？               trash_reason != "file" → FMT-402 RestoreBucketMissing：
                         「…是随 Bucket「X」一起删除的，只能整体恢复那个桶」（第 58 节）
 ↓
数据还在？               .files/ 下的源文件不在 → FMT-002，消息给出路径
 ↓
检查文件名冲突           目标位置已有同名正常文件 → FMT-401 RestoreConflict，
                         消息点明冲突那条的 file_id；不覆盖、不改名（第 55 节）
```

无冲突：

```text
恢复实际文件             move_file(回收站 → 仓库原位置)，先 ensure_directory 建父目录
 ↓
is_trash = false
 ↓
trash_reason / deleted_at 一起清空
 ↓
写回 file.json（顺手兼容清理老 trash.json，第 17.1 节）
```

**回退可以按 `dry_run` 预检**（`args.dry_run = true` / HTTP `?dry_run=1`）：
返回 `{needs_confirm, blocked, entry, message?}`。**上面三种拦住的情况都是
`blocked = true`、`needs_confirm = false`**——它们不是「确认一下就能做」的事
（同名冲突、随桶删除、数据缺失都靠 y/N 解决不了，第 55、58 节）。
CLI 目前只在 `file.delete` / `trash.delete` / `bucket.delete` 前自动发预检；
`trash restore` 的预检供 HTTP / 脚本入口用，`trash get` 的 `restorable` 字段也把
「能不能单独回退」提前摊出来。

---

# 55. 冲突恢复

> **适用范围（提交 `0fc242b` 之后已实现）**：本节规则属于**文件级恢复**
> （`trash restore <file_id>`）。**桶级回退不走这里**——桶级是整单搬回或整单合并
> （第 56 节）：目标 Bucket 已存在时先看有没有同名文件——有就**一个都不搬**
> （`FMT-403`，见第 56.3 节），没有才在用户确认之后把文件合进去；
> 两条路都**不覆盖、不改名**。
> 原口径「本节属阶段 7」已作废。

如果目标位置已有同名正常文件：

```text
不覆盖
不改名
```

被恢复文件：

```text
继续留在 Trash
```

metadata：

```text
保持不变
```

提示用户：

```text
存在文件名冲突，请处理冲突后再次恢复。
```

**实现口径（提交 `0fc242b`，`FileService::check_restore()` / `restore()`）**：
冲突判定是「同用户 + 非回收站 + 名字相同（`iequals`）+ **与记录自己的 Bucket 同名**」
——名字在同一个 Bucket 内唯一（第 38 节，作用域在本次改动（未提交） 收窄到桶内：
回退落点由 `record.bucket` 决定，别的桶里的同名文件与本条记录不相干），
所以要么没有、要么撞一条。命中就返回 **`FMT-401 RestoreConflict`**
（退出码 4），消息**点明冲突那条的 `file_id`**：

```text
回退失败：Bucket「工作」里已经有同名正常文件：a.txt（file_id fmt-20261008-3）。
请先改名或删除它，或者用 trash delete 永久删除回收站里这一份
```

两份数据**都保留**（新的在仓库里、旧的还在 `.files/` 下），`restore` 不覆盖、不改名。
目标位置被**文件系统里别的东西**占着（`file.json` 里没有这条记录、磁盘上有同名文件）时
同样拦住（也是 `FMT-401`，消息给出路径）——否则 `move_file` 会把它覆盖掉。
用例 `Trash.回退遇同名冲突要拦住` 钉这两条。

---

# 56. Bucket 回退（原「Bucket 部分恢复」口径已作废）

> **口径更正（阶段 4 已实现，commit c2d545d；「已存在时合并」本次修复补上）**：
> 本节旧口径是「Bucket 恢复 = 检查 Bucket → 检查内部文件 → 逐个判断（a.txt 恢复、
> b.txt 恢复、c.txt 冲突留 Trash），最终 Bucket 可以处于『部分恢复』状态」。
> **桶级回退不再这么做**：它是**整单搬回**（目标不在）或**整单合并**（目标已存在，
> 一个文件撞名就一个都不搬，第 56.3 节）——**不存在「部分恢复」的桶**。
> 「部分恢复」的归属是**文件级恢复**（`trash restore <file_id>`，**提交 `0fc242b` 已落地**）；
> 第 54、55 节的「逐个文件判断、冲突不覆盖不改名、留在 Trash」在那里**已经生效**
> （原口径「属阶段 7」已作废）。

旧口径（**作废，仅作对照**）：

```text
a.txt → 无冲突 → 恢复
b.txt → 无冲突 → 恢复
c.txt → 冲突 → 保留 Trash
最终 Bucket 可以处于「部分恢复」状态          ← 阶段 4 起不再有这种状态
```

## 56.1 桶级回退：整单搬回 + 已存在时合并（阶段 4 落地；合并在「桶已存在」修复里补齐）

命令：

```text
fmt> trash restore <名称>
op = "trash.restore"，args.argv = ["<名称>"]
HTTP：POST /api/trash/<名字>/restore（路径参数百分号解码）
```

`<名称>` 的**定位方式**（先按回收站名精确匹配、再按原名且必须唯一）：

```text
1. 先按**回收站里的名字**精确匹配 .original 的 trashed 字段
   （例如 lazy-fox_20261008012233）
2. 再按**原桶名**匹配 original 字段；此时同名多条**必须唯一**，
   否则 FMT-001（InvalidArgument，退出码 2）并列出候选的 trashed 名：
   「有多个同名 Bucket 被删除，请用回收站里的名字指定：lazy-fox_20261008012233、…」
3. 都没匹配上：
   - 回收站里有该目录但 .original 里没有它的记录 → FMT-001
     「回收站条目缺少原桶名记录（.original），无法回退：<名称>」——**不猜名字**
   - 否则 → FMT-400 TrashEntryNotFound（退出码 3）
```

> **命令层的标识解析在 `TrashService::resolve()`**（提交 `0fc242b`，第 53.4 节：
> ① 回收站目录名 → ② `file_id` → ③ 桶原名 → ④ 文件名；③④ 多条 → `blocked` + 候选）。
> 走到桶级之后，`BucketService::find_trashed()` 仍是「回收站名精确优先、原桶名必须唯一」
> 这一套（下面 1～3 的规则对**桶级**仍逐字成立）。
>
> **`trash get` / `trash delete` 复用同一套定位（提交 `4fee290`）**：三者共用
> `BucketService::find_trashed()`——回收站名字优先、原桶名必须唯一（多条 → `FMT-001`
> 并列候选）。区别只在匹配之后：`restore` 搬目录（本节），`get` 报详情（第 53.3 节），
> `delete` 永久删除（第 60 节）。**`get` / `delete` 都允许「索引里有、目录没了」的条目**：
> `get` 返回 `present: false`（不报错），`delete` 照样能把它清掉；
> **孤儿目录**（索引里没有）两者都按目录名处理——`get` 能查到（`original` 为空）、
> `delete` 能删掉，只有 `restore` 会拒绝（原名未知，不猜）。

判定与执行：

```text
目标 repository/<user>/<原名> 已存在？
  ├─ 否 → 整个目录一次 std::filesystem::rename 搬回
  │        （整棵树一次搬走，所以**不存在文件级冲突**）
  └─ 是 → **合并**（第 56.3 节，本次修复）：把回收站目录里的文件逐个搬进那个桶
           预检：现有桶里有同名文件 → 一个文件都不搬 → FMT-403（确认也解决不了）
                 没有同名文件 → 说清「搬几个、目标已有几个」→ 要一次确认（FMT-016）
                 确认之后才逐个 move；**不覆盖、不改名**
```

完整顺序（与删除方向对称：**索引最后减，减不掉就把目录退回去**）：

```text
1. current_user 为空 → FMT-604；名称为空 → FMT-001
2. 读 trash/<user>/.original：损坏 → FMT-006 JsonParseError，停止
3. 按上面的定位方式找到唯一一条；original 为空 → FMT-001（缺原桶名记录）
4. 回收站目录不存在 → FMT-400「回收站目录已不存在：<trashed>」
   （trash list 里这一条同时标 present=false）
5. 预检（TrashService::check_restore，只读，第 56.3 节）：
     ├─ 目标不在                 → 纯回退，不需要确认
     ├─ 目标在 + 有同名文件冲突   → FMT-403 RestoreMergeConflict，一个文件都不搬
     └─ 目标在 + 无冲突          → needs_confirm=true（合并）；缺 force → FMT-016
6. 目标不在：整个目录 rename 回 repository/<user>/<原名>（一次搬完）
   目标在  ：逐个 move 回收站目录里的文件 → 目标桶（父目录按需建，已存在就跳过）
7. 从 .original 里**删掉这一条**并落盘；纯回退时**写不进去就把目录 rename 回收站**并报错；
   合并时文件已经落在正确位置，写不进去只留一条幽灵索引记录（记 ERROR 日志，如实报错）
8. 只把 trash_reason == "bucket" 的记录翻回正常：is_trash=false、trash_reason=""；
   用户自己单独删过的文件（trash_reason="file"）保持不动。
   **合并时只翻真正搬过去的那几条**——没搬成的留在回收站，记录保持 is_trash=true
9. 返回 TrashBucket{trashed_name, original_name, deleted_at, directory_present=true}
```

> **恢复桶**不会**自动把它设回当前 Bucket（真机核实，2026-10-09）**：删掉的如果是当前桶，
> `current_bucket` 已经被置空（第 30 节），`trash restore` 只把目录搬回 `repository/`，
> **不碰 `current_bucket`**——用户还要自己敲一次 `bucket use <原名>`。
> 在此之前任何需要当前桶的命令（如 `file list`）会报 **`FMT-305 未设置当前 Bucket`**
> （退出码 3）。这是设计（不替用户选桶），不是缺陷。

响应 `data`（与源码一致；**提交 `0fc242b` 起统一成 `{entry, message}`**）：

```json
{ "entry": { "type": "bucket", "id": "lazy-fox_20261008012233", "name": "lazy-fox",
             "deleted_at": "2026-10-08T01:22:33", "present": true, "restorable": true,
             "trash_path": "trash/user/lazy-fox_20261008012233" },
  "message": "Bucket 已回退：lazy-fox" }
```

> 上面的 `{trashed, original, restored_to, message}` 是 `4fee290` 时的旧形状，**已作废**
> （第 53.1、53.3 节与 `FMT 技术文档.md` 第 12.3.2.1 节有完整表）。
> 真机核实（2026-10-09）：回退成功时打印是
> 「`[桶] tmp-verify` / 标识 / 删除时间 / **`Bucket 已回退：tmp-verify`**」——
> **没有**「状态：数据已不存在」（`present` 不翻转，提交 `8f0fd5c`）。

## 56.2 「部分恢复」属于文件级（**提交 `0fc242b` 已落地**）

文件级恢复（`trash restore <file_id>`）逐个文件判断：无冲突的恢复、冲突的留在 Trash
（不覆盖、不改名，见第 54、55 节）。**如果用户想要的是「桶里的文件能救一个是一个」，
那要走文件级条目，不是 `trash restore <桶名>`**——桶级命令不做部分恢复。

> 但**随桶一起删除的文件不适用**这一节（提交 `0fc242b`）：它们的 `trash_reason`
> 是 `"bucket"`、数据在 `trash/<用户>/<桶>_<时间戳>/` 里，`trash restore <file_id>`
> 会返回 **`FMT-402 RestoreBucketMissing`**，让用户整体恢复那个桶（第 58 节）。
> 「能救一个是一个」只对 `trash_reason == "file"`（自己单独删的）成立。

> **文件级条目的落点已在提交 `4fee290` 定下来**：
> `trash/<user>/.files/<bucket>/YYYY/MM/DD/<file_name>`（`PathManager::trash_file`，第 21 节），
> 而桶级删除的落点是 `trash/<user>/<bucket>_<YYYYMMDDHHMMSS>/`。
> **两者不再共用 `trash/<user>/` 这一层**——原口径「文件级条目直接落在
> `trash/<user>/<bucket>/`、桶级扫描会跳过 `.` 开头的条目但**不会跳过**
> `trash/<user>/<bucket>/` 这种目录」**已作废**。那正是 `.files/` 这一层要解决的问题：
> 文件级的桶目录如果和桶级条目同层，阶段 5 落地 `file delete` 之后，`trash list` 的桶级扫描
> 就会把 `trash/<user>/<bucket>/` 误当成「孤儿桶条目」列出来。
> 现在桶级扫描跳过点开头的条目（`.files` / `.original`），并且只认
> `<名字>_<14 位时间戳>`（可带 `_<1-3 位序号>`）形状的目录（第 53.1 节）。
> 桶被删时，先前文件级删掉的东西仍留在 `trash/<user>/.files/<bucket>/` 下，
> 桶目录搬回 `repository/<user>/<bucket>/` 后它还在原处。

## 56.3 目标 Bucket 已存在：合并进去（**本次修复，取代原来的整单拒绝**）

### 56.3.1 问题（用户报的场景）

```text
最开始没有桶 → 创建 test → 上传 a1.jpg、a5.png、a3.jpg
  → file delete a3.jpg            （文件级删除，数据进 trash/<用户>/.files/test/）
  → bucket delete test            （桶级删除，目录进 trash/<用户>/test_<时间戳>/）
  → trash restore a3.jpg          （文件级回退：**会把 repository/<用户>/test/ 建出来**）
  → trash restore test            （桶级回退）
       ↓ 以前
    执行失败：FMT-401 回退失败：Bucket 已存在：test（退出码 4）
```

`a1.jpg` / `a5.png` 明明还躺在回收站目录里，却因为「目标桶已存在」这条整单判定
**一个都拿不回来**——而那个目标桶往往是回退文件时**顺手建出来的空目录**。
「回退文件会把空桶建出来」本身不是缺陷（不建的话文件放不回桶里），缺陷是
桶级回退只有「整目录搬回」这一条路。

### 56.3.2 新口径：合并（要么整单合并，要么一个都不搬）

```text
目标 Bucket 已存在？
  ├─ 有同名文件（回收站里一份、目标里一份）
  │    → **FMT-403 RestoreMergeConflict**（退出码 4）：一个文件都不搬
  │      消息列出冲突的名字，并说明「本操作不覆盖、不自动改名」
  │      用户处理完冲突（改名或永久删掉其中一份）再执行合并
  ├─ 没有任何同名文件
  │    → **合并**：把回收站目录里的文件逐个搬进目标桶（父目录按需建）
  │      **必须先确认**（缺 force → FMT-016「确认合并请加 force（CLI：--yes）」）
  │      现有桶里的东西一个字节都不动
  └─ 回收站条目里已经没有文件（空日期树 / 已经被逐个回退过）
       → 只摘掉 .original 那条记录并收掉空目录，**不打扰用户**（不需要确认）
```

要点：

```text
搬什么     回收站目录里的**普通文件**，按桶内相对路径落到目标桶的同名位置
           （例如 trash/user/test_.../2026/10/12/a1.jpg → repository/user/test/2026/10/12/a1.jpg）。
           .files/ 是**文件级条目**的落点，不在桶级条目里：随桶删除的文件本来就
           带着 trash_reason="bucket" 记录、数据跟着桶目录走（第 56.2、58 节）。
不覆盖     目标位置已经有东西（同名文件，或 file.json 里没有的残留）就跳过那个文件，
           **绝不覆盖、绝不改名**；预检已经拦过，执行时是第二道保险
名字拼写   目标桶在磁盘上的实际拼写必须与原桶名（.original 的 original）**完全一致**，
           否则 FMT-401 —— file.json 里的 bucket 字段是删除时的拼写，所有路径都由记录
           推导，改名会让那些记录指向不存在的目录
索引       全部文件都搬完才摘 .original；中途有搬运失败的那些**留在回收站**、
           .original 保留（下次还能再来一次），日志记下失败的文件
记录       只翻**真正搬过去**的那几条 file.json 记录（is_trash=false、trash_reason=""）；
           没搬成的继续 is_trash=true（否则 file list 会列出磁盘上不在那儿的文件）
空目录     全部搬完之后回收站目录里只剩空的日期树 → 收掉它（remove_all）。
           不收的话它会以「孤儿桶条目」的形状继续出现在 trash list 里
```

预检（`trash.restore` 的 `dry_run` / HTTP `?dry_run=1`）在原有字段之外多带：

```json
{ "needs_confirm": true, "blocked": false, "merge": true,
  "entry": { "type": "bucket", "id": "test_20261012001929", "name": "test", "present": true,
             "restorable": true, "files": 1, "bytes": 5,
             "trash_path": "trash/user/test_20261012001929" },
  "merge_target_exists": true, "merge_files": 1, "merge_bytes": 5, "target_files": 0,
  "conflicts": ["2026/10/12/a1.jpg"],
  "message": "回退：Bucket「test」已存在，将把回收站里的 1 个文件（5B）合并进去，现有 0 个文件保持不动、不覆盖、不改名" }
```

`conflicts` 只在有冲突时出现；有它时 `blocked=true`（确认也解决不了）。
执行成功的响应把结果写进 `entry.message`，命令层照实转述：

```text
Bucket「test」：已合并 1 个文件
Bucket「test」：已合并 1 个文件，1 个留在回收站里（搬动失败）
```

预检里的**同名判定**同时看两处，避免「记录里没有、磁盘上有」的漏网：

```text
① 目标桶在磁盘上的文件树（桶内相对路径或文件名相同，iequals 比）
② 目标位置本身是否存在（file.json 里没有记录的残留）
```

实现落点：`BucketService::check_merge()` / `merge()` / `merge_into_existing()`
（`include/fmt/bucket/bucket.hpp`、`src/bucket/bucket.cpp`），闸门在
`TrashService::check_restore()` / `restore(identifier, confirmed)`
（`include/fmt/trash/trash.hpp`、`src/trash/trash.cpp`）；
命令层 `trash.restore` 校验 `force` 并转发（`src/service/commands.cpp`）；
CLI 把 `trash.restore` 加进「先预检再确认」的破坏性操作清单（`src/cli/cli.cpp`）——
普通回退的预检 `needs_confirm=false`，所以这条门槛不妨碍它。

### 56.3.3 为什么不是「自动合并」/「自动改名」

自动合并会在用户没看见的情况下**改变现有桶的内容**（多出文件）；
自动改名（`test` → `test_2`）会让 `file.json` 里的 `bucket` 字段与磁盘路径对不上。
两条都会制造「记录与磁盘各说各话」的隐患，所以合并**必须**由用户确认，
冲突**必须**由用户处理。

---

# 57. Bucket 恢复的数据原则

Bucket 本身没有 `file_id`。

Bucket 下文件只修改：

```text
is_trash
trash_reason
```

例如：

```text
is_trash=true, trash_reason="bucket"      ← 桶被删时置的
```

变为：

```text
is_trash=false, trash_reason=""
```

**只翻回 `trash_reason == "bucket"` 的那些**；用户自己删过的文件
（`trash_reason="file"`，阶段 5 起）保持不动，继续留在 Trash。
（阶段 4 已按这条实现：`BucketService::set_bucket_files_trash_flag`。）
**合并回退（第 56.3 节）再筛一道**：只翻**真正搬进目标桶**的那几条——
没搬成的文件还在回收站目录里，记录必须保持 `is_trash=true`。

其他 File metadata：

```text
file_id
user
bucket
file_name
extension
file_type
size
md5
```

保持不变。

---

# 58. 原 Bucket 不存在时恢复 File

> **适用范围（提交 `0fc242b` 之后已实现）**：本节讲的是**文件级**恢复
> （`trash restore <file_id>`）遇到「随桶一起删除」或「原 Bucket 已不在」的情况；
> 桶级回退见第 56 节。桶被**软删除**（在回收站里）不触发本节——那时原位置空着，
> 桶级回退直接把整个桶搬回去，或者按 `file_id` 回退时返回 `FMT-402` 让你整体恢复它。
> **原口径「属阶段 7」已作废。**

如果：

```text
原 Bucket 已永久删除（或文件是随桶一起删的）
```

此时：

```text
trash restore <file_id>
```

返回 **`FMT-402 RestoreBucketMissing`**（退出码 3），消息给出该文件所属的桶名，
要求用户**整体恢复那个桶**（`trash restore <桶的回收站名>`）。

不能：

```text
自动创建 Bucket
```

（这条**没有变**：V1 绝不自动创建恢复目标。`FMT-402` 的语义在提交 `0fc242b` 之后
扩展为「这个文件不能单独回退，回退它的桶」——新的消息与用例
`Trash.随桶删除的文件不能单独回退` 都按这个口径。）

---

# 59. Permanent Delete

命令：

```text
trash delete <标识>        两级通用（提交 0fc242b）：file_id（文件）/ 回收站目录名（桶）/
                           原名（桶名或文件名，见第 53.4 节）
```

这是永久删除。

**阶段状态（提交 `0fc242b`：两级都已实现）**：桶级在 `4fee290` 落地，**文件级在
`0fc242b` 落地**（`FileService::purge()`：删 `.files/` 下的数据 → 从 `file.json` 删掉
那条记录 → 顺手清掉老的 `trash.json` 记录）。原口径「桶级未实现 → `FMT-602`」
「文件级永久删除属阶段 7」**都已作废**。

**执行前要求明确确认（提交 `711da4c` 起错误码改为 `FMT-016`）**：

```text
服务端          请求里没有 force == true → **FMT-016 ConfirmRequired**（退出码 2），
                默认消息「该操作需要显式确认（force）」，这里的具体消息是
                「永久删除不可恢复，需要确认（force = true）」。任何入口都一样。
预检            args.dry_run = true（HTTP：?dry_run=1）→ TrashService::check_purge()，
                **只读**，needs_confirm **恒为 true**（永久删除永远要确认），
                并把要毁掉的东西摊开：type / id / name / bucket / deleted_at /
                bytes / files / trash_path，消息形如
                「永久删除后不可恢复：工作_20261008012233（原桶 工作，1 个文件，26B）」；
                文件级条目形如「永久删除后不可恢复：a.txt（Bucket 工作，1.2KB）」
CLI 交互窗口    先打印预检（条目详情 + 消息），再问「确认执行？(y/N)」；
                答 n（或直接回车）→ 打印「已取消」，退出码 0，**不发请求**
CLI 一次性命令  必须加 --yes（或 -y），否则**本地拒绝**：
                stderr 打印「该操作需要确认（FMT-016）：请加 --yes，或在交互窗口里执行」，退出码 2
HTTP            DELETE /api/trash/<标识> 需要 ?force=1（或 force=true，大小写不敏感）
                或请求体 {"force":true}，否则 400 + FMT-016
```

> **原口径「缺确认 → `FMT-001 InvalidArgument`」已作废**（提交 `711da4c`）：
> 缺确认是一个**独立**的错误码 `FMT-016 ConfirmRequired`（退出码 2，HTTP 400，
> 第 66、82 节），不再与「参数错误」共用 `FMT-001`。这样脚本既能区分
> 「参数写错」与「忘了确认」，也不必再靠消息文本判断。
>
> **`--yes` / `-y` 是 CLI 本地开关**，不会作为位置参数发给服务端——CLI 只是在预检通过
> 之后置 `request.args["force"] = true`。**预检负责「说清楚」，force 负责「兜底」**：
> 别的客户端绕过预检直接发 `trash.delete`，服务端照样用 `FMT-016` 拦下来。

流程（**提交 `0fc242b` 之后两级都按这套走**，顺序是刻意的，理由见第 60 节）：

```text
预检（dry_run，只读）    说清楚要毁掉什么：类型 / 标识 / 名称 / 所属 Bucket /
                        删除时间 / 大小或文件数 / 回收站路径
 ↓
用户确认（y/N 或 --yes）
 ↓
检查 Trash               标识解析（第 53.4 节）：多命中 → blocked + 候选，不再往下走
 ↓
删除实际数据             文件：.files/ 下的那个文件；桶：整个回收站目录 remove_all
 ↓
删除 metadata            文件：从 file.json 删掉那条记录
                        桶：清 trash_reason == "bucket" 的 file.json 记录（第 60 节）
 ↓
删除 Trash 记录          文件：记录已经删了；桶：从 .original 摘掉那条
 ↓
兼容清理                 老的 trash.json 记录顺手清掉（失败只记 WARN）
 ↓
清理相关 Share           ← **尚未实现**：阶段 7 的收尾项（share 模块本身已落地），见第 60 节
```

**响应 `data`（提交 `0fc242b` 统一形状）**：

```json
{ "entry": { "type": "bucket", "id": "lazy-fox_20261008012233", "name": "lazy-fox",
             "bucket": "", "deleted_at": "2026-10-08T01:22:33",
             "bytes": 1228, "files": 2, "present": false, "restorable": true,
             "trash_path": "trash/user/lazy-fox_20261008012233" },
  "message": "已永久删除：lazy-fox" }
```

> **原口径的顶层 `trashed` / `original` / `removed_files` / `removed_records` /
> `message` 形状已作废**（提交 `0fc242b`）：现在统一是 `{entry, message}`，
> 条目形状见第 53.3 节的表（`entries` 里每一条同形）。`message` 也从
> 「已永久删除：X（N 个文件，M 条记录）」简化为「已永久删除：<name>」——
> 文件数/占用在预检里已经说过一遍，条目里也带着。

---

# 60. Bucket Permanent Delete

如果永久删除 Bucket：

```text
Bucket
 ↓
所有实际文件
 ↓
全部删除
```

同时：

```text
file.json
trash.json
share.json
```

中的关联记录进行清理。**桶级的实际口径（提交 `4fee290`）**：

```text
file.json    只清 user + bucket 匹配、is_trash == true **且 trash_reason == "bucket"** 的记录；
             文件级删除（trash_reason == "file"）的记录**绝不动**——那些文件的数据
             不在这个回收站目录里，跟着删就真丢了。
trash.json   桶级条目本来就不写这里（第 17.2 节），所以没有动作；桶里那些文件的
             老 trash.json 记录也只是遗留副本（权威在 file.json，第 17.1 节），
             不因为桶被永久删除就额外处理
share.json   **尚未实现**：阶段 7 的收尾项——share 模块本身已落地，只是现在还没有
             任何联动清理 share.json 的动作（见本节末）。
```

**桶级永久删除的三步（提交 `4fee290` 已实现，`BucketService::purge()`）**：

```text
① 删磁盘数据    remove_all 整个回收站目录 trash/<user>/<trashed>/
                失败就什么都没变——索引还在，可以重来。
 ↓
② 清 metadata   只清 file.json 里 user + bucket 匹配、is_trash == true
                且 trash_reason == "bucket" 的记录（removed_records 是清掉的条数）
 ↓
③ 摘索引        最后从 trash/<user>/.original 里删掉对应那条（trashed 匹配）
```

**顺序是刻意的**：先把数据删掉，中途任何一步失败都不会出现「索引没了、目录还在」的
幽灵条目（那种条目反而永远清不掉）；如果反过来先摘索引再删目录，失败就会留下一个
谁也认不出的孤儿目录。三步都做完才算成功，中途失败可以整条命令重来。

两条刻意允许的边界：

- **幽灵条目**（索引里有、目录已经没了）**也能永久删掉**——否则它永远清不掉，只能一直
  被 `trash list` 标成 `present: false`。此时第 ① 步跳过（没东西可删），`removed_files = 0`。
- **孤儿目录**（目录在、索引里没有）也能删：按目录名直接删掉，第 ③ 步没有索引可摘。

> 原口径「**桶级还要清理身份记录**：永久删除回收站里的桶时必须从 `trash/<user>/.original`
> 里删掉对应那条（`trashed` 匹配）」仍然成立，就是上面的第 ③ 步；作废的只是它末尾那句
> 「持久化删除本身仍未实现（阶段 7，`FMT-602`）」。
>
> **提交 `0fc242b` 补充**：`trash delete <桶的标识>` 现在走 `TrashService::purge()` →
> `BucketService::purge()`，上面的三步与顺序一个字没变；**新增的是**动手之前的
> `check_purge()` 预检（`needs_confirm` 恒为真 + 条目详情 + 消息）与
> **`FMT-016`** 那个错误码（缺 `force` 时，见第 59 节）。伪条目 / 孤儿目录的两种边界
> 也照旧允许删。

响应 `data`（提交 `0fc242b` 起**统一形状**，与第 59 节相同）：

```json
{ "entry": { "type": "bucket", "id": "lazy-fox_20261008012233", "name": "lazy-fox",
             "bucket": "", "deleted_at": "2026-10-08T01:22:33",
             "bytes": 1228, "files": 2, "present": false, "restorable": true,
             "trash_path": "trash/user/lazy-fox_20261008012233" },
  "message": "已永久删除：lazy-fox" }
```

| 字段 | 说明 |
| --- | --- |
| `entry.type` / `id` / `name` | 桶级条目：`"bucket"` / 回收站目录名 / 原桶名 |
| `entry.files` / `bytes` | 预检时递归数出来的文件数与占用（执行后沿用同一份） |
| `entry.present` | 执行后为 `false`（数据已经删掉） |
| `entry.restorable` | 执行后无意义，保留原值 |
| `message` | 人话摘要：`已永久删除：<name>` |

> 原字段 `trashed` / `original` / `removed_files` / `removed_records` **已作废**
> （提交 `0fc242b`）。`removed_files` / `removed_records` 这两个数字曾经是唯一的
> 「删掉了多少」来源，现在改由**预检**（`dry_run`）在动手之前给出，
> 更符合「先检查 → 说清楚 → 再确认」的顺序。

服务端要求 `force == true` 才肯执行（第 59 节）；CLI 交互窗口问一次，一次性命令必须
`--yes` / `-y`。用例 `Bucket.条目详情与永久删除`、`Bucket.永久删除只清桶级记录`、
`Bucket.有索引没目录的条目可以永久删掉` 覆盖这三条。

**已知缺口（阶段 7 的收尾项）**：本节原来的要求里包含「清理相关 Share」。这一步**还没做**
——share 模块本身已落地，只是永久删除 / 删桶时还没有联动清理的动作——因此现在
**没有任何**清理 `share.json` 的动作：一个被永久删除的桶如果曾经有分享记录，那些记录会
留在 `share.json` 里。补的时候要在永久删除时加上「按 `bucket` 清掉对应 share 记录」这一步。

---

# 61. 当前 Bucket 状态维护

`current_bucket` 必须满足：

```text
存在
+
属于当前用户
+
处于正常状态
```

否则：

```text
current_bucket = ""
```

不自动选择其他 Bucket。

**阶段 4 实现口径（已落地）**：这条规则由 `BucketService::refresh_current_bucket()` 实现，
落在 `include/fmt/bucket/bucket.hpp` / `src/bucket/bucket.cpp`：

```text
current_bucket 为空            → 什么都不做（不查目录、不写配置）
current_bucket 非空且目录存在  → 什么都不做
current_bucket 非空且目录不存在 → 置空 + 保存 config.json，并记一行 WARN（模块 Bucket）
绝不自动选择别的 Bucket        → 即使只剩一个 Bucket 也不切换
```

「失效」在 V1 的判定就是**目录不存在**：存在性等于 `repository/<user>/<bucket>/` 是否
存在（第 27 节）；`current_bucket` 属于当前用户是由「目录在 `<user>` 之下」天然保证的，
因此不需要额外的归属校验。

**已接线（运行时生效）**：`ServerRuntime` 在两个时机于业务锁下调用它：

```text
服务启动        ServerRuntime::start()      建好上下文后立刻校一次
数据根切换      ServerRuntime::apply_root()（hello 触发的那条路径）换根后校一次
后果            current_bucket 指向的目录不存在 → 置空并落盘；存在 → 保持不动
失败处理        调用返回错误只记一条 WARN（模块 Bucket），不影响启动与换根
```

因为它与业务命令共用运行体的同一把锁，所以不会出现「启动时置空」与「并发命令刚设好
current_bucket」互相覆盖。单测覆盖两种情形：失效置空、有效不清
（`tests/bucket_test.cpp` 的 `当前Bucket失效时置空` 与 `tests/service_test.cpp` 的
`启动时把失效的当前Bucket置空`；后者同时断言「存在的当前 Bucket 不能被误清」）。

---

# 62. 数据一致性检查

建议提供内部：

```text
ConsistencyChecker
```

检查：

```text
File metadata
实际文件
Bucket
Trash
Share
```

---

# 63. 一致性规则

检查：

```text
metadata 存在 && file 不存在
```

属于异常。

检查：

```text
file 存在 && metadata 不存在
```

属于异常。

检查：

```text
metadata.size != actual.size
```

属于异常。

检查：

```text
metadata.md5 != actual.md5
```

属于异常。

---

# 64. 一致性异常处理

V1 默认：

```text
发现异常
 ↓
记录错误
 ↓
停止危险操作
```

不能：

```text
自动删除文件
自动覆盖 metadata
自动重新生成 metadata
```

除非后续提供明确的数据修复命令。

---

# 65. Logger

日志分为三级（**V1 不设 DEBUG**）：

```text
INFO
WARN
ERROR
```

级别与去向（已冻结）：

| 级别 | 日志文件 | 控制台 |
| --- | --- | --- |
| INFO | ✅ | 视 CLI 场景 |
| WARN | ✅ | ✅ |
| ERROR | ✅ | ✅ |

日志文件：

```text
FMT_ROOT/log/
├── fmt.log        全部日志（超过 5 MB 轮转成 fmt.log.1，只留一代）
├── error.log      仅 ERROR 级（同样轮转）
└── （上面两个各可能有一个 .1 备份）

整个 log/ 的预算：8 MB（超过就先删 .1 备份，再保留活动文件最新的一段，见下）
```

不放入 `data/`，因为日志不是业务数据。

**日志轮转（提交 `821aba3`，原来「V1 不做轮转」的口径已作废）**：

```text
上限       超过 5 MB 轮转成 fmt.log.1，**只留一代**；error.log 同理。
           0 表示不轮转（Logger::Options::max_log_bytes，
           默认 kDefaultMaxLogBytes = 5 * 1024 * 1024）。
前置条件   日志**每行开-写-关**（append_line()），不再长期持有 ofstream。
           两个进程（CLI 与服务）共用同一个文件：长期开着的句柄既会挡住改名，
           也会让另一个进程继续往已改名的文件里写——没有这一步，轮转做不成。
检查节奏   每写 **64 行**检查一次大小（kRotationCheckInterval）。不用时间节流：
           写入频率差异大，按行计数既便宜又确定，**测试也能预期**。
           改名失败（另一个进程正好在写）不报错，下一次检查再试。
轮转之后   在新文件里写一行说明：「日志超过 N 字节，已轮转：fmt.log -> fmt.log.1」，
           用户翻日志能看到断点。
打开时     仍然验一次可写（写空串）✓：写不了要立刻报错，而不是等第一条日志静默丢掉。
用例       Logger.超过上限会轮转出一代（上限 1 KB、写 400 行 → .1 存在、两代都非空、
           新文件里有「轮转」说明行）；Logger.上限为零时不轮转（写 200 行 → 没有 .1）
（`FMT 技术文档.md` 第 14.2、14.6、18.36 节）
```

**日志清理（本次改动（未提交），`FMT 技术文档.md` 第 14.7 节）**：轮转管「一个文件别太大」，
清理管「整个 log/ 别太大」——轮转的上限是两代 × 两个文件 ≈ 20 MB，而**每个数据根各有一份**。

```text
预算       整个 log/ 默认 8 MB（kDefaultLogBudgetBytes）；`log clean --keep` 可改。
超预算才动 先扫目录求和：不超预算就**一个字节都不动**（无副作用，常态下只有几次 file_size）。
先删备份   fmt.log.1 / error.log.1（纯附加物）；删不掉（另一进程握着）就跳过、不报错。
再裁活动   仍超预算才裁：活动文件保留**最新的一段**，keep = max(预算/2, 256KB)，
           顶部补一行「日志已裁剪：保留最近 N 字节（原 M 字节），更早的记录已删除」。
           **256KB 是有意的下限**：预算再小也不该把日志裁到一个字节不剩。
只认四个名 fmt.log / error.log / 各自 .1；用户手放进 log/ 的东西一律不动。
预算为 0   只删备份、不裁活动文件（与「max_log_bytes = 0 表示不轮转」对称）。
目录不存在 什么都不做、不创建目录。
两条入口   CLI 启动（run() 里、**开日志器之前**）与服务启动（initialize_service_context()）
           各清一次；清理结果写进**清理之后**的日志文件，模块名 `Log`。
手动命令   `log clean [--keep 8MB] [--dry-run]`——本地执行、**不需要服务**；
           `--dry-run` 只报告会删什么、会裁什么、能释放多少，磁盘一个字节都不动。
踩过的坑   ① 自动清理无条件跑在前头，`log clean --dry-run` 会报「未超过上限」（预演看不到
           真相）→ **显式 log clean 时跳过自动那一步**，由命令自己按用户的预算清；
           ② 自动清理只认固定默认预算（不读 --keep），所以手动那一次必须自己清。
用例       Logger.未超预算时一个字节都不动 / 超预算先删备份再裁活动文件 / 预演只报告不动磁盘 /
           预算为零只删备份不裁活动文件 / 日志目录不存在不是错误
```

每行格式：

```text
时间 [级别] [模块] 消息
2026-10-05 23:40:01 [INFO] [Main] FMT 启动
2026-10-05 23:40:01 [INFO] [Config] 加载 config.json
2026-10-05 23:40:02 [WARN] [Config] 配置文件不存在，使用默认配置
2026-10-05 23:40:06 [ERROR] [File] 文件删除失败: example.txt
```

模块短名：`Main`、`Config`、`File`、`Share`、`Trash`、`Bucket`、`Storage`、`Http`、`Service`、`Ipc`，
以及 CLI 侧的两个：`Cli`（CLI 本体）、`Elevated`（提权副本）。

原则：

> **控制台负责用户交互和重要异常；日志文件负责完整运行记录。**

**CLI 与 Service 追加同一个日志文件**：两个进程都以「追加」方式打开同一份
`<数据根>/log/fmt.log`（`error.log` **仅 ERROR 级**，同样追加），一次一行写入
（**每行开-写-关**，这也正是轮转能做成的前提，见上）；
MSVC 文件流是共享模式，因此不再存在「两个进程争抢同一日志文件」的问题。

> **`error.log` 的口径是「仅 ERROR 级」**（提交 `821aba3` 顺带对齐）：`logger.hpp` 的
> 头注释原来写着「WARN 也进 `error.log`」，与本节口径**冲突**；作者一度照注释改了代码，
> **被既有用例当场抓住**（`Logger.写入两个文件且ERROR单独成文件` 断言 `error.log` 只有
> 1 行），于是改回代码、修掉注释。**以本节为准：WARN 只进 `fmt.log`。**
> 同时修掉的还有「只有 Service 打开日志文件」那句旧话（下面这段旧口径本来就已经作废）。

旧口径是「只有 Service 写日志文件，CLI 只输出控制台」，已被推翻。理由看 `service stop`
最清楚：用户在 CLI 里敲下这条命令，服务随即被停掉，于是这次操作在日志里**一个字都没有**
——日志跟不上用户做过什么。日志要能回答「谁在什么时候对服务做了什么、结果如何」，
因此改为两个进程都写。

CLI 写这些内容：

```text
CLI 启动 / 退出
用户敲的原始命令（形如  fmt> service stop ）
每条 service 命令的结果（四条动作命令还记提权过程；`status` 不提权，只记查询结果）
提权副本自身的执行与结果
业务命令的请求与结果
连接失败
```

四条边界：

```text
1. CLI 双击时先对自己所在数据根执行与服务共用的幂等体检与补齐
   （六个目录 + 六个默认 JSON，只补缺失、已存在不动、损坏 JSON 只报告不重置，见第 91～92 节）；
   这一步在打开日志器之前完成，所以 log/ 与 temp/ 都在同一条「新建目录」清单里，
   而**结果只进日志**（等日志器开好后写 log/fmt.log，模块 Cli），控制台一行都不打。
   除此之外 CLI 不改任何业务数据：不写 data/*.json 的内容、不删文件、不改名。
   旧口径里「CLI 只允许创建 log/ 与 temp/ 这两个目录」的说法已被这一步覆盖。
2. --help / --version / -v / version / help / exit 不写日志、不创建任何目录（它们不该在磁盘上留下东西；`version` 命令是提交 `d108c80` 新增的）。
3. 两个进程的数据根可能不同（服务可能被别人启动在另一个目录）：各写各自数据根下的
   log/fmt.log；这种情况下 CLI 会额外写一行 WARN，指明服务当前数据根与服务侧日志的位置。
4. 控制台只留交互与异常：横幅、提示符、命令结果、以及 stderr 上的异常；
   服务的当前状态、数据根体检结果都只进日志（见第 127 节）。
```

日志目录位于**当前数据根**下（`FMT_ROOT/log/`），由服务在初始化时创建（见第 92 节）；
CLI 双击时的那一次幂等补齐也会建出它（在打开日志器之前）。临时文件目录 `temp/` 同理
（见第 5 节）：由服务在初始化时创建，CLI 双击时也会补出来；执行提权命令前若仍不存在，
就被退回系统临时目录 `%TEMP%` 并写一行 WARN。

CLI 与服务之间的通道是命名管道（见第 76 节）：CLI 把命令发给服务，服务执行后把
`{ok, code, message}` 回传，CLI 把结果打印到控制台**并追加写入数据根下的 `log/fmt.log`**。

CLI 连接管道时，如果管道**还不存在**（服务刚被 `service start` 拉起、监听尚未就绪），
最多重试 **5 秒**再报 `FMT-601`；**权限类错误（`ERROR_ACCESS_DENIED`）不重试**，
直接报 `FMT-004`。

V1 **不实现**异步日志、日志线程、日志队列、压缩、ELK 或复杂配置。
（**轮转与目录级清理都有**：轮转见本节上面与第 65 节，清理见 `log clean` / 启动时的自动兜底。）
将来若出现大量并发网络请求，再升级为「业务线程 → 日志队列 → 日志线程 → 文件」。

日志用于：

```text
服务启动
服务停止
上传
删除
恢复
异常
HTTP 请求
数据一致性问题
```

---

# 66. 错误系统

错误码清单已冻结，见 `FMT 项目架构.md` 附录 A。

错误结构（已冻结）：

```cpp
struct Error {
    ErrorCode code;
    std::string message;
};
```

返回值（已冻结）：

```cpp
template <typename T>
using Result = std::variant<T, Error>;

using Status = std::variant<std::monostate, Error>;
```

`Result<T>` 用于有返回值的操作，`Status` 用于 `delete`、`save`、`move`、`write` 等
成功时没有返回值的操作。**成功值与错误值互斥**，不得使用
`bool success; T value; Error error;` 这类允许误读的结构。

`ErrorCode` 统一定义在 `common/error`，业务代码不直接写字符串错误码，由统一转换层生成。

两条规则：

```text
1. 错误码稳定，错误消息可修改
2. 编号一旦发布，不复用、不修改语义；新增错误只能追加新编号
```

CLI、日志、测试与将来的网络 API 都应依赖错误码，而不是中文或英文提示文本。

---

# 67. CLI Exit Code

建议：

```text
0     成功
1     通用错误
2     参数错误
3     对象不存在
4     冲突
5     权限/访问错误
6     数据一致性错误
7     配置错误
8     Service 错误
```

退出码编号**已冻结**，并已在第一阶段实现（`include/fmt/core/app.hpp` 的
`ExitCode`）。错误码 `FMT-NNN` 到退出码的映射见 `FMT 项目架构.md` 附录 A；未映射的
错误码默认返回 `1`（通用错误）。

脚本主要依赖：

```text
Exit Code
```

而不是中文提示。

---

# 68. Help

帮助有**两个入口**：一次性的 `--help`，以及 `help` 命令（交互式与一次性都支持）。

```text
fmt.exe --help             完整用法：横幅 + 用法 + 命令总览 + 退出码表 + 「详细说明」一行
fmt.exe help               只列命令总览（不加描述）
fmt.exe help <组>          该组详情
help / help <组>           交互循环内同上（--help 在交互里是 help 的别名）
```

`help`（不带参数）只列命令、不加描述（**提交 `d108c80` 起多了 `(version)` 一行；
**提交 `674d0b0` 起 `trash` 多了 `empty`、新增 `(config) list set`，
`(share)` 从「尚未实现」组移进「可用命令」组**）：

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
```

> **提交 `d5779db` 把「业务命令（服务端尚未实现，现在会返回 FMT-602）」那一组整个删掉了**
> （`(share)` 移进「可用命令」组时，那一组只剩它自己）——所以上面的总览里**没有**
> 「尚未实现」组，`(share)` 落在 `(trash)` 之后、`(config)` 之前，
> 与 `src/cli/cli.cpp` 的 `print_command_list()` 逐字一致。

> **`FMT-602` 现在只对应两件事**（提交 `d5779db`）：`server.*` 这个空壳，
> 以及**未实现的 HTTP 路由**（兜底路由的 501，见第 82 节）。
> 原口径「share 未实现、返回 FMT-602」**已作废**。


**`help version` 的正文（提交 `d108c80`，照源码抄）**：

```text
version —— 打印程序名、版本与构建日期
  与启动横幅、`--version` 共用同一份文本，不会各说一套。
  不需要服务在运行，也不写任何磁盘内容：
    fmt.exe version          一次性执行
    fmt> version             窗口里执行
```

> **`version` 命令（提交 `d108c80`）**：窗口里原来敲 `version` 会说「未知命令」
> （只有 `--version` 旗标实现了）。现在两者**共用同一份文本** `fmt::cli::version_text()`
> ——也就是横幅用的那份 `banner_text()`，**三处不可能各说一套**：
>
> ```text
> fmt.exe version            一次性执行
> fmt.exe --version / -v     一次性执行（原有）
> fmt> version               窗口里执行（也接受 --version / -v）
> 输出都是一行：File Manager Tool  v1.0  ( build  2026.10.09 )
>              版本取 version::MAJOR.MINOR，构建日期由 CMake 配置时生成
> ```
>
> **两个性质**：① **不需要服务在运行**（不连管道、不查服务状态、不弹 UAC）；
> ② **不写任何磁盘内容**——一次性分支放在「开日志器」之前，所以 `log/fmt.log` 里
> 不会因为敲 `version` 多出记录（与 `--help` / `--version` 同一条口径：**从这里开始
> 都是真的干活，才值得写日志**）。
> `help <组>` 支持的组也跟着多了 `version`（第 68 节的支持列表）。

**提交 `188e85d` 之后当时的逐字输出是**（`src/cli/cli.cpp` 的 `print_command_list()`）：
`(file) upload list get delete` 已经移进「可用命令」组，顺序是 service → bucket → file →
trash → help → exit；「业务命令（服务端尚未实现，现在会返回 FMT-602）」这一组**只剩
`(share)` 一行**（`d108c80` 的 `(version)`、`674d0b0` 的 `(config)`、`d5779db` 的
`(share)` 之后，**这一组整个被删掉**；现在的完整输出见上面的块与第 95 节）。

（`bucket` 已实现（阶段 4），因此列在「可用命令」组；**桶级 `trash` 的四条命令
`list` / `get` / `restore` / `delete` 也都已实现**（`list` / `restore` 提交 `c2d545d`，
`get` / `delete` 提交 `4fee290`），同样列在「可用命令」组。
`file` 四条命令在提交 `188e85d` 落地，也从这一组移了出去；
「服务端尚未实现」那一组里**只剩 `share` 一行**——原口径「只对 `file` / `share` 两组
成立」**已作废**。提交 `8a5e554`、`c2d545d`、`4fee290`、`188e85d` 已把帮助文案与实现同步；
**提交 `674d0b0` / `d5779db` 之后 `share` 也落地了**（第 95、105 节）。）

> 上面那段是 `188e85d` 时的口径。当时这一组标题里的「现在会返回 FMT-602」是**字面输出**，
> 不是文档笔误：`share.*` 走了 `is_known_business()` 的前缀表、但没有任何实现，
> 所以拿到的是 `FMT-602`（「操作尚未实现：share.create」）而不是「未知操作」的 `FMT-001`；
> **`share` 落地（`d5779db`）之后这一条不再成立**。

`help <组>` 支持 `service` / `bucket` / `file` / `share` / `trash` / `help` / `exit`
（`quit` 等同 `exit`；**提交 `d108c80` 起多了 `version`**）。`help service`：

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

> **`reinstall` 是提交 `c573f14` 新增的正式命令**（原来只有双击引导内部会用它，
> 「文档冻结的是四条命令」那句已作废）。用法提示行因此是
> `用法：service install | uninstall | start | stop | reinstall | status`（两处都是）。
> 语义上的三个「不变」见第 126.1 节。

四组业务命令的详情里原本都要注明「服务端尚未实现，现在返回 FMT-602」。

**阶段 4 收尾后的口径（提交 `8a5e554`、`c2d545d`、`4fee290` 已同步）**：`bucket` 与
**桶级 `trash` 全部四条命令**都已经实现，这条说明**只适用于 `file` / `share` 两组**
（原口径里「以及 `trash get` / `trash delete`」已作废）。

**提交 `188e85d` 之后范围再收一次**：`file` 四条命令也落地了，所以这句话当时
**只适用于 `share` 一组**（命令总览里「尚未实现」那一组只剩 `(share)` 一行）。
`help file` 的正文见下面（照源码抄，已不含「尚未实现」字样）；**`help share` 也在提交
`d5779db` 换成了数据面正文**——原口径「仍写着『服务端尚未实现，现在返回 FMT-602』，
与实况一致」**已作废**（第 68 节的 `help share` 块、第 95、105 节）。

`help bucket` 现在是这样（`src/cli/cli.cpp`；最后几行是提交 `9c3d2cb` 与 `0fc242b`
新增的，照源码抄）：

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

`help trash` 现在是这样（`src/cli/cli.cpp`，提交 `0fc242b` 改过，照源码抄）：

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
  empty            **清空整个回收站，不可恢复**：两级条目一次全删。
                   同样先说明「几项、多少」再问一次；一次性命令加 --yes。
                   （提交 674d0b0 新增；**空回收站不打扰**：直接成功返回 0 项）

标识可以是 file_id（文件）、回收站里的目录名（桶）或原名；
命中多条会报候选，请用 file_id 或完整的回收站名指定。
文件级条目记在 file.json 里（is_trash / trash_reason / deleted_at 是权威），
数据收在 trash/<用户>/.files/ 下；桶级记录在 trash/<用户>/.original，
目录名一律带删除时间戳，两者不会互相干扰。
```

> 本段原来的样文里 `get <id>` / `delete <id>` 写的是「尚未实现（阶段 7）」
> 与「当前是桶级条目；文件级条目随阶段 5/7 进来」，**已作废**——两级都落地了，
> 标题也换成了「两类条目：文件级 `[文件]` 与桶级 `[桶]`，都会标出来」
> （第 52、53.3、59、60 节）。`bucket delete` 的提醒也是提交 `0fc242b` 加的
> （第 30 节）。

`help file` 现在是这样（`src/cli/cli.cpp`；`delete` 那两行在提交 `0ad9efc` 改过，
`upload` 与末行在 `a2b6cd1` 之后也改过，`upload` / `get` 与末行在提交 `9c3d2cb` 再改过，
**照源码抄**）：

```text
file —— 文件（当前用户在当前 Bucket 里的文件）
  upload <来源> [文件名]   来源可以是 http:// 或 https:// 的 URL，也可以是本机路径。
                           文件名省略时取来源的最后一段。**判重只看当前 Bucket**，
                           而且先看内容：桶内已有相同内容（不管名字同不同）→
                           报「文件已存在」（FMT-304），**不要求改文件名**；
                           内容不同而文件名相同 → 报「文件名重复」（FMT-105），
                           提示修改文件名；两者都不同就正常入库。
                           别的 Bucket 里的同名/同内容与本桶无关。
                           大小上限取 config.json 的 max_upload_size。
                           文件名不能与文件标识同形（fmt-YYYYMMDD-N）：
                           那会和 file_id 混淆，属保留形状（FMT-106）。
  list [--sort name|size|id] [--search 关键字] [--page N --page-size M]
                           列出当前 Bucket 的正常文件；默认按名字，
                           size 大的在前，id 是入库顺序
                           （提交 53ec4be：--search 按文件名/file_id 做不区分大小写的
                             子串匹配；--page 从 1 开始，--page-size 省略或 0 = 不分页）
  get <file_id|文件名>     按文件名查**当前 Bucket** 的正常文件（同名的在别的桶时
                           会告诉你在哪个桶）；按 file_id 全局查，连回收站里的
                           也查得到（带 is_trash 与 trash_path、预览链接与下载链接）
  delete <file_id|文件名>  软删除进回收站，file_id 不变；之后用
                           trash list / trash restore 找回来。按名字只在当前
                           Bucket 内生效；要删别的桶里的那份请用 file_id（会先确认）

文件落在 repository/<用户>/<Bucket>/YYYY/MM/DD/ 下；上传先写 temp/，
校验（大小上限、MD5、文件名）通过后才移动入库。
名字与 file_id 的比较都不区分大小写（Windows 习惯）。
网络下载走系统组件（WinHTTP + Schannel），支持 https，不需要 OpenSSL。
```

> **上面这块被改过四处，已照新源码抄**：`delete` 那两行在提交 `0ad9efc` 从
> 「`delete <file_id>` 软删除进回收站，`file_id` 不变（可用 trash 查回）」改成现在这样
> ——`delete` 与 `get` 一样同时收 `file_id` 和文件名；
> **提交 `674d0b0` 又把括号里那句「文件级回收站目前只能写、还不能从 trash 查回，待阶段 7」
> 换成「之后用 trash list / trash restore 找回来」**——两级读取侧早在 `0fc242b` 就落地了，
> 那句早就过时（第 43、53.2、104 节）。`list` 那行也是 `674d0b0` 改的：
> 多了 `--sort name|size|id`（第 42、43 节）。`upload` 两行与末行则是 `a2b6cd1` 之后改的：
> 原来的「v1 不支持 https（需要 OpenSSL）；同时只支持 http」与行为相反，
> 现在源码写的是 `http://` 或 `https://`、「大小上限取 config.json 的 max_upload_size」、
> 末行「网络下载走系统组件（WinHTTP + Schannel），支持 https，不需要 OpenSSL」。
> **提交 `9c3d2cb` 又改了三处**：`upload` 追加「文件名不能与文件标识同形（fmt-YYYYMMDD-N）
> ……属保留形状（FMT-106）」两行（第 25、38 节）；`get` 那行从「查看文件信息与磁盘路径」
> 改成「按文件名只查正常文件；按 file_id 连回收站里的也查得到（带 `is_trash` 与
> `trash_path`）」（第 42 节）；末尾补一行「名字与 file_id 的比较都不区分大小写
> （Windows 习惯）」。标题里「当前用户在当前 Bucket 里的文件」说的是 `list`；
> `get <file_id>` 是**全局唯一**的例外、`get <文件名>` 是「当前用户 + 正常文件」
> （第 41、42 节）。**按名字定位的比较在提交 `5bf2c1f` 起不区分大小写**（第 38、43 节），
> `help` 正文到 `9c3d2cb` 才写明这一点。
>
> `share` 那组的帮助**原来**写着「服务端尚未实现，现在返回 FMT-602」——**提交 `d5779db`
> 起 share 数据面已落地**，命令总览里 `(share)` 移进「可用命令」组、`help share` 换成下面
> 这份正文（照源码抄，第 16、46～51、105 节）：

```text
share —— 分享（把正常文件开放成一条可撤销的链接）
  create <file_id|文件名>  创建分享：默认 20 次、7 天过期；返回 share_id
                          与完整分享链接（http://host:port/api/share/<id>/download，
                          地址取自 config/server.json，不看 HTTP 服务有没有开）
  get <share_id>      查看它的**全部信息**：文件与 Bucket、分享链接、下载次数
                      （用满时写「（下载次数已满）」）、到期时间（过期时写
                      「（已过期）」）、状态与一句话说明；链接不可用时也会给出，
                      但标注「当前不可下载，链接已失效」。
                      过期 / 次数用尽 / 已撤销 / 关联文件在回收站都如实报告，
                      不伪装成不存在（只有 share_id 完全查不到才是 FMT-500）
  list [<file_id|文件名>]  列出分享（最近创建的在前）：**不带参数就列全部**，
                      每行标出 下载次数 与 状态，次数用满/已过期时直接写
                      「[下载次数已满]」「[下载已过期]」（两个原因可能同时成立）；
                      带参数则只看那个文件（与 file get 同一套定位）
  delete <share_id>   撤销分享（删记录）

约束（开发文档 §50）：分享不得绕过文件状态——文件进回收站或被删掉，
分享立刻失效（FMT-503）。share_id 是 12 位随机十六进制，本身就是访问凭证。
公开下载端点已实现（提交 d3aeb3d）：GET /api/share/<share_id>/download
不要 token（链接本身就是凭证）、先记账再放行，可直接把链接交给别人。
```

> **`help config`（提交 `674d0b0` 新增；server_* 三项与 `--show-token` 是本次改动（未提交），照源码抄）**：

```text
config —— 配置（config/config.json 与 config/server.json）
  list [--show-token]           看当前生效的配置，两个文件按顺序分开输出：
                                config.json 的字段在前，接着是网络服务的
                                「网络服务 enabled / 服务地址 host / 服务端口 port」；
                                **默认不打印访问 token**（省得暴露在屏幕、日志、
                                工单与截图里）；要它加 --show-token，
                                或直接读数据根下的 data/user.json
  set max_upload_size <大小>     改上传大小上限；大小可写 10485760，
                                也可写 10MB / 512KB / 1GB（1KB = 1024 字节）
  set server_enabled <true|false>  开/关 HTTP 网络服务（config/server.json）
  set server_host <地址>         监听地址（默认 localhost，只监听本机）
  set server_port <1-65535>      监听端口（默认 4122）

server_* 三项改完**会自动重启服务**（已经提权就直接重装，否则弹一次 UAC；
提权被取消时会提示你手动执行 `service reinstall`）——因为 server.json
只在服务启动时读一次。
其余项（current_user / size_unit / language / version）只读。
current_bucket 用 bucket use 切换，不走 config。
```

> **原「⚠ 残留不一致」注记到此结清**：那条说的是 `a2b6cd1` 没有清理 `help file` 里的
> https 旧文案（当时源码里确实还写着「v1 不支持 https」）。后续提交已经把那两句改掉了，
> 现在源码与行为一致（`FMT 技术文档.md` 第 11.4、18.20 节同注）。

`help <未知组>` → stderr 打印 `没有 <组> 的帮助；输入 help 查看命令列表`，退出码 2（`FMT-001`）。
`help` / `--help` 全程**不提权、不连服务、不写日志**。

`service` 命令**不带 `--` 前缀**：旧写法 `fmt.exe --service install` 作废；
service 有 `install` / `uninstall` / `start` / `stop` / **`reinstall`** / `status` 六条，
**没有 pause，也没有 delete**（旧 `delete` 已更名为 `uninstall`）；
其中 `status` 是查询命令，不提权、不弹 UAC，其余每条都提权。
帮助文本里必须列出 `status`，见第 70 节、第 95 节。

帮助内容应该：

```text
简洁
准确
与实际命令一致
```

---

# 69. Service 模块

Service 模块负责 Windows Service 的生命周期。

`fmt.exe` 是**单一可执行文件**，同一份二进制有三种形态：

```text
CLI 形态        用户双击或命令行启动，普通用户权限
Service 形态    由 SCM 启动，LocalSystem，后台常驻
提权短命副本    由 CLI 经 UAC 启动（内部参数），做完即退
```

三种形态共享同一份业务代码；形态由**启动方式**决定，不是三份不同的 exe（见第 76 节）。

manifest 必须是：

```xml
<requestedExecutionLevel level="asInvoker" uiAccess="false" />
```

**绝不使用 `requireAdministrator`**：否则双击就直接弹 UAC，普通业务命令也会以高权限运行。

Service 形态职责：

```text
维护当前数据根
按 CLI 声明初始化数据根（建目录 + 默认 JSON）
承载业务模块（bucket / file / share / trash / config）
监听命名管道 \\.\pipe\fmt.control
监听 HTTP localhost:4122
写日志文件
```

Service 形态**不处理 pause**：服务不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`，
`HandlerEx` 只处理：

```text
SERVICE_CONTROL_STOP
SERVICE_CONTROL_SHUTDOWN
SERVICE_CONTROL_INTERROGATE
```

因此命令集中没有 pause（见第 70 节）。

---

# 70. Service 命令集

service 命令有五条：

```text
fmt.exe service install
fmt.exe service uninstall
fmt.exe service start
fmt.exe service stop
fmt.exe service status
```

固定规则：

```text
不带 -- 前缀（旧写法 fmt.exe --service install 作废）
没有 pause，也没有 delete（旧 delete 更名为 uninstall）
六条子命令都直连 SCM，不走命名管道、不走 HTTP
**除 `status` 外每条都提权**（install / uninstall / start / stop / **reinstall**）
status 是查询命令：不提权、不弹 UAC、不需要管理员权限
```

为什么必须直连 SCM：服务可能尚未安装或尚未运行，此时走任何进程间通道都会形成引导死锁。
`status` 更是要在「服务还没装」时就能回答「未安装」，所以它同样直连 SCM，而不是去连管道。

**四条动作命令都提权**，即使目标状态已经满足（例如服务已安装仍执行 `install`）也照常弹 UAC，
不做「已满足状态就免提权」的优化。

**`status` 是唯一的例外，而它不是「提权优化」**：它只读、不改变任何状态，因此不需要管理员权限，
也永远不弹 UAC。它读的是统一查询接口 `service::query_status()` 给出的状态、`QueryServiceConfigW`
的宿主路径，以及 `%ProgramData%\FMT\service.json` 里记录的当前数据根（见第 69 节、第 126 节）。

各命令提权后做什么：

| 命令 | 提权后执行 | 典型结果 |
| --- | --- | --- |
| `service install` | 创建服务 + 配置 Recovery + 启动 | 已存在 → `FMT-600` / 退出码 8 |
| `service uninstall` | 先 stop，再 `DeleteService` | 未安装 → `FMT-601` / 退出码 8 |
| `service start` | `StartServiceW`，再 `settle_state()` 等落定（见下） | 未安装 → `FMT-601` / 退出码 8 |
| `service stop` | `ControlService(SERVICE_CONTROL_STOP)`，再 `settle_state()` 等落定 | 未安装 → `FMT-601` / 退出码 8 |
| `service status` | **不提权**：`service::query_status()` + `QueryServiceConfigW`，然后打印状态 / 宿主 / 数据根 / 错误码 | 未安装 → `FMT-601` / 退出码 8；已安装未运行 → 照常打印、退出码 0 |

`service status` 的输出样例（服务已安装且运行中）：

```text
服务状态：运行中
服务宿主：D:/FMT2/fmt.exe
服务数据根：D:/FMT2
错误码：0
```

未安装时 stdout 打印 `服务状态：未安装`，退出码 8（`FMT-601 ServiceNotInstalled`）；
已安装但未运行时照常打印（如 `已停止`），退出码 0——「服务没在跑」不是错误，`status` 只回答
「它现在什么样」。状态名由 `service::state_name()` 一处映射：
`未安装` / `已停止` / `正在启动` / `正在停止` / `运行中` / `正在继续` / `正在暂停` / `已暂停` / `未知`。

**统一的状态查询接口**（技术文档 13.4.2 有完整定义）：所有读 SCM 状态的地方都走
`service::query_status()`，不在别处自己调 `QueryServiceStatusEx` 再映射状态。

```text
service::State { NotInstalled, Stopped, StartPending, StopPending, Running,
                 ContinuePending, PausePending, Paused, Unknown }
service::StatusInfo { state, wait_hint_ms, win32_exit_code, service_exit_code }
service::query_status()        -> Result<StatusInfo>
       「未安装」是**正常结果**（state = State::NotInstalled），不是错误；
       只有查询本身失败（打不开服务控制管理器等）才返回错误（FMT-602 / 退出码 8）
service::query_state()         只是 query_status() 的薄封装（取 state，失败给 Unknown）
service::installed_binary_path() -> Result<std::string>（服务宿主 binPath，去掉引号）
service::last_start_failure()  -> Result<ErrorCode>
       也基于 query_status()，读 win32_exit_code / service_exit_code；没有失败信息则返回错误
```

> 名字别混：`State` 是运行状态枚举；`ServiceState` 是 `service.json` 的结构体
> （`current_root` / `host_path` / `installed_at`，由 `load_state()` 读取）。

**「等它落定」不写死时长，按 SCM 的 `dwWaitHint` 自适应**（实现是 CLI 内的 `settle_state()`）：

```text
服务处于 State::StartPending / State::StopPending 时，每轮查询读 SCM 给的 dwWaitHint，
把它夹在 100 ms – 2000 ms 之间作为下次查询的间隔；兜底上限 30 秒（kSettleCapMs）。
状态一旦不是等待类（Running / Stopped / Paused / NotInstalled …）就立即结束等待；
settle_state() 返回落定后的 State，回来仍是等待类就说明「没起来」。
```

理由：`ServiceMain` 启动时要建目录、读 JSON、扫 `data/file.json`、起 HTTP、等管道就绪，
慢机器、大目录、杀毒软件介入时完全可能超过 8 秒——此时服务**并没有失败**，只是还在
`START_PENDING`。写死 8 秒会把「还在启动」误判成「启动失败」，然后白弹一次 UAC 去做根本解决
不了问题的重装（而且重装之后同样慢，用户会被反复弹 UAC）。**等一下多久，由 SCM 自己说。**

**V1 不做结构化输出**：`service status` 没有 `--json`，也不预留参数名。机器可读通道是
**命令退出码**（`0` 成功；未安装 `FMT-601` → `8`；查询失败 → `8`），人类可读通道就是上面那几行
固定顺序的文本（见第 127.4 节）。

提权的完整流程、四种动作命令各自何时提权、用户取消 UAC 的处理与固定输出格式，见第 126 节。

---

# 71. Service Install

命令：

```text
fmt.exe service install
```

安装参数（已冻结）：

| 项目 | 值 |
| --- | --- |
| 服务名 | `FMT` |
| 显示名 | `FMT File Management Service` |
| 启动类型 | `SERVICE_AUTO_START`（开机自启） |
| 运行账户 | `LocalSystem` |
| 可执行路径 | **首次安装时那个 `fmt.exe` 的绝对路径**，不复制到别处 |
| 接受的控件 | `SERVICE_ACCEPT_STOP` + `SERVICE_ACCEPT_SHUTDOWN`，**不声明** `SERVICE_ACCEPT_PAUSE_CONTINUE` |

流程：

```text
需要管理员权限（FMT-603）
 ↓
UAC 提权
 ↓
OpenSCManagerW
 ↓
查询服务是否已经存在
 ↓
已存在 → FMT-600，不重复创建、不覆盖已有 binPath，退出码 8
 ↓
不存在 → CreateServiceW
 ↓
ChangeServiceConfig2W 配置 Recovery（见第 74 节）
 ↓
StartServiceW
```

---

# 72. Service Start / Stop

启动：

```text
fmt.exe service start
```

```text
需要管理员权限 → UAC 提权
 ↓
OpenSCManagerW → OpenServiceW
 ↓
不存在 → FMT-601 / 退出码 8
 ↓
存在 → StartServiceW（已在运行时按成功处理）
```

停止：

```text
fmt.exe service stop
```

```text
需要管理员权限 → UAC 提权
 ↓
ControlService(SERVICE_CONTROL_STOP)
 ↓
停止接受新请求
 ↓
等待正在进行的关键操作完成
 ↓
停止 HTTP Server
 ↓
退出 Service
```

---

# 73. Service Uninstall

命令（旧 `delete` 已更名为 `uninstall`）：

```text
fmt.exe service uninstall
```

流程：

```text
需要管理员权限 → UAC 提权
 ↓
OpenServiceW
 ↓
不存在 → FMT-601 / 退出码 8
 ↓
存在 → ControlService(SERVICE_CONTROL_STOP) 先停止服务
 ↓
DeleteService
```

卸载**不得删除**数据：

```text
repository
trash
data
config
log
```

卸载只移除 Windows Service 注册信息；数据根下的任何文件都保持原样。

---

# 74. Service Recovery

使用 Windows Service Recovery，通过 `ChangeServiceConfig2W(SERVICE_CONFIG_FAILURE_ACTIONS)` 配置：

| 失败次序 | 动作 |
| --- | --- |
| 第一次失败 | 5 秒后重启 |
| 第二次失败 | 10 秒后重启 |
| 后续失败 | 30 秒后重启 |
| 失败计数重置 | 1 天（86400 秒） |

```text
服务异常退出
 ↓
Windows 检测
 ↓
按上表间隔重新启动 fmt.exe
```

V1 **不实现**独立 watchdog，也不自建守护进程。

---

# 75. fmt.exe 双击行为

双击 `fmt.exe` 的完整流程：

```text
1. 单实例互斥体 Local\FMT.CLI.v1
     已有实例 → 激活已有窗口，不新建窗口（见第 76 节）
2. 数据根体检与补齐：对自己 exe 所在目录执行与服务共用的幂等 ensure_root/check_root
     （六个目录 + 六个默认 JSON，只补缺失；已有 JSON 读一遍确认完整性，
      损坏或版本不受支持只报告、绝不重置，见第 91～92 节）
     这一步在打开日志器之前完成，所以 log/ 也在这条「新建目录」清单里；
     结果**只进日志**，控制台一行都不打，只有异常走 stderr
3. 查 SCM 状态（走统一接口 service::query_status()；「未安装」是**正常结果**，不是错误）
     未安装        → 提权 install（装 + 启动，一次 UAC）
     运行中        → 不动、不弹 UAC
     已安装未运行  → 提权 start → **等它落定**（见第 70 节：按 SCM 的 dwWaitHint 自适应）
           起来了       → 完成
           仍然没起     → 读服务留下的失败编号 dwServiceSpecificExitCode（见第 99 节）
                 数据根/配置类问题 → **不重装**（重装也解决不了），打印原因让用户先处理
                 其它             → 提权 reinstall 一次（卸载 + 安装，一次 UAC），仍失败则报错
4. 比较服务 binPath 与自身路径
     相同              → 正常继续
     不同但文件存在    → 作为客户端继续
     不同且文件已丢失  → 询问是否重新安装服务（提权副本的 reinstall operation：
                         先卸载、再安装并启动，服务未安装时忽略卸载错误，一次 UAC）
5. 服务在运行 → 连命名管道，用 hello 帧声明 root = 自身 exe 所在目录；
     switched=true 时**在 stderr 打印换根提示**（`root_switch_notice()`，提交 `8f2fbc5`）并记一行日志「数据根切换：旧 -> 新」（**原口径「只进日志」已作废**）
6. 打印横幅与 Service Running... → 空一行 → fmt> 进入交互循环
```

第 3 步判定「数据根/配置类问题」的错误码集合（命中就**跳过重装**、只打印原因让用户先处理）：

| 错误码 | 含义 |
| --- | --- |
| `FMT-006` | `JsonParseError` JSON 格式错误 |
| `FMT-007` | `JsonWriteError` JSON 写入失败 |
| `FMT-008` | `ConfigError` 配置错误 |
| `FMT-011` | `JsonUnsupportedVersion` JSON 数据版本不受支持 |
| `FMT-013` | `DirectoryCreateFailed` 目录创建失败 |
| `FMT-009` | `StorageError` 存储操作失败 |
| `FMT-005` | `IoError` 磁盘/IO 错误 |
| `FMT-014` | `PathEscape` 路径穿越 |

这些都不是「服务注册坏了」，重装服务解决不了；集合之外的失败（如 `FMT-602`）才走
「提权 reinstall 一次」。

第一次双击：

```text
检查 Service
 ↓
不存在
 ↓
请求管理员权限（一次 UAC）
 ↓
安装
 ↓
启动
 ↓
声明数据根，进入 CLI
```

再次双击：

```text
检查 Service
 ↓
已存在
 ↓
检查状态
 ↓
未运行 → 提权启动（一次 UAC）
已运行 → 不重复安装、不提权
```

换目录双击（把 exe **复制**到新目录）：

```text
双击 D:\FMT2\fmt.exe
 ↓
第 2 步先对 D:\FMT2 做幂等体检与补齐（只补缺失，含 log/ 与 temp/）
 ↓
服务宿主仍是首次安装时注册的那个 exe
 ↓
管道 hello 帧声明 root = D:\FMT2
 ↓
服务在 D:\FMT2 下幂等初始化，响应回填 switched=true / previous_root
 ↓
CLI **在 stderr 打印换根提示**（`root_switch_notice()`，提交 `8f2fbc5`）
并记一行日志「数据根切换：旧 -> 新」（**原口径「只进日志，不刷控制台」已作废**，第 127.1 节）
 ↓
旧数据原样保留，不删除
```

服务宿主路径规则：

```text
服务宿主 = 首次安装时注册的那个 exe 的绝对路径
不复制到别处，也不随双击位置改变
```

因此：

> **移动请用复制。** 剪切（移动）或删除宿主 `fmt.exe` 会让服务无法启动（SCM 报 1053），
> 需要重新执行 `service install`。

换数据根前必须先关闭旧的 CLI 窗口（单实例，见第 76 节）。

---

# 76. Service 与 CLI

`fmt.exe` 是单一可执行文件，形态由启动方式决定：

```text
wmain
 ├─ StartServiceCtrlDispatcherW 成功                    → Service 形态
 ├─ 失败且 ERROR_FAILED_SERVICE_CONTROLLER_CONNECT      → 用户启动 → CLI 形态
 └─ 其它错误                                            → FMT-602 / 退出码 8
```

命令示例：

```text
普通业务命令：fmt.exe file list
service 命令：fmt.exe service stop
```

## 76.1 两条入口，同一个 service 层

```text
CLI 窗口 ──命名管道 \\.\pipe\fmt.control──→ service 层
浏览器   ──HTTP/HTTPS localhost:4122──────→ service 层
```

**CLI 不再走 HTTP**：HTTP 可以被 `server.json` 关闭，端口也可能被占用，CLI 不应随之失效。
CLI 与 HTTP 只是两条通道，业务行为由同一个 service 层决定。

管道协议（已冻结）：

```text
帧格式   [4 字节小端长度][UTF-8 JSON]
请求     {"id":7,"op":"hello","root":"D:\\FMT2","pid":1234}
         {"id":8,"op":"file.list"}
响应     {"ok":true,"data":{...}}
         {"ok":false,"error":{"code":"FMT-305","message":"..."}}
```

`hello` 的响应 `data` 里带两个新字段：

```json
{ "id":1, "ok":true, "data":{ "root":"D:/FMT2", "switched":true, "previous_root":"D:/FMT" } }
```

`switched` = 这次声明是否**导致服务切换了数据根**；未切换时不含 `previous_root`。
`switched` 为真时 CLI **在 stderr 打印换根提示**（提交 `8f2fbc5`，`cli::root_switch_notice(previous, current)`）并记一行日志 `[Service] 数据根切换：旧 -> 新`（**原口径「只进日志」已作废**）。

响应信封与 HTTP 完全一致，一份信封两处复用；超时：连接时管道不存在最多重试 **5 秒**
（权限类错误不重试）、普通命令 **30 秒**（`ipc::kCommandTimeoutMs`）、
**`file.upload` 30 分钟**（`ipc::kUploadTimeoutMs`，提交 `a2b6cd1`；超时时 CLI 会提示
「服务端可能仍在处理」，见 `FMT 技术文档.md` 第 19.1 节）。

管道安全：服务以 `LocalSystem` 运行，必须显式授权交互用户（IU）：

```text
D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)
```

并设置强制完整性标签（MIC），否则中完整性的普通 CLI 连接会报 `ERROR_ACCESS_DENIED`：

```text
S:(ML;;NW;;;ME)
```

CLI 形态**不碰 core 业务操作、不写业务 JSON**；它双击时会对自己所在数据根跑一次与服务共用的
幂等体检与补齐（只补缺失的六个目录与六个默认 JSON，见第 5 节与第 91～92 节），此后只解析命令行、
走管道、打印结果。写日志用的 `log/` 与提权前要用的 `temp/` 都已被那一步覆盖（第 65 节）。

## 76.2 数据根由 CLI 声明

CLI 连接时的首帧是 `hello`，其中 `root` = CLI 自身 exe 所在目录（`GetModuleFileNameW` 取父目录）。

```text
root != 当前数据根 ?
 ├─ 是 → 切换当前数据根（旧根数据原样保留），对新根做幂等初始化，
 │        响应里置 switched=true 并回填 previous_root，CLI **在 stderr 打印换根提示**（`root_switch_notice()`，提交 `8f2fbc5`）
并记一行日志「数据根切换：旧 -> 新」（**原口径「只进日志，不刷控制台」已作废**，第 127.1 节）
 └─ 否 → 直接进入命令循环，响应不含 switched / previous_root
```

该连接上的所有业务命令都在该数据根下执行。因为同时只有一个 CLI 窗口，所以同时只有一个数据根。

服务开机自启且没有 CLI 连接时：

```text
%ProgramData%\FMT\service.json 已记录数据根 → 取该记录值
从未记录                                   → 取服务宿主 exe 所在目录
```

## 76.3 单实例

CLI 用命名互斥体保证单实例：

```text
Local\FMT.CLI.v1
```

已存在实例时：

```text
EnumWindows 查找 ConsoleWindowClass
 ↓
SetForegroundWindow
 ↓
失败（前台锁定限制）→ FlashWindowEx
```

**不新建第二个窗口。** 因此换数据根之前必须先关闭旧 CLI 窗口。

提权副本必须无窗口：`runas` 会另开控制台窗口，且高完整性窗口不能被中完整性进程置前（UIPI），
所以提权副本以 `SW_HIDE` 启动，结果写进结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 回传，
由父进程读回打印（见第 126 节）。
结果文件跟着 exe 走（数据根下的 `temp/`），用户一眼能找到、随时可清；
只有数据根不可写时才退回系统临时目录 `%TEMP%`（见第 5 节与第 126 节）。

---

# 77. HTTP Server 模块

HTTP Server 负责：

```text
网络请求
请求解析
参数验证
调用 Service
返回 HTTP Response
```

HTTP Server 不直接修改：

```text
file.json
repository
trash
```

所有业务操作仍通过统一业务层。

---

# 78. HTTP Server 启动

程序启动 Service 后：

```text
读取 server.json
 ↓
enabled?
 ↓
否 → 不启动 HTTP
是 → 创建 HTTP Server
 ↓
监听 host:port
```

默认：

```text
localhost:4122
```

> **提交 `22c3c3e` 起（真机验收 2026-10-09 21:29）**：`ServerConfig::host` 的**代码默认值
> 从 `127.0.0.1` 改成 `localhost`**（用户明确要求写 `localhost`）；线上
> `config/server.json` 已改成 `enabled: true, host: "localhost"`，日志里是
> 「HTTP 监听 localhost:4122」，接口全部可用（完整验收见 `FMT 技术文档.md` 第 18.39 节）。
>
> **仍是缺口：安装流程不会自动打开 `enabled`**——`ServerConfig::enabled` 默认 `false`，
> 而**全仓库没有代码把它置为 `true`**（`git grep 'enabled = true'` 在 `src/` 零命中；
> 安装流程不碰 `config/server.json`；CLI 也没有命令能开），所以**全新数据根**装完服务后
> HTTP 仍是关的，要手改 `config/server.json`（或将来加一条 `config http on` 这类命令）。
> 待用户决策：① 安装流程按文档把 `enabled` 置为 `true`，还是 ② 加一条 CLI 命令。
> 详见 `FMT 技术文档.md` 第 5.2、12.1、12.2、19.1 节。

---

# 79. HTTP Host

如果：

```text
host = localhost
```

只允许本机访问（**代码默认值，提交 `22c3c3e`：从 `127.0.0.1` 改成 `localhost`**）。
`127.0.0.1` 与 `localhost` 在本机监听上等价，但用户明确要求配置里写 `localhost`。

如果：

```text
host = 0.0.0.0
```

允许监听所有网络接口。

局域网访问功能以后正式开发时再增加相应安全控制。

**认证（提交 `4b812b5`）**：**所有 `/api/*` 都要 token**，唯一例外是
`/api/ping`（健康检查）与 `/api/share/<id>/download`（分享链接本身就是凭证）；
缺 token / 不对 → **401 + `FMT-018 Unauthorized`**（退出码 5）。
检查**只在一处**：httplib 的 pre-routing 钩子——「漏给某条路由加认证」是这类代码最典型的
事故；并且**没有注入校验器时一律 401**（fail-closed）。
token 请求头两种写法都收：`X-FMT-Token: <token>` 与 `Authorization: Bearer <token>`；
比较是**常量时间**的（密码哈希同理）。token 从 `config list` 拿（第 82 节、
`FMT 技术文档.md` 第 12.3.2 节）。

**桶在 HTTP 侧是只读的（提交 `4b812b5` 删掉整套写接口；本次改动，未提交，回补一条只读列表）**：
`GET /api/bucket` 是**只读列表**（`buckets[{name,is_current}]` + `count` + `current_bucket`）；
桶的**写**接口（`create` / `use` / `delete`）**仍然故意不要**——`current_bucket` 是服务端全局状态，
多个 HTTP 调用方一起改会互相踩，删桶还有确认流程，所以那些路径返回
**404 + `FMT-017`**：它是**故意不要**，不是「还没做」（所以不是 501）。
桶继续由 CLI 管；HTTP 客户端操作的是**当前桶**（现在至少能问出「当前是哪个桶」了）。

---

# 80. HTTP V1 功能

V1 网络层主要支持：

```text
文件查询
文件下载
文件预览
Share 访问
```

上传等网络功能可以根据实际开发阶段继续决定。

**提交 `d3aeb3d` 起这四项都已实现（流式）**——HTTP 接口的最后一部分：

```text
文件查询   GET /api/file（列表，只有文件信息）、GET /api/file/<id|名字>（详情，含 path）
文件下载   GET /api/file/<id|名字>/download —— 流式（set_content_provider），
           Content-Disposition: attachment；**任何类型都能下载**
文件预览   GET /api/file/<id|名字>/preview —— 流式、inline；
           只有预览策略允许的类型可预览，其余 400 + FMT-701
上传       **POST /api/file/upload —— 两种请求体都收，共用同一条链路**：
           ① **裸字节：请求体就是文件内容**（流式）；② `multipart/form-data`
           （Postman 的 form-data / 浏览器表单 / Spring 的 MultipartFile）——
           取第一个带 filename 的 part 的正文，普通字段不落盘；
           旧的「请求体给服务端本地路径」那套已删除
Share 访问 GET /api/share/<share_id>/download —— **公开、不要 token**（链接本身是凭证），
           且**先记账再放行**（第 50/51 节）
（`FMT 技术文档.md` 第 12.3.2、18.40 节）
```

---

# 81. HTTP Preview

预览只属于：

```text
HTTP
```

CLI 不实现 Preview。

预览前：

```text
检查 File
 ↓
检查 File Type
 ↓
确定是否支持
 ↓
返回预览内容
```

不支持的文件类型返回明确错误。

**实现（提交 `d3aeb3d`）**：`GET /api/file/<id|名字>/preview`，流式（`set_content_provider`）、
`Content-Disposition: inline`。**策略只有一份**，在文件模块 `preview_content_type(file_type,
file_name)` 里：`file_type == "image"` 按扩展名给 `image/*`；文本类
（`.txt/.md/.json/.csv/.log/.xml`）给对应 MIME；**其余一律 `FMT-701`（400，可下载但不可预览）**。
HTTP 与将来的其它入口共用这一份，不各写一套。配套还有 `content_type_of(file_name)`
（按扩展名猜 MIME，兜底 `application/octet-stream`）。

> **下载与预览是两件事**：下载**不受预览策略限制**——任何类型都能下载，
> `Content-Type` 猜不出来就给 `application/octet-stream`。
> 实现里一开始两者共用一个分支，结果 `.bin` 的**下载**被 `FMT-701` 挡掉（已修，
> `FMT 技术文档.md` 第 18.40 节）。
> CLI 仍没有 `preview` 子命令（预览只属于 HTTP）。

---

# 82. HTTP Status Code

基本约定：

```text
200 OK
```

成功并带 Response Body。

```text
204 No Content
```

成功但无 Response Body。

```text
400 Bad Request
```

参数错误。

```text
401 Unauthorized（提交 4b812b5）
```

**缺少或无效的访问 token**：`/api/*` 都要 token，唯一例外是 `/api/ping` 与
`/api/share/<id>/download`；命中 → **401 + `FMT-018 Unauthorized`**（退出码 5）。
由 httplib 的 **pre-routing 钩子**一处拒绝，且**没有注入校验器时一律 401**（fail-closed）。

```text
404 Not Found
```

资源不存在。

```text
409 Conflict
```

资源冲突。

```text
500 Internal Server Error
```

内部错误（**服务器自己坏了**）。

```text
501 Not Implemented
```

**接口尚未实现**（提交 `4ddb515`）：服务端认得这个模块，但还没有这个接口。
**别用 500 表达这件事**——500 等于告诉调用方「服务器坏了」。

```text
404 Not Found（新增一种含义，提交 4ddb515）
```

**没有这个接口**（完全打错的 `/api/...` 路径，如 `/api/nosuch`）→ **404 + `FMT-017
RouteNotFound`**（退出码 3）。原来兜底路由硬编码 `500`，把「没这个接口」说成了
「服务器坏了」（实测：`GET /api/nosuch` → `500 + FMT-602 操作尚未实现：/api/nosuch`）。

**桶的写接口也是 404 + `FMT-017`**（提交 `4b812b5` 删掉整套；本次改动，未提交，只回补了
`GET /api/bucket` 这一条只读列表）——但含义不同：它是**故意不要**（用户明确「桶不要」写接口，
而 CLI 已经能管），不是「还没做」，所以**不是 501**。
「已知模块」列表因此是 **file / trash / share / config / server / preview**，
**bucket 不在其中**（`GET /api/bucket` 已登记为路由，压根不会落到兜底那一层；
`/api/bucket/<其它路径>` 才走 ②，`POST /api/bucket` 因为没有 `/api/.*` 兜底而落通用错误处理器 → 404 + `FMT-002`）。

---

**阶段 4 已冻结：错误码 → HTTP 状态码映射**（实现见 `src/server/server.cpp`；
同一张表也写在 `FMT 技术文档.md` 第 12.5 节）。管道没有状态码这一层，只有 HTTP 需要它：

| 状态码 | 错误码 | 语义 |
|---|---|---|
| 400 | `FMT-001` **`FMT-016`** `FMT-012` `FMT-014` `FMT-100` `FMT-101` `FMT-102` `FMT-103` `FMT-104` **`FMT-106`** `FMT-202` `FMT-300` `FMT-303` `FMT-700` `FMT-701` | 参数／名称／路径／URL／**待确认** 类错误 |
| **401** | **`FMT-018 Unauthorized`**（提交 `4b812b5`） | 缺少 / 无效的访问 token（`/api/ping` 与分享下载除外），pre-routing 钩子一处拒绝，退出码 5 |
| 403 | `FMT-004` `FMT-501` `FMT-502` `FMT-503` | 权限不足与分享不可用 |
| 404 | `FMT-002` `FMT-200` `FMT-400` `FMT-500` `FMT-305` `FMT-402` **`FMT-017`** | 对象不存在（含未设置当前 Bucket、原 Bucket 已永久删除）、**没有这个接口** |
| 409 | `FMT-003` `FMT-105` `FMT-201` `FMT-203` `FMT-304` `FMT-401` **`FMT-403`** | 冲突（已存在、重名、仍被引用、**合并回退撞上同名文件**） |
| 500 | 其余（JSON／配置／存储／IO 等） | 内部错误（服务器自己坏了） |
| **501** | **`FMT-602 ServiceOperationFailed`** | **接口尚未实现**（提交 `4ddb515` 起显式登记；原来是落 `default: 500`） |

**兜底路由的三条口径（提交 `4ddb515`）**：

```text
① 已知模块下没有这个接口   /api/file | /api/trash | /api/share |
                          /api/config | /api/server | /api/preview 之下没命中的路径
                          → **501 + FMT-602**，「接口尚未实现：<path>」（`server.*` 这个空壳属这类）
② 完全打错的 /api/... 路径  → **404 + FMT-017 RouteNotFound**，「没有这个接口：<path>」
③ 已知路由、业务找不到对象  → 404 + FMT-002（不变，例如 GET /api/file/nope.bin）
④ 故意不要的接口（不打算做） → 也是 **404 + FMT-017**，但语义是「不做」而不是「打错了」：
                          桶的写接口 `/api/bucket/<其它路径>` 属这类
                          （bucket 不在 ① 的列表里；`GET /api/bucket` 本次已登记为只读路由，
                            `POST /api/bucket` 落到通用错误处理器 → 404 + `FMT-002`）
```

> ① 里的模块列表**不含 bucket**——这与实现一致（`server.cpp` 的兜底路由 `known_module`
> 只看 file / trash / share / config / server / preview）。

> **`FMT-017 RouteNotFound`（提交 `4ddb515`，退出码 3）**：属 `FMT-0xx` 通用一组，
> 默认消息「没有这个接口」。它**只由 HTTP 兜底路由产生**——管道入口没有「路由」概念
> （op 名写错是业务层的另一回事，走 `FMT-001` / `FMT-602`）。
> **原口径「未知 /api 路径一律 500」已作废**；`FMT-602` 也从「落 `default: 500`」
> 改成**显式 501**。

**提交 `9c3d2cb` 新增的 `FMT-106 FileNameLikeFileId` 属 400 一类**（参数／名称类，
退出码 2；第 25 节）。它是 `FMT-1xx` 文件名校验组里的第七个，与 `FMT-103`
（Windows 保留设备名）同为「名字是保留形状」——放在 400 而不是 409：
这不是「重名冲突」，而是**参数本身不合法、根本不允许上传**。

> **登记情况（提交 `6a40742` 补齐）**：`src/server/server.cpp` 的
> `http_status_for()` 里已有 `case ErrorCode::FileNameLikeFileId:`，与
> `FMT-100`～`FMT-104` 同组返回 **400**。原口径「它没登记、会落 `default: 500`」
> **已作废**——`6a40742` 之前确实是 500（HTTP 上传同形名字会说「服务器坏了」，
> 而实际上只是名字不合法），现在已是 400。
> 用例 `Server.File路由与上传` 里钉了这一条：`POST /api/file` 带
> `file_name = fmt-20261008-0` → **400 + `FMT-106`**
> （**提交 `d3aeb3d` 起流式上传也钉了它**：`POST /api/file/upload?name=fmt-20261008-0`
> 同样 400 + `FMT-106`）。
> `FMT 技术文档.md` 第 12.5 节同步。

**流式上传相关的状态码（提交 `d3aeb3d`）**：

```text
没给文件名               → **400 + FMT-100**（?name= 与 Content-Disposition 都没给）
上限在接收过程中命中     → **400 + FMT-303**：**立刻中止接收并删掉暂存文件**，
                          不是「写完再看」；temp/ 里不留 fmt-upload-* 碎片
预览不支持的类型         → **400 + FMT-701**（**只对预览**；下载不受它限制）
```

**需要显式确认的操作与 `FMT-016`（提交 `711da4c`，通用规则）**：

```text
需要显式确认的操作 —— 永久删除（trash delete，两级）、跨 Bucket 删除（file delete）、
非空桶删除（bucket delete）—— 在缺 force 时返回 **FMT-016 ConfirmRequired**（退出码 2，
HTTP 400），消息说明要确认什么（例如「永久删除不可恢复，需要确认（force = true）」、
「<预检消息>；确认删除请加 force（CLI：--yes）」）。
```

原口径「缺确认 → `FMT-001 InvalidArgument`」**已作废**：`FMT-016` 是独立编号，
脚本因此能区分「参数写错」与「忘了确认」。**`force` 由服务端独立校验**，
预检（`dry_run`）被绕过也拦得住。

**DELETE 路由的公共参数（提交 `711da4c` / `0fc242b`，`src/server/server.cpp` 的
`delete_args()`）**：

```text
?dry_run=1           只预检：返回 needs_confirm / blocked / entry（或 file.delete 的那组
                     布尔与 candidates）/ message，**零副作用**
?force=1 或 force=true（大小写不敏感）或请求体 {"force":true}   已确认，执行
两者都不给           预检不参与，直接执行；该确认而没确认 → 400 + FMT-016
```

`/api/trash/<标识>` 与 `/api/file/<标识>` 的 `DELETE` 共用这一个解析：
`?dry_run=1` 与 `?force=1` 都会翻译成 `args.dry_run` / `args.force`，
再连同路径参数一起交给服务端同一份业务实现（管道侧就是 `args` 里的这两个字段）。

映射集中在 `http_status_for(ErrorCode)` 一处（`switch` + `default: 500`），
新增错误码若不显式登记就落到 500——这是有意为之：宁可报「内部错误」，也不要猜一个
语义不匹配的 4xx。响应体仍是统一信封（第 83 节与 `FMT 技术文档.md` 第 12.3.2 节），
状态码只是给浏览器与调试工具多一层信息。

**回收站路由的状态码（提交 `4fee290`；`0fc242b` 起两级通用，标识见第 53.4 节）**：

```text
GET    /api/trash                    200（列表：entries + count + files + buckets）
GET    /api/trash/<标识>             200（条目详情 {entry}）；找不到条目 → 404 + FMT-400；
                                     标识命中多条 → 400 + FMT-001（并列候选）
POST   /api/trash/<标识>/restore     200（回退 {entry, message}）；随桶删除 → 404 + FMT-402；
                                     数据缺失 → 404 + FMT-002；
                                     **目标 Bucket 已存在 → 合并**（第 56.3 节）：
                                     缺 force → 400 + FMT-016；现有桶里有同名文件 →
                                     409 + FMT-403
DELETE /api/trash/<标识>             200（永久删除，返回 {entry, message}）；
                                     **?dry_run=1 → 200 + 预检**（needs_confirm / blocked /
                                     entry / message，零副作用）；
                                     **缺确认 → 400 + FMT-016**（提交 711da4c 起，
                                     原来记的是 FMT-001）：需要 ?force=1
                                     （或 force=true，大小写不敏感）或请求体 {"force":true}；
                                     找不到条目 → 404 + FMT-400
```

（`POST .../restore` 的 `?dry_run=1` 同样返回
`{needs_confirm, blocked, entry, message?, merge, merge_target_exists, merge_files,
merge_bytes, target_files, conflicts?}`（第 56.3 节）。**
回退的 `force` 只用于「目标 Bucket 已存在 → 合并」这一条**：`blocked` 的三种情况
（同名冲突 / 随桶删除 / 数据缺失）对回退是硬拒绝，不是「确认一下就能做」；
普通回退（目标不在）也不需要 `force`。）

---

# 83. Delete HTTP Response

如果删除操作没有返回内容：

```text
204 No Content
```

如果删除操作需要返回结果：

```text
200 OK
```

必须保证：

```text
204
```

不能带 Response Body。

---

# 84. 线程安全

V1 即使暂时主要是单机使用，也必须考虑并发。

重点保护：

```text
File ID
JSON 写入
File metadata
Share download_count
current_bucket
文件移动
```

**阶段 4 的实现口径（已落地）**：服务的业务命令——管道来的和 HTTP 来的**一样**——都在
运行体的**同一把锁**（`ServerRuntime::mutex_`）下串行执行：

```text
管道请求   ServerRuntime::handle() 取锁 → execute_business()
HTTP 请求  BusinessHandler lambda 取锁 → execute_business()
```

因此同一时刻只有服务在写数据根，命令之间不会互相踩。V1 只有一个 CLI 窗口，串行足够；
File 阶段如果需要更细的粒度（每个 JSON 一把锁），再在这把锁里面细分，不改变「两条入口
进同一个 service 层、共用同一批锁」这条结构。

**两条入口各自的并发形态（实现为准，不要按旧文的「每个连接一个线程」理解）**：

```text
① 命名管道：连接级严格串行
   服务端只有一条 accept 循环（ServerRuntime::run()）：accept 一次只建立一条连接，
   然后在这条连接上「读一个请求 → 处理 → 写一个响应」，客户端断开才回到 accept。
   —— 所以同一时刻只有一条 CLI 连接（配套第 75 节的「只留一个 CLI 窗口」）；
   —— 第二个客户端拿到 ERROR_PIPE_BUSY，客户端等 3 秒重试，最终归为 FMT-601；
   —— **没有「管道连接线程」这种东西**，实现里没有为连接起线程。

② HTTP：线程池并发
   cpp-httplib 默认 max(8, hardware_concurrency - 1) 个线程，请求并发进入；
   /api/ping、/api/status 不碰业务锁，因此是真并发、不会被业务命令拖住。

③ 业务命令：互斥（mutex），不是队列（queue）
   管道与 HTTP 的 bucket 命令互斥串行；但不保证先来先服务、没有优先级、
   没有排队长度上限、没有排队超时——抢不到锁的请求只是阻塞在 lock() 上。

④ 已知代价（阶段 5 必须处理）—— **已在提交 `188e85d` 落地解决**
   一个慢业务命令会同时卡住**两条入口的所有业务命令**。上传/下载可能持续几十秒到
   几分钟，如果那时仍持这把锁，bucket list / bucket get 与浏览器的 bucket 请求都会一起等，
   用户看到的是「服务像卡死了」。阶段 5 必须二选一：
     A. 收细锁粒度（按 JSON 文件 / 按 file_id 分锁）；
     B. 把长任务移出锁（登记任务 + 后台线程执行 + 轮询状态）。
   在选完之前，「上传期间其他命令一起等」是既定限制，不是 bug。
```

**选的是 B 的简化形态：「长任务不持锁」（提交 `188e85d`）。** 不加任务登记、不加进度查询
op（V1 只有一个 CLI 窗口、管道本身串行，登记任务没有收益），而是把上传拆成两段，
锁只保护其中快的那一段：

```text
ServerRuntime::run_upload(args)
  ① 锁下取快照      paths / logger / size_limit（只拿必须的东西，随即放锁）
  ② 锁外 prepare    prepare_upload()：下载或复制到 temp/、边写边算 MD5、边判大小上限
                    —— 这一段几十秒到几分钟，**全程不持锁**
  ③ 锁内 commit     commit_upload()：去重 → 重名 → file_id → 搬到仓库 → 写 file.json
                    —— 毫秒级，一次 lock_guard 就够
```

所以「上传时 `bucket list` 卡住」不再是事实：第 ② 段期间运行体的锁是**空的**，
`bucket list` / `trash *` / 浏览器的业务请求都能正常进来。`file.upload` 是**唯一**走这条
特殊路径的 op（`ServerRuntime::handle()` 与 HTTP 的 `BusinessHandler` 都先拦它、
再走 `run_upload()`），其余业务命令仍是「取锁 → `execute_business()`」。

**换根不会插进第 ② 段**：换根只由 `hello` 触发，而管道的 accept/serve 是串行的
（见下面 ①）——上传期间不会再处理第二个请求，所以第 ① 段拿到的快照在整段下载期间都成立。
HTTP 侧每次请求各自取当前 `context_`，真正的写只在第 ③ 段的锁内发生。

**数据一致性的兜底**：即使第 ③ 段失败，也不会留下「报了成功但两边不一致」的状态——
去重/重名命中时临时文件被 `commit_upload()` 删掉；`file.json` 写不进去时，
刚搬进仓库的文件被删掉（第 40 节）。所以第 ② 段与第 ③ 段之间的中间态
（只有 `temp/` 里一份文件）永远不会被当成正式文件。

**HTTP 监听器的 stop / start 必须在锁外做**（`ServerRuntime::restart_http()` /
`request_stop()`）：HTTP 的请求处理器要拿上面那把锁，持锁去 `stop()` 并 join 它的工作线程
会互相等待——直接死锁。正确顺序是「持锁把旧实例摘出来 → 放锁 → `stop()` → 起新实例 →
再持锁装回去」，实现里有注释写明这一点。

---

# 85. JSON 并发写入

多个线程不能同时：

```text
读取
修改
写入
```

而互相覆盖。

应该使用：

```text
Mutex / Lock
```

保护同一 JSON 数据的更新过程。

---

# 86. 文件操作并发

需要保护：

```text
同一个文件
同一个 File ID
同一个 Share
```

避免：

```text
删除 + 下载
恢复 + 永久删除
两个上传同时生成相同 ID
两个下载同时突破 Share 次数
```

**已落地的三条（提交 `188e85d`）**：

```text
两个上传同时生成相同 ID   不会：file_id 的分配（读 file.json → 取当天最大序号 + 1 → 写回）
                          整段在业务锁内、基于同一份快照（第 22、24 节）；
                          下载部分在锁外，但那一段根本不发 id
删除 + 下载               互相看不见：软删除在锁内完成「搬文件 + 写两份 JSON」，
                          读路径只在锁内取快照；没有「文件已经搬走、记录还是正常」的窗口
恢复 + 永久删除           文件级的恢复/永久删除**还没实现**（阶段 7），
                          所以这一条现在不适用；桶级那一半已在阶段 4 落地（第 60 节）
```

---

# 87. Upload 与 Download 并发

如果文件正在正式提交：

```text
不能让 Download
```

读取到半成品。

所以正式文件必须：

```text
临时文件
 ↓
完整完成
 ↓
原子/安全移动
 ↓
成为正式文件
```

**实现（提交 `188e85d`）**：这条纪律落成了「两段式 + 只在最后搬一次」——

```text
下载/复制阶段   只写 <数据根>/temp/fmt-upload-<随机>-<序号>.tmp，
                repository/ 下一个字节都不动（半成品永远不会出现在正式空间里）
完整性          大小上限边写边判（第 32 节）、Content-Length 必须对上（第 33 节）、
                MD5 边写边算并在收尾时校验（第 36 节）
成为正式文件    同卷 std::filesystem::rename（原子）；跨卷才「复制 → 校验大小 → 删源」
                （move_file()，第 39、88 节）——都以「一次成功或什么都没变」为准
失败            临时文件在两条失败路径上都被删掉（第 35 节）；
                file.json 写不进去时删掉刚提交的仓库文件（第 40 节）
```

`Download`（HTTP 浏览器侧的 `GET /file/download/<文件名>`）**尚未实现**，属阶段 6；
它落地时读的是 `repository/` 下的正式文件，所以上面这条「读不到半成品」的保证依然成立。

---

# 88. 文件移动原则

优先使用：

```text
同文件系统内移动
```

减少：

```text
复制
删除
```

带来的失败窗口。

如果跨磁盘导致无法直接移动，则使用：

```text
复制
校验
删除原文件
```

并进行一致性保护。

---

# 89. 文件 MD5 校验

关键操作可以校验：

```text
Upload
Restore
Consistency Check
```

正常下载不要求每次都重新计算 MD5，避免影响性能。

---

# 90. 数据备份原则

V1 不实现完整备份系统。

但关键 JSON：

```text
file.json
share.json
trash.json
user.json
```

更新时必须尽量避免直接破坏原文件。

采用：

```text
.tmp
```

替换机制。

---

# 91. 初始化流程

初始化**由 Service 与 CLI 共用同一套幂等规则**（同一份 `ensure_root` / `check_root` 实现）：

```text
谁执行  Service：启动时、以及 hello 触发换根时，对自己的数据根执行
        CLI    ：双击时，对自己 exe 所在的数据根执行
                （在打开日志器之前完成，所以 log/ 也由它创建）
共同规则 只补缺失；六个目录缺则建、已存在一律不动（不删除、不覆盖、不改名）
        默认 JSON 缺则写；已有的会被真正读一遍（解析 + 版本检查）确认完整性
        读不出来或版本不受支持 → 只报告、绝不重置（沿用「JSON 损坏不能静默重置」）
        两边都不碰业务数据内容：不写 data/*.json 的内容、不删文件、不改名
```

CLI 侧的结果**只进日志**（`log/fmt.log`，模块 `Cli`），控制台一行都不打：

```text
[Cli] 数据根检查：D:/FMT2
[Cli] 数据根新建目录：repository, trash, config, data, log, temp
[Cli] 数据根新建文件：D:/FMT2/config/config.json, D:/FMT2/config/server.json,
                      D:/FMT2/data/file.json, D:/FMT2/data/share.json,
                      D:/FMT2/data/trash.json, D:/FMT2/data/user.json
```

第二次及以后双击（什么都没缺）只是日志里换成 `[Cli] 数据根完整`；发现损坏时日志记
`[Cli] 数据根损坏（未自动修复）：data/file.json`，同时把
`数据根文件损坏（未自动修复）：data/file.json` 送到 **stderr**（异常才进控制台）。
数据根无法补齐时同样走 stderr：`数据根无法补齐：FMT-013 …`。

这条归位守的是第 65 节的原则：**控制台负责用户交互与重要异常，日志文件负责完整运行记录**。
双击时用户只想知道「能不能开始敲命令」，不该被一串「建了什么」淹掉。

这条统一了旧文档「CLI 只读不建目录」与「首次运行创建目录」的口径冲突。旧口径把「服务建、CLI 只读」
当成唯一答案，只给日志 `log/` 留了一个例外；**现在的口径是**：两边跑同一套幂等规则，CLI 双击时补的
就是它自己数据根缺的那部分，所以既不与「首次运行就自动建出了目录」的实测打架，也不再把「建目录」
当成 CLI 的禁忌——真正的禁忌是**碰业务数据内容**（`data/*.json` 的内容、文件的删除与改名）。

触发时机有两处：

```text
时机 1：Service 启动时，对自己的数据根（%ProgramData% 记录值或宿主 exe 目录）执行
时机 2：CLI 双击时对自己的数据根执行；连上服务后用 hello 帧声明 root，
        服务发现该根与当前数据根不同（或该根从未初始化）时，再对服务侧执行一次
```

```text
CLI 声明 root
 ↓
CLI 先对自己所在 root 做一次幂等体检与补齐（第 2 步，见第 75 节）
 ↓
服务检查 root
 ↓
创建必要目录
 ↓
创建 config/
 ↓
创建 data/
 ↓
创建 repository/
 ↓
创建 trash/
 ↓
创建 log/
 ↓
创建 temp/
 ↓
写入默认 JSON（.tmp 原子替换）
 ↓
加载配置
 ↓
把该根记为当前数据根
 ↓
响应回填 switched / previous_root
```

幂等要求：

```text
不存在则创建
已存在 → 保持原样，不修改、不清空、不覆盖
已有 JSON → 读一遍确认（解析 + 版本检查），读不出来只报告、绝不重置
绝不删除用户已经存在的数据
```

服务开机自启且没有 CLI 连接时，数据根取 `%ProgramData%\FMT\service.json` 的记录值；
从未记录过则取服务宿主 exe 所在目录，并在该根下执行同样的初始化。

**切换数据根时不删除旧根的任何数据**，旧根只保留在磁盘上不再被使用。

---

# 92. 初始化目录

必要目录：

```text
repository/
trash/
config/
data/
log/
temp/
```

由 **Service 与 CLI 共用的同一套规则**在数据根下补齐：不存在则创建，已存在不动。
Service 在启动/换根时对自己的数据根执行，CLI 在双击时对自己 exe 所在的数据根执行
（这一步在打开日志器之前，所以 `log/` 也包含在同一条「新建目录」清单里）。
`temp/` 是**临时文件目录**（既不是业务数据、也不是日志），内容随时可以清空：
Service 启动时会删除它下面以 `fmt-` 开头的遗留文件（用户手放的其它文件不动），见第 5 节与第 126 节。

对应默认文件（六个，缺则补）：

```text
config/config.json     {"version":1,...}
config/server.json     {"version":1,...}
data/user.json         {"version":1}
data/file.json         {"version":1,"files":[]}
data/share.json        {"version":1,"shares":[]}
data/trash.json        {"version":1,"trash":[]}
```

默认 JSON 一律「写 `.tmp` → 验证 → 替换」，已存在的文件不改、不删、不覆盖
（见第 11 节、第 13 节）。已存在的 JSON 会被**真正读一遍**（解析 + 版本检查）来确认完整性：
读不出来或版本不受支持的**只报告、绝不重置**——日志记一行
`[Cli] 数据根损坏（未自动修复）：data/file.json`，异常同时送到 stderr。

**阶段 4 补充的一步**：补齐六个目录与默认 JSON 之后，初始化还会**读一次配置并补上占位
用户名**——`current_user` 为空时置为 `user` 并立刻保存（第 9、93 节）。这一步是幂等的：
第二次初始化读到 `"user"` 就不再写文件。

`log/` 由服务与 CLI **共同追加写入**：两个进程写同一个 `fmt.log`（见第 65 节）。

CLI 只读业务数据：不改 `data/*.json` 的内容、不删文件、不改名，
只把命令经命名管道交给服务执行。它唯一会碰目录结构的动作，是双击时对自己所在数据根跑一次
与服务共用的幂等体检与补齐（只补缺失的目录与默认 JSON，见第 91～92 节）；
`log/` 与 `temp/` 也由那一步一并建出。

---

# 93. 第一次运行 current_user

如果：

```text
current_user = ""
```

**阶段 4 口径（已落地，取代旧写法）**：数据根初始化（`initialize_root`）在发现
`current_user` 为空时**自动置为占位名**并保存，因此 CLI 不需要、也没有「先设置当前用户」
这一步：

```text
user
```

```text
<数据根>/config/config.json   current_user = "user"
<数据根>/repository/user/<bucket>/…
```

需求原文是「不做用户先用 user 代替」，所以这是**正常路径**，不是降级：

```text
正常首启（config.json 缺失或 current_user 为空）
  → initialize_root 写出占位名 user 并保存
  → CLI 敲 bucket create 工作 直接成功，磁盘上得到 repository/user/工作/
```

旧的「V1 可以要求用户先设置当前用户」作废：那会让用户的第一条命令先撞一次
`FMT-604`，而 V1 并没有设置用户的命令（`config set --user` 属于阶段 5 之后的事）。

**`FMT-604 NoCurrentUser` 保留给「用户被显式清空」这一种情况**：外部把 `config.json`
的 `current_user` 改回空串时，Bucket 服务的每个业务命令仍然会先检查它并返回 `FMT-604`
（退出码 7），不会拿空用户名去拼出 `repository//<bucket>/` 这种路径。

用户系统正式开发后，再把这个占位替换为正式登录机制（占位名只是入口，不是身份）。

---

# 94. 第一次运行 Bucket

如果：

```text
current_bucket = ""
```

执行：

```text
file upload
```

或：

```text
file list
```

时：

```text
提示用户先创建或选择 Bucket
```

**阶段 4 已落地的部分**：`bucket create` 会在 `current_bucket` 为空时把第一个 Bucket
设为当前，所以「第一次运行」的正常路径是**先敲一条 `bucket create`**，此后
`current_bucket` 不再为空（第 28 节）。已实现的 `bucket list` / `bucket get` 不依赖
`current_bucket`，也不会因为它是空串而报错——它们只标出「有没有当前 Bucket」。

`FMT-305 NoCurrentBucket` 由**文件类命令**（`file upload` / `file list` 等，阶段 5）
在 `current_bucket` 为空时返回；Bucket 命令不用它。两者不要混：`FMT-305` 是「没选桶」，
`FMT-604` 是「没有用户」（第 93 节）。

---

# 95. CLI 命令最终结构

```text
fmt.exe
│
├── --help
├── help [组]
├── exit | quit
│
├── bucket
│   ├── create <name>
│   ├── list
│   ├── get <name>
│   ├── use <name>
│   └── delete <name>
│
├── file                              全部四条已落地（提交 188e85d；delete 参数 0ad9efc）
│   ├── upload <来源> [文件名]         来源 = http:// 或 https:// URL，或本机路径（a2b6cd1）；
│   │                                  文件名可省略，从来源推断（第 33.5 节）
│   ├── list
│   ├── get <file_id>
│   ├── get <filename>
│   └── delete <file_id|文件名>        软删除，file_id 不变（第 43 节）；
│                                      定位与 get 同一套规则（0ad9efc）
│
├── share                             **已落地**（数据面提交 `d5779db`；公开下载端点 `d3aeb3d`，第 105 节）
│   ├── create <file_id>
│   ├── get <share_id>
│   ├── list <file_id>
│   └── delete <share_id>
│
├── trash
│   ├── list                 **两级都可用（0fc242b）**：文件级 [文件] 与桶级 [桶] 都列，
│   │                        并标出类型（第 53.1 节）
│   ├── get <标识>           **两级都可用**：类型 / 标识 / 删除时间 / 路径 / 大小或文件数
│   ├── restore <标识>       两级：文件级按 file_id 搬回原位置；桶级目标不在 → 整单搬回，
│   │                        目标已存在 → 合并（要确认；有同名文件 FMT-403，第 56.3 节）
│   └── delete <标识>        **两级永久删除**（4fee290 + 0fc242b），需要确认（FMT-016）
│
└── service
    ├── install
    ├── uninstall
    ├── start
    ├── stop
    └── status
```

规则：

```text
service 命令不带 -- 前缀（旧写法 fmt.exe --service install 作废）
service 有 install / uninstall / start / stop / **reinstall** / status 六条子命令（提交 `c573f14` 起 `reinstall` 从引导内部用法变成正式命令）
没有 pause，也没有 delete（旧 delete 更名为 uninstall）
业务命令（bucket / file / share / trash）经命名管道交给服务执行
service 命令直连 SCM：install / uninstall / start / stop / reinstall 走 UAC 提权，
                        status 是查询、不提权、不弹 UAC
help / --help 是本地命令：不连服务、不提权、不写日志
exit / quit 是交互循环里的正式命令（help exit 有说明）
```

帮助有两个入口：`--help` 打印带横幅与退出码表的完整用法（内含命令总览）；`help` 不带参数
只列命令、`help <组>` 打印该组详情（`service` / `bucket` / `file` / `share` / `trash` /
`help` / `exit`），交互式与一次性都支持；`help <未知组>` → stderr 一行 + 退出码 2（`FMT-001`）。
各子命令的详细说明必须与实际命令一致（见第 68 节）。

**`file` 四条命令也已可用**（提交 `188e85d`，不再返回 `FMT-602`）：
`file upload <来源> [文件名]`（注意那个**可选**的文件名参数，旧文只写了 `<URL>`）、
`file list`、`file get <file_id|文件名>`、`file delete <file_id|文件名>`
（最后一条在提交 `0ad9efc` 从只收 `file_id` 扩成两种，与 `file get` 同一套定位规则，第 43 节）。
CLI 侧不特殊处理 `file`：它就是通用业务命令，`parts[0] + "." + parts[1]` 拼出 op，
其余位置参数装进 `args.argv`（`run_business_command()`）——所以
`file upload D:/a.txt 新名字` 发出的就是 `{"op":"file.upload","args":{"argv":["D:/a.txt","新名字"]}}`。
输出样例见第 127.6 节与 `FMT 技术文档.md` 第 11.14 节。

**阶段 4 的 `bucket` 命令已可用**（不再返回 `FMT-602`），输出样例：

```text
fmt> bucket create 工作
Bucket 已创建：工作（已设为当前 Bucket）
执行成功...
错误码：0
```

```text
fmt> bucket list
* 工作  (当前)
  生活
共 2 个 Bucket
执行成功...
错误码：0
```

```text
fmt> bucket get 工作
Bucket：工作
当前：是
路径：repository/user/工作
执行成功...
错误码：0
```

展示规则：**服务端只返回结构化数据，怎么打印放在 CLI 一侧**（`print_business_data` /
`print_trash_entry`）——
有 `buckets` 数组就按列表逐条打印（当前项前缀 `* `、后缀 `  (当前)`，末行 `共 N 个 Bucket`）；
有 `entries` 数组就按回收站列表打印（**提交 `0fc242b` 起文件与桶都标出**，
末行 `共 N 项（X 个文件、Y 个桶）`；旧口径的 `deleted_buckets` 形状**已作废**）；
有 `entry` 对象就按**条目详情**打印（提交 `0fc242b`：`[文件]` / `[桶]` + 标识 + Bucket/大小
或文件数 + 删除时间 + 回收站路径 + 状态/可回退）；有 `bucket` + `is_current` 就按单条打印
（可选的 `path` 追加一行；**提交 `6a40742` 起 `path` 也是规范化后的名字**）；
否则打印服务给的 `message`。`bucket use` / `bucket delete` / `trash restore` / `trash delete`
就是 `message` 那一类；**破坏性操作的预检另走 `print_precheck()`**（第 127.7 节）。

**`file` 的三条规则（提交 `188e85d`，与前面的判断顺序一致：先 `files` 数组、再 `file_id` + `size`、
最后才落到 `message`）**：

```text
有 files 数组（且不是 trash 的条目详情）  按列表打印：每行「  文件名  大小」，
                                          末行「共 N 个文件」        ← file list
有 file_id + size                        按单条打印：文件 / file_id / Bucket / 类型
                                          / 大小 / MD5 / 路径 / 状态   ← file get
                                          状态：正常 或 在回收站（<trash_reason>）
                                          有 path 才打印「路径」那一行
否则                                     打印 message               ← file delete / file upload
```

注意 `files` 这个字段名被两处复用：`trash get` 的条目详情是
`{trashed, original, deleted_at, present, path, files, bytes}`（`files` 是**数字**），
`file list` 的 `files` 是**数组**。CLI 靠「`trashed` 存在 + `files` 存在」先认条目详情、
再认数组，所以两条规则不会打架（`src/cli/cli.cpp` 的判断顺序就是如此）。

```text
fmt> bucket use 生活
已切换到 Bucket：生活
执行成功...
错误码：0

fmt> bucket delete 生活
Bucket 已删除（移入回收站）：生活  ->  生活_20261008012233
执行成功...
错误码：0
```

桶级回收站的四条命令现在都已落地（`list` / `restore` 提交 `c2d545d`，`get` / `delete`
提交 `4fee290`），输出样例：

```text
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
（回退那个桶，分两种情况——**目标 Bucket 已存在时**）
预检：回退：Bucket「lazy-fox」已存在，将把回收站里的 2 个文件（1.2KB）合并进去，
      现有 1 个文件保持不动、不覆盖、不改名
确认执行？(y/N) y
[桶] lazy-fox
  标识：lazy-fox_20261008012233
  删除时间：2026-10-08T01:22:33
Bucket「lazy-fox」：已合并 2 个文件
执行成功...
错误码：0

（目标 Bucket 已存在、且里面有同名文件时——**一个都不搬**）
fmt> trash restore lazy-fox
执行失败：FMT-403 回退失败：Bucket「lazy-fox」已存在，合并进去会撞上 1 个同名文件，
          本操作不覆盖、不自动改名，因此一个文件都没动：2026/10/08/a.txt。
          请先给现有 Bucket 里的同名文件改名或删除，再执行合并；或者用 trash delete
          永久删除回收站里这一份
错误码：4
```

> 列表里**每一条都标出是 `[文件]` 还是 `[桶]`**（提交 `0fc242b`，用户明确要求），
> 文件那一行带它的 Bucket 与大小；桶那一行在 `name` 与原桶名不同时印
> `  ->  <回收站目录名>`。旧口径的 `deleted_buckets` 形状与
> `共 N 个已删除的 Bucket` 末行**已作废**。
> **原口径「目标已存在时 `restore` 是整单拒绝（FMT-401）」已作废**（第 56.3 节）：
> 现在目标已存在要走**合并**——预检说清搬几个、要一次确认，确认后逐个搬文件；
> 只有**同名冲突**才是硬拒绝（`FMT-403`，退出码 4）。

`trash get` / `trash delete` 的输出样例（提交 `0fc242b`；`get` 对两级同形）：

```text
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
```

```text
fmt> trash delete lazy-fox_20261008012233
永久删除后不可恢复：lazy-fox_20261008012233（原桶 lazy-fox，3 个文件，1.2KB）
[桶] lazy-fox
  标识：lazy-fox_20261008012233
  文件数：3
  删除时间：2026-10-08T01:22:33
  回收站路径：trash/user/lazy-fox_20261008012233
确认执行？(y/N) y
已永久删除：lazy-fox
执行成功...
错误码：0

fmt> trash delete lazy-fox_20261008012233
永久删除后不可恢复：lazy-fox_20261008012233（原桶 lazy-fox，3 个文件，1.2KB）
[桶] lazy-fox
  …（同上，预检先打印，再问）
确认执行？(y/N) n
已取消
错误码：0

fmt.exe trash delete lazy-fox_20261008012233
该操作需要确认（FMT-016）：请加 --yes，或在交互窗口里执行
错误码：2

fmt> trash delete lazy-fox_20261008012233        ← 服务端侧的兜底（任何入口都不例外）
执行失败：FMT-016 永久删除不可恢复，需要确认（force = true）
错误码：2
```

> **流程是「先检查 → 说清楚 → 再确认」**（提交 `711da4c`，第 127.7 节）：
> 交互窗口先发一次只读预检、把条目详情与「不可恢复」那一行打出来，再问 `(y/N)`；
> **答 n 或直接回车就取消**（打印「已取消」，退出码 0，**不发请求**）；
> 一次性命令必须 `--yes`（或 `-y`），否则打印上面那行提示、退出码 2。
> `--yes` / `-y` 是本地开关，**不会作为位置参数发给服务端**。服务端不信任任何入口，
> 自己再检查一次 `force == true`。HTTP 侧对应 `DELETE /api/trash/<标识>?force=1`
> （或请求体 `{"force":true}`），否则 400 + **`FMT-016`**（第 59、60、82 节）。
> 预检本身用 `?dry_run=1`。

> **帮助文案已随阶段 4 同步**（提交 `8a5e554`、`c2d545d`，`4fee290` 收尾）：命令总览把
> `(bucket)` 与 `(trash) list get restore delete` 移进「可用命令」组，`help bucket` 的标题是
> 「存储空间（Bucket 就是一个目录，没有独立 ID）」并写明五条子命令的真实行为，
> `help trash` 写明四条子命令的真实行为；仍写「尚未实现，返回 FMT-602」的
> **只有 `file` / `share` 两组**（原口径「以及 `trash get` / `trash delete`」已作废，第 68 节）。
>
> **提交 `188e85d` 再同步一次**：`(file) upload list get delete` 也移进「可用命令」组
> （命令总览的顺序是 service → bucket → file → trash → help → exit），
> 「尚未实现」组**只剩 `(share)` 一行**，`help file` 的正文照源码写进第 68 节
> （不含「尚未实现」字样）。上面的「只有 `file` / `share` 两组」是**阶段 4 收尾时**的原话；
> **提交 `674d0b0` / `d5779db` 之后连 `share` 也落地了**：命令总览里 `(share)` 移进
> 「可用命令」组（落在 `(trash)` 之后、`(config)` 之前），**「尚未实现」组整个被删掉**
> （`d5779db`），`help share` 换成数据面正文（第 68 节；**公开下载端点那两行由提交
> `d3aeb3d` 补上**）。
> 原口径「`help share` 仍注明『服务端尚未实现，现在返回 FMT-602』，与实况一致」**已作废**。

---

# 96. 开发顺序

V1 不建议一次性开发全部功能。

Service 与 CLI 提前到**阶段 2 / 3**（旧设计排在阶段 9），先让「双击即用的服务 + CLI」闭环成立，
业务功能（bucket / file / share / trash）排在后面。

推荐：

```text
阶段 1
项目骨架
CMake
Ninja
fmt.exe
```

↓

```text
阶段 2
common
Error / Result / Time / String / Path / Logger
config
storage
core 初始化
```

↓

```text
阶段 3
service
ipc
cli
```

↓

```text
阶段 4
Bucket
+ 桶级回收站（trash list / get / restore / delete；get 与 delete 在提交 4fee290 补齐）
```

↓

```text
阶段 5
File
Upload
Trash（文件级条目）
Share
```

↓

```text
阶段 6
server
HTTP
Preview
```

阶段 2 + 3 完成后，「双击即用的服务 + CLI」闭环成立（见第 99 节）。

---

# 97. 阶段 1：项目骨架

目标：

```text
CMake
+
Ninja
+
MSVC
```

能够成功生成：

```text
fmt.exe
```

首先实现：

```text
fmt.exe --help
```

---

# 98. 阶段 2：common、config、storage 与 core 初始化

实现：

```text
common
    Error
    Result
    Time
    String
    Path
    Logger
config
storage
core 初始化
```

完成后，服务在 CLI 声明的数据根下自动创建（CLI 双击时也对自己所在数据根跑同一套幂等补齐）：

```text
<数据根>/
├── repository/
├── trash/
├── config/
├── data/
├── log/
└── temp/
```

并写入默认 JSON（`.tmp` 原子替换，已存在不动；已有的会被读一遍确认，损坏只报告不重置）。

验收：

```text
能在指定数据根建出六个目录 + 默认 JSON（Service 与 CLI 共用 ensure_root/check_root）
第二次执行日志里只多一行「数据根完整」，不再新建任何东西，控制台无输出
JSON 损坏报配置错误（退出码 7），且不修改原文件，只报告（日志 + stderr）
```

---

# 99. 阶段 3：service、ipc 与 cli

实现：

```text
service
    service install
    service uninstall
    service start
    service stop
    service status（查询，不提权）
    统一查询接口 StatusInfo / query_status / query_state / last_start_failure
    按 dwWaitHint 自适应的落定等待（100 ms – 2000 ms 夹取，兜底 30 秒）
    ServiceMain + HandlerEx
    ServiceMain 失败时上报 FMT 编号到 dwServiceSpecificExitCode
    Recovery
    %ProgramData%\FMT\service.json
ipc
    命名管道 \\.\pipe\fmt.control
    帧格式 [4 字节小端长度][UTF-8 JSON]
    hello 响应回填 switched / previous_root
    安全描述符（授权 IU）+ MIC 标签
cli
    交互循环与横幅
    单实例互斥体 Local\FMT.CLI.v1
    单实例窗口激活
    UAC 提权与结果回显
    双击行为（首次 / 再次 / 换目录）：先做数据根幂等体检与补齐，
      再查 SCM（走 query_status()，「未安装」是正常结果）；
      已安装未运行 → 提权 start → settle_state() 等落定（按 dwWaitHint 自适应）→
      仍没起则读失败编号，数据根/配置类问题跳过重装，其余提权 reinstall 一次
```

并配置：

```text
自动启动（SERVICE_AUTO_START）
异常自动恢复（Recovery 5 秒 / 10 秒 / 30 秒，失败计数 1 天重置）
```

同时确定：

```text
manifest 为 asInvoker
服务不声明 SERVICE_ACCEPT_PAUSE_CONTINUE（没有 pause）
数据根由 CLI 用 hello 帧声明，响应回填 switched / previous_root
初始化由 Service 与 CLI 共用同一套幂等规则（ensure_root/check_root）：
  Service 在启动/换根时执行，CLI 在双击时对自己的数据根执行；两边都只补不缺
service status 是查询命令，不提权、不弹 UAC
服务状态与失败编号统一走 service::query_status() / query_state() / last_start_failure()；
  「未安装」是正常结果（state = NotInstalled），不是错误
「等它落定」按 SCM 的 dwWaitHint 自适应：夹在 100 ms – 2000 ms，兜底 30 秒，
  不是等待类状态就立即结束（不写死时长）
```

验收：

```text
全新环境双击 → 一次 UAC → 服务装好且开机自启
service stop / service start 各弹一次 UAC，输出与第 126 节样例一致
service status 不弹 UAC，输出「服务状态：运行中 / 服务宿主 / 服务数据根 / 错误码：0」
未安装时 service status 打印「服务状态：未安装」且退出码 8
人为让服务启动变慢 → 等待随之变长，不误判为启动失败、不弹 UAC 重装
复制 exe 到新目录双击 → 在新目录建出数据（只补缺失），旧目录数据保留，
  并**在 stderr 打印换根提示** + 记一行日志「数据根切换：旧 -> 新」（提交 `8f2fbc5`）
```

---

# 100. 阶段 4：Bucket（含桶级回收站）

实现：

```text
bucket create
bucket list
bucket get
bucket use
bucket delete
trash list          ← 桶级（阶段 4 追加）
trash restore       ← 桶级（阶段 4 追加）
trash get           ← 桶级（阶段 4 收尾提交 4fee290 追加）
trash delete        ← 桶级（阶段 4 收尾提交 4fee290 追加，永久删除 + 强制确认）
```

首先确保：

```text
current_bucket
```

逻辑稳定。

**状态：✅ 已完成（`arch-restart` 分支，commit 32249ea「feat(bucket): create, list, get, use and delete over both entries」；
回收站形状与桶级 trash 的 `list` / `restore` 在 commit c2d545d 落地；
桶级 `trash get` / `trash delete`（永久删除 + 强制确认）在 commit `4fee290` 收尾落地）。**

完成内容：

```text
common/validation   validate_bucket_name / validate_file_name / is_windows_reserved_name
                    / looks_like_file_id（提交 9c3d2cb）
                    （第 25、26 节；FMT-100～104、FMT-106 与 FMT-202 的分工见该节表）
config              current_user 空时自动补占位名 user（第 18、93 节）
bucket              BucketService：create / list / get / use / remove
                    + list_trashed / restore / get_trashed / purge
                    + directory_of / refresh_current_bucket；
                    私有 find_trashed（三者共用定位）/ remove_bucket_file_records；
                    Bucket 无独立 ID（第 27～31 节）
回收站形状          目录名一律 <原桶名>_<YYYYMMDDHHMMSS>（同秒冲突加 _2）；
                    桶级身份记录的唯一权威是 trash/<user>/.original
                    （kOriginalIndexName = ".original"，第 17.2、30、56 节）
                    file.json 新增 trash_reason："bucket" / "file"
文件级落点          trash/<user>/.files/<bucket>/YYYY/MM/DD/<file>（提交 4fee290，
                    PathManager::build 的 inner 参数；顶层留给桶级条目，第 21 节）
桶级扫描形状        跳点开头 + 只认 <名字>_<14 位时间戳>（可带 _<1-3 位序号>）
service             service::execute_business 的 bucket.* 五种 op +
                    trash.list / trash.restore / trash.get / trash.delete，
                    参数走 args.argv；trash.delete 要求 args.force == true，
                    否则 FMT-001（`FMT 技术文档.md` 第 12.3.2、13.9.3 节）
runtime             服务启动（start）与数据根切换（apply_root，hello 触发）时，
                      在业务锁下校一次 current_bucket：失效置空、有效不动（第 61 节）
ipc / cli           CLI `bucket create|list|get|use|delete` 与
                      `trash list|get|restore|delete` 经管道执行并按形状打印
                      （第 95、127 节）；trash delete 在交互窗口问一次、
                      一次性命令要 --yes（第 59 节）；
                      帮助文案把 bucket 与桶级 trash 四条移出「尚未实现」组
                      （提交 8a5e554、c2d545d、4fee290）
server              GET/POST /api/bucket、GET/POST /api/bucket/<name>[/use]、
                    DELETE /api/bucket/<name>、GET /api/trash、
                    POST /api/trash/<名字>/restore、GET /api/trash/<名字>、
                    DELETE /api/trash/<名字>（需 ?force=1 或 {"force":true}，
                    否则 400 + FMT-001；**提交 711da4c 起是 400 + FMT-016**，
                    且 ?dry_run=1 只预检）；路径参数百分号解码，
                    请求体接受 {"name":"工作"} 或 {"argv":["工作"]}
                    （`FMT 技术文档.md` 第 12.3.2 节）
                    ——**本次改动（未提交）后的现行口径**：桶的写路由仍然不提供
                    （`4b812b5` 删除后未回补），只新增 `GET /api/bucket`（只读列表）、
                    `GET /api/config`（只读，一律不返回 token、连开关都不提供）与
                    `DELETE /api/trash/empty`（清空整站，`?dry_run=1` / `?force=1`，
                    **必须注册在 /api/trash/([^/]+) 之前**）；
                    见第 43 条决策与 `FMT 技术文档.md` 第 18.43 节
```

验收（`tests/bucket_test.cpp`、`tests/validation_test.cpp`、`tests/service_test.cpp`）：

```text
首个 Bucket 自动成为当前；第二个不抢走「当前」
重复创建 → FMT-201；名称非法（a/b、CON、结尾空格或点、超长）→ FMT-202
use 只改 current_bucket，两个 Bucket 目录都还在；不存在 → FMT-200
get 返回名称与当前标记；不存在 → FMT-200
删除 → 目录移到 trash/<user>/<bucket>_<时间戳>/，原目录消失，数据仍在回收站
     → 本桶 file.json 记录 is_trash=true 且 trash_reason="bucket"，别的桶不受影响
     → trash/<user>/.original 多一条记录（trashed / original / deleted_at），
       且**没有 file_id**；data/trash.json **不再**出现桶级条目
     → 删的是当前 Bucket 则置空，不自动切换
一律带时间戳：同一个桶删两次 → 两份数据、两条 .original 记录，目录名不同
.original 损坏 → bucket delete 直接拒绝（FMT-006），目录一个字都没动
trash list → {deleted_buckets:[{trashed, original, deleted_at, present}], count}；
             目录没了标 present=false；目录有而索引没有则 original 为空
trash restore → 目标 Bucket 已存在 → **合并**（第 56.3 节）：先预检说清搬几个文件、
             有同名文件就 FMT-403（一个都不搬）、没冲突则要一次确认（缺 force → FMT-016）；
             确认后把文件合并进现有桶（不覆盖、不改名），全部搬完才摘 .original
             目标不存在 → 整个目录一次搬回，.original 少一条，
             只有 trash_reason="bucket" 的记录被翻回正常（"file" 的不动）
trash restore 定位：回收站名精确匹配；原桶名同名多条 → FMT-001 并列出候选
trash get → {trashed, original, deleted_at, present, path, files, bytes}；
            索引有目录没了不报错、present=false；孤儿目录按目录名也能查到；
            两者都没有 → FMT-400；遍历目录数 files/bytes
            （**提交 18f16ca 起 `trash list` 的桶级条目也带这两个数**，见第 53.1 节）
trash delete（永久删除，提交 4fee290）→ 先删目录、再清 trash_reason="bucket" 的
            file.json 记录（"file" 的绝不动）、最后摘 .original；
            返回 removed_files / removed_records；幽灵条目也能删；
            **不带 force → FMT-001 + 退出码 2**；CLI 交互窗口答 n → 「已取消」+ 退出码 0
            且不发请求；一次性命令不带 --yes → 本地拒绝、退出码 2、不连服务
回收站扫描形状：trash/<user>/ 下点开头的条目被跳过，只认
            <名字>_<14 位时间戳>（可带 _<1-3 位序号>）形状的目录
当前 Bucket 目录失效 → refresh_current_bucket 置空（只有一个 Bucket 也不自动切换）
当前 Bucket 接线：服务启动时把失效的当前 Bucket 置空，
                 而指向**存在**的 Bucket 时不被误清（tests/service_test.cpp）
current_user 被显式清空 → FMT-604（正常路径由占位名 user 兜住）
管道 op：bucket.create / bucket.list / bucket.get / trash.list / trash.restore /
         trash.get / trash.delete 端到端通过；trash.delete 不带 force 被拒（FMT-001/2）、
         带 force 成功；file.* 仍是 FMT-602（第 100 节这条记的是**阶段 4 收尾时**的状态；
         提交 188e85d 已把 file 四条命令落地，见第 101～103、110 节。
         **缺确认的错误码在提交 711da4c 改成 FMT-016**，用例断言也改了）
HTTP 路由：GET /api/trash、POST /api/trash/<名字>/restore、GET /api/trash/<名字>、
          DELETE /api/trash/<名字>；DELETE 不带 force → 400 + FMT-001
          （**提交 711da4c 起是 400 + FMT-016**，且 ?dry_run=1 只预检），
          带 ?force=1 → 200 并真的删掉
帮助：命令总览把 (bucket) 与 (trash) list get restore delete 列进「可用命令」，
     「尚未实现」组只剩 (file) / (share)（阶段 4 收尾时的原话；提交 188e85d 之后
      该组**只剩 (share)**，见第 68、95 节）；
     help bucket / help trash 不含「尚未实现」字样
新用例：Bucket.条目详情与永久删除、Bucket.永久删除只清桶级记录、
       Bucket.回收站扫描只认桶级条目、Bucket.有索引没目录的条目可以永久删掉、
       Service.管道能执行回收站命令、Server.Bucket路由与状态码、
       PathManager.回收站保持原层级（104 个单元测试全绿）
```

> **上面这一段是阶段 4 收尾（`4fee290`）的历史记录**，其中三处后来已改，
> 引用现状请看对应小节：① `trash list` / `get` / `delete` 的形状改成统一 `entries` /
> `entry`（提交 `0fc242b`，第 53.1、53.3、127.6 节）；② 缺确认的错误码由 `FMT-001`
> 改成 **`FMT-016`**（提交 `711da4c`，第 59、82 节）；③ `file.*` 早已落地
> （提交 `188e85d`，第 101～103 节）。测试计数也从 104 走到了 **140**（第 109、112、125 节）。

**下一步只剩阶段 7 的「永久删除时清理 `share.json`」**。阶段 5 已经全部做完：
`file` 四条命令（提交 `188e85d`，第 101～103 节）、文件级与桶级的 list/get/restore/delete
（文件级读取侧提交 `0fc242b`，第 52～55、59 节；桶级四条命令更早在阶段 4 完成——
`list` / `restore` 见第 53、56 节，`get` / `delete` 见第 53.3、59、60 节）、
`share` 数据面与公开下载端点（提交 `d5779db` / `d3aeb3d`，第 105 节）；
阶段 7 现在只剩永久删除时的 `share.json` 清理。

> **阶段 5 的实况（提交 `188e85d`）**：`file` 四条命令（`upload` / `list` / `get` /
> `delete`）**已经落地**，第 101～103 节现在是「已完成」而不是「待做」；
> **`share` 仍未实现**（第 105 节），阶段 5 剩下它；文件级 `trash` 的读取侧仍在阶段 7
> （第 53.2、104 节）。「上传期间其他命令一起等」这条已知限制**已经解决**——
> 上传改两段式、长任务不持锁（第 101 节的落地方案）。
>
> **更正（后续提交）**：上面那两句只描述 `188e85d` 当时的状态——`share` 数据面已在
> `d5779db` 落地、公开下载端点已在 `d3aeb3d` 落地，文件级 `trash` 的读取侧也已由
> `0fc242b` 关闭（第 105、53.2、104 节）。

---

# 101. 阶段 5：File 基础

实现：

```text
file.json
file_id
filename
extension
file_type
size
md5
is_trash
```

先不要加入：

```text
Share
HTTP
Preview
```

**阶段 5 开工前必须先决定并发模型（第 84 节 ④，已知限制）**：阶段 4 是「两条入口的
业务命令共用运行体的一把互斥锁」，上传/下载可能持续几十秒到几分钟，持锁期间
`bucket list` / `bucket get` 与浏览器的 bucket 请求会一起阻塞在 `lock()` 上。
写 `file` 之前必须二选一：

> 下面这几段记的是**决策前的状态与原话**（阶段 5 开工时的问题描述）；
> 「二选一」的结果与落地见本节更下面的「这条限制已经落地解决（提交 `188e85d`）」。
> 本文件里凡出现「上传持锁 / 上传期间其他命令一起等」，都指的是**决策前**的状态。

```text
A. 收细锁粒度：按 JSON 文件（file.json / trash.json / share.json / config.json）
   或按 file_id 分锁，让 bucket 命令与上传不再互相挡
B. 把长任务移出锁：登记任务（返回 task_id / 进度查询 op）
   + 后台线程执行 + 轮询状态，锁只在读写元数据的那一小段持有
```

在 A 或 B 落地之前，「上传期间其他命令一起等」是**既定限制**，不要当 bug 排查。
同时要定的还有长耗时命令的超时值（普通命令现在是 30 秒，上传要单独声明，
见 `FMT 技术文档.md` 第 13.9.4、18.16 节）。
—— 这半句已在提交 `a2b6cd1` 定完：`file.upload` 用 `ipc::kUploadTimeoutMs = 30 分钟`，
其余命令仍 30 秒（第 33、101 节与 `FMT 技术文档.md` 第 13.9.4、19.1 节）。

**这条限制已经落地解决（提交 `188e85d`）：选的是 B 的简化形态——「长任务不持锁」。**
不再登记任务、不加进度查询 op（V1 只有一个 CLI 窗口、管道本身串行，登记任务没有收益），
而是把上传拆成两段：

```text
① prepare_upload()   锁外：下载/复制到 temp/、边写边算 MD5、边判大小上限
② commit_upload()    锁内：去重 → 重名 → file_id → 搬到仓库 → 写 file.json
```

下载期间**根本不持业务锁**，所以 `bucket list` / `trash *` / 浏览器请求都不会被堵住；
锁只在第 ② 段——一次元数据提交——held 住，毫秒级。运行体
`ServerRuntime::run_upload()` 就是这两段的编排，**管道与 HTTP 共用这一份**
（第 32、39 节，`FMT 技术文档.md` 第 15.1、18.16、18.19 节）。

**阶段 5 不需要再做的一件事**：`current_bucket` 失效校验**已经接线**——服务启动
（`ServerRuntime::start()`）与数据根切换（`apply_root()`，hello 触发）时都会在业务锁下调用
`refresh_current_bucket()`（失效置空、有效不动，失败只记 WARN，第 61 节）。
`file` 命令只需要在 `current_bucket` 为空时返回 `FMT-305 NoCurrentBucket`，
不必自己再判「这个 Bucket 还在不在」。

> **状态：✅ 已完成（提交 `188e85d`「feat(file): upload, list, get and soft delete」）**。
> 落地内容：`file.json` 十字段记录（第 14 节）、`file_id` = 当天最大序号 + 1（第 22 节）、
> `extension` / `file_type`（小写扩展名 + 五类映射）、`size`、`md5`（CNG）、
> `is_trash` / `trash_reason`、存储日期由 `file_id` 推出（第 20、21 节）。
> `http://` 与 `https://` 下载、本地路径复制都在 `prepare_upload()` 里；
> **来源双方都支持**（提交 `a2b6cd1` 起 https 也支持，第 33 节）。
> 这里原文写的是「`https` 明确不支持」——**已作废**。

---

# 102. 阶段 5：Upload

实现：

```text
file upload <来源> [文件名]
```

完成：

```text
HTTP 与 HTTPS 下载    ← 实况（提交 a2b6cd1）：http:// 与 https:// 都支持，
                        走 common/http_client（WinHTTP + Schannel，不用 OpenSSL、
                        不分发 DLL，自动使用系统代理）；本机路径同样支持
                        【188e85d 时期的写法，已作废：只支持 http://；
                         https 返回 FMT-300；下载走 httplib::Client 的 http】
临时文件              <数据根>/temp/fmt-upload-<随机>-<序号>.tmp（第 34 节）
大小限制              边写边判（> max_upload_size → FMT-303），不是下完再看
MD5                   边下载边算（Windows CNG / bcrypt，第 36 节）
桶内判重              同用户 + **当前 Bucket** + 正常文件，**先看内容、再看名字**
                      （作用域收窄到桶内是本次改动（未提交）；名字比较不区分大小写，
                       提交 5bf2c1f）：
                      内容相同（名字同与不同都一样）→ FMT-304「文件已存在」，不提改名（第 37 节）；
                      内容不同 + 名字相同 → FMT-105「文件名重复」，提改名（第 38 节）；
                      内容不同 + 名字不同 → 正常入库
File ID               fmt-<今天>-<当天最大序号 + 1>（第 22 节）
metadata              写 file.json；写不进去就删掉刚提交的仓库文件（第 40 节）
```

**状态：✅ 已完成（提交 `188e85d`；下载客户端在 `a2b6cd1` 换成 WinHTTP，https 可用）**。
用例见第 110、111 节与
`FMT 技术文档.md` 第 18.19、18.20 节。

---

# 103. 阶段 5：File 操作

实现：

```text
file list
file get <file_id>
file get <filename>
file delete <file_id|文件名>
```

验证：

```text
正常文件
+
当前 Bucket
+
文件名唯一性
```

**状态：✅ 已完成（提交 `188e85d`；`file delete` 的参数在 `0ad9efc` 扩成两种）**：

```text
file list                  当前用户 + 当前 Bucket + 正常文件，按 file_id 排序（第 41 节）
file get <file_id>         全局唯一，不限用户/Bucket（软删除后也能查到，is_trash=true）
file get <文件名>          当前用户 + **当前 Bucket** + 正常文件（本次改动（未提交）：名字的命名
                           空间是桶；当前桶没有、别的桶里有 → FMT-002 并说清在哪个桶）
                          命令层先当 file_id 查，查不到再当文件名查（第 42 节）
file delete <file_id|文件名>  软删除：file_id 不变，搬进 trash/<用户>/.files/…、
                          写文件级 trash.json 记录、置 is_trash/trash_reason（第 43 节）
                          定位规则与 file get 完全一致（提交 0ad9efc，第 43 节）：
                          先当 file_id → 再当文件名（**当前 Bucket**；别的桶里的同名不再
                          命中，报 FMT-002 并说清它在哪个桶）→
                          名字在、但已在回收站 → FMT-001（消息带 file_id）；
                          都没有 → FMT-002 FileNotFound（退出码 3）
                          **三处比较都不区分大小写（提交 5bf2c1f，第 38、43 节）**
                          回收站落点用**记录自己的 bucket**，不是当前 Bucket（第 21、43 节）
                          顺序与回滚：搬文件 → 写 trash.json（失败搬回）→ 写 file.json
                          （失败撤掉 trash 记录并搬回），见第 43 节
```

用例：`File.列表与查询`、`File.软删除进回收站`、`File.按文件名也能软删除`、
`File.桶内判重先看内容再看名字`、`File.判重不跨桶`、
`File.判重不跨桶时按名字删除只作用于当前桶`、
`File.按file_id删别的桶的文件时回收站用记录自己的Bucket`、
`File.大小写不同的同名必须被当成重名`、
`File.按名字查询不区分大小写`、`Service.管道能上传与操作文件`
（第 109 节，`FMT 技术文档.md` 第 18.19、18.22 节）。

**并入的决策（本次改动（未提交）：判重收窄到桶内 + 完整链接）**：

```text
用户口径：① 一个桶内不能存在相同文件名和相同内容的文件；1 桶有这个文件，
            2 桶也可以有 1 桶内容相同的文件。
          ② 上传时检查同一个桶里面内容相同就输出「存在」，内容不同、文件名相同就
            提示更改文件名。
          ③ `share create <文件id|文件名>` 要返回 share_id 与完整分享链接，
            **无论有没有开网络服务**。
          ④ `file get <文件id|文件名>` 要拼一条完整预览链接，同样不看服务开没开。
          ⑤（第三轮补充，四条口径，**判重的最终形态**）
            1) 当前桶内 内容相同 + 文件名不同 → 「文件已存在」，**不提示改文件名**
            2) 当前桶内 内容相同 + 文件名相同 → 「文件已存在」，**不提示改文件名**
            3) 当前桶内 内容不相同 + 文件名相同 → 「文件名重复」，**提示改文件名**
            4) 当前桶内 内容不相同 + 文件名不相同 → 正常上传
改了什么：① 判重作用域从「同用户 + 任何 Bucket」收窄到「同用户 + 当前 Bucket」；
          ② 桶内判重改成**先看内容、再看名字**（口径 ⑤）：md5 相同 → FMT-304
            「文件已存在」（名字同与不同都一样，**不提改名**）；
            内容不同而名字相同 → FMT-105「文件名重复」（**要提改名**）；
            两者都不同 → 正常入库（同内容也不同名字不再允许并存）；
          ③ ShareService::create/list 收 file_id 或文件名（新增公开的 resolve()，
            与 file get / file delete 同一套两步定位）；share.create / share.get /
            share.list 都回 share_url（= url），地址由 config/server.json 的 host/port
            算出，不看 enabled；
          ④ file.get 回 preview_url 与 download_url（路径用 file_id），同样不看 enabled；
            CLI 分别打印「分享链接：…」「预览链接：…」「下载链接：…」。
为什么：Bucket 是命名空间（记录里的 bucket 决定落盘目录），两个桶各存一份同内容
        文件不会互相覆盖——这条推翻提交 188e85d 的「Bucket 不构成文件名命名空间」；
        桶内先判内容：同一份内容只留一份，用户要的答案是「这份已经有了」而不是
        「你的名字撞了」；只有「内容不同却同名」才会落到同一个磁盘路径，那才必须改名。
        链接不看服务开关：用户要的是「这条链接该长什么样」，不是「现在能不能连」；
        服务没开时链接不会变，开了就能用。
连带改动：get_by_name() / match_record() 改成桶内作用域（别的桶里的同名只写进错误消息，
        不参与定位）；按名字删/查不再够到别的桶，跨桶只能用 file_id（仍要 force 确认）；
        文件级 trash restore 的同名冲突检查只看记录自己的桶；CLI help file / help share
        文案同步（第 37、38、42、43、47、54 节）。
协议：只给 http://——服务端只监听明文 HTTP（server.json 没有 TLS 配置），
      要 https 得自己在前挂反向代理，不编造 https 地址。
测试（175 → 176 项）：File.桶内判重先看内容再看名字（四条口径逐条断言 + 不留 temp 碎片）/
      File.判重不跨桶 / File.判重不跨桶时按名字删除只作用于当前桶 /
      File.按file_id删别的桶的文件时回收站用记录自己的Bucket /
      Service.管道能上传与操作文件（同内容换名 → FMT-304 且不提改名；同名不同内容 →
      FMT-105 且提改名；file.get 带两条链接）/
      Service.分享的创建查看列出撤销与计数（share_url 形状、按文件名创建与列出、
      名字不存在 → FMT-002）/ Service.破坏性操作先预检再确认（跨桶按名字 → FMT-002）/
      Server.File路由与上传（/api/file/<id> 回 preview_url / download_url）/
      CliE2e.核心链路走真实exe与真实管道（四条口径的 CLI 文案 + 预览链接、下载链接两行）/
      CliE2e.分享能按文件名创建并打印完整链接（新增）/
      CliE2e.交互式确认答n不删答y才删（`FMT 技术文档.md` 第 9.2、10.2、10.2.4、12.3.2、18.41、18.45、18.46 节）。
```

---

# 104. 阶段 5：Trash

实现（**只做文件级**：桶级四条 `trash list` / `get` / `restore` / `delete` 都已落地——
`list` / `restore` 见第 53、56 节，`get` / `delete` 提交 `4fee290`，见第 53.3、59、60 节）：

```text
trash list          **两级都可用**（提交 0fc242b）：文件与桶一起列出并标出类型
trash get <标识>    **两级都可用**：条目详情（file_id / 回收站目录名 / 原名都可）
trash restore <标识>   文件级逐个文件判定（冲突不覆盖不改名）；
                       桶级：目标不在 → 整单搬回；**目标已存在 → 合并**（第 56.3 节，
                       有同名文件 FMT-403、无冲突要一次确认）
trash delete <标识>    **两级永久删除都可用**（桶级 4fee290、文件级 0fc242b），要确认
```

**实况（提交 `0fc242b`）：这一节整个落地了。** 原口径「只有产生条目那一半」**已作废**：

```text
✅ 已落地   file delete 把文件搬到 trash/<用户>/.files/<桶>/YYYY/MM/DD/、
            file.json 置 is_trash / trash_reason / **deleted_at**（不再写 trash.json）
✅ 已落地   上面四条命令：TrashService 把文件级与桶级合成一份 TrashEntry 视图，
            trash list 标出 [文件] / [桶]；回退的硬拒绝（同名冲突 FMT-401、
            随桶删除 FMT-402、数据缺失 FMT-002）都是 blocked（第 52～55、59 节）
✅ 已落地   桶级回退的**合并**这条路（第 56.3 节）：目标 Bucket 已存在不再是死路，
            合并要确认（FMT-016）、撞名一个都不搬（FMT-403）
```

**原「阶段 5 与阶段 7 的分界线」已不成立**：文件级条目的写入与读取都已经落地，
阶段 7 现在只剩**永久删除时的 `share.json` 清理**（第 60 节的已知缺口）。

重点测试（提交 `0fc242b` 起的实况）：

```text
文件删除                     File.软删除进回收站（断言 file.json 的 is_trash /
                            trash_reason / deleted_at，并断言 trash.json 保持空）
文件恢复 + 列出              Trash.文件级条目能列出并回退（列表标出 type=file、
                            按 file_id 取到同一条、回退后三个字段复位、列表变空）
文件名冲突                   Trash.回退遇同名冲突要拦住（FMT-401，两份数据都还在）
随桶删除的文件               Trash.随桶删除的文件不能单独回退（FMT-402、restorable=false、
                            列表里只有那个桶）
文件级永久删除               Trash.永久删除文件级条目（预检 needs_confirm 恒真、预检不改数据）
文件级 trash_reason="file" 的记录在桶回退时保持不动
文件级条目落在 trash/<user>/.files/<bucket>/YYYY/MM/DD/ 下，
  桶级扫描不会把它当成孤儿桶条目（第 21、53.1 节）
桶已存在时回退=合并        Bucket.目标桶已存在时回退是合并进去（文件落到各自日期目录、
                          目标里原有文件一个字节没动、.original 少一条、记录翻回正常）
合并撞名                  Bucket.合并撞上同名文件就一个都不搬（FMT-403、两份数据都在）
合并要求拼写一致          Bucket.合并要求目标桶的实际拼写与原桶名一致（否则 FMT-401）
空条目合并只收尾          Bucket.回收站里没文件的条目合并只收尾（不打扰用户、收掉索引与空目录）
端到端（真实 exe）        CliE2e.桶已存在时回退是合并而不是回退失败（用户报的原始场景：
                          不带 --yes → FMT-016 且一个都没搬；带 --yes → 「已合并」，
                          两个文件都在同一个桶里、回收站清空）
服务层预检                Service.目标桶已存在时回退要先确认再合并（dry_run 的 merge 系列字段、
                          缺 force → FMT-016、带 force → 合并完成）
```

**提交 `0ad9efc` / `0fc242b` 落地的验收**：

```text
文件删除（按名字）     File.按文件名也能软删除：按名字删与按 id 删同一条记录；
                      名字不存在 → FMT-002；名字在但已软删除 → FMT-001（消息带 file_id）
文件删除（跨桶落点）   File.按名字删除用的是记录自己的Bucket：在桶 B 按名字删桶 A 的文件，
                      落点是 trash/<用户>/.files/<A 的桶名>/…（第 21、43 节）
跨桶要确认             Service.破坏性操作先预检再确认：预检说清两个桶名、缺 force →
                      FMT-016 / 退出码 2、带 force 成功且 message 带 Bucket
```

阶段 7 现在只保留：**永久删除时的 `share.json` 清理**（第 60 节）、
以及两级回收站口径的真机端到端实测（第 125 节）。

---

# 105. 阶段 5：Share

实现：

```text
share create
share get
share list
share delete
```

重点：

```text
20 次默认限制
过期时间
下载计数
并发安全
Trash 状态
永久删除
```

**状态：✅ 已落地**（数据面提交 `d5779db`、公开下载端点提交 `d3aeb3d`）。`src/share/`
真实存在，`data/share.json` 有完整读写（`load_shares()` / `save_shares()`，第 16 节）；
`share_id` = **12 位随机十六进制**（`BCryptGenRandom`，撞号重摇最多 16 次、生成失败当错误
返回）；默认 **20 次 + 7 天**（两个条件**相互独立**）；`share get` **如实报状态**（未知 id
才是 `FMT-500`，检查顺序按第 49 节）；下载计数由 `register_download()` 在**业务锁内**一次
完成「全部检查 + 计数 +1 + 落盘」，**计数写不进去就拒绝这次下载**（第 50/51 节）；
文件进回收站 → share 立刻不可用（`FMT-503`）且阻断新的 create。CLI 侧 `(share)` 早已移进
「可用命令」组（第 68、95 节），`help share` 里写明公开下载端点
（`GET /api/share/<share_id>/download`，不要 token、先记账再放行）。
**唯一还没做的**：永久删除 / 删桶时联动清理 `share.json`（第 60 节，阶段 7 的缺口）。

> **与 `file` 的对照**：`file.*` 原来也在这一组里，提交 `188e85d` 之后
> **「服务端尚未实现」这一组只剩 `share` 一行**——用例
> `Service.管道能执行Bucket命令` 与 `Service.未实现的操作与未知操作被明确拒绝`
> 里原来拿 `file.list` 当「未实现」的例子，已改成 `share.list` / `share.create`。
>
> **更正（后续提交）**：上面是 `188e85d` 当时的对照。`share` 落地（`d5779db`）之后，
> 这两条用例又改成拿 **`server.nosuch`** 当「未实现」的例子——`server.*` 是唯一还剩的空壳
> （`tests/service_test.cpp`，`FMT 技术文档.md` 18.38）。

---

# 106. 阶段 6：HTTP Server 与 Preview

实现：

```text
server.json
HTTP Server
文件查询
文件下载
Share
```

最后加入：

```text
Preview
```

HTTP 作用于**当前数据根**，默认只监听 `localhost:4122`；CLI 不依赖 HTTP（见第 76 节）。

---

# 107. 测试原则

每完成一个模块：

```text
开发
 ↓
单元测试
 ↓
集成测试
 ↓
异常测试
 ↓
再进入下一模块
```

不要所有模块完成以后才开始测试。

---

# 108. Bucket 测试

必须测试：

```text
创建 Bucket
重复创建
查询
切换
删除
删除当前 Bucket
删除非当前 Bucket
重启后 current_bucket
无效 current_bucket
```

**阶段 4 的覆盖情况（已落地；回收站形状用例随 commit c2d545d 补齐）**：

| 要求 | 用例（`tests/bucket_test.cpp`） | 状态 |
|---|---|---|
| 创建 Bucket | `首个Bucket自动成为当前` | ✅ |
| 重复创建 | `名称非法与重复创建被拒`（`FMT-201`） | ✅ |
| 名称校验 | `名称非法与重复创建被拒` + `tests/validation_test.cpp` | ✅ |
| 查询 | `get返回名称与当前标记` | ✅ |
| 切换 | `use只改当前不动Bucket` | ✅ |
| 删除 | `删除移入回收站名字带时间戳` | ✅ |
| 删除当前 Bucket | 同上（断言 `was_current` 且 `current_bucket` 置空、不自动切换） | ✅ |
| 删除非当前 Bucket | 同上（`seed_file_record(*paths, "生活", …)` 的记录不被标记） | ✅ |
| 重启后 `current_bucket` | `use只改当前不动Bucket` 会把配置重新读一遍确认落盘 | ✅（进程内） |
| 无效 `current_bucket` | `当前Bucket失效时置空` | ✅ |
| 一律带时间戳 | `删除移入回收站名字带时间戳`（目录名 = 原名 + 时间戳） | ✅ |
| 同秒删两次不覆盖 | `同一秒删两次也不覆盖`（加 `_2` 序号，两份数据都在） | ✅ |
| `.original` 损坏 | `索引损坏时拒绝删除`（`FMT-006`，目录一个字没动） | ✅ |
| 回退 / 目标已存在 | `回退成功与已存在拒绝`（`restore()` 只做整目录搬回，目标在就拒绝；合并走 `merge()`） | ✅ |
| 目标已存在 → 合并 | `目标桶已存在时回退是合并进去`（文件落到自己的日期目录、目标原有文件没动） | ✅ |
| 合并撞上同名文件 | `合并撞上同名文件就一个都不搬`（`FMT-403`，两份数据都在） | ✅ |
| 合并要求拼写一致 | `合并要求目标桶的实际拼写与原桶名一致`（`FMT-401`） | ✅ |
| 空条目合并只收尾 | `回收站里没文件的条目合并只收尾`（不打扰用户、索引与空目录都收掉） | ✅ |
| 同名多条要指定 | `同名多条回退要指定回收站名字`（`FMT-001` 并列候选） | ✅ |
| 没有身份记录 | `没有身份记录的目录只报告不回退`（`original` 为空、拒绝回退） | ✅ |
| 用户提的完整场景 | `删空桶重建再删然后回退不会互相覆盖`（两次删除名字不同；目标已存在时合并，两边数据都不丢） | ✅ |
| 无当前用户 | `没有当前用户时拒绝`（`FMT-604`） | ✅ |

「删除非当前 Bucket 后 `current_bucket` 不变」与「`repository/<user>/<bucket>` 用占位名
`user`」两处也在这批用例里断言。跨进程的端到端实测（真实 `fmt.exe` + 真实服务）见
`FMT 技术文档.md` 第 18 节。

---

# 109. File 测试

必须测试：

```text
正常上传
中文文件名
空格文件名
非法文件名
超长文件名
重复文件名
不同 Bucket 同名
不同用户同名
```

**阶段 5 的覆盖情况（提交 `188e85d`，`0ad9efc` / `5bf2c1f` / `9c3d2cb` / `711da4c` /
`0fc242b` 各补若干条；`tests/file_test.cpp` / `tests/service_test.cpp` /
`tests/server_test.cpp`——`Trash.*` 那 4 条也在 `tests/file_test.cpp` 里）**：

| 要求 | 用例 | 状态 |
|---|---|---|
| 正常上传（本地路径） | `File.本地文件暂存`、`File.入库写记录并分配file_id` | ✅ |
| 中文 / 空格文件名 | `File.从来源推断文件名`（`%E5%B7%A5%E4%BD%9C.txt` → `工作.txt`）、`File.扩展名与类型` | ✅ |
| 非法 / 超长文件名 | `validate_file_name` 的用例在 `tests/validation_test.cpp`；上传路径上由 `prepare_upload()` / `commit_upload()` 各查一次 | ✅ |
| ① 内容相同 + 文件名不同 | `File.桶内判重先看内容再看名字`（`FMT-304`「文件已存在」，消息里**没有**「修改文件名」，且点名已有的那份 `source.txt`） | ✅ |
| ② 内容相同 + 文件名相同 | `File.桶内判重先看内容再看名字`（同样 `FMT-304`、同样不提改名）；`Service.管道能上传与操作文件`（同一条命令重放 → `FMT-304`） | ✅ |
| ③ 内容不同 + 文件名相同 | `File.桶内判重先看内容再看名字`（`FMT-105`「文件名重复」，**有**「修改文件名」） | ✅ |
| ④ 内容不同 + 文件名不同 | `File.桶内判重先看内容再看名字`（`brand-new.txt` 正常入库、md5 是本次内容） | ✅ |
| **同内容只留一份、跨桶各留一份**（本次改动（未提交）） | `File.桶内判重先看内容再看名字`（桶内换名字也拒）；`File.判重不跨桶`（2 桶里同名 + 同内容照样入库，两条记录各指向自己的桶目录） | ✅ |
| 文件名推断（URL / 本地路径 / 推不出来） | `File.从来源推断文件名` | ✅ |
| 扩展名与类型映射 | `File.扩展名与类型`（`.jpg` → image、`.gz` → archive、`.bashrc` 无扩展名…） | ✅ |
| 没有当前 Bucket 时拒绝上传 | `File.没有当前Bucket时拒绝上传`（`FMT-305`） | ✅ |
| 列表与查询（按 id / 按名字） | `File.列表与查询`、`Service.管道能上传与操作文件` | ✅ |
| 软删除进回收站 | `File.软删除进回收站` | ✅ |
| 软删除也收文件名（提交 `0ad9efc`） | `File.按文件名也能软删除`：名字不存在 → `FMT-002`「文件不存在」；按名字删与按 id 删等价（同一条记录、`is_trash=true`）；再删一次 → `FMT-001` 且消息里带 `file_id`（**不是** `FMT-002`） | ✅ |
| 回收站落点用记录自己的 Bucket（提交 `0ad9efc`；本次改动（未提交）改用 `file_id` 走同一条断言） | `File.按file_id删别的桶的文件时回收站用记录自己的Bucket`：切到另一个桶按 `file_id` 删，路径里是 `/工作/` 而**不是** `/生活/` | ✅ |
| **大小写不同的同名必须被当成重名**（提交 `5bf2c1f`，**数据损坏回归**） | `File.大小写不同的同名必须被当成重名`：先传 `doc.txt` 再传 `DOC.TXT` 必须被 `FMT-105` 拒绝；断言里**先查数据完整性**（第一个文件还在、大小 == 记录里的 `size`、内容仍是 `"hello world"`）再断言拒绝——顺序刻意如此，磁盘一旦被覆盖，先炸的就是完整性断言 | ✅ |
| 按名字查询 / 删除不区分大小写（提交 `5bf2c1f`） | `File.按名字查询不区分大小写`：`get_by_name("report.txt")` 找得到 `Report.txt`，`remove("REPORT.TXT")` 可用 | ✅ |
| **与 `file_id` 同形的名字不能上传**（提交 `9c3d2cb`，保留形状 `FMT-106`） | `File.与file_id同形的名字不能上传`：**两条路都拦**——显式名（`prepare_upload(..., "fmt-20261008-0", ...)`）与从来源推断的名字（`temp/fmt-20261008-0` 走 `file_name_from_source()`）都拿到 `ErrorCode::FileNameLikeFileId`；判定函数本身的用例在 `tests/validation_test.cpp` 的 `Validation.与file_id同形的文件名被拒`（含放行反例 `fmt-20261008-0.txt` / `my-fmt-20261008-0` / `fmt-20261008` / `fmt-2026100-0`） | ✅ |
| **标识与名字同时命中时报歧义**（提交 `9c3d2cb`，只服务 `file delete`） | `File.标识与名字同时命中时报歧义`：手工造旧数据（一个文件 `file_id = fmt-20261008-0`、另一个文件 `file_name = fmt-20261008-0`）后 `remove("fmt-20261008-0")` 必须返回 `FMT-001`，消息里同时出现「歧义」与另一条的 `file_id`（`fmt-20261008-1`）；不歧义的名字照常走 | ✅ |
| **删除预检会把情况说清楚**（提交 `711da4c`） | `File.删除预检会把情况说清楚`：同桶时 `other_bucket` / `ambiguous` 都为假、`message` 为空（**CLI 不打扰用户**）；切到另一个桶后**按名字查不到**（`FMT-002`，消息里点出同名那份在 `工作`），改按 `file_id` 预检时 `other_bucket` 为真、`message` 里同时出现两个桶名；预检**不改数据**（文件仍在仓库里） | ✅ |
| **`file get` 回预览 / 下载链接**（本次改动（未提交），第 42 节） | `Service.管道能上传与操作文件`、`Server.File路由与上传`、`CliE2e.核心链路走真实exe与真实管道`：`preview_url` / `download_url` 的形状与 `http://localhost:4122/api/file/<file_id>/…` 完全一致，且**不看 HTTP 服务开关** | ✅ |
| **`share create` 收文件名 + 回完整分享链接**（本次改动（未提交），第 47 节） | `Service.分享的创建查看列出撤销与计数`：按 `file_id` 与按文件名都能创建、`share_url` 形状正确、按文件名列出（`count` 命中）、名字不存在 → `FMT-002`；单元用例 `CliE2e.分享能按文件名创建并打印完整链接`：真实 exe 里 `share create first.txt` 打印「分享链接：http://localhost:4122/api/share/<12 位>/download」，`share get` / `share list` 也带链接；`fixture.cli({"share", "create", "nope.txt"})` → 退出码 3 | ✅ |
| **`file get` 大写 `file_id` 也查得到**（提交 `6a40742`） | `File.列表与查询` 追加断言：把 `file_id` 全大写后 `get_by_id()` 仍命中（标识比较一律 `iequals`，9.4） | ✅ |
| **粘贴路径里的不可见字符会被清掉**（提交 `a9af276`，**复刻用户报的场景**） | `File.粘贴路径里的不可见字符会被清掉`：中文目录 `头像/` 下的文件 + `U+202A` / `U+202C` 包裹路径 → 上传预检成功；Explorer 引号包裹 → 成功；仍然找不到时消息里出现 `U+202A` 与 `U+202C`；名字里的不可见字符被 `validate_file_name()` 拒绝（`FMT-101`）。**配套用例**：`tests/string_test.cpp` 的 `String.清理粘贴带进来的路径污染`（`U+202A`/`U+202C`、成对与不成对引号、首尾空白、`U+00A0`、中文路径不受影响、`invisible_characters()` 的去重与码位名），见第 33.1.1、25 节 | ✅ |
| **位置参数的信封形状**（提交 `2c841c8`，**复刻 CLI 崩溃**） | `Cli.位置参数的信封形状`（`tests/cli_test.cpp`）：旧写法（在位置参数**数组**上用字符串下标挂 `dry_run`）必须抛 `type_error.305`；`argument_envelope()` 产出的必须是**带 `argv` 且开关同级**的对象（第 127.7 节）。**这条钉的是机制本身**——测试不再自己拼形状，避免「测试全绿、CLI 一敲就崩」 | ✅ |
| **两个写者同时原子写同一个文件**（提交 `5b316b3`） | `Storage.两个写者同时写同一个文件不会互相踩`：8 线程 × 40 轮写同一个文件，断言零失败、内容必须是某一次**完整**写入、且**不留 `.tmp`**（第 11 节） | ✅ |
| **版本文本只有一个来源**（提交 `d108c80`） | `Cli.版本文本只有一个来源`（`tests/cli_test.cpp`）：断言 `version_text()` 里含 `File Manager Tool` / `v1.0` / `build`——**不钉具体日期**，因为构建日期是 CMake 配置时生成的。钉住的是「横幅 / `--version` / `version` 命令同源」这件事（第 68、127.1 节） | ✅ |
| **数据根切换提示要把两个根都说清楚**（提交 `bb7a40f`，**第一次给控制台输出加测试**） | `Cli.数据根切换提示要把两个根都说清楚`（`tests/cli_test.cpp`）：断言 `cli::root_switch_notice("D:/old","D:/new")` 里**同时**出现旧根、新根、「数据根」、「切回去」；并检查旧根为空（服务首次启动）时也包含新根。**文本收进 `root_switch_notice()` 就是为了能断言**——别把文本再写回连接路径里（第 127.1 节） | ✅ |
| **service 子命令集合**（提交 `c573f14`，**命令集合第一次有断言**） | `Cli.service子命令集合`（`tests/cli_test.cpp`）：断言 `cli::is_user_service_command()` 对 `install` / `uninstall` / `start` / `stop` / **`reinstall`** 为真，对 `status`、空串、`Install`（大小写不同）、乱写的词为假。**这个集合决定「敲了什么会被当成什么」**，所以它从匿名命名空间搬到 `cli.hpp` 里专门钉住（第 126 节） | ✅ |
| HTTP 下载入库 | `File.从HTTP下载入库`（本机起一个 httplib 服务端当地源） | ✅ |
| 端到端（管道） | `Service.管道能上传与操作文件`（upload → list → get×2 → 去重被拒 → delete → get 仍可查到；**【9c3d2cb】再追加**：删除后 `file get` 命中回收站记录时 `data.trash_path` 以 `trash/user/.files/` 开头，见第 42 节） | ✅ |
| 端到端（HTTP） | `Server.File路由与上传`（**【6a40742】追加**：`file_name = fmt-20261008-0` → 400 + `FMT-106`；**【0fc242b】追加**：`DELETE ?dry_run=1` 只读、零副作用） | ✅ |
| 端到端（真实 exe + 真实管道，提交 `a340d1e`） | **`tests/cli_e2e_test.cpp`（套件 `CliE2e`）**：进程内起 `ServerRuntime`（用 `FMT_PIPE` 私有管道名）→ 把 `fmt.exe` 复制到临时数据根 → `CreateProcessW` 拉起**真实 exe**、喂 stdin、合并收 stdout+stderr → 断言退出码与用户看到的文字。两条用例：`CliE2e.核心链路走真实exe与真实管道`（version / bucket create WORK → 小写归一 + 提示 / bucket list / file upload / FMT-304 / FMT-105 / file get 按名与按 id / FMT-002 / file delete → trash list（含 `[文件]`）→ trash get（含「回收站路径」）→ trash restore / 跨桶删除无 `--yes` → `FMT-016` 且**文件仍在**、带 `--yes` → 成功且消息带「Bucket：work」/ 永久删除无 `--yes` → `FMT-016` / 删桶：空桶成功但仍打印「只能整体恢复」、非空桶无 `--yes` → `FMT-016`）、`CliE2e.交互式确认答n不删答y才删`（**交互式确认此前完全没有自动化覆盖**：喂 `n` → 「确认执行？」+「已取消」且文件仍在；喂 `y` → 不再出现「已取消」且条目进了回收站） | ✅ |

> **为什么必须有这一层（工程教训，与 `abort()` 那次崩溃同源）**：`file delete a7.jpg`
> 弹「Debug Error! abort() has been called」那次，**单元测试全绿却没挡住**——测试直接调
> 业务层、**按服务端期望的形状拼请求**，而 CLI 拼的是另一种形状（把 `dry_run` 挂在位置参数
> **数组**上 → `type_error.305` → `abort()`，第 127.7 节）。只有让**真实 exe 走一遍用户走的路**，
> 这类回归才会当场露出来。
> **那次崩溃的代码路径对所有破坏性操作都生效**，所以本套件的第一个 `file delete` 用例
> 就会当场失败——不是碰巧覆盖到的。
> **耗时**：全量约 17 秒（端到端部分约 5 秒）；这个套件会拉起子进程，比纯单元测试慢，
> 但仍在十几秒量级。`FMT 技术文档.md` 第 13.9.1、17.2、18.35 节。

| 不同 Bucket / 不同用户同名 | **作用域已按「当前用户 + 当前 Bucket」实现**（本次改动（未提交） 收窄，第 37、38 节）：同一个文件可以在两个桶里各存一份（`File.判重不跨桶`），按名字删/查不再够到别的桶（`File.判重不跨桶时按名字删除只作用于当前桶`），跨桶只能用 `file_id`（`File.按file_id删别的桶的文件时回收站用记录自己的Bucket` 钉删除落点、`File.删除预检会把情况说清楚` 与 `Service.破坏性操作先预检再确认` 钉「跨桶缺 `force` → `FMT-016`」）；**桶侧的大小写另有一条**：`Bucket.大小写不同也认得同一个桶`（提交 `5bf2c1f`）。**文件级 trash 的用例已在提交 `0fc242b` 补上**（`Trash.文件级条目能列出并回退` 等 4 条，第 112 节）；「不同用户同名」仍无用例 | ✅ |

> **名字长得像 `file_id` 的「先查 id 遮住」问题：提交 `9c3d2cb` 已定稿并落地**
> （第 25、43 节）：① 上传时按保留形状拒绝 → `FMT-106 FileNameLikeFileId`（用例
> `Validation.与file_id同形的文件名被拒`、`File.与file_id同形的名字不能上传`）；
> ② 旧数据里已经存在的这种名字，`file delete` 在两条索引命中**不同**记录时报
> `FMT-001` 歧义（用例 `File.标识与名字同时命中时报歧义`）。原来记在第 122 节
> 「比较与定位」三条待决里的这一条**已从待决清单移出**；那三条里现在只剩
> 「按名字删除可能跨 Bucket」一条——**该条已在本次改动（未提交） 定稿**：按名字只作用于
> 当前 Bucket（别的桶里的同名不再命中，报 FMT-002 并说清它在哪个桶），跨桶只能用
> `file_id`（并且要 `force` 确认）。

---

# 110. MD5 测试

必须测试：

```text
相同文件
不同文件
不同文件名 + 相同 MD5
相同文件名 + 不同 MD5
```

预期：

```text
相同 MD5 → 去重
同名不同内容 → 冲突
```

**实现与用例（提交 `188e85d`）**：

```text
Hash.MD5已有向量          对已知向量断言（含空串 d41d8cd98f00b204e9800998ecf8427e）
Hash.分块与一次算结果一致 同一份数据「分块累加 update()」与 md5_hex() 一次算必须同结果
                          —— 这条直接保护「边下载边算」的正确性
File.本地文件暂存         upload.md5 == md5_hex("hello world")
File.桶内判重先看内容再看名字  ① 内容同 / 名字不同 → FMT-304（不提改名）
                              ② 内容同 / 名字相同 → FMT-304（不提改名）
                              ③ 内容不同 / 名字相同 → FMT-105（提改名）
                              ④ 内容不同 / 名字不同 → 正常入库
```

「内容相同（名字随意）」与「内容不同但同名」两条就是上面那四行里的 ①② 与 ③；
`md5_hex()` 供测试与小数据使用，上传路径走增量接口（第 36 节）。

---

# 111. Upload 异常测试

必须测试：

```text
404
URL 错误
网络中断
下载超时
文件超过最大大小
MD5 重复
文件名冲突
磁盘空间不足
metadata 写入失败
```

**阶段 5 的覆盖情况（提交 `188e85d`）**：

```text
404 / 网络中断 / 下载超时     覆盖手段是 File.从HTTP下载入库（正例）+ 失败分支；
                              代码里对应 FMT-301 DownloadFailed / FMT-302 DownloadTimeout
                              与 status != 200 两个分支
                              【a2b6cd1】另有 HttpClient.* 6 条直测 http_client：
                              非2xx不交给调用方 / 中止下载 / 连接失败与协议校验 /
                              https会真的做TLS握手 / 真实https下载可选
                              （FMT 技术文档.md 第 17.2.1 节）
URL 错误（协议不支持）        File.暂存失败会清理临时文件 的中间一段：
                              【a2b6cd1 起】ftp://example.com/a.bin → FMT-300 UrlInvalid
                              （原文是 https://example.com/a.bin；https 现在是合法来源）
文件超过最大大小               File.暂存失败会清理临时文件：上限设 5 字节、
                              源文件 11 字节 → FMT-303 SizeLimitExceeded
本地文件不存在                 File.暂存失败会清理临时文件 → FMT-002 FileNotFound
内容相同（当前桶）             File.桶内判重先看内容再看名字 → FMT-304（①② 两条口径）
文件名重复（内容不同）         File.桶内判重先看内容再看名字 → FMT-105（退出码 4）
没有当前 Bucket                File.没有当前Bucket时拒绝上传 → FMT-305
「临时文件必须被清理」         File.暂存失败会清理临时文件：三种失败之后断言
                              temp/ 里常规文件数 == 0（不留垃圾）
磁盘空间不足 / metadata 写入失败   没有专门用例（要制造写失败）；实现里
                              metadata 写失败走第 40 节回滚，代码路径存在但未被测试覆盖
```

---

# 112. Trash 测试

必须测试：

```text
删除文件
恢复文件
恢复冲突
永久删除
删除 Bucket（目录名一律带时间戳；.original 有一条记录）
恢复 Bucket（目标不存在 → 整棵树一次搬回；**目标已存在 → 合并**：有同名文件
           FMT-403、没冲突先要确认（缺 force → FMT-016）再逐个搬，见第 56.3 节）
Bucket 部分恢复 —— 文件级口径（逐个文件；桶级不做部分恢复，见第 56 节）
Bucket 永久删除（同时从 .original 删掉身份记录）
.original 损坏 / 写不进去（删除拒绝、目录搬回原位）
trash list 的两类异常如实报告：present=false、original 为空
```

**桶级已落地部分对应的真实用例（提交 `4fee290`，`tests/bucket_test.cpp`）**：

```text
Bucket.条目详情与永久删除         trash get 的六个字段 + trash delete 的三步顺序
Bucket.永久删除只清桶级记录       trash_reason="file" 的记录一条都不能少
Bucket.回收站扫描只认桶级条目     点开头跳过 + <名字>_<14 位时间戳> 形状检查
Bucket.有索引没目录的条目可以永久删掉   幽灵条目也要能删掉（否则永远清不掉）
Bucket.大小写不同也认得同一个桶   【5bf2c1f】create("work") 后 use("WORK")，
                                list() 的「当前」标记必须落在实际的 work 目录上
                                （**提交 9c3d2cb 改口径**：current_bucket 里存的
                                  不再是用户敲的 "WORK"，而是磁盘上的实际名字 "work"，
                                  见下两条与第 29 节）
Bucket.创建时大写会转成小写      【9c3d2cb】create("WORK") → BucketCreation{requested=
                                "WORK", name="work", renamed=true, became_current=true}，
                                config.current_bucket == "work"，**磁盘上的实际条目名
                                是小写 "work"**（注意：不能用 directory_exists 判断大小写，
                                那在 Windows 上恒为真，用例是遍历目录比对 entry 名）；
                                再敲一次 create("WORK") → FMT-201（同一个桶）
Bucket.use大写规范化到磁盘上的名字 【9c3d2cb】create("work") 后 use("WORK")，
                                config.current_bucket == "work"（不是 "WORK"），
                                get("WORK").name == "work"
PathManager.回收站保持原层级      trash/<用户>/.files/<桶>/YYYY/MM/DD/<文件>
Service.管道能执行Bucket命令      list/restore/get/delete 端到端；不带 force 被拒
                                （**提交 711da4c 起是 FMT-016 / 退出码 2**；
                                  原来记的 FMT-001 已作废），带 force 成功；
                                **【9c3d2cb】再追加 create("WORK") → data.bucket=="work"
                                且 data 里有 note（内容含 "WORK"）**；
                                **【6a40742】再追加 bucket get WORK → bucket 与 path 都是 work**；
                                **【0fc242b】再追加 bucket.delete 的非空桶预检**
Service.破坏性操作先预检再确认      【711da4c + 0fc242b】file.delete 跨桶的 dry_run 形状
                                （needs_confirm / bucket / current_bucket / message）；
                                无 force → FMT-016、带 force 成功且 message 带 Bucket；
                                trash.delete 的 dry_run 带 entry（files/bytes/original）；
                                bucket.delete 非空桶 → 预检 needs_confirm、
                                无 force → FMT-016、带 force 成功
Trash.文件级条目能列出并回退      【0fc242b】列表标出 type=file / id / name / bucket /
                                deleted_at；按 file_id 取到同一条；回退后
                                trash_reason 与 deleted_at 清空；列表变空
Trash.回退遇同名冲突要拦住        【0fc242b】删掉后又上传同名 → 预检 conflict（blocked）、
                                restore 返回 FMT-401（退出码 4），两份数据都还在
Trash.随桶删除的文件不能单独回退   【0fc242b】列表里只有那个桶（文件不单独列）；
                                按 file_id 查得到但 restorable=false；restore → FMT-402（退出码 3）
Trash.永久删除文件级条目          【0fc242b】预检 needs_confirm 恒真 + 条目详情；
                                预检不改数据；执行后记录一并清掉
Server.Bucket路由与状态码         【0fc242b】GET /api/trash 回 entries（type / name），
                                trash get / restore / delete 回 entry；
                                DELETE ?dry_run=1 → 200 + 预检；
                                不带 force → 400 + **FMT-016**（提交 711da4c 起）、
                                带 ?force=1 → 200
Server.File路由与上传             【6a40742】file_name = fmt-20261008-0 → 400 + FMT-106；
                                【0fc242b】DELETE ?dry_run=1 只读、零副作用
```

> 本节的「永久删除」原来指的是**文件级**（阶段 7）；现在**桶级**的永久删除已经落地，
> 上面五条用例就是它的验收；**文件级那一半也已在提交 `0fc242b` 落地**
> （上表 `Trash.永久删除文件级条目` 等 4 条，第 53.2、104 节）。

**提交 `188e85d` 之后补一句（重要，别读错覆盖范围）**：文件级条目**已经能被生产**
（`file delete` 写 `trash.json` + 搬进 `trash/<用户>/.files/…`，用例
`File.软删除进回收站`、`File.按文件名也能软删除`、`File.按名字删除用的是记录自己的Bucket`、
`Service.管道能上传与操作文件` 覆盖），
~~但**文件级的 list / get / restore / delete 一条都没实现**~~
**已在提交 `0fc242b` 关闭**：文件级的 list / get / restore / delete 全部落地
（`TrashService` 把两级合成一份列表并负责标识解析；上面的 `Trash.*` 四条用例就是它的验收，
第 52～55、59 节），`BucketService::list_trashed()` 仍只扫 `trash/<用户>/` 顶层并跳过
点开头的条目。所以上表里的「恢复文件」「恢复冲突」**都已有实现与用例**；
「Bucket 部分恢复 —— 文件级口径」仍**故意不做**——随桶删除的文件只能整体恢复那个桶，
单独恢复会被拒绝（第 53.2、104 节）。

---

# 113. Share 测试

必须测试：

```text
创建 Share
获取 Share
删除 Share
20 次限制
过期
次数耗尽
文件进入 Trash
文件恢复
文件永久删除
并发最后一次下载
```

**已落地部分对应的真实用例（数据面提交 `d5779db`、公开下载端点提交 `d3aeb3d`）**：

```text
Service.分享的创建查看列出撤销与计数   【tests/service_test.cpp】一条用例把
                                create / get / list / download / 撤销 / 过期 / 文件状态
                                串起来跑：
                                create   share_id 是 12 位小写十六进制；默认
                                         max_download_count = 20、download_count = 0、
                                         expire_time 非 null、is_valid；连续两次 create
                                         的 share_id 不同
                                get      可用 → available=true / state=「可用」/ 带 file_name；
                                         未知 id → FMT-500（退出码 3）
                                list     count = 2，最近创建的在前
                                download 连下 20 次都成功，**第 21 次 → FMT-502（退出码 5）**；
                                         之后 get 报 state=「下载次数已用尽」、download_count=20
                                         （这一半就是第 51 节「19 + 两次 = 21」的计数面）
                                文件状态 file.delete（带 force）后：get 报「关联文件不可用」、
                                         download → FMT-503、新的 create 也被 FMT-503 挡住
                                撤销     share.delete 后 get → FMT-500，再 delete → FMT-500
                                过期     先把文件 trash.restore 回来（否则按第 49 节的顺序
                                         拿到的是「关联文件不可用」），再把 share.json 里那条
                                         expire_time 改成 2000-01-01 → get 报「已过期」

Service.永久删除后分享如实报文件不存在  【tests/service_test.cpp】把「关联文件不可用」的
                                两种原因分开钉住，并覆盖恢复：
                                软删除   file.delete → get 仍 ok（不是「不存在」）、
                                         available=false、state=「关联文件不可用」、
                                         message 含「回收站」
                                恢复     trash.restore → get → available=true、state=「可用」
                                永久删除 再软删 + trash.delete（force=true）→ get 仍 ok、
                                         state=「关联文件不可用」、message 含「已不存在」
                                         （与「回收站」是**两种原因**，分开钉）；
                                         share.download → FMT-503（退出码 5），
                                         不是 FMT-002 / 404

Service.并发最后一次下载不会超发      【tests/service_test.cpp】第 51 节「19 + 两次 = 21」：
                                先串行下满 19 次，再**两个线程同时**请求最后一次——
                                恰好 1 个成功、1 个 FMT-502，之后 download_count == 20
                                （不是 21）、state=「下载次数已用尽」。靠
                                ServerRuntime::handle() 全程持业务锁（src/service/runtime.cpp），
                                「检查 + 计数 + 落盘」是一次完成的

Server.分享路由与公开下载             【tests/server_test.cpp，注入真实 handler】
                                POST /api/share（body {"file_id":…}）→ 200 + 12 位 share_id
                                         + max_download_count=20
                                GET /api/share?file_id=… → count 1；
                                GET /api/share/<id> → available=true / state=「可用」/
                                         file_name=share.txt / download_count=0
                                GET /api/share/<id>/download **不带 token → 200**、
                                         逐字节一致、Content-Disposition: attachment；
                                         下载后计数变 1（先记账再放行）
                                DELETE /api/share/<id> → 之后 GET → 404 + FMT-500
                                未知 share_id 的公开下载 → 404 + FMT-500（公开路径不校验
                                         token，但仍是 404 而不是 401）
                                （同一文件里 Server.未知路由返回错误信封 对 /api/share/x 的
                                 501 断言**不算**分享路由的覆盖——那条用例没注入 handler，
                                 任何命中路由都会 501）
```

**真机冒烟与客户端（端到端跑真实 exe + 真实服务，不是单元测试）**：

```text
tools/smoke-test.ps1 第 8 节   POST /api/share 建分享（share_id 12 位）；
                              GET /api/share/<id>/download **不带 token → 200**、
                              内容与源文件逐字节一致、之后 GET 该分享计数变成 1；
                              DELETE /api/share/<id> 撤销后再 GET → FMT-500
tools/smoke-test.ps1 第 7 节   CLI 侧同一条链路：create 默认 20 次 / 7 天、get 可用、
                              list 一条、download 记账 1/20、改 expire_time 后 get 报
                              「已过期」+ download 被拒 FMT-501（退出码 5）、delete 撤销、
                              下满 20 次后第 21 次 FMT-502、未知 id 退出码 3
java（另一模块，不是 tests/*.cpp）
  FmtChannelTest.downloadShareNeedsNoToken   对假服务端 StubFmtServer 断言客户端
                              downloadShare() 刻意不带 token——钉的是**客户端**，
                              不是服务端行为
```

**「必须测试」十项的覆盖对照（十项现在都有明确用例）**：

| 必须测试 | 状态 | 用例 / 现状 |
|---|---|---|
| 创建 Share | ✅ | `Service.分享的创建查看列出撤销与计数`（`tests/service_test.cpp`）+ `Server.分享路由与公开下载`（HTTP 侧，`tests/server_test.cpp`）；真机冒烟第 7、8 节同 |
| 获取 Share | ✅ | `Service.分享的创建查看列出撤销与计数`（HTTP 侧还有 `Server.分享路由与公开下载`）：可用 / 未知 id（`FMT-500`）/ 已过期 / 次数用尽 / 关联文件不可用，五种状态都钉住 |
| 删除 Share | ✅ | `Service.分享的创建查看列出撤销与计数`：撤销后 `get` 与再 `delete` 都是 `FMT-500`；HTTP 侧 `DELETE /api/share/<id>` 之后 `GET` → 404 + `FMT-500` |
| 20 次限制 | ✅ | `Service.分享的创建查看列出撤销与计数`：连下 20 次都成功，**第 21 次 → `FMT-502`（退出码 5）** |
| 过期 | ✅ | `Service.分享的创建查看列出撤销与计数`：手改 `share.json` 的 `expire_time` → state「已过期」 |
| 次数耗尽 | ✅ | `Service.分享的创建查看列出撤销与计数`：state「下载次数已用尽」、`download_count = 20` |
| 文件进入 Trash | ✅ | `Service.分享的创建查看列出撤销与计数` + `Service.永久删除后分享如实报文件不存在`：`get` 报「关联文件不可用」（message 含「回收站」）、`download` → `FMT-503`、新的 `create` 也被 `FMT-503` 挡住 |
| 文件恢复 | ✅ | `Service.永久删除后分享如实报文件不存在`：`trash.restore` 之后 `get` → `available=true`、state「可用」（**明确断言**，不再是隐含覆盖） |
| 文件永久删除 | ✅ | 同一条用例：`trash.delete`（`force=true`）之后 `get` 仍 ok、state「关联文件不可用」、message 含「已不存在」（与「回收站」两种原因分开钉），`download` → `FMT-503` |
| 并发最后一次下载 | ✅ | `Service.并发最后一次下载不会超发`：串行 19 次 + **两个线程同时**请求最后一次，恰好 1 个成功 / 1 个 `FMT-502`，`download_count == 20`（不是 21）；靠 `ServerRuntime::handle()` 全程持业务锁（`src/service/runtime.cpp`），`register_download()` 的「检查 + 计数 +1 + 落盘」是一次完成的 |

---

# 114. 数据一致性测试

人为制造：

```text
metadata 有，文件没有
```

以及：

```text
文件有，metadata 没有
```

以及：

```text
size 不一致
MD5 不一致
```

验证系统：

```text
能够发现
不会静默覆盖
不会静默删除
```

---

# 115. Service 测试

必须测试：

```text
第一次安装（全新环境 install → 服务存在、开机自启、Recovery 已配置）
重复安装（已存在 → FMT-600 / 退出码 8，不重复创建、不覆盖 binPath）
未安装时 start（FMT-601 / 退出码 8）
未安装时 stop（FMT-601 / 退出码 8）
uninstall 之后再 start（FMT-601 / 退出码 8）
install → start → stop → uninstall 全流程
service status（运行中 → 退出码 0，打印状态 / 宿主 / 数据根 / 错误码：0）
service status 不弹 UAC、不需要管理员权限（非管理员账户下也能成功）
service status 未安装（打印「服务状态：未安装」，退出码 8 / FMT-601）
service status 已安装未运行（打印「已停止」，退出码 0）
提权（四条动作命令各弹一次 UAC，输出与第 126 节样例一致；status 不弹 UAC）
用户取消 UAC（ERROR_CANCELLED 1223 → FMT-004 / 退出码 5，CLI 继续循环不退出）
提权等待超时（FMT-602 / 退出码 8）
提权副本不新开控制台窗口，结果写进结果文件 <数据根>\temp\fmt-elev-<父进程 pid>.json 回到原窗口打印
Service 启动时清理 temp/ 下遗留的 fmt-* 文件（用户手放的其它文件不动），删除数量记一行 INFO
（服务宿主 exe 已丢失 → reinstall 一次 UAC 完成）
ServiceMain 初始化失败 → dwServiceSpecificExitCode 带 FMT 编号，
  CLI 打印「服务启动失败：FMT-008 配置错误」；
  命中数据根/配置类错误码（FMT-005/006/007/008/009/011/013/014）时**不触发重装**
服务启动失败（其它错误码）→ 已安装未运行时提权 reinstall 一次
等待落定按 dwWaitHint 自适应（不写死时长）：
  正常启动（START_PENDING 短）→ 很快结束等待，不空转到上限
  人为让 ServiceMain 初始化变慢（sleep 或填大目录）→ 等待随之变长，**不误判为启动失败**
  dwWaitHint 为 0 / 异常值 → 退回下限 100 ms，不忙轮询
  确认状态一旦不是等待类就立即结束等待（不等满间隔、不等满 30 秒）
  慢启动场景下**不弹 UAC、不触发 reinstall**（这是本项要防的回归）
query_status() 的契约：
  未安装 → 正常结果 state=NotInstalled（**不返回错误**），命令层才决定 FMT-601 / 退出码 8
  已安装未运行 → 正常结果 state=Stopped
  打不开服务控制管理器 → FMT-602 / 退出码 8
  Win32 状态码不认识 → state=Unknown，不猜测、不崩溃
命令行退出码与文本两套通道可用：脚本读退出码即可判断「服务在不在」
开机自启（重启电脑后服务自动运行，横幅显示 Service Running...）
Service Recovery（异常退出后按 5 秒 / 10 秒 / 30 秒重启，失败计数 1 天重置）
uninstall 后 repository / trash / data / config / log / temp 仍在
换目录声明新数据根（复制 exe 到新目录双击 → 新根完成初始化，旧根数据保留，
  hello 回执 switched=true 且 CLI **在 stderr 打印换根提示**（`root_switch_notice()`，提交 `8f2fbc5`）
并记一行日志「数据根切换：旧 -> 新」（**原口径「只进日志，不刷控制台」已作废**，第 127.1 节））
CLI 双击时对自己数据根做幂等体检与补齐（首次日志里有「数据根新建目录 / 数据根新建文件」，
  第二次只有「数据根完整」；**两种情况控制台都不打印这些行**）
CLI 体检发现损坏 JSON（FMT-006）→ 日志记「数据根损坏（未自动修复）：…」，
  **stderr** 出现「数据根文件损坏（未自动修复）：…」，不重置、不覆盖原文件
CLI 体检不触碰已有内容（预置同名目录与自写 JSON 后双击，内容与时间戳不变）
数据根无法补齐（只读目录等）→ stderr 出现「数据根无法补齐：FMT-013 …」且不阻断后续流程
help 命令（`help` 列命令总览、`help service` 看详情、`help 未知组` → stderr + 退出码 2、
  交互式与一次性都支持；不提权、不连服务、不写日志）
横幅与 --version 同一串 `File Manager Tool  v1.0  ( build  <日期> )`；重跑 CMake 后日期变化
控制台整洁：双击后 stdout 只有横幅 + 状态行 + 提示符（外加需要提权时的四行），无「建了什么」
数据根不可写（exe 放在只读位置）→ 提权结果文件退回 %TEMP% 并写一行 WARN，命令仍能完成
单实例（再次双击 → 激活已有窗口，不新建第二个窗口）
服务宿主 exe 被移动或删除 → 提示重新安装服务
HandlerEx 不响应 pause（服务不声明 SERVICE_ACCEPT_PAUSE_CONTINUE）
```

---

# 116. HTTP 测试

测试：

```text
正常请求
错误参数
404
下载
Share
过期 Share
Trash 文件
Preview
```

---

# 117. 性能原则

V1 不追求极限性能。

但是：

```text
不能一次性把整个大文件加载进内存
```

文件下载采用：

```text
stream
```

文件上传下载尽量使用：

```text
buffer
```

控制内存占用。

---

# 118. 安全原则

外部 URL 属于不可信输入。

必须防止：

```text
路径穿越
非法文件名
恶意 URL
异常 HTTP Response
超大文件
```

用户输入不能直接作为系统路径执行。

**阶段 4 已落地的部分**：`common/validation` 就是这条原则的第一个执行者（第 25、26 节）：

```text
用户输入 ../test.txt   → validate_file_name  → FMT-102 FileNameSeparator（拒绝）
用户输入 test/name.txt → validate_file_name  → FMT-102 FileNameSeparator（拒绝）
用户输入 CON / con.txt → validate_*          → FMT-103 / FMT-202（拒绝）
用户输入 a:b.txt       → validate_file_name  → FMT-101 FileNameInvalidChar（拒绝）
Bucket 名 ../../etc    → validate_bucket_name → FMT-202（拒绝）
```

落盘路径一律由 `PathManager` + `path_from_utf8` 生成（第 19、20 节），业务代码不拿用户
输入直接拼窄字符串路径——否则 MSVC 的 `std::filesystem::path` 会按 ANSI 代码页解释，
中文 Bucket 名会静默变成乱码目录（`FMT 技术文档.md` 第 18.14 节的纪律第 1、2 条）。

HTTP 侧多一道输入：**路径参数里的中文会被客户端百分号编码**，服务端必须先用
`url_decode`（`common/string`）解码再当业务参数用，绝不把 `%E5%B7%A5` 当成 Bucket 名
（第 82 节与 `FMT 技术文档.md` 第 12.3.2 节）。

---

# 119. 数据安全原则

任何涉及用户文件的操作：

```text
删除
恢复
覆盖
永久删除
```

必须明确。

FMT 不允许：

```text
静默覆盖
静默改名
静默删除
静默清空 metadata
```

---

# 120. V1 不实现的功能

明确暂不实现：

```text
分块上传
断点续传
取消上传
复杂用户认证
权限系统
文件夹系统
文件逻辑对象
物理对象引用计数
复杂搜索引擎
数据库
复杂 API 鉴权
```

> **`HTTPS` 已从这份清单里删掉（提交 `a2b6cd1`）**：上传来源的 `https://` **已经支持**
> （第 33 节，WinHTTP + Schannel，不引 OpenSSL、不分发 DLL），不再属于「不实现」。
> 仍然只有 HTTP 的是**浏览器入口的监听协议**（`server.json` 的 `host` / `port`），
> 那是服务端 TLS（证书配置、端口、浏览器信任链）的事情，与下载来源无关，
> 如果要写进清单，应当写成「浏览器入口的 HTTPS 监听」而不是笼统的 `HTTPS`。

后续版本根据实际需求增加。

**不在本次范围（原 `FMT 重构设计.md` 第 1 节，该文档已删除）**：

```text
pause                本次确认不需要：服务不声明 SERVICE_ACCEPT_PAUSE_CONTINUE，
                     HandlerEx 只处理 STOP / SHUTDOWN / INTERROGATE（第 69、70 节）。
                     不做「暂停服务」这条命令，也不预留它。
HTTP 客户端 CLI      CLI 不再走 HTTP：改走命名管道直连服务，HTTP 只留给浏览器。
                     理由是「HTTP 可关闭、端口可被占」——CLI 不应因为浏览器入口关着
                     或 4122 被占用而整条命令链路失效（第 76 节、
                     `FMT 技术文档.md` 第 12.1 节）。早期设计的「CLI 是 HTTP 客户端、
                     与服务通过 localhost:4122 通信」**已作废**。
多用户与权限系统      V1 没有用户系统：current_user 用占位名 user（第 122 节规则 20），
                     不做登录、不做用户管理命令、不做按用户隔离的权限判定；
                     文件与桶的名字作用域仍是「同用户」这一层。
                     浏览器侧只做**单账号 + token 认证**（那把 token 就是凭证），
                     不是多用户体系；复杂用户认证与权限系统留在上面那份「不实现」清单里。
```

---

# 121. 后续扩展方向

未来可以增加：

```text
V2
├── 大文件分块上传
├── 断点续传
├── 上传取消
├── HTTP 上传
├── HTTP Range
├── 浏览器入口的 HTTPS 监听（下载侧的 https 已在 V1 支持，第 33 节）
├── 用户系统
├── 权限系统
└── 更完善的 Preview
```

> **清单已改（提交 `a2b6cd1`）**：原文这里列的是笼统的 `HTTPS`。**下载来源的 https
> 在 V1 就已经支持了**，所以它不该再挂在「未来扩展」里；真正留到 V2 的是**浏览器入口
> 自己用 HTTPS 监听**（要处理证书、端口与浏览器信任链，与下载来源无关）。

---

# 122. 开发中的冻结规则

以下规则在 V1 开发期间视为核心规则：

```text
1. Bucket 没有独立 bucket_id
2. File 使用 file_id
3. Share 使用 share_id
4. File 使用 MD5 去重
5. 同用户正常文件名唯一
6. Trash 不占用正常文件名空间
7. file_id 不因删除/恢复改变
8. Bucket 删除不创建 file_id
9. Bucket 下文件通过 is_trash 管理；进回收站时同时写 trash_reason
   （"bucket" = 桶被删、"file" = 文件自己删的，阶段 5 起），回退桶只翻回 "bucket" 的那些
10. 冲突恢复不覆盖、不改名 —— 这是**文件级**恢复的规则；
    桶级回退是**整单搬回**（目标不在）或**整单合并**（目标已存在：有同名文件
    一个都不搬 → FMT-403；没冲突先确认再搬，见第 56.3 节），不做部分恢复
11. current_bucket 无效时不自动选择
12. max_download_count 属于 Share
13. Share 默认最大下载次数为 20
14. File 本身不保存下载限制
15. JSON 损坏不能静默重置
16. 关键文件操作必须具备事务式处理
17. Windows Service V1 保持简单（六条子命令 + SCM + Recovery，不自建 watchdog）
18. CLI（命名管道）与 HTTP 使用统一业务核心
19. 单一 fmt.exe，三种形态：CLI / Service / 提权短命副本
20. manifest 为 asInvoker，绝不 requireAdministrator
21. service 有 install / uninstall / start / stop / **reinstall** / status 六条子命令（提交 `c573f14` 起 `reinstall` 从引导内部用法变成正式命令），没有 pause、没有 delete
22. service 的动作命令（install / uninstall / start / stop / **reinstall**）一律走 UAC 提权，
    不做免提权优化；status 是查询命令，不提权、不弹 UAC
23. CLI 走命名管道，HTTP 只给浏览器（CLI 不走 HTTP）
24. 数据根由 CLI 声明，服务维护当前数据根，切换不删旧数据；
    hello 响应回填 switched / previous_root，CLI 据此**在 stderr 打印换根提示**（`root_switch_notice()`，提交 `8f2fbc5`）并记一行日志「数据根切换：旧 -> 新」
25. 初始化由 Service 与 CLI 共用同一套幂等规则（ensure_root/check_root）：
    Service 在启动/换根时执行，CLI 在双击时对自己的数据根执行；两边都只补缺失、
    都不碰业务数据内容；已有 JSON 读一遍确认，损坏只报告不重置
26. 服务自身状态写在 %ProgramData%\FMT\service.json，不属于业务数据
27. 同时只有一个 CLI 窗口，因此同时只有一个数据根
28. 服务宿主为首次安装时注册的 exe 绝对路径，移动请用复制
29. ServiceMain 初始化失败时把 FMT 编号写进 dwServiceSpecificExitCode（不是退出码），
    CLI 通过统一查询接口读回并打印「服务启动失败：FMT-008 配置错误」
30. 双击引导：数据根体检与补齐 → 查 SCM → 已安装未运行则提权 start 并等它落定 →
    仍没起就读失败编号；命中数据根/配置类错误码集合（FMT-005/006/007/008/009/011/013/014）
    不重装，其余才提权 reinstall 一次
31. service status 未安装时打印「服务状态：未安装」并返回 FMT-601 / 退出码 8；
    已安装未运行时照常打印、退出码 0
32. 服务状态与失败编号统一走 service::query_status() / query_state() / last_start_failure()；
    「未安装」是正常结果（state = NotInstalled），不是错误——只有查询本身失败才是 FMT-602
33. 「等它落定」不写死时长：按 SCM 的 dwWaitHint 自适应，夹在 100 ms – 2000 ms，
    兜底上限 30 秒，状态一旦不是等待类就立即结束
34. V1 不做结构化输出：没有 --json，也不预留参数名；机器可读通道是命令退出码，
    人类可读通道是固定顺序的那几行文本
35. 程序横幅名固定 File Manager Tool，版本部分 v<MAJOR>.<MINOR>（当前 v1.0），
    构建日期由 CMake 配置时生成（FMT_BUILD_DATE，%Y.%m.%d 本地时间）；
    横幅与 --version 共用 banner_text()，源码里不得硬编码版本号或日期；
    用法标题是「用法：fmt.exe [命令]」
36. 控制台只留交互：双击时数据根体检的结果（新建目录 / 新建文件 / 完整）与服务当前状态
    只进日志；只有异常（无法补齐、文件损坏）走 stderr
37. help 是正式命令：help 列命令总览（只列命令、不加描述），help <组> 看详情，
    支持 service / bucket / file / share / trash / help / exit；help <未知组> →
    stderr 一行 + FMT-001 / 退出码 2；exit / quit 也是正式命令
38. current_user 默认为占位名 user：数据根初始化时若为空就置 user 并保存，
    不需要用户先设置；FMT-604 保留给「用户被显式清空」（第 18、93 节）
39. 两条入口一套参数：管道 op = "<组>.<动作>"（bucket.create …），位置参数放 args.argv；
    HTTP 请求体接受 {"name":…} 或 {"argv":[…]}，路径参数里的中文由服务端 url_decode 解码
40. 错误码 → HTTP 状态码映射已冻结：400 / 403 / 404 / 409 / 500 五档，
    实现是 http_status_for() 的 switch + default: 500；新错误码未登记就落 500，不猜 4xx
41. Bucket 删除是移入回收站、不是丢弃：整个目录移到 trash/<user>/<bucket>_<YYYYMMDDHHMMSS>/，
    目录名**一律**带删除时间戳（同秒冲突加 _2），桶级身份记录写进
    trash/<user>/.original（trashed / original / deleted_at，无 file_id）；
    data/trash.json 只服务文件级条目，不再记桶级记录；
    回退是整单搬回（目标不在）或整单合并（目标已存在：有同名文件 FMT-403、
    没冲突先确认再搬），绝不靠剥离时间戳猜原名（第 17.2、30、56 节）
42. 并发：管道连接级严格串行（一次只 accept 一条连接，没有每连接一个线程），
    HTTP 是线程池并发，业务命令共用运行体的一把互斥锁（是互斥，不是队列：
    没有先来先服务、没有优先级、没有排队上限与排队超时）；HTTP 的 stop / start 必须在锁外做；
    阶段 5 的上传/下载必须先把锁粒度问题解决（第 84 节 ④）
    —— **已在提交 `188e85d` 落地**：上传两段式，下载在锁外（prepare_upload）、
    登记在锁内（commit_upload），长任务不持业务锁（第 84 节 ④、第 101 节）
43. Bucket 名称统一小写（提交 `9c3d2cb`）：create 先 to_lower() 再校验再建目录
    （WORK 建成 work，renamed 为真时回 note 提示用户）；use / get / delete 用
    canonical_name() 规范化到**磁盘上的实际名字**，current_bucket、file.json 的 bucket、
    .original 的 original 只留一份拼写（第 27、28、29、30 节）
44. 文件名与 file_id 同形（fmt-YYYYMMDD-N）是**保留形状**（提交 `9c3d2cb`）：
    上传时按 FMT-106 FileNameLikeFileId 拒绝（退出码 2，属 FMT-1xx 文件名校验，
    判定 looks_like_file_id()，显式名与推断名都拦）；旧数据里已有的这种名字，
    file delete 在两条索引命中**不同**记录时报 FMT-001 说清歧义并点名两条记录
    ——只给 delete 加，file get 的两种查询范围保持不变（第 25、42、43 节）
45. iequals() 只折叠 ASCII（提交 `9c3d2cb`）：>= 0x80 的字节原样比较，不交给
    std::tolower（setlocale 一被调用就会改坏 UTF-8 名字）。这是「比较一律不区分
    大小写」那条口径的实现约束；file get 命中回收站记录时另回 trash_path
    （相对数据根、正斜杠），仓库里找不到时不回 path（第 42 节）。
    提交 6a40742 补齐：get_by_id() 也用 iequals（标识比较一律不区分大小写）
46. 破坏性操作先检查、说清楚、再确认（提交 `711da4c`）：file.delete / trash.delete /
    bucket.delete 支持 args.dry_run = true（HTTP ?dry_run=1），**只读、零副作用**；
    CLI 先打印预检、再问 y/N（一次性命令要 --yes），**用户同意之前不发任何破坏性
    请求**。歧义 / 同名冲突 / 随桶删除 / 数据缺失是 blocked（y/N 解决不了，
    只能让用户改用 file_id）；需要确认的操作缺 force → **FMT-016 ConfirmRequired**
    （退出码 2、HTTP 400，不再复用 FMT-001）。预检负责「说清楚」，服务端的 force
    校验负责「兜底」（第 43、82、127.7 节）
47. 回收站是一份两级视图（提交 `0fc242b`）：src/trash/ 的 TrashService 组合
    FileService 与 BucketService，合并 TrashEntry 列表 + 跨命名空间标识解析
    （① 回收站目录名 → ② file_id → ③ 桶原名 → ④ 文件名；③④ 多条 → 候选）。
    权威：文件级 = file.json（is_trash / trash_reason / deleted_at），
    桶级 = trash/<user>/.original；**data/trash.json 不再写入**（只读兼容）。
    trash list 标出 [文件] / [桶]；回退的三种硬拒绝（FMT-401 / FMT-402 / FMT-002）
    都是 blocked（第 52～55、59 节）。
    提交 18f16ca：桶级条目的 files/bytes 在列表里就对每个 present 条目遍历一次目录算出来
    （trash list 因此不是纯索引查询）；「不要继续」的退出码按预检自身错误码原样透出
48. 粘贴污染要清掉、不可见字符不许进名字（提交 `a9af276`）：
    位置参数在 argument() 一处收口过 clean_user_path()（CLI 与 HTTP 共用），
    上传来源与显式文件名在 prepare_upload() 再清一次；清掉的是不可见格式字符
    （U+00A0 / U+00AD / U+200B–U+200F / U+202A–U+202E / U+2060–U+2064 /
     U+2066–U+2069 / U+FEFF）与**成对**引号（`"` / `'` / `“”`），首尾空白也去。
    清掉之后仍找不到 → FMT-002，消息**点名**被清掉的码位（如 U+202A、U+202C）；
    只去了引号/空白时另有一句说明。**名字里不允许**这些字符：
    validate_file_name → FMT-101、validate_bucket_name → FMT-202（第 25、33.1.1 节）
49. 请求形状必须由**同一个构造函数**产出（提交 `2c841c8`）：
    cli::argument_envelope(positional, dry_run, force) 一处产出「位置参数放 argv、
    开关放同级」的信封，预检与真实请求都用它；测试**不许自己拼形状**
    （旧写法在数组上挂 dry_run 会抛 type_error.305 → 未捕获即 abort()，
     用户敲 file delete a7.jpg 弹出的 Debug Error 就是它）。
    用例 Cli.位置参数的信封形状 钉住机制本身（第 127.7 节）
50. 进程间共写的两处纪律（提交 `5b316b3`）：
    ① temp/ 的启动清理**只清十分钟以前**的 fmt-* 文件——提权结果文件的临时文件
       也叫 fmt-elev-<pid>.json(.tmp)，`service install` 会在同一次操作里启动服务，
       一律清掉会让父进程报「FMT-602 提权副本没有返回结果」（**服务其实装好了**，
       用户看到的是假失败）；正在回传的结果寿命只有几十毫秒（第 5、126 节）
    ② 原子写的临时名必须**每个进程、每次调用都不同**：<目标>.<pid>.<序号>.tmp。
       固定 <目标>.tmp 时两个写者（安装器与服务启动都写 service.json）会互相踩：
       先完成的一方把 .tmp rename 走，另一方读回校验时报「无法打开文件」，
       而调用方忽略了返回值 → service.json 静默不更新。另外进程内加互斥、
       MoveFileExW 遇到 ACCESS_DENIED / SHARING_VIOLATION / LOCK_VIOLATION
       （以及 FILE_NOT_FOUND）**短暂重试** 40 次 × 5 ms（第 11 节）
51. TrashEntry.present 的语义写死为「**数据在不在磁盘上**」（提交 `5b316b3` 文件级、
    提交 `8f0fd5c` 补齐桶级）：回退/永久删除的**结果条目不翻转它**——恢复成功后数据在
    仓库里，它就是 true；拿它当「还在不在回收站」会打印「状态：数据已不存在」这种
    误导信息。结果由 message 说明。**两级、restore/purge 四种结果都不翻转**，
    用例在 Service.管道能执行回收站命令（桶级）与 Trash.文件级条目能列出并回退 /
    Trash.永久删除文件级条目（文件级）里各有一条 present 断言（第 53.1、53.3 节）
52. HTTP 兜底路由要分清「没这个接口」与「服务器坏了」（提交 `4ddb515`）：
    已知模块下没有这个接口 → **501 + FMT-602**（「接口尚未实现：<path>」，`server.*` 这个空壳属这类）；
    完全打错的 /api/... → **404 + 新错误码 FMT-017 RouteNotFound**（「没有这个接口」，
    退出码 3，只由 HTTP 兜底路由产生）；已知路由但业务找不到对象 → 404 + FMT-002。
    原口径「兜底硬编码 500 + FMT-602 操作尚未实现」已作废——500 等于说服务器坏了。
    FMT-602 的映射也从「落 default 500」改成显式 501（第 82 节）
53. **待决缺口（真机实测，2026-10-09）：`server.json` 的 `enabled` 没人打开**——
    `ServerConfig::enabled` 默认 false，`src/` 里没有任何代码置 true，安装流程不碰它，
    CLI 也没有命令能开，所以装好的服务在 `localhost:4122` 不监听（CLI 不受影响，走管道）。
    手动置 true 并重启后 HTTP 完全正常。**未定**：让安装流程置 true，还是加 CLI 命令
    （如 `config http on`）。**在用户拍板前不许写成已实现**（第 78 节、
    `FMT 技术文档.md` 第 5.2、12.1、19.1 节）
54. `version` 是正式命令，与横幅、`--version` / `-v` **同源**（提交 `d108c80`）：
    三处都走 `cli::version_text()`（内部是 `banner_text()`），输出一行
    「File Manager Tool  v1.0  ( build  <CMake 配置日期> )」；
    窗口里、`fmt.exe version`、`fmt.exe --version` / `-v` 都能用。
    **不需要服务在运行**（不连管道、不查状态）；**不写任何磁盘内容**——一次性分支在
    开日志器之前，`log/fmt.log` 不会因此多记录（与 `--help` / `--version` 同口径）。
    `help` 总览里有 `(version)  version  打印版本与构建日期` 一行，
    `help version` 有正文（第 68、127.1 节）。**原口径「窗口里敲 version → 未知命令」
    已作废**
55. 换根通知要上控制台（提交 `8f2fbc5` + `bb7a40f`，**取代**原口径「换根只进日志、
    不刷控制台」）：
    ① **切换发生的那一刻**（hello 回执 switched=true）CLI 在 **stderr** 打印一段提示，
       文本来自 `cli::root_switch_notice(previous, current)`：两个根都点名、说明原因是
       「数据根由 CLI 声明」、并给出「用数据根正确的那个 fmt.exe 再执行一条命令切回去」
       （正斜杠，不带反斜杠转义）；**同时**仍记一行日志。
    ② **交互窗口横幅区永远多一行「数据根：…」**（数据来自 `service::load_state()`），
       它与本程序所在目录不一致时再补一行「注意：本程序所在目录是 …，连上之后服务会切到
       本目录（数据根由 CLI 声明）。」——**触发条件如实说明**：双击引导会先连上服务并声明
       本目录，所以服务在跑时两边通常已经一致，这一行主要在**服务不可用**时出现。
    ③ **为什么「连接那一刻报警」就够**：服务**一次只接受一条连接**（第 15.1 节①严格串行），
       交互窗口握着管道时别的 CLI 连不上（`ERROR_PIPE_BUSY` → 重试 → `FMT-601`），
       所以「会话中途被搬走」**不可能发生**——切换只可能发生在某个 CLI 连上的那一刻。
       用例 `Cli.数据根切换提示要把两个根都说清楚` 钉住文本（控制台输出第一次有测试覆盖）
56. `service reinstall` 是正式命令（提交 `c573f14`）：`is_user_service_command()` 加入
    `reinstall`（该函数已从匿名命名空间移到 `cli.hpp`，便于测试），两处用法提示、
    命令总览、`help service` 正文都跟着更新。
    **service 子命令是六条**：install / uninstall / start / stop / **reinstall** / status，
    其中**除 `status` 外每条都提权**（原口径「文档冻结的是四条命令」「reinstall 只在引导
    流程内部使用」**已作废**）。`reinstall` = 一次 UAC 里「卸载 → 按**当前这个 exe** 重新
    注册 → 启动」，因此是**更新 exe 的正确路径**（不需要先复制、只要一次 UAC），
    也用于修复宿主 exe 被移动或删除。**三个不变**：业务数据不变
    （`uninstall()` 只 `DeleteService`，不删 `repository / trash / config / data / log / temp`）、
    `C:\ProgramData\FMT\service.json` 不变（卸载不删状态目录 → `current_root` 保留）、
    数据根仍由 CLI 声明（与宿主是谁无关）。第 126.1 节与
    `FMT 技术文档.md` 第 13.8.2 / 13.8.4 节
57. 管道名可被环境变量 `FMT_PIPE` 覆盖（提交 `a340d1e`）：`ipc::pipe_name()` 默认返回
    `kPipeName`（`\\.\pipe\fmt.control`），非空时用环境变量。使用点两处：服务端
    accept 循环（**循环外算一次**）与 CLI 的 `ensure_connected()`。
    **线上行为不变**（真服务由 SCM 启动、不带这个变量）；这是**端到端测试的隔离前提**
    ——测试要在同一台机器上再起一个进程内服务，而真服务通常正占着默认名，不换名字
    要么起不来、要么把命令打到**用户的真服务上**。安全边界不变：仍是本机同一用户范围内
    的名字覆盖，DACL / MIC（第 15.1 节那套）不改，服务侧的变量来自 SCM 环境
58. 端到端冒烟测试是**必须的一层**（提交 `a340d1e`，`tests/cli_e2e_test.cpp`）：
    进程内起 `ServerRuntime` → 把 `fmt.exe` 复制到临时数据根 → `CreateProcessW` 拉起
    **真实 exe**、喂 stdin、合并收 stdout+stderr → 断言退出码与用户看到的文字。
    **为什么**：`file delete` 弹 `abort()` 那次崩溃，单元测试全绿却没挡住——测试直接调
    业务层、自己拼请求形状，而 CLI 拼的是另一种形状；**只有真实 exe 走一遍用户走的路
    才会当场露出来**（那次崩溃对所有破坏性操作都生效，所以第一个 `file delete`
    用例就会失败）。全量约 17 秒（端到端约 5 秒），比纯单元测试慢但仍可每次跑
    （第 109 节、`FMT 技术文档.md` 第 13.9.1、17.2、18.35 节）
59. 日志要轮转（提交 `821aba3`）：`fmt.log` 超过 **5 MB** 轮转成 `fmt.log.1`
    （**只留一代**），`error.log` 同理；**0 表示不轮转**（`Logger::Options::max_log_bytes`，
    默认 5 MB）。**前置条件是「每行开-写-关」**（`append_line()`）：两个进程共用同一个
    日志文件，长期持有的 `ofstream` 既会挡住改名、也会让另一个进程继续往已改名的文件里写。
    每写 **64 行**检查一次大小（不用时间节流：按行计数既便宜又确定，测试也能预期）；
    改名失败（另一个进程正好在写）不报错，下一次再试；轮转后在新文件里写一行说明。
    打开时仍验一次可写（写空串）。**口径对齐**：`error.log` **仅 ERROR 级**——
    `logger.hpp` 原来写「WARN 也进 `error.log`」，与第 65 节冲突，被既有用例
    `Logger.写入两个文件且ERROR单独成文件` 当场抓住后改回代码、修掉注释
    （第 65 节、`FMT 技术文档.md` 第 14.2、14.6、18.36 节）
60. `trash empty` 一次清空两级（提交 `674d0b0`）：dry_run 预检回
    {files, buckets, bytes, needs_confirm, blocked:false, message}；缺 force → FMT-016；
    CLI 侧 `--yes` 或窗口答 `y`；**空回收站 needs_confirm = false、直接成功返回 0 项**。
    **每删一项都重新 list 一遍**（删掉一项后其余条目的索引/路径会变，用旧列表接着删会
    大面积失败）；**先删文件级、再删桶级**（避免「条目没了、数据还在」的孤儿）；
    单条失败**跳过并记日志**，不卡死整个清空，kMaxRounds 兜底防死循环。
    **HTTP 路由没有加**（`DELETE /api/trash`）：HTTP 入口现在按用户决定是关闭的，
    先不加，等那批「简单接口」一起做（第 52 节、`FMT 技术文档.md` 第 12.3 节）
61. `file list --sort name|size|id`（提交 `674d0b0`）：**默认 name**（不区分大小写，
    同名用 file_id 保证稳定）；size 大的在前；id 是入库顺序。
    **乱写 → FMT-001**，不静默按默认排；响应新增 `sort` 字段回显实际排序；
    CLI 侧 `--sort` 是**本地开关**（不进 argv），单独放进 `args.sort`（第 42 节）
62. `config list` / `config set max_upload_size <大小>`（提交 `674d0b0`；
    **本次改动（未提交）扩成四项 + server.json 也进 list**）：
    list 返回 current_user / current_bucket / max_upload_size / size_unit / language /
    path，**外加 config/server.json 的 server_enabled / server_host / server_port /
    server_path**（CLI 打成「网络服务 server_enabled：true」这样的行），CLI 打成标签行；
    set 可改 **max_upload_size / server_enabled / server_host / server_port**，
    其余只读 → FMT-001「只能改 max_upload_size / server_enabled / server_host /
    server_port（其余只读）：<key>」。大小写法：纯字节或
    10MB / 512KB / 1GB（1KB = 1024 字节），下限 1KB、上限 100GB；非法值 FMT-001。
    **server_\* 改完要重启服务才生效**：服务只回 `restart_required`，由 CLI 自动
    执行 `service reinstall`（已提权就直接重装，否则弹一次 UAC；取消则提示手动执行）。
    落盘走 save_config / save_server_config，**落盘失败回滚内存里的值**并返回错误（第 10 节）
63. `share` 数据面已落地（提交 `d5779db`）：`src/share/` 真实存在，
    `share create/get/list/delete` 可用，**公开下载端点已在提交 `d3aeb3d` 落地**
    （`GET /api/share/<share_id>/download`，不要 token、先记账再放行）。
    `share_id` = **12 位随机十六进制**（`BCryptGenRandom`，撞号重摇最多 16 次，
    生成失败当错误返回——绝不退化成可预测 id）；默认 **20 次 + 7 天**（两个条件
    **相互独立**）；`share get` **如实报状态**：未知 id 才是真错误 `FMT-500`，
    「已过期 / 次数用尽 / 已撤销 / 关联文件在回收站」都是**成功 + 状态**，
    不伪装成不存在；检查顺序按第 49 节：有效性 → 文件存在 → 不在回收站 →
    **过期 → 次数**（第 50 节那段流程图把次数写在过期前面，**以第 49 节为准**）；
    `expire_time` 解析失败按**已过期**处理。`share.download` 是新增 op：
    `register_download()` 在**业务锁内**一次完成「全部检查 + 计数 +1 + 落盘」，
    **计数写不进去就拒绝这次下载**。原口径「share 未实现、返回 FMT-602」**已作废**
    ——`FMT-602` 现在只对应 `server.*` 与**未实现的 HTTP 路由**（第 16、46～51 节）
64. HTTP 接口第 1、2 步（提交 `bfd89f7` / `4b812b5` / `ff237d5` / `22c3c3e`）：
    **监听 `localhost:4122`**（代码默认 `host` 从 `127.0.0.1` 改成 `localhost`；
    线上 `server.json` 已 `enabled: true`）。**所有 `/api/*` 都要 token**，唯一例外是
    `/api/ping` 与 `/api/share/<id>/download`（分享链接本身就是凭证）；缺 / 错 token →
    **401 + `FMT-018 Unauthorized`**（退出码 5）。认证**只在一处**：httplib 的
    pre-routing 钩子（不逐个路由判断——「漏给某条路由加认证」是这类代码最典型的事故），
    且**没有注入校验器时一律 401**（fail-closed）；两种请求头都收
    （`X-FMT-Token: <token>` 与 `Authorization: Bearer <token>`），比较是常量时间。
    token 从 **`config list`** 拿。**桶的 HTTP 接口在 `4b812b5` 被整体删除**（用户明确「桶不要」）：
    `/api/bucket*` → **404 + `FMT-017`**（故意不要，不是 501），「已知模块」列表里
    **没有 bucket**；**本次改动（未提交）之后**只回补了 `GET /api/bucket`（只读列表），
    **写接口仍然不做**——`/api/bucket/<其它路径>` 仍是 404 + `FMT-017`，
    `POST /api/bucket` 是 404 + `FMT-002`；同一轮还加了 `GET /api/config`（只读，
    **一律不返回 token、连 `?include_token=1` 这类开关都不提供**）与 `DELETE /api/trash/empty`
    （清空回收站：`?dry_run=1` 预检、`?force=1` 执行、有内容缺 force → 400 + `FMT-016`），
    见第 43 条决策。分享路由：`POST /api/share`（`{"file_id":…}`）、
    `GET /api/share/<share_id>`、`GET /api/share?file_id=…`、
    `DELETE /api/share/<share_id>`（都要 token）；`GET /api/share/<id>/download`
    公开但**流式下载第 3 步才做**（现在 501 + `FMT-602`）——
**提交 `d3aeb3d` 起它已实现**（流式回文件 + 先记账再放行），见第 80 节与
`FMT 技术文档.md` 第 18.40 节。
    `data/user.json` 的最终 schema 见第 18 节（**没有 buckets 字段**）。
    **仍是缺口**：安装流程不会自动打开 `enabled`（新数据根要手改配置）
    （第 18、78、79、82 节与 `FMT 技术文档.md` 第 12.3.2、18.39 节）
65. 测试隔离：只隔离管道不够（提交 `ff237d5`，真事故）：端到端交互测试用**无参数**跑真的
    `fmt.exe` → 那是**双击引导** → 引导会装 / 启动 / **重装真实的 Windows 服务**；
    `FMT_PIPE` 只隔离管道，**管不到 SCM** —— 测试因此把服务注册指向自己的临时目录，
    测试结束目录被清理，**线上服务指向不存在的文件、部署的 exe 也没了**（已修复）。
    防护：新增 **`FMT_NO_SERVICE=1`**（测试专用），引导函数开头看到它**完全不碰服务管理**；
    端到端夹具为每个子进程都设上它。**规则**：凡是要跑「用户双击也会走的入口」，
    必须显式切断它对 **SCM / 注册表 / ProgramData** 的写入能力
    （`FMT 技术文档.md` 第 13.9.1、17.2、18.35、18.39 节）
66. HTTP 第 3 步：流式上传 / 下载 / 预览 + 公开分享下载（提交 `d3aeb3d`）
    ——**HTTP 接口的最后一部分**：`POST /api/file/upload`（**请求体就是文件内容**，
    文件名来自 `?name=` 或 `Content-Disposition`；**旧的「请求体给服务端本地路径」
    那套已删除**——对远端客户端没有意义）；`GET /api/file/<id|名字>/download` 与
    `/preview` 用 `set_content_provider` **流式回**（下载 `attachment`、预览 `inline`）。
    **下载不受预览策略限制**（任何类型都能下载，猜不出类型就给
    `application/octet-stream`）；**预览策略只有一份**（`preview_content_type()`，
    不支持的 → `FMT-701` 400）。**边收边写边判上限**：超 `max_upload_size` 立刻中止
    接收并删暂存文件（`FMT-303` → 400），不是「写完再看」；暂存名
    `fmt-upload-<pid>-<序号>.tmp` 沿用 `fmt-` 前缀，启动清理能收走碎片（10 分钟年龄保护）；
    入库**只有一次移动、不二次拷贝**。新增 op **`file.upload_stream`** 与函数
    `prepare_staged_upload()` / `content_type_of()` / `preview_content_type()`。
    **分享下载公开**（`GET /api/share/<id>/download`，链接本身是凭证），且
    **必须先记账再放行**（第 50/51 节：计数写不进去就不下载，避免超发）。
    `file.get` 与 `share.download` 的响应新增 **`path`**（相对数据根）；
    **`file.list` 不加**。两个坑记在下面第 67 条（第 33、80、81、82 节、
    `FMT 技术文档.md` 第 12.3.2、18.40 节）
67. 两个真实 bug（提交 `d3aeb3d`，都属「陷阱」类）：① **ContentReader 型处理器早退
    不读请求体 → httplib 直接断开连接**，客户端拿到的是「没有响应」而不是我们精心写的
    错误码（没给文件名本该回 `FMT-100`）；修法是早退前先 `drain_reader()` 把体读干净。
    ② **下载误用了预览策略**：一开始下载与预览共用一个分支，结果 `.bin` 的**下载**
    被 `FMT-701` 挡掉——**下载与预览是两件事**，下载不受预览策略限制
    （`FMT 技术文档.md` 第 18.40 节）
68. `file list` 的搜索与分页（提交 `53ec4be`，CLI 与 HTTP 同一套业务实现）：
    `search` 对**文件名**做不区分大小写的**子串**匹配、**也匹配 file_id**（用户手里常有 id）；
    `page` 从 **1** 开始，`page_size` **缺省或 0 = 不分页**（保持老行为，只是多回字段）、
    上限 **1000**（超了 `FMT-001`）。响应新增 `total`（**命中总数，不是本页条数**）、
    `page`、`page_size`、`total_pages`，有搜索时回显 `search`；**越界页回空页**，不是错误。
    **顺序冻结为「过滤 → 排序 → 分页」**：先切片再排序、或排序不稳定，会让同一个文件
    出现在两页、另一个一页都不出现（测试遍历三页断言「不漏不重」）。
    CLI：`file list [--sort name|size|id] [--search 关键字] [--page N --page-size M]`，
    打印「匹配「jpg」共 2 个文件（第 1/2 页，本页 1 条）」。第 41 节、
    `FMT 技术文档.md` 第 10.2、12.3.2、18.41 节
69. 两个新坑（提交 `53ec4be`，都属「陷阱」类）：① **`GET /api/file` 曾经「声明了
    `?search=/?sort=/?page=/?page_size=` 却全部忽略」**——路由忘了把查询串放进 `args`，
    参数看着支持、实际无效；现已透传并加测试，且 `page_size=abc` 报 `FMT-001`
    （不能悄悄当 0）。② **`args` 不是 JSON 对象时（`null` 等）在服务端会抛异常**——
    与那次 `abort()` 同一类；新增的取值函数**先判类型、再取默认，绝不抛**
    （`FMT 技术文档.md` 第 18.41 节）
70. multipart 上传（**本次改动（未提交）**，`POST /api/file/upload` 的第二种请求体）：
    Postman 默认的 form-data、浏览器表单、Spring 的 `MultipartFile` 都是
    `multipart/form-data`，而这条路由是 `ContentReader` 型处理器、只登记了「原始字节」那条
    回调——multipart 请求会被 httplib 交给它自己的 `MultipartFormDataParser`，而那一对回调
    是**空的 `std::function`**：解析到第一个 part 的头就 `bad_function_call`，客户端拿到的是
    **空 500 + `EXCEPTION_WHAT` 头**（`routing()` 外面那层 try/catch 兜的底），既没有 FMT
    错误码、也没有信封。修法是同一个处理器里按 `request.is_multipart_form_data()` 分两支：
    multipart 支用 `reader(header, receiver)` 重载，取**第一个带 `filename` 的 part** 的正文
    写进同一份暂存文件（boundary 与 part 头一个字节都不进文件），普通文本字段读掉但**不落盘**；
    多个文件 part → `FMT-001`（不能让「传了两个只存一个」悄悄发生），没有文件 part →
    `FMT-100`（消息直说 Postman 里该把哪一行选成 File），请求体没收完 → `FMT-701` 并丢弃暂存
    数据。名字优先级 `?name=` > 请求级 `Content-Disposition` > part 的 `filename`（part 的
    名字与 `?name=` 走同一个清洗函数）。**裸字节那条路的口径与行为没变**（共用同一个 `sink`，
    只多一个「当前 part 要不要落盘」的开关）；顺手修掉一个真问题：**连接断在中途时原来会把
    半截文件当成功入库**（`reader()` 的返回值以前被丢掉），现在两条路都丢弃暂存数据并回
    `FMT-701`。新增用例 **`Server.流式上传支持multipart`**：① Postman 形态 + 下载逐字节一致、
    ② 字段混排不污染正文、③ 没有文件 part、④ 文件 part 超限、⑤ 两个文件 part、
    ⑥ `?name=` 优先于 part 名字、⑦ 手工拼的 `filename*` 中文名、⑧ 四次成功入库 / 两次失败
    一个都没进。真机冒烟（`tools/smoke-test.ps1` 第 8 节）也加了一个 multipart 探针：
    `curl.exe -F` 推一个文件 + 一个普通字段（断言 200、`file_name` 取 part 的 filename、
    大小只算文件、下载逐字节一致），再用「只有普通字段」的请求断言 `FMT-100`。
    **177 项全绿**（176 + 净 +1），见 `FMT 技术文档.md` 第 12.3.2 节
71. 上限真正接通、并从「落盘的字节」改成「**收到的字节**」（**本次改动（未提交）**，接第 70 条）：
    用户点名的两条一起收。① `HttpServer::start` 的最后一个参数从 `std::uintmax_t`（一个数）
    改成 **`UploadLimitProvider`（取值器）**：运行体注入的 lambda **每次上传请求现读**
    `context_->config.max_upload_size`（锁内只读一个字段，不干别的）——**`config set
    max_upload_size` 之后不用重启 HTTP 监听**。为什么不能只用快照：调**大**上限时 HTTP 层
    还按旧的小值中止接收，业务层根本没机会看到新值（调小只是白传一次，不会错）。在这之前
    运行体**压根没传**这个参数，HTTP 层拿到默认 `0`（= 这一层不判），所以「边收边判」只存在
    于文档与测试里，线上所有超限都是收完之后由业务层拒的。② 上限改判**收到的字节**：
    multipart 的普通字段也计入（不计的话 multipart 就成了绕过上限的口子）；HTTP 层的消息
    相应改成「请求体超过上传上限（已收 N 字节，上限 M 字节；multipart 的普通字段也算在内）」，
    与业务层那句「文件超过上传上限（N > M 字节）」区分开——**消息本身成了「这一层到底有没有
    生效」的可观测证据**。新增用例 **`Server.上传上限每次请求现取`**（同一个服务实例上把上限
    调大 → 下一次请求立刻能过；调小 → 下一次请求立刻 400）；`Server.流式上传支持multipart`
    加了「文件 10 字节 + 普通字段 5000 字节 > 4096 → `FMT-303`」与「消息来自 HTTP 层」两条
    断言；真机冒烟第 8 节也加了一条：把上限降到 1KB、用 4KB 的请求体断言 `FMT-303` 且消息
    来自 HTTP 层，再恢复 50MB。**178 项全绿**（177 + 净 +1）

```

---

# 123. 开发完成标准

FMT V1 不能只以：

```text
程序能够运行
```

作为完成标准。

必须同时满足：

```text
功能正确
+
数据一致
+
异常可处理
+
Service 稳定
+
CLI 可用
+
测试通过
```

---

# 124. V1 最终开发目标

最终用户可以通过：

```text
fmt.exe
```

完成：

```text
创建 Bucket
 ↓
选择 Bucket
 ↓
上传文件
 ↓
查询文件
 ↓
下载文件
 ↓
创建 Share
 ↓
删除文件
 ↓
进入 Trash
 ↓
恢复文件
 ↓
永久删除
```

并且 FMT 可以作为 Windows Service：

```text
开机启动
 ↓
后台运行
 ↓
提供 HTTP 服务
 ↓
支持文件查询
 ↓
支持下载
 ↓
支持预览
 ↓
支持 Share
```

整个 V1 保持：

```text
简单
稳定
快速
可维护
可扩展
```

---

# 125. 文档状态

当前：

```text
FMT 开发文档.md
```

为：

```text
V1 开发规范（`arch-restart` 重构版）
```

本文件以：

```text
FMT 项目架构.md
```

为上层设计依据；本次重构的差异说明与决策索引见第 122 节与第 128 节（`FMT 重构设计.md` 已并入本文与其他两份文档）。

本次更新已并入的冻结决策：

```text
单一 fmt.exe 三种形态（manifest 为 asInvoker）
service install / uninstall / start / stop / reinstall / status（无 pause、无 delete）
service 四条动作命令一律 UAC 提权 + 结果经结果文件 <数据根>\temp\fmt-elev-<父进程 pid>.json 回传
  （提权副本 operation 五种：install / uninstall / start / stop / reinstall）
service status 是查询命令：不提权、不弹 UAC，未安装时 FMT-601 / 退出码 8
CLI 走命名管道 \\.\pipe\fmt.control，不走 HTTP
数据根由 CLI 声明（hello 响应回填 switched / previous_root）
初始化由 Service 与 CLI 共用同一套幂等规则（ensure_root/check_root），两边都只补缺失；
  CLI 双击时对自己数据根执行体检与补齐，损坏 JSON 只报告不重置
ServiceMain 初始化失败上报 FMT 编号（dwServiceSpecificExitCode），CLI 读回并打印原因
服务状态统一走 query_status / query_state / last_start_failure；「未安装」是正常结果而非错误
等待落定按 SCM 的 dwWaitHint 自适应（100 ms – 2000 ms，兜底 30 秒），不写死时长
服务状态文件 %ProgramData%\FMT\service.json
CLI 单实例与固定界面输出（横幅、提示符、stdout / stderr）
横幅 = File Manager Tool  v1.0  ( build  <CMake 配置日期> )，与 --version 同一串
控制台只留交互与异常：数据根体检结果与服务当前状态只进日志
help 命令（总览 / 分组详情 / 未知组 → FMT-001 退出码 2）；exit / quit 是正式命令
V1 不做 --json 结构化输出，机器可读通道是退出码
开发顺序：阶段 2 = common/config/storage/core，阶段 3 = service/ipc/cli
```

**阶段 5 落地后并入的决策（提交 `188e85d`）**：

```text
file 四条命令：file upload <来源> [文件名] / file list / file get <file_id|文件名> / file delete <file_id|文件名>
          【0ad9efc：file delete 原先只收 file_id，现在与 file get 同一套定位规则；
            见下面「file delete 收两种参数后并入的决策」】
上传来源：http:// URL 或本机路径；**https 不支持**（V1 不引入 OpenSSL），返回 FMT-300
          【已作废，提交 a2b6cd1：http:// 与 https:// 都支持，走 WinHTTP + Schannel，
            不用 OpenSSL、不分发 DLL；见下一条决策组与第 33 节】
文件名推断：先砍 scheme://host、去查询串/锚点/结尾斜杠、取最后一段、再百分号解码；
            推不出来（如 http://example.com/）→ FMT-100，要用户显式给名字
上传两段式：prepare_upload() 在锁外下载（边写边算 MD5、边判大小上限、失败即删临时文件）；
            commit_upload() 在锁内完成判重（先内容后名字）/file_id/搬文件/写 file.json
            —— 长任务不持业务锁，上一轮遗留的「阶段 5 必须定锁粒度」由此落地
file_id 分配：fmt-<今天>-<当天已有 id 的最大序号 + 1>，不用「条数」，删除过的 id 不复用
桶内判重的作用域：**同用户 + 当前 Bucket + 正常文件**（本次改动（未提交） 收窄，不是
            「同用户跨 Bucket」），而且**先看内容、再看名字**：
            内容相同（名字同与不同都一样）→ FMT-304「文件已存在」，不提改名；
            内容不同 + 名字相同 → FMT-105「文件名重复」，提改名；
            内容不同 + 名字不同 → 正常入库（第 37、38 节）
存储日期由 file_id 推出：repository/<user>/<bucket>/YYYY/MM/DD/<file_name>；
            resolve_path() 先推、推不出来在桶的日期树里兜底找同名文件（多命中 → FMT-015）
MD5：Windows CNG（bcrypt），增量 Md5 + 一次性 md5_hex()，不引入第三方哈希实现
```

**阶段 5（下）落地后并入的决策（提交 `a2b6cd1`）**：

```text
下载客户端换成 WinHTTP + Schannel：新增 include/fmt/common/http_client.hpp +
  src/common/http_client.cpp；src/common/CMakeLists.txt 链 winhttp；
  src/file/CMakeLists.txt 不再链 cpp-httplib（浏览器服务端仍用它）
为什么换：cpp-httplib 的 Client 走 https 必须 OpenSSL，自己编要 Perl + NASM，
  破坏「离线可构建、只依赖 vendored 单头文件」；静态链接 OpenSSL 虽不引 DLL，
  但 /MT 与常见 /MD 静态包混 CRT 会出问题。WinHTTP 是系统组件，TLS 走 Schannel
  （系统证书库），不分发任何 DLL，还自动使用系统代理（AUTOMATIC_PROXY，
  失败退回 DEFAULT_PROXY）——访问 https 站点基本都要走代理
接口：Result<HttpDownloadResult> http_download(const HttpDownloadRequest&,
        const std::function<bool(const char*, std::size_t)>& sink)
请求：GET、跟随重定向（WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS）、
      连接/发送/接收超时默认 10s / 30s / 300s（**接收超时是单次读取的空闲超时，不是总时长**）
响应：**只有 2xx 的响应体交给 sink**；非 2xx 的响应体直接丢弃、状态码照实返回
      （错误页不该落进用户的文件）
中止：sink 返回 false = 调用方要求中止 → 返回**成功**且 aborted = true，
      被拒绝的那一块**不算收到**（bytes 不含它）
压缩：**明确要求 Accept-Encoding: identity**；若服务器仍返回非 identity 的
      Content-Encoding → FMT-301「服务器返回了 xxx 压缩内容，暂不支持」
      （压缩会让 Content-Length 与实际落盘字节对不上，也可能把压缩内容当文件存下）
错误：超时（ERROR_WINHTTP_TIMEOUT）→ FMT-302 DownloadTimeout；
      域名/连接/TLS/响应异常 → FMT-301 DownloadFailed（消息带 Win32 原因）；
      证书类失败额外读 WINHTTP_OPTION_SECURITY_FLAGS → 「根证书不受信任 /
      证书主机名不符 / 证书已过期或尚未生效」
协议：只支持 http:// 与 https://（is_remote_url()）；URL 里带用户名密码不接受
来源判定顺序（第 33.1 节）：http/https → 下载；含 "://" 但非 http/https → FMT-300；
  其余 → 本地路径，存在性不过 → FMT-002。**ftp:// 报 FMT-300 而不是 FMT-002**
上传命令超时：include/fmt/ipc/protocol.hpp 新增 kUploadTimeoutMs = 30 * 60 * 1000；
  CLI 对 file.upload 用它，其余命令仍是 kCommandTimeoutMs = 30000。超时时 CLI 打印
  「提示：等待服务响应超时。服务端可能仍在处理，稍后用 file list 确认；
   也可以查看 log/fmt.log。」——**已知边界 + 现有缓解，30 分钟上限仍可能超**
测试：每条用例开始前打印 [开始] <套件>.<名称>，输出逐行刷新
  （sync_with_stdio(false) + cout.setf(std::ios::unitbuf)），卡住时最后一行就是它；
  新增 tests/http_client_test.cpp 6 条；全套 124 项全绿
    【0ad9efc：再新增 2 条 File.* 到 126 项全绿，见下一条决策组与第 109 节；
      5bf2c1f：再新增 2 条 File.* + 1 条 Bucket.* 到 **129 项全绿**；
      9c3d2cb：再新增 5 条（Validation/File/Bucket 各见第 25、43、109、112 节），
      并把两条既有端到端用例各加一段断言，到 **134 项全绿**；
      711da4c + 6a40742：再新增 2 条（File.删除预检会把情况说清楚、
      Service.破坏性操作先预检再确认），并把若干既有用例改成新形状，到 **136 项全绿**；
      0fc242b：再新增 4 条 Trash.*（第 112 节），到 **140 项全绿**】
口径推翻：https 不支持 → 支持；「https 会把 DLL 带进产物」→ 表述不准确且不再成立；
  「http:// 下载交 httplib::Client」→ 交 WinHTTP；
  File.暂存失败会清理临时文件 的协议用例 https → ftp://
```

**`file delete` 收两种参数后并入的决策（提交 `0ad9efc`「feat(file): let file delete
take a file name too」）**：

```text
命令：file delete <file_id|文件名>（原口径「只收 file_id」已作废，第 43 节）
用户的两个疑问：① 文档里从来没解释为什么 file delete 只收 id；② file delete <文件名.txt>
  报的是「文件不存在」而不是「请输入 file_id」——两句都指同一件事：不对称 + 诊断错
为什么改：file get 两种参数都收，file delete 只收一种，开发文档通篇没写出理由，
  是对称性缺失；且按 id 单向查找必然落空 → 报 FMT-002 文件不存在，可文件明明存在，
  只是用名字称呼它。改成与 file get 同一套定位规则，用户不必先 file get 换 id
实现：私有 locate_record(records, key)（第 42、43 节同一套规则的删除侧实现——
`file get` 的「命令层」用 get_by_id() + get_by_name()，`remove()` 用这个），三步——
  ① 先当 file_id（全局唯一、形状固定 fmt-YYYYMMDD-N，先查不误伤名字）
  ② 再当文件名（当前用户 + !is_trash + 同名 + **与当前 Bucket 同名**，本次改动（未提交））
     所以**同一个桶内**名字唯一，无歧义；别的桶里的同名只写进错误消息）
  ③ 名字在、但记录 is_trash → FMT-001「该文件已经在回收站里：<名字>（file_id <id>）」
     **不能报成 FMT-002**：文件还在，只是不在正常区
  都没有 → FMT-002 FileNotFound（退出码 3）
签名：Result<FileRecord> remove(std::string_view file_id_or_name)
  （include/fmt/file/file.hpp；参数名由 file_id 改为 file_id_or_name）
不变的：命中后的两道校验（不属于当前用户 → FMT-004；按 id 命中的 is_trash → FMT-001）、
  搬文件 → 写 file.json 的顺序与回滚（**提交 0fc242b 起不再写 trash.json**，
  第 17.1、43 节）、响应 data 形状（**提交 711da4c 起增加 bucket 字段**）
回收站落点：用**记录自己的 bucket**（trash_path_of()），不是当前 Bucket——
  在桶 B 里按名字删桶 A 的文件，文件进 trash/<用户>/.files/<A 的桶名>/…（第 21、43 节）
测试：新增 File.按文件名也能软删除、File.按名字删除用的是记录自己的Bucket；
  全套 126 项全绿（这是 0ad9efc 时的数字；紧接着的 5bf2c1f 把它推到 129，
  之后 9c3d2cb → 134、711da4c → 136、0fc242b → **140**，见后面的决策组）
【0ad9efc 之后紧跟的 5bf2c1f 已把这些比较改成不区分大小写，见下一组决策】
```

**比较一律不区分大小写（提交 `5bf2c1f`「fix(file): compare names the way Windows does,
or uploads overwrite data」）——这是数据损坏修复，不是体验优化**：

```text
损坏路径（回归用例 File.大小写不同的同名必须被当成重名 先复现、后修复）：
  ① 上传 doc.txt（"hello world"，11 字节）→ 记录 fmt-<今天>-0，size = 11
  ② 再传同名不同大小写的 DOC.TXT（"different content here"，22 字节）
     旧行为：重名判定是 == 精确比较 → 放过 → 产生两条记录
             而两者在 Windows 上落到**同一个磁盘路径**
             → 第二次上传把第一个文件的字节**覆盖**了，
               而 file.json 里第一条记录还写着 size = 11 / 旧的 md5
     后果：**同一个磁盘文件被两条记录指向 + 字节被覆盖 + 元数据失真**
           （断言证据：仓库里那个文件的 file_size 期望 11、实际 22）
改了什么（iequals()，ASCII 大小写折叠）：
  src/file/file.cpp
    commit_upload()  重名判定不再区分大小写（同用户 + 正常文件 + 同名即 FMT-105，第 38 节；
                     本次改动（未提交） 起作用域收窄为当前 Bucket，且不再按 md5 判重）
    get_by_name()    按名字查（file get <名字>）不区分大小写
    locate_record()  ① file_id ② 文件名 ③ 已在回收站的名字，三处比较都不区分大小写
                     【9c3d2cb 追加 ②.5：两个索引命中**不同**记录 → FMT-001 报歧义，
                       只给 delete；同时把 iequals() 收紧成只折叠 ASCII】
  src/bucket/bucket.cpp
    list()           is_current = iequals(info.name, config_.current_bucket)
    remove()         was_current = iequals(config_.current_bucket, name)
    find_trashed()   回收站名字与「原桶名」两轮定位都不区分大小写
由此定下的三条口径：
  1. **凡按名字/标识定位，一律不区分大小写**（文件名、file_id、桶名、回收站条目名）
     ——Windows 的文件系统与路径本身就不区分大小写，用户敲 REPORT.TXT 不该得到
     「文件不存在」
  2. **名字在同一个 Bucket 内唯一（不区分大小写）**：doc.txt 与 DOC.TXT 不能共存
     ——这不是限制，是上面那起覆盖事故的根因防线
     【本次改动（未提交） 把作用域从「同用户」收窄到「当前 Bucket」，第 37、38 节】
  3. bucket use WORK（目录实际叫 work）时：current_bucket 保留用户敲的拼写，
     但 bucket list 的「当前」标记按不区分大小写匹配，bucket remove 的 was_current 同理
     【**已改口径，提交 9c3d2cb**：落盘时规范化成磁盘上的实际名字（canonical_name()），
       `use WORK` 存进 current_bucket 的就是 "work"；create 也统一转小写。
       见下面「Bucket 名称统一小写」决策组】
测试：新增 File.大小写不同的同名必须被当成重名、File.按名字查询不区分大小写、
  Bucket.大小写不同也认得同一个桶；全套 126 → **129 项全绿**
（`FMT 技术文档.md` 第 10.1、10.2.4、18.22 节）
```

**比较与定位暴露出来的三条：两条已定稿（提交 `9c3d2cb`），一条仍是待决**：

```text
① 名字长得像 file_id 时会被「先查 id」遮住 —— **已定稿，移出待决清单**
   【提交 9c3d2cb】两条可选做法**都做了**：
   a) 上传时按**保留形状**拒绝形如 fmt-YYYYMMDD-N 的文件名 → FMT-106 FileNameLikeFileId
      （退出码 2，属 FMT-1xx 文件名校验；判定 looks_like_file_id()，最短 14 字符、
       前缀 fmt- 按 ASCII 折叠、日期段恰好 8 位数字、序号段非空全数字；
       显式名与从来源推断的名字走同一道校验。第 25、38 节）
   b) 旧数据里已经存在的这种名字，file delete 在两个索引命中**不同**记录时报
      FMT-001「有歧义：…请直接用 file_id 指定要删哪一个」——**只给 delete 加**
      （locate_record() 只被 remove() 用）；file get 的两种查询范围保持不变，
      这是**有意的差异**：get 只读，最坏是把 id 命中的那条给你看；delete 不能猜
      （第 42、43 节）。file get 命中回收站记录时另回 trash_path
② 按名字删除可能删到别的 Bucket 的文件 —— **已定稿（本次改动（未提交）），移出待决清单**
   名字的作用域收窄成「当前 Bucket」（第 9.2、38、42、43 节）：人在「生活」桶里敲
   file delete a.txt 再也够不到「工作」桶里的 a.txt，报 FMT-002 并说清同名的那个在
   哪个桶；要删别的桶里的那份只能给 file_id（跨桶删除仍要 force 确认）。
   提交 `711da4c` 加的那条「跨桶删除要先说清两个桶名」继续对 file_id 路径生效，
   成功消息也照旧带 Bucket 字段
③ bucket use 的拼写规范化 —— **已定稿，移出待决清单**
   【提交 9c3d2cb】落盘时规范化到磁盘上的实际名字（canonical_name()：不区分大小写地
   扫桶目录），use WORK 存进 current_bucket 的是 "work"；get 显示的、delete 写进
   回收站目录名与 .original 的 original 也都是实际名字。create 先 to_lower() 再建目录
   （WORK → work，renamed 为真时回 note 让 CLI 提示用户）。理由：Windows 目录不区分
   大小写，WORK 与 work 本来就是同一个目录，不统一拼写会让 current_bucket、
   file.json 的 bucket、.original 的 original 各留一份，日后比对与恢复都踩坑
   （第 27、28、29、30 节）
```

**Bucket 名称统一小写 + 文件名的保留形状（提交 `9c3d2cb`「feat: lowercase bucket names,
reserve the file_id shape, report the trash path」）**：

```text
① 新错误码 FMT-106 FileNameLikeFileId（include/fmt/common/error.hpp、
   src/common/error.cpp）：退出码 2，默认消息「文件名与文件标识同形（fmt-YYYYMMDD-N），
   会与 file_id 混淆」，属 FMT-1xx 文件名校验一组；HTTP 语义上属 400 一类
   （第 82 节注明：src/server/server.cpp 的 http_status_for() 还没登记它，
    现在会落 default: 500，属待办）
② validate_file_name() 在「Windows 保留设备名」之后加这一条 → FMT-106
   判定 bool looks_like_file_id(std::string_view)（include/fmt/common/validation.hpp）
   形状：fmt-YYYYMMDD-N，最短 14 个字符（4 + 8 + 1 + 1），前缀按 ASCII 折叠比较，
         日期段恰好 8 位数字，序号段全是数字且非空
   拒绝：fmt-20261008-0 / FMT-20261008-0 / fmt-20261008-123
   放行：fmt-20261008-0.txt / my-fmt-20261008-0 / fmt-20261008（没有序号）/
         fmt-2026100-0（日期 7 位）
   理由：定位是「先按 file_id 查、查不到再按名字查」（第 42/43 节）。若一个文件就叫
         fmt-20261008-0，而另一个文件的 file_id 恰好是它，按名字提交的删除会删错对象
         ——所以这个形状是**保留形状**，与 Windows 保留设备名同类
   来源：用户要求「上传时拒绝这种名字」，同时要求实现歧义检测（②）
③ FileService::locate_record() 分两步各自查找（by_id / by_name），两个索引命中
   **不同**记录 → FMT-001 InvalidArgument，消息形如「有歧义：fmt-20261008-0 既是
   fmt-20261008-0 的文件标识，又是另一个文件的文件名（file_id fmt-20261008-1）。
   这种名字现在不允许上传；请直接用 file_id 指定要删哪一个」——只给 delete 加
④ Bucket 名称统一小写（include/fmt/bucket/bucket.hpp 新增 struct BucketCreation，
   create 返回值 Status → Result<BucketCreation>）：
   create   名称先 to_lower()（只折叠 ASCII，中文不受影响）再校验、再建目录；
            WORK 建成 work，renamed = true，调用方据此提示用户；再敲 create WORK
            会被当成同一个桶 → FMT-201
   use      落盘时规范化成磁盘上的实际名字（canonical_name()：不区分大小写地扫桶目录），
            use WORK 存进 current_bucket 的是 "work"
   get      返回的 name 也是磁盘上的实际名字（get WORK 显示 work），is_current 用 iequals
   delete   回收站目录名与 .original 里的 original 同样用磁盘实际名字，
            以后 restore 出来的目录拼写才一致
   为什么：Windows 目录不区分大小写，WORK 与 work 本来就是同一个目录；不统一拼写，
           current_bucket、file.json 的 bucket、.original 的 original 会各留一份，
           日后比对与恢复都会踩坑
   服务端：bucket.create 的 data 增加 note（仅在发生转换时出现），形如
           "note": "Bucket 名称统一使用小写：已把 WORK 转为 work"；bucket.use 的
           bucket 字段改为**规范化后的名字**，值被改过时也给 note；
           CLI 把 note 打成单独一行「提示：…」（第 76.1、127.6 节）
⑤ file get 对回收站里的记录返回 trash_path（相对数据根、正斜杠，形如
   trash/user/.files/工作/2026/10/08/test.txt）；仓库里没有该文件时**不返回 path**
   ——这是设计，不是缺失。CLI 多打一行「回收站路径：…」（第 42、127.6 节）
⑥ iequals() 改为只折叠 ASCII（src/common/string.cpp）：>= 0x80 的字节直接原样比较，
   不再交给 std::tolower（C locale 下虽是恒等，但一旦有人调 setlocale 就会把 UTF-8
   名字改坏）。这是「比较一律不区分大小写」那条口径的**实现约束**
⑦ 帮助文案（src/cli/cli.cpp，照源码抄进第 68 节）：help bucket 增加小写与按实际名字
   处理的两行；help file 的 upload 增加保留形状两行、get 改成「按文件名只查正常文件；
   按 file_id 连回收站里的也查得到（带 is_trash 与 trash_path）」、末尾补
   「名字与 file_id 的比较都不区分大小写（Windows 习惯）」
测试：129 → **134 项全绿**。新增 Validation.与file_id同形的文件名被拒、
  File.与file_id同形的名字不能上传（显式名与从来源推断的名字两条路都拦）、
  File.标识与名字同时命中时报歧义、Bucket.创建时大写会转成小写（含「磁盘上的实际
  条目名是小写」——注意不能用 directory_exists 判断大小写，那在 Windows 上恒为真）、
  Bucket.use大写规范化到磁盘上的名字；另外 Service.管道能执行Bucket命令 追加
  create WORK → bucket=work + 有 note，Service.管道能上传与操作文件 追加
  file get 回收站记录带 trash_path
（`FMT 技术文档.md` 第 9.3、9.4、10.1、10.2.4、12.3.2.1、18.23 节）
```

**预检与确认并入的决策（提交 `711da4c`「feat: check first, say what conflicts, then
confirm」，缺口收尾 `6a40742`）**：

```text
① 新错误码 FMT-016 ConfirmRequired（include/fmt/common/error.hpp、src/common/error.cpp）：
   退出码 2，默认消息「该操作需要显式确认（force）」，HTTP 400。
   通用规则：需要显式确认的操作（永久删除（两级 trash delete）、跨 Bucket 删除
   （file delete）、非空桶删除（bucket delete））在缺 force 时返回它，消息说明要确认什么。
   原口径「缺确认 → FMT-001」作废（脚本因此能区分「参数写错」与「忘了确认」）
② 预检 dry_run：args.dry_run = true（HTTP ?dry_run=1，server.cpp 的 delete_args() 解析，
   同时保留 ?force=1 与请求体 {"force":true}）
   file.delete    FileDeleteCheck：ambiguous / other_bucket / blocked / needs_confirm /
                  current_bucket / file_id / file_name / bucket / path / candidates[] / message
                  **歧义是 blocked 而不是 needs_confirm**（y 无法表达删哪一个，
                  只能让用户改用 file_id；候选两条都摊开）——这是刻意的判断
   trash.delete   TrashCheck：needs_confirm 恒为 true + 条目详情 + 「不可恢复」消息
   bucket.delete  BucketDeleteCheck：有内容才 needs_confirm（空桶不打扰用户）
③ CLI 统一流程（confirm_before_acting() / print_precheck()，第 127.7 节）：
   连服务 → 发只读预检 → 打印情况 → 交互问「确认执行？(y/N)」/ 一次性 --yes →
   同意后才带 force 发真实请求。**用户确认之前一个破坏性请求都不发**；
   预检失败就直接报预检的错；--yes 是本地开关、不进 argv；答 n → 「已取消」+ 退出码 0；
   一次性缺 --yes → 退出码 2；服务端仍独立校验 force（预检负责「说清楚」，
   force 负责「兜底」）
④ 顺带：file.delete 成功响应增加 bucket 字段，跨 Bucket 时 message 变成
   「文件已移入回收站：a.txt（Bucket：工作）」
⑤ 三个缺口（6a40742）：http_status_for 登记 FMT-106 → 400；get_by_id 改用 iequals；
   bucket.get 的 path 改用规范化后的名字
测试：134 → **136 项全绿**（新增 File.删除预检会把情况说清楚、
  Service.破坏性操作先预检再确认；另有多条既有用例追加断言）
（`FMT 技术文档.md` 第 10.1、10.2.4、10.4、11.6、11.14、11.15、12.5、18.24 节）
```

**回收站两级视图并入的决策（提交 `0fc242b`「feat(trash): make the file level readable,
and warn before a bucket goes」）**：

```text
① 新模块 src/trash/（include/fmt/trash/trash.hpp + src/trash/trash.cpp）：
   TrashService 组合 FileService 与 BucketService，合并两级视图 + 跨命名空间标识解析，
   自己不存状态。原口径「src/trash/ 是计划位置」作废
② 权威来源：文件级 = file.json（is_trash / trash_reason / **新增 deleted_at**），
   路径由 file_id 与记录推出；桶级 = trash/<user>/.original。
   **data/trash.json 不再写入**（以前只是第二份副本）：只读兼容老记录
   （legacy_deleted_at() 补 deleted_at；remove_trash_record() 在回退/永久删除时清掉）
③ 统一 TrashEntry：type / id / name / bucket / deleted_at / bytes / files / present /
   restorable / trash_path / message。响应：trash.list → {entries, count, files, buckets}；
   get/restore/delete → {entry, message?}；预检 → {needs_confirm, blocked, entry, message?}。
   **旧的 deleted_buckets 与顶层 trashed / original / files / bytes 形状作废**
④ trash list 标出 [文件] / [桶]（用户明确要求），CLI 末行「共 N 项（X 个文件、Y 个桶）」
⑤ 标识解析（不猜）：① 桶的回收站目录名（精确）→ ② file_id（查所有回收站记录）
   → ③ 桶的原名 → ④ 文件名；③④ 多条 → blocked + 候选
⑥ 回退的三种「确认解决不了」（blocked，不是 needs_confirm）：
   同名正常文件占着目标位置 → FMT-401（点名冲突那条的 file_id）；
   trash_reason == "bucket"（随桶删除）→ FMT-402（让用户整体恢复那个桶，
   这类文件不出现在 list 的文件区、但按 file_id 查得到且 restorable = false）；
   数据缺失 → FMT-002（消息给出路径）
⑦ bucket.delete 新增预检（check_remove）：有内容时报告文件数与占用，并写明
   「删除后只能整体恢复这个桶、无法只恢复其中某个文件」（是当前桶再补「会被置空」）；
   缺 force → FMT-016；空桶不打扰用户。bucket.delete 也进了「检查→提示→确认」名单
⑧ 帮助文案：help trash 改成「两类条目：文件级 [文件] 与桶级 [桶]，都会标出来」，
   list/get/restore/delete 按新行为写；help bucket 的 delete 加「桶里有文件时会先提醒…」
测试：136 → **140 项全绿**。新增 Trash.文件级条目能列出并回退、
  Trash.回退遇同名冲突要拦住、Trash.随桶删除的文件不能单独回退、
  Trash.永久删除文件级条目；File.软删除进回收站 改为断言 file.json 的
  is_trash / trash_reason / deleted_at 并断言 trash.json 保持空；
  Service.破坏性操作先预检再确认 追加 bucket.delete 非空桶预检；
  Service.管道能执行回收站命令、Server.Bucket路由与状态码、Server.File路由与上传
  改成新形状 / 追加 dry_run 断言
（`FMT 技术文档.md` 第 7.3、10.4、11.4、11.14、12.3.2.1、18.25 节）
```

**文档复核引出的两处修正（提交 `18f16ca`「fix(cli): report the precheck's own exit code,
and count files in the list」，140 项仍全绿）**：

```text
① 预检失败时的退出码：原来一次性命令的预检失败时 stderr 打真实错误码、
   进程退出码却统一走 2（「需要确认」）。现在 confirm_before_acting() 返回
   ConfirmOutcome{proceed, exit_code}，调用点直接用 outcome.exit_code：
     预检自身失败（FMT-002 等） → 预检的那个错误码（FMT-002 → 3）
     预检通信失败（FMT-601 等） → 通信错误的码（8）
     blocked / 一次性缺 --yes  → 2（FMT-001 / FMT-016）
     交互窗口答 n               → 0
   顺带：缺 --yes 的提示语改成「该操作需要确认（FMT-016）：请加 --yes，或在交互窗口里执行」
   （第 127.7 节有完整表）
② trash list 里桶级条目的 files / bytes 不再是 0：TrashService::list() 对每个
   present 的桶级条目遍历一次目录把文件数与占用算出来（第 53.1 节），
   CLI 打印成「  [桶]    工作（3 个文件，5.0KB）  ->  工作_20261008151538」；
   代价是每个桶条目多遍历一次目录，trash list 因此不是纯索引查询（显式命令，可接受）
（`FMT 技术文档.md` 第 10.4、11.14、12.3.2.1、18.26 节）
```

**全功能真机测试暴露的一修一缺口（提交 `4ddb515`「fix(server): answer unknown routes
with 404, unimplemented ones with 501」，2026-10-09 对已安装服务跑了 68 项检查）**：

```text
① 已修：未知 / 未实现的 /api 路径原来一律 500
   实测 GET /api/nosuch → 500 + FMT-602「操作尚未实现：/api/nosuch」；
   原因是兜底路由 Get(R"(/api/.*)") 里**硬编码 response.status = 500**
   现在：已知模块但没这个接口 → 501 + FMT-602「接口尚未实现：<path>」；
        完全打错的 /api/... → 404 + **新错误码 FMT-017 RouteNotFound**（退出码 3）；
        已知路由、业务找不到对象 → 404 + FMT-002（不变）
   新错误码只由 HTTP 兜底路由产生（管道没有「路由」概念）；FMT-602 的映射
   从「落 default 500」改成**显式 501**（第 82 节）
   144 项用例仍全绿（只改实现与既有用例，没有新增用例）

② 真缺口（**已收敛，只剩「安装流程不自动打开」**；提交 `22c3c3e` 之后）
   当时：服务装好、在跑，但 localhost:4122 没有监听——ServerConfig::enabled 默认 false，
   而全仓库没有代码把它置为 true（安装流程不碰 config/server.json，CLI 也没有命令能开）；
   技术文档 5.2 里那句「Service 场景下由安装流程置为 true」**没实现**；
   手动把 enabled 改成 true、重启服务后，HTTP 入口完全正常（真实端口上全通）。
   **提交 `22c3c3e` 起**：代码默认 `host` 从 `127.0.0.1` 改成 **`localhost`**，
   线上 `config/server.json` 已 `enabled: true`，真机验收通过（第 78、79 节、
   `FMT 技术文档.md` 第 5.2、12.1、18.39 节）。
   **仍是缺口**：**全新数据根**装完服务后 enabled 仍默认 false（没人自动打开），
   待决：① 安装流程置 true，还是 ② 加一条 CLI 命令（如 `config http on`）

③ 68 项检查里**通过**的部分（含 HTTP、上传、回收站、跨桶确认等）整理成一张表，
   放在 `FMT 技术文档.md` 第 18.31 节「真机测试通过的部分」——那里是这次最完整的
   端到端记录（对已安装服务实测，2026-10-09）
```

> **真机顺带确认的两点行为**（第一次真机确认，已写进对应小节）：
> ① 删除**当前** Bucket 后 `current_bucket` 被置空，之后 `file list` 报
> **`FMT-305 未设置当前 Bucket`**（退出码 3）；`trash restore` 把桶恢复回来**不会**自动设回
> 当前桶，需要再敲一次 `bucket use`（第 30、56.1 节）。
> ② `FMT-301 DownloadFailed` 的退出码是 **1**（与附录 A / 实现一致，已核对）。

**`version` 命令并入的决策（提交 `d108c80`「feat(cli): add the version command」）**：

```text
现象：窗口里敲 version 回答「未知命令」，而 fmt.exe --version 可用
      （原来只有旗标实现了）
修复：新增命令 version —— 与横幅、--version / -v **共用同一份文本**
      fmt::cli::version_text()（内部就是 banner_text()），三处不可能各说一套
三种用法（同一份输出）：
      fmt.exe version            一次性执行
      fmt.exe --version / -v     一次性执行（原有）
      fmt> version               窗口里执行（也接受 --version / -v）
      输出一行：File Manager Tool  v1.0  ( build  2026.10.09 )
                （版本取 version::MAJOR.MINOR，构建日期由 CMake 配置时生成）
两个性质：① 不需要服务在运行（不连管道、不查服务状态）；
         ② 不写任何磁盘内容——一次性分支放在「开日志器」之前，
            log/fmt.log 不会因为敲 version 多出记录
            （与 --help / --version 同口径：「从这里开始都是真的干活，才值得写日志」）
命令总览与帮助：print_command_list() 多一行
      (version)  version                打印版本与构建日期
      help <组> 的支持列表多了 version，且 help version 有正文（照源码抄，第 68 节）
测试：144 → **145 项全绿**。新增 Cli.版本文本只有一个来源
      （断言含 File Manager Tool / v1.0 / build，**不钉具体日期**——它是配置时生成的）
（`FMT 技术文档.md` 第 11.4、11.6、12.3.2、17.2、18.32 节）
```

**换根通知上控制台并入的决策（提交 `8f2fbc5`「feat(cli): say out loud when the service
data root moves」+ `bb7a40f`「test(cli): give the root notice a home and a test」）**：

```text
用户选的就是这条：数据根被别的 fmt.exe 抢走时，控制台上看不见
① 切换发生时（hello 回执 switched=true）：CLI 在 **stderr** 打印
   cli::root_switch_notice(previous, current) 的文本：
     注意：服务的数据根已切换
       原来：D:/Data/Temp/JMT/fmt
       现在：D:/Data/CLionProjects/FMT/cmake-build-debug/bin
     原因是「数据根由 CLI 声明」：谁连上服务，服务就用谁的目录。
     如果这不是你想要的，请用数据根正确的那个 fmt.exe 再执行一条命令切回去。
   （正斜杠、不带反斜杠转义）同时仍记一行日志
② 交互窗口横幅区**永远多一行**「数据根：…」（service::load_state()）；与本程序所在目录
   不一致时紧跟「注意：本程序所在目录是 …，连上之后服务会切到本目录（数据根由 CLI 声明）。」
   **触发条件如实说明**：双击引导会先连上服务并声明本目录，所以服务在跑时两边通常已经
   一致；这一行主要在**服务不可用**（引导没连上）时出现
③ 为什么「连接那一刻报警」够：服务一次只接受一条连接（第 15.1 节① 严格串行），
   交互窗口握着管道时别的 CLI 连不上（ERROR_PIPE_BUSY → 重试 → FMT-601），
   所以「会话中途被搬走」不可能发生——只需覆盖「连上那一刻」
④ 文本收进 root_switch_notice() 的原因就是**能断言**：这是控制台输出第一次有测试覆盖，
   别把文本再写回连接路径里
实测（2026-10-09，对已安装服务）：从构建目录执行 bucket list → stderr 出现
  「数据根已切换：D:/Data/Temp/JMT/fmt -> .../cmake-build-debug/bin」；
  再用部署目录的 exe 执行 → 反向那一条；交互窗口横幅多出「数据根：D:/Data/Temp/JMT/fmt」
测试：145 → **146 项全绿**。新增 Cli.数据根切换提示要把两个根都说清楚
（断言旧根、新根、「数据根」、「切回去」都在；旧根为空时也包含新根）
（`FMT 技术文档.md` 第 11.12、13.9.3、13.10、15.1、17.2、18.33 节；
  原口径「只进日志、不刷控制台」作废）
```

**`service reinstall` 正式化并入的决策（提交 `c573f14`「feat(cli): expose service reinstall」）**：

```text
事实：提权副本早就有 reinstall（src/cli/elevation.cpp）——
      一次 UAC 内「卸载（先停服务、解开旧 exe 的文件占用）→ 按**当前这个 exe**
      重新注册 → 启动」；本轮只做接线。
接线：is_user_service_command() 加入 reinstall（该函数已从匿名命名空间移出、
      在 cli.hpp 声明，便于测试）、两处用法提示、命令总览、help service 正文。
命令集：service **六条子命令** install / uninstall / start / stop / reinstall / status，
      其中**除 status 外每条都提权**。
      原口径「service 只有四条命令」「文档冻结的是四条」「reinstall 只在引导流程内部
      使用」**均已作废**。
用法行：用法：service install | uninstall | start | stop | reinstall | status
help service 新增正文（照源码抄，第 68 节）：两个用途（用新 exe 更新服务 /
      修复宿主 exe 被移动或删除）+ **业务数据一个都不动** + 宿主会变成你运行的这一份、
      数据根仍由连上来的 CLI 声明。
三个「不变」：① 业务数据不变（uninstall() 只 DeleteService，不删
      repository / trash / config / data / log / temp）；
      ② C:\ProgramData\FMT\service.json 不变（卸载不删状态目录 → current_root 保留，
         重装后数据根仍是原来那个，直到某个 CLI 连上来重新声明）；
      ③ 数据根仍由 CLI 声明，与「宿主 exe 是谁」无关。
为什么重要：服务在跑时旧宿主 exe 被占用、无法直接覆盖；reinstall 先停旧宿主解开占用，
      再把注册指向**你运行的那一份**新 exe——**不需要先复制，也只要一次 UAC**
      （用户之前要 uninstall → install 两次）。与宿主 exe 丢失的修复路径是同一件事
      （FMT 技术文档 13.8.4）。
测试：146 → **147 项全绿**。新增 Cli.service子命令集合（断言 install / uninstall /
      start / stop / reinstall 为真；status、空串、Install、乱写为假）——
      **命令集合第一次有断言**，因为它决定「敲了什么会被当成什么」
实测（2026-10-09，对已安装服务）：服务已停止时 reinstall → exit 0、服务 RUNNING、
      宿主 = 执行它的那份 exe、数据根仍是 D:/Data/Temp/JMT/fmt、桶/文件/回收站全未变；
      服务正在运行时 → exit 0、宿主 PID 2088 → 9212（换成新进程）、STATE 仍 RUNNING、
      业务命令正常
（`FMT 技术文档.md` 第 11.3、11.4、11.6、13.7、13.8.2、13.8.4、17.2、18.34 节）
```

**端到端冒烟测试并入的决策（提交 `a340d1e`「test(cli): drive the real exe over the real
pipe」）**：

```text
① 管道名可用 FMT_PIPE 覆盖（这是测试能隔离的前提）
   ipc::pipe_name()（include/fmt/ipc/pipe.hpp 声明、src/ipc/pipe.cpp 实现）：
   默认 kPipeName = \\.\pipe\fmt.control；环境变量非空时用它。
   使用点：ServerRuntime::run() 的 accept 循环（循环外算一次）、CLI ensure_connected()。
   **线上行为不变**：真服务由 SCM 启动，不会带这个变量；默认名照旧。
   **为什么留口子**：端到端测试要在同一台机器上再起一个进程内服务，而真服务通常正占着
   默认名——不换名字要么起不来，要么更糟：把命令打到用户的真服务上、动到真数据。
   安全边界不变：本机同一用户范围内的名字覆盖，DACL / MIC 那套不改（第 15.1 节），
   服务侧的变量来自 SCM。
② 新测试文件 tests/cli_e2e_test.cpp（套件 CliE2e，2 条用例）
   做法：进程内起 ServerRuntime → 把 fmt.exe 复制到临时数据根（「数据根 = CLI 所在目录」）
        → CreateProcessW 拉起真实 exe、喂 stdin、合并收 stdout+stderr
        → 断言退出码与用户看到的文字
   覆盖（逐条见第 109 节的表）：version；bucket create WORK（小写归一 + 提示）、list；
        file upload、FMT-304、FMT-105；file get 按名/按 id、FMT-002；
        file delete → trash list（含 [文件]）→ trash get（含「回收站路径」）→ trash restore；
        跨桶删除无 --yes → FMT-016 且随后确认文件仍在、带 --yes → 成功且消息带「Bucket：work」；
        永久删除无 --yes → FMT-016、带 --yes → 成功；
        删桶：空桶成功（仍打印「只能整体恢复」）、非空桶无 --yes → FMT-016；
        **交互式确认**（此前完全没有自动化覆盖）：喂 n → 「确认执行？」+「已取消」且文件仍在，
        喂 y → 不再出现「已取消」且条目进了回收站
③ 为什么要有它（工程教训）
   file delete 弹 abort() 那次崩溃，单元测试**全绿**却没挡住——测试直接调业务层、
   按服务端期望的形状拼请求，而 CLI 拼的是另一种形状。只有让真实 exe 走一遍用户走的路，
   这类回归才会当场露出来。**那次崩溃的代码路径对所有破坏性操作都生效**，
   所以本测试的第一个 file delete 用例就会当场失败。
④ 计数与耗时：147 → **150**（新增 CliE2e.核心链路走真实exe与真实管道、
   CliE2e.交互式确认答n不删答y才删、Ipc.管道名可被FMT_PIPE覆盖）；
   全量约 17 秒（端到端约 5 秒）——会拉起子进程，比纯单元测试慢，但仍在十几秒量级
实测（2026-10-09，对已安装且**正在运行**的真服务）：跑 CliE2e 前服务运行中、
   数据根 D:/Data/Temp/JMT/fmt；跑完 2/2 通过，服务仍运行中、数据根不变，
   桶 lazy、2 个文件、回收站里 a7.jpg ——**完全未变**；全量 150 项通过、17.3 秒、
   无残留进程与临时目录
（`FMT 技术文档.md` 第 13.9.1、13.9.2、13.9.3、17.2、18.35 节）
```

**日志轮转并入的决策（提交 `821aba3`「feat(logger): rotate the log file instead of
growing for ever」）**：

```text
背景：fmt.log 原来**无上限追加**（服务跑几周就一直长）。现在超过 5 MB 轮转成 fmt.log.1
      （**只留一代**），error.log 同理。
为什么现在才可能：日志原来长期持有 ofstream ✗——CLI 与服务共用同一个文件，句柄一直
      开着既会挡住改名，也会让另一个进程继续往已改名的文件里写。现在改成**每行开-写-关**
      （append_line()），改名才有可能成功。**这是轮转的前置条件，不是顺手改的。**
检查节奏：每写 **64 行**检查一次大小（kRotationCheckInterval）。不用时间节流：写入
      频率差异大，按行计数既便宜又确定，测试也能预期。改名失败（另一个进程正好在写）
      **不报错**，下一次检查再试。
轮转之后：在**新文件**里写一行说明「日志超过 N 字节，已轮转：fmt.log -> fmt.log.1」，
      用户翻日志能看到断点。
配置：Logger::Options::max_log_bytes，**0 表示不轮转**（测试与需要完整日志的场景）；
      默认 kDefaultMaxLogBytes = 5 * 1024 * 1024。
打开时：仍然验一次可写（写空串）✓——写不了要立刻报错，而不是等第一条日志静默丢掉。
顺带修掉头文件两处过时说法（与第 65 节/技术文档 14.2、14.3 对齐）：
  ① 「只有 Service 打开日志文件；CLI 用 console_only()」——早已不是这样：CLI 也往
     同一个 <数据根>/log/fmt.log 追加（「日志跟着用户敲的命令走」）。
  ② 「WARN 也进 error.log」——与第 65 节冲突（error.log **仅 ERROR 级**）。作者一开始
     照头文件改了代码，**被既有测试当场抓住**（Logger.写入两个文件且ERROR单独成文件
     断言 error.log 只有 1 行），于是改回代码、修掉头文件那张表。
测试：150 → **152 项全绿**。新增 Logger.超过上限会轮转出一代（上限 1 KB、写 400 行 →
      .1 存在、两代都非空、新文件里有「轮转」说明行）、Logger.上限为零时不轮转
      （写 200 行 → 没有 .1，200 行全在一个文件里）。
（`FMT 技术文档.md` 第 14.2、14.6、19.1、18.36 节）
```

**HTTP 第 3 步并入的决策（提交 `d3aeb3d`「feat(http): stream uploads, downloads and
previews」）**：

```text
路由表（最终形态，要 token；认证见提交 4b812b5）
  POST   /api/file/upload            **请求体就是文件内容**（流式）
  GET    /api/file                  列表（只有文件信息）
  GET    /api/file/<id|名字>        详情（响应含相对数据根的 path）
  GET    /api/file/<id|名字>/download  下载（attachment，流式）
  GET    /api/file/<id|名字>/preview   预览（inline，流式）
  DELETE /api/file/<id|名字>        软删除（?dry_run=1 / ?force=1）
  GET    /api/trash、GET /api/trash/<标识>、DELETE /api/trash/<标识>、
  POST   /api/trash/<标识>/restore
  POST   /api/share、GET /api/share/<share_id>、GET /api/share?file_id=…、
  DELETE /api/share/<share_id>
公开（**唯一不要 token**）：GET /api/share/<share_id>/download
  —— 分享链接本身就是凭证；**必须先记账再放行**（计数写不进去就不下载，避免超发）

流式上传：请求体即内容；`?name=` 或 `Content-Disposition`（含 filename*=UTF-8''…）；
  没给文件名 → FMT-100（400）；边收边写边判上限，超了立刻中止并删暂存文件
  （FMT-303 → 400）；暂存名 fmt-upload-<pid>-<序号>.tmp（沿用 fmt- 前缀，
  启动清理会收走，10 分钟年龄保护）；入库只有一次移动、不二次拷贝；
  新增 op file.upload_stream 与 prepare_staged_upload()
下载/预览：都用 set_content_provider 流式回；**下载不受预览策略限制**；
  预览策略只有一份 preview_content_type()（image/* 与文本类，其余 FMT-701 400）；
  Content-Disposition 用 filename*=UTF-8''<百分号编码>，中文名任何客户端都能落地
数据字段：file.get 与 share.download 新增 path（相对数据根）；file.list 不加
两个坑：① ContentReader 型处理器早退前必须 drain_reader()，否则 httplib 断开连接、
  客户端拿不到错误码；② 下载不能复用预览策略（`.bin` 的下载被 FMT-701 挡过）
测试：仍 **156 项全绿**——流式断言加进既有 Server.File路由与上传（没给文件名 →
  FMT-100；同形名字 → FMT-106；10KB → FMT-303 且 temp/ 无 fmt-upload-* 碎片；
  100 字节成功；下载逐字节一致且是 attachment；.bin 预览 → FMT-701）
真机验收（localhost:4122）：525 字节上传 → md5 3f39d8bc…；下载逐字节一致；
  preview 200 text/plain inline；POST /api/share → 0d7b3cf4bc2e；
  **不带 token** GET /api/share/0d7b3cf4bc2e/download → 200 且 download_count = 1；
  清理后 file list 仍 2 个、trash list 仍 1 项、temp/ 无残留
（`FMT 技术文档.md` 第 12.3.2、18.40 节）
```

**`file list` 搜索与分页并入的决策（提交 `53ec4be`「feat(file): search and pagination for
the file list」）**：

```text
search：按**文件名**不区分大小写的**子串**匹配，**也匹配 file_id**（用户手里常有 id）
page：从 1 开始；page_size：**缺省或 0 = 不分页**（老行为一次给全 + 多回字段），
      上限 1000，超了 FMT-001
响应新增：total（**命中总数，不是本页条数**）、page、page_size、total_pages；
      有搜索时回显 search；越界页回**空页**，不是错误
顺序冻结：**过滤 → 排序 → 分页**——先切片再排序、或排序不稳定，会让同一个文件出现在
      两页、另一个一页都不出现（测试遍历三页断言「不漏不重」）
CLI：file list [--sort name|size|id] [--search 关键字] [--page N --page-size M]
     打印「匹配「jpg」共 2 个文件（第 1/2 页，本页 1 条）」；
     --search / -s、--page、--page-size 都是本地开关（不进 argv）
两个坑：① GET /api/file 曾经**声明了 ?search=/?sort=/?page=/?page_size= 却全部忽略**
      （路由忘了把查询串放进 args）：参数看着支持、实际无效；已透传并加测试，
      page_size=abc → FMT-001，不悄悄当 0
      ② **args 不是 JSON 对象时（null 等）服务端会抛异常**（与 abort() 那次同类）：
      新增取值函数先判类型、再取默认，绝不抛
测试：156 → **157 项全绿**（新增 Service.文件列表的搜索与分页，净 +1）
真机验收（localhost:4122）：?search=jpg&page=1&page_size=1 → total 2 / count 1 /
      page_size 1 / total_pages 2；?page_size=abc → 400 FMT-001「page_size 必须是整数」；
      ?sort=size&page_size=2 → 3.71MB 的在前；
      CLI --search jpg → 「匹配「jpg」共 2 个文件」；
      CLI --page 9 --page-size 1 → 「共 2 个文件（第 9/2 页，本页 0 条）」，成功退出
（第 41、68、69 条与 `FMT 技术文档.md` 第 10.2、12.3.2、18.41 节）
```

> **实测风险 → 已被提交 `8f2fbc5` 处理（2026-10-09）**：做 `version` 测试时在**构建目录**里
> 跑了交互模式，CLI 按设计把**自己所在目录**声明为数据根，于是服务的数据根被切到了构建目录
> （日志里是 `[Main] 数据根切换: D:/Data/CLionProjects/FMT/cmake-build-debug/bin
> -> D:/Data/Temp/JMT/fmt`，随后又用部署目录的 exe 发命令切了回来）。
> 当时这件事**只进日志、控制台上看不见**；用户随后选了这一项并实现——**现在两者都上控制台**：
> 切换的那一刻 stderr 打印 `root_switch_notice()`，交互窗口横幅永远显示「数据根：…」
> （第 127.1 节、规则 55）。**原口径「只进日志、不刷控制台」已作废。**

**粘贴污染并入的决策（提交 `a9af276`「fix(upload): strip what paste adds, and name it
when it hurts」）**：

```text
用户报的现象：fmt> file upload ‪C:\...\头像\asdva.jpg → FMT-002 本地文件不存在，
而文件确实存在（130184 字节）。原因是路径首尾各夹了一个不可见的格式字符：
开头 U+202A（LEFT-TO-RIGHT EMBEDDING）、结尾 U+202C（POP DIRECTIONAL FORMATTING）
——从聊天窗口、网页、终端复制路径时常见，屏幕上完全看不出来。

① 清理函数（include/fmt/common/string.hpp / src/common/string.cpp）：
   clean_user_path(text)          清掉不可见格式字符 + **成对**引号 + 首尾空白
   invisible_characters(text)     按出现顺序去重返回码位名（{"U+202A","U+202C"}）
   清掉的码位：U+00A0、U+00AD、U+200B–U+200F、U+202A–U+202E、U+2060–U+2064、
   U+2066–U+2069、U+FEFF；引号 `"` / `'` / `“”` **成对**才去（不成对不动）
② 生效位置（一处收口 + 一处纵深防御）：
   src/service/commands.cpp 的 argument()  —— **所有**位置参数读取的唯一入口，
   CLI 与 HTTP 共用；prepare_upload() 再清一次来源与显式文件名
③ 报错口径：清掉后仍找不到 → FMT-002，消息点名码位：「本地文件不存在：<清理后>
   （你粘贴的路径里有不可见字符 U+202A、U+202C，它会让路径对不上；已自动清掉，
   请检查路径是否还有别的问题）」；只去了引号/空白 → 「（已去掉粘贴带进来的引号或空白）」
④ 名字规则：不可见字符**不允许**进名字——validate_file_name() → FMT-101、
   validate_bucket_name() → FMT-202。理由：屏幕上看不出来、没法重新敲一遍，
   按名查找/排序/日志也全对不上
测试：140 → **142 项全绿**。新增 String.清理粘贴带进来的路径污染、
  File.粘贴路径里的不可见字符会被清掉（复刻用户场景：中文目录 + U+202A/U+202C 包裹）
（`FMT 技术文档.md` 第 9.1、9.3、10.2、11.14、12.3.2.1、18.27 节）
```

**实测暴露的三处修复并入的决策（提交 `2c841c8`「fix(cli): build the precheck arguments
as an envelope, not on the array」+ `5b316b3`「fix: stop the state file and the temp sweep
from fighting each other」+ `8f0fd5c`「fix(trash): stop the bucket results from claiming
the data is gone」）**：

```text
① CLI 崩溃：开关挂到了位置参数数组上（2c841c8）
   现象：fmt> file delete a7.jpg → 「Debug Error! abort() has been called」
   原因：check.args = arguments; check.args["dry_run"] = true;
        arguments 是位置参数**数组**，nlohmann 对数组用字符串下标抛 type_error.305，
        没人接 → abort()。服务端期望的是开关与 argv **同级**：
        {"argv":["a7.jpg"], "dry_run":true}
   修复：新增 fmt::cli::argument_envelope(positional, dry_run, force)（cli.hpp / cli.cpp），
        **预检与真实请求都用它**，两者因此不可能各错一处
   教训：单元测试原来**自己照着服务端期望的形状拼请求**，CLI 拼的是另一种形状——
        测试全绿、CLI 一敲就崩。规则定成「形状必须由同一个构造函数产出，
        测试不许自己拼」；用例 Cli.位置参数的信封形状 直接钉住机制
        （旧写法必须抛 type_error.305、新写法必须是带 argv 且开关同级的对象）
② 提权结果文件被 temp 清理误删 → 假失败 FMT-602（5b316b3）
   实测：提权副本报「执行成功」，紧接着「结果文件写入失败：无法打开
        …temp\fmt-elev-2296.json.tmp」，同时 [Service] 清理了 temp/ 里 1 个文件，
        最后 CLI 报「FMT-602 提权副本没有返回结果」——**服务其实装好并启动了**
   原因：service install 会在同一次操作里启动服务，而服务启动时清理 temp/ 里所有
        fmt-* 文件；提权结果文件的临时文件正好叫 fmt-elev-<pid>.json(.tmp)
   修复：clean_temp_directory() **只清十分钟以前**的（结果文件寿命只有几十毫秒），
        读不到时间戳的也不动（第 5、126 节）
③ 原子写的临时文件重名 → service.json 静默不更新（5b316b3）
   原因：write_text_file_atomic() 固定用 <目标>.tmp，而同一个目标有两个写者
        （安装器在启动服务后写 service.json，服务启动时也写它）；先完成的一方把
        .tmp rename 走，另一方读回校验时报「无法打开文件 …tmp」；安装器那处是
        (void)save_state(updated)（忽略返回值）→ service.json 时间戳一直停在旧值
   修复：临时名带 pid + 序号（<目标>.<pid>.<n>.tmp）、进程内对原子写加互斥、
        MoveFileExW 对 ACCESS_DENIED / SHARING_VIOLATION / LOCK_VIOLATION
        （及 FILE_NOT_FOUND）短暂重试（最多 40 次 × 5 ms）
   用例：Storage.两个写者同时写同一个文件不会互相踩（8 线程 × 40 轮，零失败、
        内容必须是某一次完整写入、不留 .tmp）
④ 回收站恢复结果里的误导字段（5b316b3 文件级；`8f0fd5c` 补齐桶级）
   现象：trash restore 成功后打印「状态：数据已不存在」，而数据刚被搬回仓库
   原因：TrashService::restore() / purge() 把返回条目的 present 改成 false
        （本意是「已经不在回收站里了」），但 present 的语义是**数据在不在磁盘上**
   修复：5b316b3 改掉文件级两处，8f0fd5c 再删掉桶级两处（`src/trash/trash.cpp`
        的 restore / purge 里的 `done.present = false`）——**两级、restore/purge
        四种结果都不再翻转它**，结果由 message 说明；语义写死在 53.1 / 53.3 的字段表里。
        测试补上断言：Service.管道能执行回收站命令（桶级 restore 与 purge 各一条）、
        Trash.文件级条目能列出并回退 / Trash.永久删除文件级条目（文件级各一条），
        **计数仍是 144**（只加断言，没有新增用例）
测试：142 → **144 项全绿**（2c841c8 新增 Cli.位置参数的信封形状 到 143、
  5b316b3 再新增 Storage.两个写者同时写同一个文件不会互相踩 到 144；
  8f0fd5c 只补断言，仍是 144；
  改动：Service.启动时清理temp里的遗留临时文件 现在是「新的留着、把时间拨回
  一小时后的旧的清掉、用户手放的其它文件始终不动」）
（`FMT 技术文档.md` 第 5.1、6.6、11.15、13.8.3、18.28、18.29、18.30 节）
```

**提交 `8f0fd5c` 之后对着已安装的服务实测（已核实）**：

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

顺带在真机上确认了两件之前只写在文档里的行为：**空桶删除不打扰用户**
（`needs_confirm = false`，`--yes` 多余但无害），以及 `bucket delete` 成功后消息里
带「**（之后只能整体恢复这个桶）**」。

**已知限制（如实记录；`188e85d` / `a2b6cd1` / `0fc242b` 逐轮复核）**：

```text
~~文件级回收站只有「写」没有「读」~~ **已在提交 0fc242b 关闭**：
  文件级条目现在能列出、回退、永久删除（TrashService 合成两级视图，
  trash list 标出 [文件] / [桶]；第 52～55、59 节）
~~share 整组未实现（第 105 节）~~ **已落地**（数据面 `d5779db`、公开下载端点 `d3aeb3d`）：命令总览里 `(share)` 早已进「可用命令」组，`server.*` 是唯一还剩的业务空壳
Download（HTTP 浏览器侧 GET /file/download/<文件名>）属阶段 6
永久删除不清理 share.json：阶段 7 的收尾项——share 模块本身已落地，只是 purge() 现在还
  没有任何联动清理动作（第 60 节）——**这是阶段 7 现在唯一的缺口**
file.upload 的 30 分钟命令超时残余风险：上限到了仍可能出现「用户看到超时、
  服务端其实还在下载甚至已经入库」；CLI 会额外提示「服务端可能仍在处理」（第 19.1 节）
【已删除】「https 不支持」不再是限制（提交 a2b6cd1，第 33 节）
```

> **自查用**：本文件里不应再出现这些与实现冲突的说法——
> 「file.* 返回 FMT-602」「https 不支持 / 需要 OpenSSL / 会引入 DLL」
> 「ftp:// 报 FMT-002」「上传受 30 秒超时且无缓解」
> 「上传持锁 / 上传期间其他命令一起等（作为现状）」
> 「需要用户先给文件名」「文件级回收站已可用」
> 「`file delete` 只接受 `file_id`」「`file delete` 按 id 单向查找，找不到就 FMT-002」
> 「名字存在但已在回收站报 FMT-002」。出现它们的地方都已在上面注明作废或改口径
> （最后三条自提交 `0ad9efc` 起成立，见第 43 节）。
>
> **提交 `9c3d2cb` 之后再补五条**（这五条同样不该再出现）：
> 「`current_bucket` 保留用户敲的拼写 / `bucket use WORK` 存 `WORK`」
> 「Bucket 名称大小写不敏感地各存一份」「文件名可以是 `fmt-YYYYMMDD-N`」
> 「`file get` 按名字能查到回收站里的记录」「`file get` 命中回收站记录时只有
> `is_trash` / `trash_reason`、没有 `trash_path`」。现在的事实是：Bucket 名称统一小写
> 并规范化到磁盘上的实际名字（第 27～30 节）；`fmt-YYYYMMDD-N` 是保留形状（第 25 节）；
> `file get` 按名字**只查正常文件**、按 `file_id` 才连回收站一起查，且回收站记录带
> `trash_path`（第 42 节）；`iequals()` 只折叠 ASCII（第 43 节）。
>
> **后续三轮（`6a40742` / `711da4c` / `0fc242b`）再补六条**（同样不该再出现）：
> 「`FMT-106` 没登记、HTTP 上传同形名会落 500」（已登记 → 400，第 82 节）；
> 「`get_by_id` 用精确比较、与 delete 不一致」（现在一律 `iequals`，第 42 节）；
> 「`bucket get WORK` 打印 `repository/user/WORK`」（`path` 也走规范化名字，第 127.6 节）；
> 「永久删除缺 `force` → `FMT-001`」（现在是 **`FMT-016`**，第 59、82 节）；
> 「跨桶删除静默执行 / 按名字只在当前桶里找」（现在是预检 + 确认，第 43、127.7 节）；
> 「`trash.json` 记文件级条目」「`trash list` 只有桶级条目」「文件级条目要等阶段 7」
> 「删除桶不提醒」「`src/trash/` 不存在」（分别见第 17.1、52、53、30、4 节）。
>
> **测试计数走过的台阶**：118（`188e85d`）→ 124（`a2b6cd1`）→ 126（`0ad9efc`）→
> 129（`5bf2c1f`）→ 134（`9c3d2cb`）→ 136（`711da4c`）→ 140（`0fc242b`）→
> 142（`a9af276`）→ 143（`2c841c8`）→ 144（`5b316b3`）→ 144（`8f0fd5c` 只补断言，
> 计数不变）→ 145（`d108c80`）→ 146（`bb7a40f`）→ 147（`c573f14`）→ 150（`a340d1e`）→
> 152（`821aba3`）→ 153（`674d0b0`）→ 154（`d5779db`）→ 156（`bfd89f7` / `4b812b5`，
> `ff237d5` 只修测试隔离不新增用例）→ 157（`53ec4be`）→ 158（`64eccce`：
> `data/user.json` 改成单用户文件，`App.已有空users文件时也要补建默认账号` 拆成
> `App.老的多用户文件会被改写成单用户形状` 与 `App.老的空users文件要补建单用户账号`）→
> **159（本次改动（未提交）：新增 `Server.配置桶列表与清空回收站路由`；并把
> `Server.管理接口要token且桶路由已下线` 改名为 `Server.管理接口要token且桶写接口未提供`）**
> → **162（本次改动（未提交）再新增三条分享用例：
> `Service.永久删除后分享如实报文件不存在` / `Service.并发最后一次下载不会超发` /
> `Server.分享路由与公开下载`；实测 `fmt_tests.exe` → 「共 162 项：通过 162，失败 0」，
> `Service.` 19 项、`Server.` 8 项全绿，`ctest` 100%）**。
> 用例条数只维护在上面这条台阶链里（当前 **162**；第 109、112 节是按覆盖范围列用例、
> 不写条数）。

后续开发过程中，如果发现：

```text
实际 Windows 文件系统限制
C++ 实现限制
HTTP 实现限制
并发问题
数据一致性问题
```

导致当前设计需要调整，应：

```text
发现问题
 ↓
修改设计文档
 ↓
确认
 ↓
修改代码
```

而不是：

```text
直接修改代码
 ↓
让代码逐渐偏离设计
```

最终目标是：

> **让 FMT 的代码实现、项目架构和开发文档始终保持一致。**

---

# 126. 提权流程

## 126.1 `service reinstall` 的语义（提交 `c573f14` 起是正式命令）

`reinstall` **本来就有**（提权副本的 operation，双击引导内部用它），提交 `c573f14` 只是
把它**接线给用户**：加进 `is_user_service_command()`、两处用法提示、命令总览与
`help service`。它的实现是一次 UAC 里做完「卸载 → 按**当前这个 exe** 重新注册 → 启动」：

```cpp
} else if (operation == "reinstall") {
    const Status removed = service::uninstall();          // 未安装也当作可继续
    const bool removable = ok(removed) || error_of(removed)->code == ErrorCode::ServiceNotInstalled;
    status = removable ? service::install(path_to_utf8(executable_path()), true) : removed;
}
```

**三个「不变」（这是 `reinstall` 最值得记住的部分）**：

```text
① 业务数据不变   uninstall() 只 DeleteService，**明确不删** repository / trash / config /
                 data / log / temp（service.cpp 里那句注释就是这条口径），
                 所以桶、文件、回收站、配置全都原样
② C:\ProgramData\FMT\service.json 不变   卸载不删状态目录，所以 current_root 保留——
                 重装之后数据根**仍是原来那个**，直到某个 CLI 连上来重新声明
③ 数据根仍由 CLI 声明   与「宿主 exe 是谁」无关；重装完打开窗口会看到横幅上的
                 「数据根：…」（提交 8f2fbc5），换根提示照旧在连上那一刻报
```

**为什么这是「更新 exe」的正确路径**（用户之前要 `uninstall` → `install` 两次 UAC）：

```text
服务在跑时，旧宿主 exe 被占用 → 直接覆盖会失败（文件被锁）。
reinstall 先停掉已注册的宿主（解开占用），再把服务注册指向**你运行的那一份** exe
并启动——所以**不需要先把新 exe 复制过去**，也**只要一次 UAC**。
宿主 exe 被移动或删除时同理：它按现在这份 exe 重新注册（与第 4.8 节
「服务宿主 exe 已丢失」的修复路径是同一件事，`FMT 技术文档.md` 第 13.8.4 节）。
```

service 命令里的**四条动作命令**（`install` / `uninstall` / `start` / `stop`）**每条都走 UAC 提权**，
即使目标状态已经满足也照常弹 UAC，不做免提权优化。提权副本额外支持一个组合操作
`reinstall`（= 先卸载再安装并启动，服务未安装时忽略卸载阶段的错误），用于「服务宿主 exe
已丢失、需要重新指向当前目录」的场景，**一次 UAC 做完**。

**第五个命令 `service status` 不走提权流程**：它是查询命令，只读、不改变任何状态，
因此不需要管理员权限，也不弹 UAC、不生成结果文件；它直接 `OpenSCManagerW` +
`OpenServiceW(SERVICE_QUERY_STATUS)` + `QueryServiceStatusEx` + `QueryServiceConfigW` 后打印
（见第 70 节）。本节剩下的内容只描述那四条动作命令与 `reinstall`。

## 126.1 判断是否已提权

```text
OpenProcessToken
 ↓
GetTokenInformation(TokenElevation)
 ↓
已提权 → 直接执行 SCM 操作
未提权 → 进入提权流程
```

## 126.2 提权方式

命令行形状（已定稿）：

```text
结果文件：<数据根>\temp\fmt-elev-<父进程 pid>.json
          固定名、跟着 exe 走，用户一眼能找到、随时可清（见第 5 节）
          数据根不可写时退回 %TEMP%\fmt-elev-<父进程 pid>.json，并记一行 WARN
命令行  ：fmt.exe --elevated <operation> --result "<结果文件的绝对路径>"
operation ∈ install | uninstall | start | stop | reinstall
```

```text
1. 先确保 <数据根>\temp\ 存在（建不出来 → 退回 %TEMP% + 一行 WARN），
   再删除可能残留的结果文件 <数据根>\temp\fmt-elev-<父进程 pid>.json
2. 打印「需要管理员权限」        ← FMT-603 AdminRequired
   打印「正在提权...」
   ↓
   ShellExecuteExW(
       lpVerb = L"runas",
       lpFile = self.c_str(),        ← self 必须是具名变量，见下
       fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI,
       nShow  = SW_HIDE)
   ↓
   WaitForSingleObject(hProcess, 60000)
   ↓
   GetExitCodeProcess → CloseHandle
3. 读结果文件（读完即删除）→ 按 {ok, code, message, exit} 打印
```

**`SHELLEXECUTEINFOW::lpFile` 必须指向具名变量**：写成
`info.lpFile = path_from_utf8(self).c_str();` 会指向一个语句结束就析构的临时 `std::wstring`，
`ShellExecuteExW` 拿到悬垂指针，实测表现为 **Win32 1155 `ERROR_NO_ASSOCIATION`**
（「没有应用程序与此操作的指定文件有关联」），看不出是提权的问题。先
`const std::wstring self = executable_path();` 再取 `self.c_str()`；`lpParameters` 同理。

失败分类（与 `FMT 技术文档.md` 13.8.3 一致）：

| 失败点 | 表现 | 错误码 / 退出码 |
| --- | --- | --- |
| 用户点 UAC 的「否」 | `ShellExecuteExW` 失败，`ERROR_CANCELLED`（1223） | `FMT-004` / **5** |
| 非管理员 / 策略禁止提权 | `ShellExecuteExW` 失败，`ERROR_ACCESS_DENIED`（5） | `FMT-603` / **5** |
| `ShellExecuteExW` 其它失败 | 其它 `GetLastError()` | `FMT-602` / **8** |
| 提权副本卡住 | `WAIT_TIMEOUT`（60 秒） | `FMT-602` / **8** |
| 结果文件不存在（子进程崩了） | 读文件失败 | `FMT-602` / **8**，「提权副本没有返回结果」 |
| **结果文件被 temp 清理误删**（提交 `5b316b3` 已修） | 提权副本报「执行成功」，父进程却读不到结果 | `FMT-602` / **8**——**假失败**，见下 |

> **`FMT-602` 的一个已修成因（提交 `5b316b3`，照着实测日志记）**：

```text
[Elevated] 提权副本执行成功：install（错误码 0）
[Elevated] 结果文件写入失败：无法打开文件：D:\...\temp\fmt-elev-2296.json.tmp
[Service]  清理 temp/ 中 1 个遗留临时文件
[Cli]      service install 提权失败：FMT-602 提权副本没有返回结果（退出码 1）
```

> **服务其实装好并启动了，用户看到的却是失败。** 原因：`service install` 会在同一次操作里
> **启动服务**，而服务启动时清理 `temp/` 里所有 `fmt-*` 文件——提权副本回传结果的临时文件
> 正好叫 `fmt-elev-<pid>.json(.tmp)`，**也被清掉了**。
> **修复**（`src/service/runtime.cpp` 的 `clean_temp_directory()`）：**只清十分钟以前**的
> （正在回传的结果文件寿命只有几十毫秒），读不到时间戳的也不动。
> 详见第 5 节与 `FMT 技术文档.md` 第 13.8.3 节。

`runas` 启动的控制台程序会**另开一个控制台窗口**，所以提权副本必须：

```text
以 SW_HIDE 无窗口启动
只做 SCM 操作（operation ∈ install / uninstall / start / stop / reinstall），短命，不进入命令循环
结果 {ok, code, message, exit}
    写进结果文件 <数据根>\temp\fmt-elev-<父进程 pid>.json（父进程 pid 命名，并发双击不打架）
    命令行为 fmt.exe --elevated <operation> --result "<结果文件的绝对路径>"
由父进程读回该文件并打印（读完即删除）
```

结果文件内容：

```json
{ "ok": true,  "code": "FMT-000", "message": "成功",     "exit": 0 }
{ "ok": false, "code": "FMT-601", "message": "服务未安装", "exit": 8 }
```

**为什么不用命名管道回传**：提权副本是高完整性进程，高完整性进程创建的命名管道带高完整性
标签，而父进程（非提权 CLI，中完整性）受 MIC「禁止向上写」限制，**连接和读取都会被拒**
（与 `FMT 技术文档.md` 13.9.2 的「坑 2」同一机制，见 13.8.3）。结果文件写在数据根下的
`temp/` 里（同一个用户、只是令牌不同），父子两边都能正常读写，不需要放宽任何安全描述符；
只有数据根不可写时才退回用户自己的 `%TEMP%`，并记一行 WARN。

提权副本在写结果文件前会**自己确保 `temp/` 存在**：它有权限，所以调用方数据根只读时它仍能建出来。
Service 启动时会清理 `temp/` 下遗留的 `fmt-*` 文件（只清固定前缀的那些，用户手放的其它文件不动），
删除数量记一行 INFO。

这是「只留一个窗口」的前提：整条命令始终只有一个可见控制台。

## 126.3 五种 operation 各自何时提权

| 命令 | 提权时机 | 提权后执行 |
| --- | --- | --- |
| `service install` | 命中该命令即提权，不检查服务是否已存在 | `CreateServiceW` + Recovery + `StartServiceW` |
| `service uninstall` | 命中该命令即提权，不检查服务是否在运行 | `ControlService(STOP)` + `DeleteService` |
| `service start` | 命中该命令即提权，不检查服务是否已在运行 | `StartServiceW`，随后 `settle_state()` 按 `dwWaitHint` 等落定（第 70 节） |
| `service stop` | 命中该命令即提权，不检查服务是否已停止 | `ControlService(SERVICE_CONTROL_STOP)`，随后 `settle_state()` 等落定 |
| `service reinstall` | 命中该命令即提权（宿主 exe 已丢失时由引导流程触发） | `OpenServiceW` 失败（`ERROR_SERVICE_DOES_NOT_EXIST`）则**忽略** → `DeleteService`（若存在）→ `CreateServiceW` + Recovery → `StartServiceW`，**一次 UAC 完成** |
| `service status` | **从不提权**（它是查询命令，不进入提权流程，也不生成结果文件） | 直接 `service::query_status()` + `QueryServiceConfigW` 打印状态 / 宿主 / 数据根 / 错误码 |

`reinstall` 是提权副本的 operation 之一（`--elevated reinstall --result "<路径>"`），
不是「先跑一次 uninstall 再跑一次 install」两条命令——两次命令会弹两次 UAC。

## 126.4 用户取消 UAC 与超时

```text
用户取消 UAC（ERROR_CANCELLED，1223）  → FMT-004 / 退出码 5
非管理员 / 策略禁止提权（ACCESS_DENIED，5） → FMT-603 / 退出码 5
提权等待超时（60 秒）                  → FMT-602 / 退出码 8
ShellExecuteExW 其它失败               → FMT-602 / 退出码 8
结果文件不存在（提权副本崩了）          → FMT-602 / 退出码 8，「提权副本没有返回结果」
SCM 操作失败                           → FMT-602 / 退出码 8
服务不存在                             → FMT-601 / 退出码 8
服务已存在（重复 install）             → FMT-600 / 退出码 8
reinstall 时服务不存在                 → 忽略卸载阶段的 FMT-601，继续安装
service status 发现服务未安装           → FMT-601 / 退出码 8（不提权，不是提权失败）
service status 已安装但未运行           → 照常打印「已停止」/ 退出码 0
```

用户取消 UAC 或提权失败**不退出 CLI**：打印错误后回到提示符继续等待输入（见第 127 节）。

## 126.5 固定输出

成功：

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

失败（后两行替换为执行失败行与错误码行）：

```text
File Manager Tool  v1.0  ( build  2026.10.08 )
Service Running...

fmt >service stop
需要管理员权限
正在提权...
执行失败：FMT-601 服务未安装
错误码：8

fmt >
```

约定：

```text
「执行成功...」/「执行失败：FMT-NNN <消息>」是结果行
「错误码：N」是该次命令的进程退出码
FMT-NNN 与退出码分属两层：错误码定位原因，退出码给脚本判断
fmt> 提示符前先输出一个空行（横幅之后、以及每条命令之后都如此），
  这样输出不会和提示符挤在一起；用户只敲回车（空命令）时不再重复空行
```

---

# 127. CLI 界面约定

## 127.1 横幅

CLI 启动时打印（`banner_text()` 产出，与 `--version`、`version` 命令同一串；
**提交 `8f2fbc5` 起横幅区永远多一行数据根**）：

```text
File Manager Tool  v1.0  ( build  2026.10.09 )
Service Running...
数据根：D:/Data/Temp/JMT/fmt
```

服务未运行时第二行改为：

```text
File Manager Tool  v1.0  ( build  2026.10.09 )
Service Stopped...
数据根：D:/Data/Temp/JMT/fmt
```

> **「数据根：…」那一行（提交 `8f2fbc5`）**：数据来自 `service::load_state()`，
> **永远显示**（服务记的数据根为空时不打）；它与**本程序所在目录**不一致时，紧跟一行
> 「注意：本程序所在目录是 …，连上之后服务会切到本目录（数据根由 CLI 声明）。」
> 触发条件见下面的说明（服务在跑时通常已经一致）。

| 项 | 约定 |
| --- | --- |
| 程序名 | 固定 `File Manager Tool`（文档名/工程名仍叫 FMT，只有横幅用这个名字） |
| 版本部分 | `v<MAJOR>.<MINOR>`，当前 `v2.0`；工程版本仍是 `2.0.0` |
| 构建日期 | CMake 配置时生成（`FMT_BUILD_DATE`，`%Y.%m.%d` 本地时间），每次重新配置都会变 |
| 单一来源 | 横幅、`--version` / `-v`、以及 `version` 命令（提交 `d108c80`）调同一个 `version_text()`（内部就是 `banner_text()`），不各写一份；源码里不得硬编码版本号或日期 |
| 用法标题 | `--help` 里是 `用法：fmt.exe [命令]`，不再用 `FMT 2.0.0 - Windows 文件管理系统` 这类旧标题 |

**双击时控制台只有这些行**（数据根体检与服务状态都不打印）：

```text
需要管理员权限        ← 仅当需要装/启服务时才有这两行
正在提权...
执行成功...
错误码：0

File Manager Tool  v1.0  ( build  2026.10.08 )
Service Running...

fmt> 
```

数据根体检的「建了什么 / 完不完整」与服务当前状态**只进日志**（`log/fmt.log`，
模块 `Cli` / `Service`）：

```text
[Cli] 数据根检查：D:/FMT2
[Cli] 数据根新建目录：repository, trash, config, data, log, temp
[Cli] 数据根新建文件：D:/FMT2/config/config.json, ...
[Cli] 数据根完整                      ← 第二次双击
[Service] 当前状态：运行中
```

**只有异常才走 stderr**：`数据根无法补齐：FMT-013 …`、
`数据根文件损坏（未自动修复）：data/file.json`。

**换根通知是个例外：它现在也上控制台（提交 `8f2fbc5`，原口径「换根只进日志、不刷控制台」
已作废）**——因为「服务在看哪个目录」是用户必须立刻知道的事，藏在日志里等于没说：

```text
切换发生的那一刻（hello 回执 switched=true）→ CLI 在 **stderr** 打印（文本来自
  cli::root_switch_notice(previous, current)）：

注意：服务的数据根已切换
  原来：D:/Data/Temp/JMT/fmt
  现在：D:/Data/CLionProjects/FMT/cmake-build-debug/bin
原因是「数据根由 CLI 声明」：谁连上服务，服务就用谁的目录。
如果这不是你想要的，请用数据根正确的那个 fmt.exe 再执行一条命令切回去。

同时仍记一行日志（完整运行记录不变）：
[Service] 数据根切换：D:/FMT -> D:/FMT2        ← 仅当 hello 回执 switched=true
[Service] 服务数据根已经是：D:/FMT2            ← 未切换时（这一条仍然只进日志）
```

**交互窗口的横幅区永远多一行数据根**（`run_interactive`，数据来自 `service::load_state()`，
提交 `8f2fbc5`）：

```text
File Manager Tool  v1.0  ( build  2026.10.09 )
Service Running...
数据根：D:/Data/Temp/JMT/fmt
```

当它与**本程序所在目录**不一致时，紧跟一行（正斜杠、`iequals` 比较）：

```text
注意：本程序所在目录是 D:/Data/CLionProjects/FMT/cmake-build-debug/bin，连上之后服务会切到本目录（数据根由 CLI 声明）。
```

> **这一行的触发条件（如实说明）**：双击引导会**先连上服务并声明本目录**，所以服务在跑时
> 横幅上两边通常已经一致、看不到这一行；它主要在**服务不可用**（引导没能连上）时出现，
> 用来说明「服务记录的数据根与你所在目录不同」。服务记的数据根为空时连「数据根：」都不打。

这样归位仍守第 65 节那条老原则——**控制台负责用户交互与重要异常，日志文件负责完整运行记录**
——`version` / `--help` 不写日志是另一条口径（第 68 节），两条不冲突：
换根通知既上控制台、也照旧写进日志。

横幅之后**先输出一个空行**，再打印提示符，等待用户输入。
空行的作用是把提示符和上面的横幅分开，输出不会和提示符挤在一起。

## 127.2 提示符与交互循环

提示符为：

```text
fmt> 
```

提示符打印规则：

```text
打印 fmt> 之前先输出一个空行：横幅之后一次，以及每条命令执行完之后一次
用户只敲回车（空命令）时不再重复空行——不叠出连续两个空行
```

规则：

```text
空行忽略：不报错、不退出
正常结果 → stdout
错误 → stderr
命令失败后继续循环，不退出
help    → 打印命令总览；help <组> 看该组详情（交互里 --help 是 help 的别名）
exit 或 quit 退出（正式命令，不只是「关窗口」）
```

命令按 `fmt> ` 提示符逐条输入，例如：

```text
fmt >service stop

fmt >help service
```

`help` 的输出（**与第 68 节同一份**，改口径时两处要一起改）：

```text
fmt> help
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
```

> 上面是**当前源码**的实际输出（`src/cli/cli.cpp` 的 `print_command_list()`；
> `(file)` 在 `188e85d` 之后移进「可用命令」组、`(trash)` 在 `674d0b0` 多了 `empty`、
> `(config)` 同批新增、`(version)` 自 `d108c80` 起在列）。**`d5779db` 把原来的
> 「业务命令（服务端尚未实现，现在会返回 FMT-602）」那一组整个删掉**（`(share)` 进了
> 「可用命令」组，落在 `(trash)` 之后、`(config)` 之前）——原口径「『尚未实现』这句只对 `(share)`
> 一组成立」**已作废**：「尚未实现」组现在不存在，`server.*` 仍是唯一没有实现的业务 op
> （走管道拿到 `FMT-602`，但不列在总览里）。
> **第 68 节的逐字输出与说明是权威那份**，两处必须一起改。

```text
fmt> version
File Manager Tool  v1.0  ( build  2026.10.09 )
执行成功...
错误码：0
```

> **`version`（提交 `d108c80`）**：窗口里原来敲它得到「未知命令」（只有 `--version`
> 旗标实现了）。现在它与 `--version` / `-v`、横幅**共用同一份 `version_text()`**
> ——三处不可能各说一套。**不需要服务在运行，也不写任何磁盘内容**
> （一次性分支在开日志器之前，`log/fmt.log` 不会多出记录）。详见第 68、127.6 节。

> 上面是**当前源码**的实际输出（提交 `8a5e554` 起，`c2d545d` 补了桶级 trash 的 `list` /
> `restore`，`4fee290` 补齐 `get` / `delete`）。
> `bucket` 五条命令与**桶级 `trash` 四条命令**在阶段 4 已经可用，
> 所以它们列在「可用命令」组；「尚未实现」这句现在只对 `file` / `share` 两组成立
> （**这句是阶段 4 收尾时的原话**；提交 `188e85d` 之后 `(file)` 也进了「可用命令」组，
> 该组只剩 `(share)`，见本节上面那一段）。
> （原口径「与 `trash get` / `trash delete`」已作废，第 68、95、127.6 节）。

`service status` 的输出也走 stdout（它是查询命令，成功与「未安装」两种结果都是它的正常输出）：

```text
fmt> service status
服务状态：运行中
服务宿主：D:/FMT2/fmt.exe
服务数据根：D:/FMT2
错误码：0

fmt>
```

## 127.3 退出

```text
exit
quit
```

两者都是**正式命令**（`help exit` 有说明，`quit` 等同 `exit`）：结束 CLI 循环并以退出码 `0`
退出；直接关闭窗口同样只结束 CLI，**不影响后台服务**（服务由 SCM 托管）。

## 127.4 输出通道

```text
stdout   命令的正常结果、横幅、提示符、提权过程提示（需要管理员权限 / 正在提权...）
stderr   错误码与错误消息（执行失败：FMT-NNN <消息>、错误码：N）
```

**机器可读通道是退出码，不是结构化文本**：V1 不做 `--json`，也不预留参数名。
脚本判断「服务在不在」读退出码即可（`0` 成功；未安装 `FMT-601` → `8`；查询失败 → `8`），
要拿具体状态就读上面那几行**顺序固定**的文本。等真的需要读到 `wait_hint_ms` 这类结构化字段时
再加（见第 70 节）。

CLI 把结果输出到控制台，**同时追加写入数据根下的 `log/fmt.log`**（见第 65 节）；
`--help` / `--version` / `-v` / `version` / `help` / `exit` 不写日志、不创建任何目录。
**控制台只留交互与异常**：双击时的数据根体检结果与服务当前状态都只进日志（见 127.1），
只有「数据根无法补齐」「数据根文件损坏（未自动修复）」才走 stderr。

## 127.5 CLI 与服务的边界

```text
CLI 不碰 core 业务操作、不写业务 JSON；双击时对自己所在数据根跑一次与服务共用的幂等体检与补齐
  （只补缺失的六个目录与六个默认 JSON，不删除、不覆盖、不改名、不重置损坏 JSON，
   结果只进日志、控制台不打印，见第 5 节、第 91～92 节）
CLI 只解析命令、走管道、打印结果、写自己的日志
help / --help 本地处理：不连服务、不提权、不写日志
业务命令经命名管道交给服务执行（见第 76 节）
service 命令直连 SCM：四条动作命令走 UAC 提权、status 是查询不提权（见第 126 节）
CLI 单实例：已有窗口则激活，不新建窗口
```

## 127.6 业务命令的输出（阶段 4：Bucket；Trash 见提交 4fee290 + 0fc242b；`file` 见 188e85d）

**服务端只返回结构化数据，展示在 CLI 一侧完成**（`print_business_data` / `print_trash_entry`）：
有 `buckets` 数组按列表打印、有 `entries` 数组按回收站列表打印（**提交 `0fc242b`：
文件与桶都标出**；旧的 `deleted_buckets` / `trashed` 形状**已作废**）、
有 `entry` 对象按回收站条目详情打印（提交 `0fc242b`，`trash get` / `restore` / `delete`）、
有 `files` **数组**按文件列表打印（提交 `188e85d`）、
有 `file_id` + `size` 按单条文件打印（提交 `188e85d`，**在回收站里的记录多打一行
「回收站路径：…」**）、
有 `bucket` + `is_current` 按单条打印、否则打印 `message`。
**提交 `9c3d2cb` 起**：`data` 里有 `note`（服务端的提示，例如「Bucket 名称统一使用小写」）
就先单独打一行 `提示：<note>`，再打其余字段；`bucket create` 回的是**实际建成的名字**，
`bucket get` 显示的是**磁盘上的实际名字**。
`bucket` 五条命令、桶级 `trash` 四条命令与 `file` 四条命令都已经可用，实际输出：

```text
fmt> bucket create 工作
Bucket 已创建：工作（已设为当前 Bucket）
执行成功...
错误码：0

fmt> bucket create 生活
Bucket 已创建：生活
执行成功...
错误码：0

fmt> bucket create WORK          ← 提交 9c3d2cb：名称统一小写，note 单独一行
提示：Bucket 名称统一使用小写：已把 WORK 转为 work
Bucket 已创建：work
执行成功...
错误码：0

fmt> bucket create WORK          ← 再敲一次：同一个桶
执行失败：FMT-201 Bucket 已存在：work
错误码：4

fmt> bucket get WORK             ← 找得到就按磁盘上的实际名字处理（提交 6a40742 起 path 也是）
Bucket：work
当前：否
路径：repository/user/work
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

fmt> bucket delete 生活
Bucket 已删除（移入回收站）：生活  ->  生活_20261008012233
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

fmt> trash delete lazy-fox_20261008012233
永久删除后不可恢复：lazy-fox_20261008012233（原桶 lazy-fox，3 个文件，1.2KB）
[桶] lazy-fox
  标识：lazy-fox_20261008012233
  文件数：3
  删除时间：2026-10-08T01:22:33
  回收站路径：trash/user/lazy-fox_20261008012233
确认执行？(y/N) y
已永久删除：lazy-fox
执行成功...
错误码：0

fmt> trash delete lazy-fox_20261008012233
永久删除后不可恢复：lazy-fox_20261008012233（原桶 lazy-fox，3 个文件，1.2KB）
[桶] lazy-fox
  标识：lazy-fox_20261008012233
确认执行？(y/N) n
已取消
错误码：0

fmt.exe trash delete lazy-fox_20261008012233
该操作需要确认（FMT-016）：请加 --yes，或在交互窗口里执行
错误码：2

fmt> file delete a.txt            ← 跨 Bucket：先预检说清归属，再问一次
「a.txt」属于 Bucket「工作」，而当前 Bucket 是「生活」
确认执行？(y/N) y
文件已移入回收站：a.txt（Bucket：工作）
执行成功...
错误码：0

fmt> file delete fmt-20261008-0   ← 歧义：blocked，不是「确认一下」能解决的
「fmt-20261008-0」既是文件标识、又是另一个文件名，无法确定删哪一个：
a.txt（file_id fmt-20261008-0）与 fmt-20261008-0（file_id fmt-20261008-1）。
请用 file_id 明确指定
候选：
  a.txt（Bucket：工作，file_id fmt-20261008-0）
  fmt-20261008-0（Bucket：工作，file_id fmt-20261008-1）
这项操作不能靠确认解决，请按上面的提示指定具体对象
```

`bucket delete` 的预检（提交 `0fc242b`）：桶里有文件时先提醒「之后只能整体恢复这个桶」，
空桶不打扰用户。

```text
fmt> bucket delete 工作
「工作」里有 2 个文件（2.4KB）。删除后整个桶移入回收站，之后只能整体恢复这个桶，
无法只恢复其中某个文件；当前 Bucket 会被置空
Bucket：工作
当前：是
文件数：2
占用：2.4KB
确认执行？(y/N) y
Bucket 已删除（移入回收站）：工作  ->  工作_20261008151538（之后只能整体恢复这个桶）
执行成功...
错误码：0
```

`file` 四条命令的输出样例（提交 `188e85d`；服务端 `data` 形状见
`FMT 技术文档.md` 第 12.3.2.1 节）：

```text
fmt> file upload D:/test/报告.txt
文件已入库：报告.txt（fmt-20261008-0）
执行成功...
错误码：0

fmt> file upload http://example.com/a.bin 改名.bin
文件已入库：改名.bin（fmt-20261008-1）
执行成功...
错误码：0

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

fmt> file get 报告.txt        ← 先当 file_id 查、查不到再当文件名查，结果同上
文件：报告.txt
...

fmt> file delete 报告.txt   ← 换成文件名也一样（提交 0ad9efc：与 file get 同一套定位规则）
文件已移入回收站：报告.txt
执行成功...
错误码：0

fmt> file delete 没有这个.txt
执行失败：FMT-002 文件不存在：没有这个.txt
错误码：3

fmt> file delete 报告.txt   ← 已经在回收站里（按名字命中会走到定位的第 ③ 步）
执行失败：FMT-001 该文件已经在回收站里：报告.txt（file_id fmt-20261008-0）
错误码：2

fmt> file delete fmt-20261008-0   ← 按 id 命中，走的是删前那两道校验，消息形状一致
执行失败：FMT-001 该文件已经在回收站里：fmt-20261008-0
错误码：2

fmt> file get fmt-20261008-0  ← 软删除后仍能按 file_id 查到（trash_reason="file"）；
                             ← 提交 9c3d2cb：仓库里没有它，所以**不打「路径」**，
                               改打「回收站路径」（trash_path）
文件：报告.txt
file_id：fmt-20261008-0
Bucket：工作
类型：text.txt
大小：1.2 KB
MD5：d41d8cd98f00b204e9800998ecf8427e
回收站路径：trash/user/.files/工作/2026/10/08/报告.txt
状态：在回收站（file）
执行成功...
错误码：0

fmt> file get 报告.txt        ← 按**文件名**查只查当前用户的**正常**文件，
                             ← 所以这个已经在回收站的名字查不到（与按 id 查是两种范围）
执行失败：FMT-002 文件不存在：报告.txt
错误码：3

fmt> file upload ftp://example.com/a.bin
执行失败：FMT-300 只支持 http:// 与 https:// 的来源：ftp://example.com/a.bin
错误码：2

fmt> file upload http://example.com/
执行失败：FMT-100 无法从来源推断文件名，请显式给出文件名
错误码：2

fmt> file upload D:/test/报告.txt                 ← 这个桶里已经有同样内容的一份（名字甚至可能不同）
执行失败：FMT-304 文件已存在：Bucket「工作」里已有相同内容的文件：报告-旧.txt（file_id fmt-20261008-0）
错误码：4

fmt> file upload D:/test2/报告.txt 报告.txt      ← 同名但内容不同：这才要求改文件名
执行失败：FMT-105 文件名重复：Bucket「工作」里已有同名文件：报告.txt（file_id fmt-20261008-0，内容不同；请修改文件名后重新上传）
错误码：4

fmt> file upload ‪C:\Users\lenovo\Pictures\pet-food-store\头像\asdva.jpg
                 ↑ 首尾是不可见的 U+202A / U+202C（从聊天窗口/网页复制粘贴带进来的）
执行失败：FMT-002 本地文件不存在：C:\Users\lenovo\Pictures\pet-food-store\头像\asdva.jpg
（你粘贴的路径里有不可见字符 U+202A、U+202C，它会让路径对不上；已自动清掉，
请检查路径是否还有别的问题）
错误码：3

fmt> file upload "C:\图片\头像\asdva.jpg"        ← Explorer「复制路径」的一对引号
文件已入库：asdva.jpg（fmt-20261008-4）         ← 成对引号已清掉，正常入库
执行成功...
错误码：0

fmt> bucket create 工作‪                    ← 名字里带 U+202A：**不允许**进名字
执行失败：FMT-202 Bucket 名称里有不可见字符（U+202A），请把名字重敲一遍
错误码：2
```

> 三个跟实现对齐的细节：`file list` 的**末行是「共 N 个文件」**（不是「Bucket」）；
> `file get` 的**「类型」行是 `<file_type><extension>` 拼起来的**（`text` + `.txt`
> 打成 `text.txt`，源码里就是这么 `printf` 的）；`file delete` 走 `message` 那一类、
> 只打一行「文件已移入回收站：<文件名>」，库里带 `file_id` / `moved_to` 但 CLI 不打印它们
> （要看回收站路径就 `file get <file_id>`）。
>
> **上面 `file delete` 的三段新样例是提交 `0ad9efc` 加的**：文件名与 `file_id` 都能删
> （同一套定位规则）；名字根本不是文件时是 `FMT-002 文件不存在：<参数>`（退出码 3，
> **不是**「参数错误」）；名字在、但已经软删除过则是 `FMT-001`，消息里带 `file_id`
> （`file delete <名字>` 与 `file delete <file_id>` 两条都带，只是「按名字」那条由定位的
> 第 ③ 步报、「按 id」那条由删前校验报，第 43 节）。
>
> **提交 `9c3d2cb` 再加一种失败**：`file delete <参数>` 若该参数**同时**命中一条记录的
> `file_id` 与**另一条**记录的文件名（只可能来自旧数据），定位的第 ②.5 步报
> `FMT-001 有歧义：…请直接用 file_id 指定要删哪一个`（退出码 2，第 43 节）；
> `file get` **没有**这一步（它按 `file_id` 查是全局的、按名字只查正常文件，第 42 节）。
>
> 大小文本由 `format_size()`（`src/common/string.cpp`）生成：**单位与数字之间没有空格**
> （`512B` / `1.2KB` / `1.5MB`），≥ 1 KiB 时保留到两位小数并去掉尾随的 `0` 与 `.`。
> 本节与第 95 节里桶级样例的 `1.2 KB`（带空格）是**旧样例的排版**，与源码不符——
> 以源码为准。
>
> **本段样文已改（提交 `a2b6cd1`）**：原来举的是
> `file upload https://example.com/a.bin` →「FMT-300 V1 不支持 https（需要 OpenSSL）」。
> **https 现在是合法来源**，所以换成 `ftp://` 的例子——`FMT-300` 仍出现在这个位置，
> 只是理由变成「协议不是 http/https」（第 33.1 节）。

展示规则：

| 服务端 data 形状 | CLI 打印 |
|---|---|
| `{buckets:[{name,is_current}], count, current_bucket}` | 每行一个：当前项 `* 名称  (当前)`，其余 `  名称`；末行 `共 N 个 Bucket` |
| `{deleted_buckets:[{trashed,original,deleted_at,present}], count}`（**已作废，提交 `0fc242b`**） | 旧形状，见下面 `entries` 那一行 |
| `{entries:[{type,id,name,bucket,deleted_at,bytes,files,present,restorable,trash_path?,reason?}], count, files, buckets}`（提交 `0fc242b`） | 每行按类型：`  [文件]  <name>（Bucket <bucket>，<人类可读大小>）` 或 `  [桶]    <name>（N 个文件，<人类可读大小>）  ->  <id>`（桶那一段字数只在 `files > 0` 时打，`name` 与 `id` 相同时不打 `  ->  <id>`；**提交 `18f16ca` 起桶级条目在列表里就带这两个数**）；末行 `共 N 项（X 个文件、Y 个桶）`——**文件与桶都标出来** |
| `{entry:{…}}` + 可选 `message`（`trash.get` / `restore` / `delete`，提交 `0fc242b`） | `print_trash_entry()`：`[文件]\|[桶] <name>` / `  标识：<id>` / 文件→`  Bucket：…` + `  大小：…`，桶→`  文件数：N`（>0 才打）/ `  删除时间：…` / `  回收站路径：…` / `  状态：数据已不存在` / `  可回退：否（<reason>）`；有 `message` 再打一行 |
| `{…, message}` | 打印 `message` 一行（`file upload` 走这一行：`文件已入库：<名字>（<file_id>）`；`file delete` 走这一行：`文件已移入回收站：<名字>` 或跨桶时 `…（Bucket：工作）`；`trash restore` / `trash delete` 走这一行：`文件已回退：<名字>` / `Bucket 已回退：<名字>` / `已永久删除：<名字>`） |
| `{bucket, is_current, path}` | `Bucket：…` / `当前：是\|否` / `路径：…`（有 `path` 才打印第三行）。**提交 `9c3d2cb` + `6a40742`**：`bucket` 与 `path` 都是**磁盘上的实际名字**（`bucket get WORK` → `Bucket：work`、`路径：repository/user/work`），命令层传的是 `canonical_name()` 的结果——**原口径「path 按用户敲的拼写拼、会打印 .../WORK」已作废** |
| `{files:[{file_id,file_name,extension,file_type,size,md5}], count, current_bucket}`（提交 `188e85d`） | 每行 `  <file_name>  <人类可读大小>`；末行 `共 N 个文件`（注意是「文件」不是「Bucket」） |
| `{file_id, file_name, bucket, extension, file_type, size, md5, is_trash, trash_reason, path?, trash_path?}`（提交 `188e85d`；`trash_path` 提交 `9c3d2cb`） | `文件：…` / `file_id：…` / `Bucket：…` / `类型：<file_type><extension>`（源码就是两个 `%s` 拼起来，形如 `text.txt`）/ `大小：<人类可读>` / `MD5：…` / `路径：…`（**有 `path` 才打印**）/ `回收站路径：…`（**有 `trash_path` 才打印**，提交 `9c3d2cb`）/ `状态：正常` 或 `状态：在回收站（<trash_reason>）`。回收站里的记录只会有 `trash_path`、没有 `path`（第 42 节） |
| `{…, message}` | 打印 `message` 一行（`trash delete` 走这一行：`已永久删除：…（N 个文件，M 条记录）`；`file upload` 走这一行：`文件已入库：<名字>（<file_id>）`；`file delete` 走这一行：`文件已移入回收站：<名字>`） |
| 带 `note` 的对象（提交 `9c3d2cb`） | **先**单独打一行 `提示：<note>`（`bucket.create` 名称被转换、`bucket.use` 被规范化时出现），再按上表打印其余字段 |
| 其它/空 | 退回 `data.dump(2)`，不吞输出 |

错误路径仍是固定两行（走 stderr）：`执行失败：FMT-201 Bucket 已存在：工作` +
`错误码：4`；桶级回退撞名是 `执行失败：FMT-401 回退失败：Bucket 已存在：lazy-fox` +
`错误码：4`。**永久删除未确认**：CLI 侧一次性命令是本地拒绝，stderr 一行
`该操作需要确认（FMT-016）：请加 --yes，或在交互窗口里执行` + `错误码：2`（错误码自提交
`711da4c` 起是 `FMT-016`，**提示语里的「（FMT-016）」是提交 `18f16ca` 补的**；
原文案「永久删除不可恢复：请加 --yes 明确确认…」已作废）；
服务端侧的兜底是 `执行失败：FMT-016 永久删除不可恢复，需要确认（force = true）` + `错误码：2`
（提交 `711da4c` 起不再是 `FMT-001`）。
中文 Bucket 名全程 UTF-8，不经过控制台代码页转换。

> **帮助文案已与上面这些输出一致**（提交 `8a5e554`、`c2d545d`，`4fee290`；`0fc242b` 再同步
> `trash` 与 `bucket delete`）：命令总览里 `(bucket)` 与 `(trash) list get restore delete`
> 在「可用命令」组，`help bucket` / `help trash` 写明各子命令的真实行为，
> 不含与实际不符的「尚未实现」或「只有桶级条目」字样（见第 68、95 节）。

## 127.7 破坏性操作：先检查 → 说清楚 → 再确认（提交 `711da4c`）

`file.delete` / `trash.delete` / `bucket.delete` 三个 op 在 CLI 侧走同一条流程
（`src/cli/cli.cpp` 的 `confirm_before_acting()` / `print_precheck()`）：

```text
① 连上服务（ensure_connected）
② 发一次**预检**：同一个 op + args.dry_run = true，**只读、零副作用**
③ 把预检说的原样打印（print_precheck）：
     file.delete   目标属于哪个 Bucket / 当前 Bucket 是哪个；歧义时**两条候选各自的
                   file_name、Bucket、file_id**
     trash.delete  条目详情（type / id / name / 删除时间 / 路径 / 大小或文件数）
                   + 一句话「永久删除后不可恢复：…」
     bucket.delete Bucket 名 / 是否当前 / 文件数 / 占用 + 「之后只能整体恢复这个桶」
④ 交互窗口问「确认执行？(y/N)」；一次性命令必须 --yes
⑤ 同意之后**才**把真实请求带上 force 发出去
```

**关键性质：用户确认之前，一个破坏性请求都不会发出去**（预检是只读的）。

**预检与真实请求的参数形状由同一个构造函数产出（提交 `2c841c8`）**：

```cpp
// include/fmt/cli/cli.hpp（提交 2c841c8）
// 一条业务命令的**参数信封**：位置参数放 args.argv，开关（dry_run / force）放**同级**字段。
nlohmann::json argument_envelope(const nlohmann::json& positional,
                                 bool dry_run = false, bool force = false);

// 预检：argument_envelope(arguments, /*dry_run=*/true)  → {"argv":["a7.jpg"],"dry_run":true}
// 真实请求：argument_envelope(arguments, false, confirmed || destructive)
//                                                    → {"argv":["a7.jpg"],"force":true}
```

**用户实测的崩溃（照实记录）**：敲 `file delete a7.jpg` 弹出
**「Debug Error! abort() has been called」**。原因是确认流程里把开关挂到了位置参数
**数组**上：

```cpp
check.args = arguments;          // arguments 是位置参数**数组** ["a7.jpg"]
check.args["dry_run"] = true;    // nlohmann 对数组用字符串下标 → 抛 type_error.305
```

异常没人接 → `abort()`（Debug 版就弹那个框）。**服务端期望的形状是开关与 `argv` 同级**：
`{"argv":["a7.jpg"], "dry_run":true}`。

> **工程教训（值得单独记住）**：原来的单元测试**自己照着服务端期望的形状拼请求**，
> 而 CLI 拼的是另一种形状——**测试全绿，CLI 一敲就崩**。测试复制了「契约」，
> 却没有共用「产出契约的那段代码」，于是两边的形状各自漂移、谁也没钉住谁。
> 所以规则是：**形状必须由同一个构造函数产出，测试不许自己拼**。
> 新增用例 `Cli.位置参数的信封形状` 直接钉住机制本身：旧写法必须抛
> `type_error.305`，新写法必须是**带 `argv` 且开关同级**的对象。
> 四份文档里凡出现「预检请求 `args.dry_run`」的地方，样例本身就是同级写法
> （`{"argv":[…],"dry_run":true}`）。

几条要说准的细节：

```text
预检就失败（文件不存在、已在回收站、桶不存在）→ 直接报预检的错误，**不再发执行请求**，
                   **退出码用预检自己的错误码**（提交 18f16ca 起，见下表）
--yes / -y        CLI **本地**开关：只置 args.force，**不作为位置参数发给服务端**
交互窗口答 n       打印「已取消」，**退出码 0**（用户主动取消不是错误），不发请求
一次性缺 --yes     stderr「该操作需要确认（FMT-016）：请加 --yes，或在交互窗口里执行」，
                   **退出码 2**（提交 18f16ca 起提示语带上了 FMT-016）
blocked 的情况     歧义（file.delete）、同名冲突 / 随桶删除 / 数据缺失（trash.restore）：
                   打印候选或原因 + stderr「这项操作不能靠确认解决，请按上面的提示指定
                   具体对象」，**不发执行请求**，**退出码 2**（FMT-001）
服务端独立校验     force 由服务端再查一遍：预检被绕过（别的客户端直接发）时
                   跨桶删除 / 非空桶删除 / 永久删除照样被 FMT-016 拦下
                   —— **预检负责「说清楚」，force 负责「兜底」，这是两件事**
```

**四种「不要继续」的退出码（提交 `18f16ca` 修正，逐条对着 `confirm_before_acting()`
的 `ConfirmOutcome{proceed, exit_code}`）**：

| 情况 | 退出码 |
|---|---|
| 预检自身失败（文件不存在 `FMT-002`、已在回收站等） | **预检的那个错误码**（`FMT-002` → 3） |
| 预检通信失败（`FMT-601` 等） | **通信错误的码**（8） |
| `blocked`（歧义 / 同名冲突 / 随桶删除 / 数据缺失） | 2（`FMT-001`） |
| 一次性命令缺 `--yes` | 2（`FMT-016`） |
| 交互窗口里答 n（用户主动取消） | **0** |

> **原口径「预检失败时 stderr 打真实错误码、进程退出码却统一走 2」已作废**（那是
> `711da4c` 到 `18f16ca` 之间的实况）：预检自身的错误码现在原样透出——文件不存在就是 3、
> 通信失败就是 8，不会被「需要确认」的 2 盖掉，脚本不会误读。
> 用例 `Service.破坏性操作先预检再确认` 覆盖这条路（第 112 节）。

---

# 128. 相对旧设计的差异与理由（原 `FMT 重构设计.md` 第 11 节）

> **来源**：这一节整体搬自 `FMT 重构设计.md` 第 11 节「与旧设计的差异清单」（该文件已并入本文与其他两份文档，随后删除）。
> 它回答的是「相对旧设计改了什么、为什么改」，与第 122 节「开发中的冻结规则」互补：
> 第 122 节说**现在是什么规则**，这一节说**为什么不是旧那样**。

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
| 13 | 数据根切换对 CLI 不可见 | `hello` 响应加 `switched` / `previous_root`，切换时 CLI **在 stderr 打印换根提示**（提交 `8f2fbc5`，`root_switch_notice()`；原口径「只记日志」已作废）并记一行日志 `[Service] 数据根切换：旧 -> 新` | 双击新目录能在日志里看见服务跟过来了，又不往控制台刷例行状态 |
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
| 29 | 破坏性操作（删文件、永久删除、删桶）要么直接执行、要么只问一句 `y/N`；跨 Bucket 按名字删除会**静默**删到别的桶；缺确认复用 `FMT-001`，脚本分不清「参数错」与「忘了确认」 | **提交 `711da4c`**：三类 op 都先发**只读预检**（`args.dry_run` / `?dry_run=1`），把「属于哪个桶、哪两条记录撞车、要永久删掉什么」原样打印，再问 `确认执行？(y/N)`（一次性命令要 `--yes`）；**用户同意前不发任何破坏性请求**；歧义 / 同名冲突 / 随桶删除 / 数据缺失算 `blocked`（y/N 解决不了，让用户改用 `file_id`）；新错误码 **`FMT-016 ConfirmRequired`**（退出码 2，HTTP 400），服务端仍独立校验 `force` | 「说清楚」和「兜底」是两件事：预检让用户知道自己在删什么，`force` 保证绕过预检的客户端也删不掉跨桶的东西。`FMT-016` 独立编号让脚本能区分「参数写错」与「忘了确认」 |
| 30 | 回收站只有桶级能读（`deleted_buckets`），文件级「写得进、读不出」；权威散在 `trash.json`（第二份副本）与 `.original` 两处 | **提交 `0fc242b`**：新建 `src/trash/` 的 `TrashService`，把两级合成统一 `TrashEntry` 列表 + 跨命名空间标识解析；**权威改为「文件级 = `file.json`（含 `deleted_at`）、桶级 = `.original`」**，`data/trash.json` 不再写入（只读兼容）；`trash.list` 回 `{entries, count, files, buckets}` 并标出 `[文件]` / `[桶]`；回退的三种硬拒绝（`FMT-401` / `FMT-402` / `FMT-002`）都是 `blocked`；`bucket.delete` 也有预检（非空桶提醒「只能整体恢复这个桶」，缺 `force` → `FMT-016`） | 文件级条目本来就已经在 `file.json` 里有全部信息，再维护一份 `trash.json` 只会制造两个可能不一致的真相；两级合成一份视图，用户才不必先猜「这个名字是文件还是桶」 |
| 31 | 路径参数**按原样**使用（用户从聊天窗口/网页/终端复制的路径会夹进 `U+202A` 这类**看不见**的格式字符，Explorer「复制路径」还会套一对引号）——屏幕上路径完全正常，`exists()` 却说不存在；名字里也一样会混进这些字符 | **提交 `a9af276`**：`clean_user_path()` 清掉不可见格式字符（`U+00A0` / `U+00AD` / `U+200B`–`U+200F` / `U+202A`–`U+202E` / `U+2060`–`U+2064` / `U+2066`–`U+2069` / `U+FEFF`）与**成对**引号、首尾空白；**一处收口**在 `argument()`（所有位置参数的唯一入口，CLI 与 HTTP 共用），`prepare_upload()` 再清一次来源与显式文件名；清掉后仍找不到 → `FMT-002` 且**点名码位**；**名字里不允许**这些字符（`FMT-101` / `FMT-202`） | 这类字符**屏幕上看不见**，用户会坚持「路径明明是对的」——原样使用等于把一个无法自查的失败扔给用户；清掉路径里的（粘贴必然产生）、拒掉名字里的（名字要长期存下来、还要被重敲一遍），才是可诊断的行为 |
| 32 | 请求的 `args` 由调用方**各自拼**（CLI 把 `dry_run` 直接挂在位置参数**数组**上）；单元测试也**自己照着服务端契约拼**；temp/ 启动清理「见 `fmt-` 就清」；原子写的临时名固定 `<目标>.tmp`；`TrashEntry.present` 被当成「还在不在回收站」用 | **提交 `2c841c8` + `5b316b3` + `8f0fd5c`**：① 新增 `cli::argument_envelope(positional, dry_run, force)`，**预检与真实请求共用**（nlohmann 对数组用字符串下标会抛 `type_error.305`，未捕获即 `abort()`——`file delete a7.jpg` 的 Debug Error 就是它）；**测试不许自己拼形状**，用例 `Cli.位置参数的信封形状` 钉住机制。② temp/ 启动清理**只清十分钟以前**的 `fmt-*`（提权结果文件也叫 `fmt-elev-<pid>.json(.tmp)`，`service install` 会启动服务 → 一律清就报假失败 `FMT-602`）。③ 原子写临时名 `<目标>.<pid>.<序号>.tmp` + 进程内互斥 + 替换遇共享冲突重试 40 × 5 ms（两个写者同时写 `service.json` 会互相踩，加上调用方忽略返回值 → 状态文件静默不更新）。④ `present` 语义写死为「数据在不在磁盘上」，回退/删除的结果**不翻转它**（`5b316b3` 文件级、`8f0fd5c` 桶级；原来恢复成功后打印「状态：数据已不存在」；测试补断言、真机跑过核实） | 这四条都是**用户在自己机器上重装服务、跑真命令**才暴露的：单测自己拼契约就挡不住 CLI 崩溃；两个进程写同一个临时名会互相踩，而「忽略返回值」把失败变成了静默的旧数据；把 `present` 当「回收站里还有没有」用会打印与事实相反的结论。**共同点：能被自证的形状/状态，就不要让两个地方各写一份** |
| 33 | 兜底路由把「没这个接口」当成「服务器坏了」（`Get(R"(/api/.*)")` 里**硬编码 `response.status = 500`** + `FMT-602 操作尚未实现`），`FMT-602` 也落在 `default: 500` 上 | **提交 `4ddb515`**（2026-10-09 真机 68 项检查后）：① 已知模块（`bucket` / `file` / `trash` / `share` / `config` / `server` / `preview`）下没有这个接口 → **`501 + FMT-602`**「接口尚未实现：<path>」（share 整组属这类）；② 完全打错的 `/api/...` → **`404 + 新错误码 FMT-017 RouteNotFound`**（退出码 3，「没有这个接口」，**只由 HTTP 兜底路由产生**）；③ 已知路由但业务找不到对象 → `404 + FMT-002`（不变）；④ `FMT-602` 从「落 `default: 500`」改成**显式 501** | 500 是在说「服务器自己坏了」，而这两种情况的真相分别是「这个功能还没做」与「路径打错了」——**状态码要说清是谁的问题**，否则调用方（浏览器、调试工具、脚本）会被引去查服务器日志。实测证据：`GET /api/nosuch` 原来就是 `500 + FMT-602 操作尚未实现：/api/nosuch` |
| 34 | 版本只有旗标实现了：窗口里敲 `version` 得到「未知命令」；横幅与 `--version` 各走一处、命令侧根本没接 | **提交 `d108c80`**：新增 `cli::version_text()`（实现就是 `return banner_text();`）作为**唯一来源**——横幅、`--version` / `-v`、`version` 命令三处共用；三种用法一份输出（`fmt.exe version` / `fmt.exe --version` / `fmt> version`，窗口里也接受 `--version` / `-v`）；`print_command_list()` 多一行 `(version)  version  打印版本与构建日期`，`help version` 有正文。**不需要服务在运行**（不连管道、不查状态、不弹 UAC）、**不写任何磁盘内容**（一次性分支在开日志器之前，`log/fmt.log` 不因此多记录，与 `--help` / `--version` 同口径） | 同一句话在三个地方各写一份就一定会漂移——横幅改了、命令忘改，用户看到的两个「版本」就会不一致；**只留一个 `version_text()` 是唯一的办法**。用例 `Cli.版本文本只有一个来源` 钉住它（断言含 `File Manager Tool` / `v1.0` / `build`，不钉具体日期——那是配置时生成的） |
| 37 | 测试只到「直接调业务层」这一层：`file delete` 弹 `abort()` 那次崩溃，**单元测试全绿**却没挡住（测试按服务端期望的形状拼请求，CLI 拼的是另一种形状）；交互式确认（`y/N`）**完全没有**自动化覆盖 | **提交 `a340d1e`**：新增端到端冒烟套件 `CliE2e`（`tests/cli_e2e_test.cpp`）——进程内起服务、把 `fmt.exe` 复制到临时数据根、`CreateProcessW` 拉起**真实 exe**、喂 stdin、合并收 stdout+stderr，断言**退出码与用户看到的文字**；覆盖 version、bucket 小写归一、上传两类拒绝、两种 `file get`、软删除 → `trash list`·`get`·`restore`、跨桶/永久删除的 `--yes` 门槛（含「拒绝之后文件仍在」）、删桶、以及**交互式 `n`/`y`**。隔离前提是新加 `ipc::pipe_name()`（`FMT_PIPE` 覆盖管道名，第 4.2 节） | 单测「复制契约」而不是「共用产出契约的代码」，就会在 CLI 真的拼请求时漏掉整类 bug——**只有让真实 exe 走用户走的路**才挡得住；而那次崩溃的路径对**所有**破坏性操作都生效，所以这套测试不是「补几条用例」，是补一层**之前不存在的验证层次**（全量约 17 秒，可每次跑） |
| 38 | 日志**无上限追加**、且「V1 不做轮转」（轮转列为未决）；`logger.hpp` 头注释里两处说法与文档冲突（「只有 Service 打开日志文件」「WARN 也进 error.log」） | **提交 `821aba3`**：`fmt.log` 超过 **5 MB** 轮转成 `fmt.log.1`（**只留一代**），`error.log` 同理；`max_log_bytes = 0` 表示不轮转。**前置条件是日志改成「每行开-写-关」**（`append_line()`）——长期持有的 `ofstream` 既挡改名、又会让另一个进程继续往已改名的文件里写。每写 64 行检查一次；改名失败不报错、下次再试；轮转后在新文件里写一行说明；打开时仍验一次可写。头注释两处按文档口径修正（`error.log` **仅 ERROR 级**——作者一度照注释改代码，被既有用例当场抓住） | 两个进程共用一个文件时，「句柄生命周期」决定「能不能改名」——所以轮转不是一个独立的开关，而是**先改写入方式、再谈轮转**；而头注释与文档冲突时必须**以文档为准并当场对齐**，否则下一次改动又会照错的那份走 |
| 36 | `service reinstall` **只有双击引导内部在用**：用户想「换一个新 exe 当宿主」只能先 `uninstall` 再 `install`——服务在跑时旧 exe 被占用，还得先停服务、再复制文件，**两次 UAC**；命令总览与 `help service` 里根本没有它 | **提交 `c573f14`**：把它接线成正式命令（`is_user_service_command()` 加入 `reinstall`、两处用法提示、命令总览、`help service` 正文）。service 子命令是**六条**，除 `status` 外都提权；`help service` 写明两个用途（更新宿主 exe / 修复被移动或删除的宿主）与**三个「不变」**（业务数据、`service.json` 的 `current_root`、数据根仍由 CLI 声明）。用例 `Cli.service子命令集合` 把集合钉住 | 更新 exe 的正确路径本来就存在，只是没暴露：`reinstall` **先停旧宿主解开文件占用**，再把注册指向**你运行的那一份**——所以既不用先复制，也只要**一次** UAC。把「本来就能做但没人能敲」的能力接出来，比让用户绕两步更值得；而「命令集合决定敲了什么会被当成什么」，所以它被从头文件暴露出来专门断言 |
| 39 | **零碎三项 + share 数据面（提交 `674d0b0` + `d5779db`）**：`trash.empty`（一次清空两级，dry_run 预检 + `force`/`--yes`，**空站不打扰**；实现上每删一项重新 list、先文件级后桶级、单条失败跳过 + `kMaxRounds`；没加 HTTP 路由）；`file.list` 的 `--sort name\|size\|id`（默认 name、`size` 大的在前、`id` 是入库顺序，**乱写 → `FMT-001`**，响应回显 `sort`；`--sort` 是本地开关）；`config.list` / `config.set max_upload_size`（只让改这一项、其余只读 → `FMT-001`，落盘失败回滚内存值）；`FMT-203` 定稿「保留（V1 未使用）」；**share 数据面**（`share_id` = 12 位随机十六进制来自 `BCryptGenRandom`、默认 20 次 + 7 天、`share get` 如实报状态、检查顺序按开发文档 §49、`share.download` 在业务锁内记账、文件进回收站即不可用且阻断 create）。**HTTP 路由一个都没加**（用户决定） | 原口径里「`config` 只有 `get/set --user/--bucket`」「`share` 整组未实现」「`trash` 只能逐条删」「`file list` 固定入库顺序」都来自**上一层实现或早期设想**，用户实机操作时发现「这些事在窗口里根本做不到」。补齐时沿用既有形状（破坏性操作走预检 + `force`、排序键乱写报错不静默兜底、只放开真正需要调的配置项），并把「模块已实现」与「接口已开放」分开记——**业务侧就绪不等于 HTTP 该开**，接口等用户定 |
| 40 | **HTTP 接口第 1、2 步 + 账号/令牌（提交 `bfd89f7` / `4b812b5` / `ff237d5` / `22c3c3e`）**：① HTTP **已开启**、监听 **`localhost:4122`**（代码默认 `host` 从 `127.0.0.1` 改成 `localhost`；线上 `server.json` 为 `enabled: true`）。② **全部 `/api/*` 要 token**，唯一例外是 `/api/ping` 与 `/api/share/<id>/download`（分享链接本身就是凭证）；缺 / 错 → **401 + `FMT-018`**；认证**只在一处**（httplib pre-routing 钩子——「漏给某条路由加认证」是这类代码最典型的事故），**没有校验器时一律 401**（fail-closed）；`X-FMT-Token` 与 `Authorization: Bearer` 都收，比较常量时间；token 从 `config list` 拿。③ **`/api/bucket*` 五条已删除**（用户明确「桶不要」）→ **404 + `FMT-017`**（故意不要 ≠ 还没做），「已知模块」里没有 bucket。④ `/api/share` 四条落地，下载端点公开但**第 3 步才做**（501 + `FMT-602`）。⑤ `data/user.json` 定稿：**没有 buckets 字段**（桶以磁盘为准）、密码只存 PBKDF2-SHA256 哈希、token 永久有效、默认账号在数据根初始化时创建（老的空 `users` 也补建，已存在不覆盖）。⑥ **测试隔离教训**：`FMT_PIPE` 管不到 SCM，新增 **`FMT_NO_SERVICE=1`**（提交 `ff237d5`；此前出过真事故：测试把**真服务**注册指向临时目录） | 用户先前的判断是「业务侧做完再定开哪几个接口」，这一轮明确了：**接口要开，但只开该开的**——认证必须**一处集中**（逐路由判断一定会漏），默认**关着就全拒**（fail-closed），桶**从 HTTP 面整体删掉**（用户不要，且 CLI 已经能管），分享下载**公开**（链接本身是凭证）。另外两条经验：**「故意不要」和「还没做」必须用不同状态码**（404 vs 501，否则调用方以为将来会有）；**跑「用户双击也会走的入口」时必须显式切断 SCM**，换管道名远远不够 |
| 41 | **HTTP 第 3 步：流式 upload / download / preview + 公开分享下载（提交 `d3aeb3d`，HTTP 接口的最后一部分）**：`POST /api/file/upload` 的**请求体就是文件内容**（旧的「请求体给服务端本地路径」那套删除）；**边收边写、边判上限**（超 `max_upload_size` 立刻中止接收并删暂存文件，`FMT-303` → 400，不是「写完再看」；暂存名 `fmt-upload-<pid>-<序号>.tmp` 沿用 `fmt-` 前缀，启动清理能收走碎片）；落盘后补算大小与 MD5 入库，**全程只有一次移动、不二次拷贝**；新增 op **`file.upload_stream`** 与函数 `prepare_staged_upload()` / `content_type_of()` / `preview_content_type()`。`download`（attachment）与 `preview`（inline）都用 `set_content_provider` **流式回**；**下载不受预览策略限制**，**预览策略只有一份**（其余 → `FMT-701` 400）。**分享下载公开**（唯一不要 token 的接口）且**先记账再放行**。`file.get` / `share.download` 新增 `path`（相对数据根），`file.list` 不加 | 前两步定的是「谁能进来」（token 一处集中 + 桶整体拿掉），这一步定「怎么搬字节」：**流式**是唯一能同时满足「大文件不占内存」和「超上限立刻停」的做法；**上传用请求体而不是路径**是客户端决定的（Java/Python 只会推流）；**下载与预览必须分开**——策略只该约束「浏览器能不能直接看」，不该拦住「用户就是要拿走这个文件」；而**公开分享下载必须先记账**，否则 §51 的「19 + 两次 = 21」会在公开端点上重演。两个坑（`drain_reader()`、下载误用预览策略）是这一步最贵的两课 |
| 35 | 数据根被别的 `fmt.exe` 抢走时**控制台上看不见**（换根只在日志里留一行；横幅也不显示服务在看哪个目录） | **提交 `8f2fbc5` + `bb7a40f`**：① 切换那一刻（`switched=true`）在 **stderr** 打印 `cli::root_switch_notice(previous, current)`（两个根 + 原因 + 怎么切回去，正斜杠）；② 交互窗口横幅区**永远多一行「数据根：…」**，与本程序所在目录不一致时再补一行说明；③ 文本抽成函数才能**断言**——这是控制台输出第一次有测试覆盖（用例 `Cli.数据根切换提示要把两个根都说清楚`） | 数据根意味着**桶、文件、回收站整体换成另一个目录的内容**，这不是例行日志，是用户必须立刻知道的事；而「服务一次只接受一条连接」（技术文档 15.1 ①）意味着**会话中途被搬走不可能发生**，只可能在「连上那一刻」——所以在这一刻报一次 + 横幅常驻显示，覆盖就是完整的，不需要在每条命令前后都查根 |

| 42 | **`data/user.json` 收敛成单用户文件（字段一个没改）**：用户要求「改成唯一一个用户的文件，字段不变保留，以后可以拓展其他字段」——**一个数据根只有一个账号**，所以这个文件就是「那个用户」：`{"version":1,"user_id":…,"username":…,"password_hash":…,"password_salt":…,"token":…,"current_bucket":…,"created_at":…,"updated_at":…,"last_login_at":…}`。九个字段与提交 `bfd89f7` 定的**完全一致**，只是不再套 `users` 数组——JSON 归类从「集合」移到「单例」（第 13.1 节）。「还没有账号」= 文件里只有 `{"version":1}`（补建缺失文件时写的就是它，账号由 `initialize_root` → `UserStore::ensure_default()` 建）。**老文件照读**：`{"version":1,"users":[{…}]}` 取第一条，下一次 `ensure_default()` 改写成单用户形状，**token / 密码哈希 / 时间戳一个都不动**；空的老数组等价于「还没有账号」，会补建。`UserStore::load/save` 由 `vector<UserRecord>` 改成单条记录（`UserRecord::empty()` 表示「还没有账号」，`save` 拒绝写空记录），另加 `Storage::make_document(1)`。**行为口径**：`ensure_default` 不再按 `config.current_user` 找名字——文件里那条记录就是这个数据根的账号，`current_user` 只决定**创建时**叫什么（名字不一致时不新建）。**158 项全绿**（157 + 净 +1：`App.老的多用户文件会被改写成单用户形状`、`App.老的空users文件要补建单用户账号` 取代 `App.已有空users文件时也要补建默认账号`） | 用户的原话是「字段不变保留，到时候可以拓展其他字段」：这条改动**只换容器、不动字段**——既去掉「一个数据根理论上能装多个账号」这个假象（HTTP 侧本来就是单账号 + token，从没有第二条账号的入口），又把文件变成以后**加字段**的自然落点：直接平铺在根上，不必先想「加到哪个用户身上」。兼容放在**读**这一侧（老形状照读 + 一次性改写），所以 v1.0 已经建好的数据根升级后 token 不变、HTTP 调用方一行都不用改 |
| 43 | **HTTP 面回补三个缺口：`GET /api/config`（只读）、`GET /api/bucket`（只读）、`DELETE /api/trash/empty`（本次改动，未提交）**：做 Java/Spring Boot 接入（`java/` 下的通道 SDK）时，客户端实测出三个「业务层早就有、HTTP 只差一条路由」的缺口，用户选定补这三条。① **`GET /api/config` → `config.list`**，**只读**（改 `max_upload_size` 会动服务进程的全局状态，仍是 CLI 的活），并且**把 `data.token` 从响应里一律删掉——任何情况下都不返回，连 `?include_token=1` 这类开关都不提供**——调用方虽然已经拿 token 认证过，但凭证不该因为「顺手」而多出现在日志、转发、工单里（响应体会进日志、被代理转发、被贴进工单，多一份副本就多一处泄漏面）；要 token 只有两条路：CLI `config list`，或直接读数据根下的 `data/user.json`。② **`GET /api/bucket` → `bucket.list`**，只读列表（`buckets[{name,is_current}]` + `count` + `current_bucket`）；**桶的写接口仍然故意不做**（`current_bucket` 是全局状态，多个 HTTP 调用方一起改会互相踩；删桶还有确认流程），所以 `/api/bucket/<其它路径>` 仍是 **404 + `FMT-017`**、`POST /api/bucket` 是 **404 + `FMT-002`**（只有 GET 有 `/api/.*` 兜底路由）。③ **`DELETE /api/trash/empty` → `trash.empty`**：`?dry_run=1` 只预检（`needs_confirm`）、`?force=1` 才真删，有内容不给 force → **400 + `FMT-016`**，**回收站本来就空则直接成功、不为空操作要确认**（沿用第 39 条的口径，也接受请求体 `{"force":true}`）。实现上抽了 `confirm_args(request)`（`?dry_run`/`?force`）给没有位置参数的 `trash.empty` 复用，`delete_args()` 改成复用它；`dispatch(..., transform)` 成为业务处理的唯一出口，新增的 `transform` 只在**成功**时改 data（目前只有 `/api/config` 用它**无条件**剥掉 token——一律不返回、连开关都不提供），失败一律原样返回。**最贵的坑是注册顺序**：`DELETE /api/trash/empty` 必须注册在 `R"(/api/trash/([^/]+))"` **之前**——已核实 `third_party/cpp-httplib/httplib.h:1036` 的 `Handlers` 是 `std::vector`、`Server::routing()` 按**注册顺序**取第一个匹配（不是按 pattern 字典序），顺序反了 `empty` 就会被当成「名字叫 empty 的条目」走 `trash.delete`（`FMT-016` 或 `FMT-400`）。测试：新增 `Server.配置桶列表与清空回收站路由`，把 `Server.管理接口要token且桶路由已下线` 改名为 `…桶写接口未提供` 并收敛断言；用例总数 **158 → 159**（`fmt_tests.exe Server.` 实测「共 7 项：通过 7，失败 0，跳过 152」，`ctest` 100% 通过）；Java 侧同步加了 `getConfig/listBuckets/emptyTrash`，**65 项全绿**（core 59 + starter 6） | 这三条接口的取舍落在同一条原则上：**能读的都开，会变成「第二个写入者」的不开**。配置读出来让客户端上传前先看上限（少踩一次 `FMT-303`）、桶列出来让客户端知道自己正在操作哪个桶——这两件事只读就够，不需要任何写入口；而桶的创建/切换/删除会改服务端全局状态、又有确认流程，继续留在 CLI。清空回收站本来只能 CLI 做，可它是「客户端迟早要做」的操作：与其让调用方自己列一遍再一条条 `DELETE /api/trash/<标识>`（既慢又会留下半清空状态），不如把已经写好、已有预检与确认语义的 `trash.empty` 直接开出来。**这条同时部分推翻了第 40 条**里「`/api/bucket*` 五条已删除、故意不要」的口径：桶**只读一条**回到 HTTP 面，写接口仍然没有——「故意不要」和「还没有」依旧用不同状态码区分（404 vs 501），只是「故意不要」的集合变小了。至于 token 一律不返回（连 `?include_token=1` 这样的开关都不提供，要 token 走 CLI `config list` 或读 `data/user.json`）：这是「最小惊讶」——一个读配置的接口顺手把访问凭证吐回来，属于写的时候觉得方便、出事时才发现到处都是副本的设计 |
| 44 | **日志占磁盘：加目录级清理 + `log clean` 命令（本次改动（未提交））**：用户提出「日志容易占磁盘，设计一个清理方法，比如启动 CLI 的时候清一下两个日志文件」。现状是**轮转已经落地**（第 38 条：`fmt.log` / `error.log` 各超 5 MB 轮转成 `.1`、只留一代），也就是一个数据根最多约 **20 MB**，而且**每个数据根各一份**。做法：新增 `cleanup_logs(log_directory, budget = 8 MB, dry_run)`（`common/logger`，与轮转同一个模块），三步——① 扫目录求和，**不超预算就一个字节都不动**（常态下只有几次 `file_size`）；② 超预算先删 `.1` 备份（纯附加物）；③ 仍超预算才把活动文件裁成**最新的一段**（`keep = max(预算/2, 256KB)`，顶部补一行「日志已裁剪：保留最近 N 字节」留断点）。只认 `fmt.log` / `error.log` 与各自 `.1`，用户手放进 `log/` 的东西一律不动；`budget == 0` 表示只删备份；目录不存在**不创建**。接两条入口——**CLI 启动**（`run()` 里、开日志器之前：日志器一握句柄就删不动/改不动）与**服务启动**（`initialize_service_context()`，服务是长期进程），清理结果写进**清理之后**的日志（模块 `Log`）；再加一条**本地命令** `log clean [--keep 8MB] [--dry-run]`（不过服务：日志是本地文件，服务可能根本不在跑），`--dry-run` 只报告。**测试 168 → 173 项全绿**（`Logger.未超预算时一个字节都不动` / `超预算先删备份再裁活动文件` / `预演只报告不动磁盘` / `预算为零只删备份不裁活动文件` / `日志目录不存在不是错误`）。见 `FMT 技术文档.md` 第 14.7 节、开发文档第 65 节 | 轮转解决的是「**一个文件**别无限长」，它管不到「**一个目录 + 多个数据根**」的总量——两代 × 两个文件 × N 个数据根的底噪就是这么来的。清理放在**启动**而不是定时器/后台线程：V1 明确不做异步日志与日志线程，启动时扫一遍目录的成本可以忽略，而且「用户下次开 CLI 就收干净」比「后台某个线程在跑」好理解、也好测。要求用户自己删文件不行（他不知道能删、也不知道删了会不会坏事），所以要有一条**明确的、可预览的**命令；`--dry-run` 是它的重点——收日志之前先让用户看见会丢什么。两个实测坑写进了文档：**自动清理必须排在显式 `log clean` 之前跳过**（否则 `--dry-run` 报「未超过上限」，预演看不到真相）、**自动清理只认固定预算**（不读 `--keep`，所以手动那一次必须由命令自己清）。另外顺手修掉一处自相矛盾：第 65 节结尾还写着「V1 不实现…轮转」，与同节的轮转口径冲突 |

| 45 | **账号改为机主自己创建：`user` 命令组 + 首启引导（本次改动，未提交）**：用户要求「第一次启动 CLI 提示要求用户创建账号 `user create <username>`，然后再要求用户输入密码，输入时显示 `*` 号，确认密码也是」，随后又定「`user delete` 删除用户数据与除 `fmt.exe`、`log/` 之外的所有文件夹，要输入密码和 `delete` 才放行（真永久删除、不进垃圾桶、要提醒不可撤销）」「`user get` 输出用户信息」。做法：**取消自动建号**（删掉 `UserStore::ensure_default()`；老版本那个账号的口令是随机生成的、谁也不知道，拿它当凭证本来就不安全），新增 `user::UserService`（`src/user/user_service.cpp`）承载 create / get / update / delete / salt / verify，CLI 加 `run_user_command()` 与 `read_password_masked()`（`ReadConsoleInputW` 逐字符读、回显 `*`、支持退格与 Ctrl+C；stdin 被重定向时按行读，脚本与 e2e 才能跑）。`user delete` 的目录删除**由 CLI 本地做**——服务正在跑、握着 `log/` 句柄，让它删自己的数据根是踩自己的脚；服务端只负责"口令对不对"这一步。**测试 173 → 180 项全绿**。见第 18.1、18.2 节 | 用户的原话是「第一次启动」，所以放在**交互模式**：一次性命令是脚本用的，卡在提示上会让脚本挂住，它们本来就在下一层拿到明确的 `FMT-002`。`user create` 在**问口令之前**先查账号（真机反馈：先让人输两遍密码、最后才说"已经有账号"），已有账号时拒绝重建并直接给出 `user delete` → `user create` 的下一步。`user get` 刻意不打印口令哈希、盐与 token：读账号信息不该顺手吐凭证 |
| 46 | **口令明文不出 CLI 进程 + 登录门禁（本次改动，未提交）**：用户要求「使用 `fmt -u <username> -p` 回车输入密码登录（密码显示 `*`），本次 CLI 关闭后又要重新登录」「`service` 命令不被限制使用，但其他业务类型的数据就会被禁用，除非登录」，并指定「后续我要通过像 mysql 一样使用 jar 包登录和使用，token 做为临时备用钥匙」，门禁范围选**方案 A（只做在 CLI）**。做法：CLI 先向服务要 `user.salt` → 本地 `password_digest(口令, 盐)`（PBKDF2-SHA256 10 万轮）→ 只把摘要发给 `user.verify` 常量时间比对；登录态只存 `Session::logged_in`（**内存，不落盘**——落盘就不叫"关掉重开要重新登录"）；门禁在 `run_interactive()` 与 `dispatch_command()` 两处，`service` / `log` / `help` / `version` / `exit` / `user get` / `user create` 放行；窗口内也能 `login` / `logout`。**服务端认证协议一个字没改**（管道与 HTTP 仍每请求认 token）。**测试 179 → 180 项全绿**（含 `-u/-p` 进窗口、窗口内 login、logout 后又被拦、未登录被拒）。见第 18.3、18.4 节 | ① **挑战-应答在密码学上走不通**：服务只存单向摘要，而 PBKDF2 不可组合（`PBKDF2(P) ≠ PBKDF2(PBKDF2(P))`），服务端无法对随机 nonce 算出同一应答值——除非存明文或"口令等价值"，那更糟。所以是"CLI 用服务给的盐算摘要、只发摘要"。② **盐必须由服务给**：老账号是**随机盐**，若一律按用户名派生，老账号口令会当场验不过、等于把机主锁在门外（真踩过，已加 `App.老账号的随机盐依然能验密与改密` 钉住）。③ 门禁选方案 A：服务端协议不动，Java/token 直连不受影响，token 就是那颗"临时备用钥匙"；要做 jar 登录接口时只加接口、不改现有认证。④ `-u/-p` 不带命令必须**进窗口**（真踩过：登录完直接 `return 0`，表现为"登录成功但窗口里什么都没发生"）。⑤ 单实例互斥体加 `FMT_MUTEX` 覆盖：否则用户开着 FMT 窗口时，e2e 的无参数交互式子进程会撞上互斥体**静默退出**（表现为"输出里只有登录成功"） |
| 47 | **`config.json` 的 `uuid` + 「密码只能改一次」（本次改动，未提交）**：用户要求「user update password 回车输入新密码确认密码，只能修改一次」，并指定判定办法「将已经创建的 `user.json` 生成一个 MD5，保存在 `config.json` 里字段 `uuid`，第一次改密码将 `user.json` 的 MD5 与 `config.json` 比对，一致说明是第一次修改允许改，改完的 MD5 肯定与之前的 `uuid` 不一致，下一次就提示不能修改、提示用户删除用户重新创建」。做法：`Config::uuid` 序列化为 `uuid`；`record_md5(record)` = `UserStore::record_json_text()`（与 `save()` 同一套序列化）的 MD5；建号/初始化写入；改密时"uuid 必须等于当前记录的 MD5"才放行，成功后把 uuid 置为**改动之前**那份记录的 MD5。**测试 178 → 179 项**（`App.初始化数据根不再自动建账号` 与新增的老账号用例）。见第 18.5、18.6 节 | 用户给的方案就是对的，直接照做。**踩过的坑必须记住**：uuid 只能取"**改之前**"的指纹——写成"保存之后再取"，等于把 uuid 设成"改完之后那份文件"的 MD5，而下一次进来加载到的正是它，于是"改前指纹 == uuid"永远成立，**可以无限次改密**（被用例钉住）。另一条隐含约束：`user.json` 在日常使用中**不能被改写**，否则指纹漂移、规则失效——`record_login` / `refresh_current_bucket` 因此都不能上请求路径 |
| 48 | **访问 token 加时效：过期一律拒绝（本次改动，未提交）**：用户要求「service 端实现 token 的时效性，过时不能用」。落地：① `data/user.json` 新增 `token_issued_at` / `token_expires_at`（本地时间 `YYYY-MM-DDTHH:MM:SS`）；② `config.json` 新增 `token_ttl_seconds`（默认 **7 天**），`config set token_ttl 7d|12h|3600`（**拒绝 0**——允许 0 就等于留了「永不过期」的后门），改它只影响**下一次签发**；③ `UserStore::find_by_token()` 先比 token 再判时效，过期回**独立错误码 `FMT-019 TokenExpired`**（与 `FMT-018` 区分：前者该去续期，后者是凭证抄错），HTTP 401 信封里带着原因与补救命令；④ 新增 `user token renew`（**凭密码摘要**，`user.token.renew` 服务 op）换发 token，旧 token 立即失效；⑤ **没有 `token_expires_at` 的老 token 按已过期处理**（升级后必须换一次——「说不清有效期的凭证不放行」）；⑥ 指纹口径调整：`config.uuid` 的指纹**排除 token 三个字段**，否则换 token 会被判成「已经改过密码」（或反过来把「已改过」抹掉）；`update_password` 同时认老口径（整份文件 MD5）以保证老账号不被锁死。**测试 180 → 185 项全绿**（`User.新建账号的token带有效期且默认七天` / `User.token过期后服务拒绝` / `User.没有有效期记录的旧token按过期处理` / `User.换发token后旧的立刻失效且密码仍只能改一次` / `Server.token过期返回FMT019而不是FMT018`，以及 `config set token_ttl` 的解析与拒绝 0）。见第 18.7 节、`FMT 技术文档.md` 第 12.3.2 节 | 用户的原话是「过时不能用」，那就不能有「永久有效」这条后门：`token_ttl_seconds = 0` 被明确拒绝。**续期必须凭密码而不是凭旧 token**——否则 token 一过期就只剩 `user delete`（数据全没）这一条路；而凭旧 token 续期又等于没有时效。**指纹必须排除 token**：`uuid` 的语义是「密码改没改过」，轮换凭证与改密码是两件事，混在一起会互相踩（真踩过的坑：uuid 取错时点导致改密可以无限次）。老 token 按过期处理是刻意的：v1.0/v2.0 的 token 是永久凭证，继续放行等于这次改动白做；代价是升级后要跑一次 `user token renew`，错误消息里直接给了这条命令 |
| 49 | **账号口径三处修正（本次改动，未提交）**：用户真机提的三条——「登录之后最后登录时间没变」「改成登录之后才能用 `user get`」「希望能输出当前 token」，并明确了改密指纹的口径：「将已经存入的**密码、盐和用户 id、创建时间**的内容进行哈希运算，存入 `config.json` 的 `uuid`」。落地：① `user.verify` 验过口令后调 `UserService::record_login()` 写 `last_login_at` / `updated_at`（写失败只记日志、不阻断登录）；② 指纹改成**只算这四项**（`user_id` / `password_hash` / `password_salt` / `created_at`），token 三件套与登录痕迹全部排除——这样「登录一次」或「换一次 token」都不会把「密码只能改一次」判歪；旧两代口径继续认（`never_changed_password()`），老账号不被锁死；③ `user get` 移进登录门禁（它现在要打 token），未登录拒之；④ `describe()` 增加 `token`，CLI 打成「访问 token：…」+「请求头：X-FMT-Token: …」。**测试 185 → 186 项全绿**（新增 `User.登录更新最后登录时间且不影响改密判定`；e2e 断言窗口内登录后 `最后登录` 有值、`user get` 给 token、未登录被拒） | 三条都是用户实测出来的，口径没有争议。**指纹必须排除"会自己动"的字段**是关键：登录写 `last_login_at`、换 token 写 token 字段，这些若进指纹，要么把机主锁死（误判已改过），要么把状态洗回去（重新放开一次改密）。用户给的四项口径正好是"账号身份 + 口令状态"，比"整份文件"稳得多。`user get` 要登录是因为它开始输出 token——凭证不该在未认证状态下给 |


以下决策在第二阶段编码前确定，**不再扩展基础架构**，各模块按此实现。

| 项目 | 决策 |
| --- | --- |
| JSON 数据格式 | 单例数据用 `{"version":1,...}`；集合数据用 `{"version":1,"<集合名>":[...]}`，见第 13 节 |
| JSON 版本策略 | 只接受明确支持的版本，未知版本直接拒绝，不降级、不猜测、不自动修改 |
| JSON 解析 | 最小但正确，支持标准核心 JSON，不容错猜测 |
| `Result` | `std::variant<T, Error>`，成功值与错误值互斥 |
| `Status` | `std::variant<std::monostate, Error>`，用于无返回值操作 |
| 错误码 | `FMT-NNN` 统一定义，编号稳定、禁止复用、语义不修改；清单见 `FMT 项目架构.md` 附录 A |
| 日志 | 文件为主；DEBUG 只进文件，WARN/ERROR 同时输出控制台 |
| 哈希 | MD5 |
| 路径 | 第二阶段引入 `PathManager`，上下文对象，不做全局单例 |
| 命名空间 | `fmt` 保持不变；第三方库 vendor 到 `third_party/`，不污染业务命名空间 |
| 第三方依赖 | vendor `nlohmann/json` + `cpp-httplib` 到 `third_party/`，`/MT` 静态链接 CRT，产物只有一个 `fmt.exe`。**URL 下载另用系统库 `winhttp`**（提交 `a2b6cd1`）：它属于 Windows 本身，不算第三方依赖，也不随产物分发任何 DLL——所以「只 vendor 两个单头文件」的前提没有被破坏，第 33.2 节 |
| 单一可执行文件 | `fmt.exe` 三种形态：CLI 形态 / Service 形态 / 提权短命副本；manifest 为 `asInvoker` |
| service 命令 | 有 `install` / `uninstall` / `start` / `stop` / `status` 五条（提权副本另有 `reinstall` 组合操作），无 `pause`、无 `delete`，命令不带 `--` 前缀 |
| service 提权 | 四条动作命令（`install` / `uninstall` / `start` / `stop`）含 `reinstall` 一律走 UAC 提权，结果经结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 回传父进程打印（命令行 `--elevated <op> --result "<路径>"`，op 五种），见第 126 节；数据根不可写时退回 `%TEMP%` 并记一行 WARN。**`status` 不提权**：只读查询，不弹 UAC、不生成结果文件 |
| CLI 通道 | 命名管道 `\\.\pipe\fmt.control`（帧 = 4 字节长度 + JSON）；HTTP `localhost:4122` 只给浏览器 |
| 数据根 | 由 CLI 用 `hello` 帧声明（自身 exe 所在目录）；服务维护当前数据根，切换不删旧数据；响应回填 `switched` / `previous_root`，CLI **在 stderr 打印换根提示**（`root_switch_notice()`，提交 `8f2fbc5`）
并记一行日志「数据根切换：旧 -> 新」（**原口径「只进日志，不刷控制台」已作废**，第 127.1 节） |
| 初始化归属 | Service 与 CLI **共用同一套幂等规则**（`ensure_root` / `check_root`）：六个目录 + 六个默认 JSON；Service 在启动/换根时执行、CLI 在双击时对自己数据根执行；两边都只补缺失，已有的读一遍确认，损坏 JSON 只报告不重置（不删除、不覆盖、不改名），见第 91～92 节 |
| 服务启动失败上报 | `ServiceMain` 初始化失败时把 FMT 编号写进 `dwServiceSpecificExitCode` 并置 `dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR`；CLI 通过统一查询接口读回并打印「服务启动失败：FMT-008 配置错误」，见第 75、99、126 节 |
| 状态查询接口 | `service::State` + `service::StatusInfo{state, wait_hint_ms, win32_exit_code, service_exit_code}` + `query_status()` / `query_state()` / `installed_binary_path()` / `last_start_failure()`；「未安装」是**正常结果**（`State::NotInstalled`）而非错误，旧的「`QueryServiceStatus` + `map_state`」写法作废。注意 `State` 是状态枚举，`ServiceState` 是 `service.json` 结构体，见第 70、126 节 |
| 等待落定 | 不写死时长：按 SCM 的 `dwWaitHint` 自适应，夹在 100 ms – 2000 ms（`kMinStepMs` / `kMaxStepMs`），兜底上限 30 秒（`kSettleCapMs`），状态一旦不是等待类就立即结束；实现是 CLI 内的 `settle_state()`。旧口径的「约 8 秒」作废，见第 70、75 节 |
| 结构化输出 | **V1 不做**：没有 `--json`，也不预留参数名；机器可读通道是命令退出码（`0` / 未安装 `FMT-601` → `8`），人类可读通道是固定顺序的几行文本，见第 70、127.4 节 |
| 双击引导 | 单实例 → 数据根体检与补齐 → 查 SCM（走 `query_status()`；未安装 install / 运行中不动 / 已安装未运行提权 start 并用 `settle_state()` 等它落定）→ 仍没起读失败编号，命中数据根/配置类错误码（FMT-005/006/007/008/009/011/013/014）不重装、其余提权 reinstall 一次 → 宿主 exe 丢失询问 reinstall → hello 声明数据根 → 横幅与提示符，见第 75 节 |
| 服务状态文件 | `%ProgramData%\FMT\service.json`（当前数据根 + 安装信息），不属于业务数据 |
| 横幅与版本 | 程序名固定 `File Manager Tool`，横幅与 `--version` 共用 `banner_text()`：`File Manager Tool  v1.0  ( build  <CMake 配置时生成的日期> )`；日期来自 `FMT_BUILD_DATE`（`%Y.%m.%d` 本地时间）→ `version.hpp` 的 `BUILD_DATE`；用法标题 `用法：fmt.exe [命令]`，见第 127.1 节 |
| 控制台约定 | 控制台只留交互与异常：横幅、提示符、命令结果、stderr 上的异常。双击时的数据根体检结果（新建目录 / 新建文件 / 完整）、服务当前状态只进日志（模块 `Cli` / `Service`）；**换根通知是例外**（提交 `8f2fbc5`）：切换时 stderr 打印 `root_switch_notice()`，交互窗口横幅永远显示「数据根：…」（**原口径「换根通知也只进日志」已作废**），见第 65、127.1 节 |
| help 命令 | `--help` 打印带横幅与退出码表的完整用法（内含命令总览）；`help` 不带参数只列命令、`help <组>` 打印该组详情（`service` / `bucket` / `file` / `share` / `trash` / `help` / `version` / `exit`），交互式与一次性都支持，不提权、不连服务、不写日志；`help <未知组>` → stderr 一行 + `FMT-001` / 退出码 2；`exit` / `quit` 是正式命令，见第 68、95、127.2 节 |
| CLI 界面 | 横幅 `File Manager Tool  v1.0  ( build  2026.10.09 )` + `Service Running...` + **`数据根：…`**（提交 `8f2fbc5`），提示符 `fmt> `（打印前先输出一个空行，空命令不重复空行），正常 → stdout / 错误 → stderr，见第 127 节 |
| Bucket 名称统一小写（提交 `9c3d2cb`） | `create` 先 `to_lower()`（只折叠 ASCII）再校验再建目录，`WORK` 建成 `work`，`renamed` 为真时回 `note` 让 CLI 提示；`use` / `get` / `delete` 用 `canonical_name()` 规范化到**磁盘上的实际名字**，`current_bucket`、`file.json` 的 `bucket`、`.original` 的 `original` 只留一份拼写，见第 27～30 节 |
| 文件名的保留形状（提交 `9c3d2cb`） | 与 `file_id` 同形（`fmt-YYYYMMDD-N`）的名字是**保留形状**：上传时 `FMT-106 FileNameLikeFileId` 拒绝（退出码 2，`looks_like_file_id()`，属 `FMT-1xx` 文件名校验、与 Windows 保留设备名同类）；旧数据里已有的这种名字，`file delete` 在两条索引命中不同记录时报 `FMT-001` 歧义并点名两条记录，**只给 delete 加**，见第 25、42、43 节 |
| `file get` 的回收站字段（提交 `9c3d2cb`） | 命中回收站记录时在 `is_trash` / `trash_reason` 之外**增加** `trash_path`（相对数据根、正斜杠）；仓库里没有该文件时**不返回** `path`（设计如此），CLI 多打一行「回收站路径：…」。按 `file_id` 查是全局含回收站、按文件名只查当前用户的正常文件，见第 42 节 |
| `iequals()` 的实现约束（提交 `9c3d2cb`） | **只折叠 ASCII**：`>= 0x80` 的字节原样比较，不交给 `std::tolower`（`setlocale` 一被调用就会改坏 UTF-8 名字）。「凡按名字/标识定位一律不区分大小写」这条口径靠它兑现。**提交 `6a40742` 补齐**：`FileService::get_by_id()` 也改用 `iequals`，标识比较两处一致，见第 42 节 |
| 破坏性操作先检查再确认（提交 `711da4c`；缺口收尾 `6a40742`） | `file.delete` / `trash.delete` / `bucket.delete` 支持 `args.dry_run = true`（HTTP `?dry_run=1`）的**只读预检**；CLI 先打印情况（目标属于哪个桶、两条歧义候选、永久删除会毁掉什么）、再问 `确认执行？(y/N)`（一次性命令要 `--yes`），**用户同意前不发任何破坏性请求**；歧义 / 同名冲突 / 随桶删除 / 数据缺失是 `blocked`（y/N 解决不了，改用 `file_id`）。需要显式确认的操作缺 `force` → **`FMT-016 ConfirmRequired`**（退出码 2、HTTP 400，不再复用 `FMT-001`）；服务端仍独立校验 `force`。**提交 `18f16ca` 起「不要继续」的退出码**：预检自身的错误码原样透出（`FMT-002` → 3、通信失败 → 8）、`blocked` 与缺 `--yes` 是 2、用户取消是 0；缺 `--yes` 的提示语也带上了 `FMT-016`。见第 43、59、60、82、127.7 节 |
| 粘贴污染清理与不可见字符（提交 `a9af276`） | `clean_user_path()` 清掉不可见格式字符（`U+00A0` / `U+00AD` / `U+200B`–`U+200F` / `U+202A`–`U+202E` / `U+2060`–`U+2064` / `U+2066`–`U+2069` / `U+FEFF`）与**成对**引号、首尾空白；在 `argument()`（所有位置参数的唯一入口，CLI 与 HTTP 共用）一处收口，`prepare_upload()` 再清一次来源与显式文件名。清掉后仍找不到 → `FMT-002` 且**点名码位**（`U+202A`、`U+202C`…）；**名字里不允许**这些字符：`validate_file_name()` → `FMT-101`、`validate_bucket_name()` → `FMT-202`。见第 25、33.2、35 节 |
| 回收站是一份两级视图（提交 `0fc242b`；列表计数 `18f16ca`） | 新建 `src/trash/` 的 `TrashService`：组合 `FileService`（文件级，权威 = `file.json`，含 `deleted_at`）与 `BucketService`（桶级，权威 = `trash/<user>/.original`），合并 `TrashEntry` 列表 + 跨命名空间标识解析；**`data/trash.json` 不再写入**（只读兼容）。`trash.list` → `{entries, count, files, buckets}` 并标出 `[文件]` / `[桶]`；**桶级条目的 `files`/`bytes` 在列表里就对每个 `present` 条目遍历一次目录算出来**（代价：`trash list` 不是纯索引查询，显式命令可接受）；`get` / `restore` / `delete` → `{entry, message?}`；回退的三种硬拒绝（`FMT-401` / `FMT-402` / `FMT-002`）都是 `blocked`；`bucket.delete` 也有预检（非空桶提醒「只能整体恢复这个桶」，缺 `force` → `FMT-016`），见第 30、52～55、59 节 |
| 目标 Bucket 已存在时回退改走**合并**（本次修复，取代原来的 `FMT-401` 整单拒绝） | **问题**：`file delete a3.jpg` → `bucket delete test` → `trash restore a3.jpg`（这一步会把 `repository/<用户>/test/` 建出来）→ `trash restore test` 得到「FMT-401 回退失败：Bucket 已存在」——回收站里的其它文件一个都拿不回来。**新口径**：目标 Bucket 已存在不再等于失败，而是**合并**——`BucketService::check_merge()` 只读预检数出「要搬几个、目标已有几个、占多少字节」，并同时按磁盘文件树与目标路径两处判定同名冲突；有冲突 → **`FMT-403 RestoreMergeConflict`**（退出码 4、HTTP 409，**一个文件都不搬**，消息列出冲突名字并提示改名或永久删除）；没冲突 → `needs_confirm`，缺 `force` → `FMT-016`，确认后 `merge()` 逐个搬文件进目标桶（**不覆盖、不改名**），全部搬完才摘 `.original`、只翻真正搬过去的 `file.json` 记录、收掉只剩空目录的条目。CLI 把 `trash.restore` 加进「先预检再确认」清单（普通回退预检 `needs_confirm=false`，不受影响）。服务端闸门是 `TrashService::restore(identifier, confirmed)`；预检多带 `merge` / `merge_target_exists` / `merge_files` / `merge_bytes` / `target_files` / `conflicts?`。见第 54～56.3、112、113 节 | 目标桶往往就是**回退文件时顺手建出来的空目录**，它不该把整桶数据锁死；但「自动合并」会在用户没看见的情况下改动现有桶，「自动改名」会让 `file.json` 的 `bucket` 与磁盘路径对不上——所以合并**必须确认**、冲突**必须由用户处理**。`FMT-403` 不动 `FMT-401` 的编号与语义（文件级回退撞名仍然是 `FMT-401`），只是按「编号只能追加」的规矩补一个新的下位错误码 |

## 附录 A.1 错误码枚举

`ErrorCode` 定义在 `common/error`，枚举名与编号的对应关系以
`FMT 项目架构.md` 附录 A 的表格为准。声明形式：

```cpp
enum class ErrorCode {
    // FMT-001 ~ FMT-018：通用、JSON、路径（FMT-016 ConfirmRequired 提交 711da4c 追加：
    //   该操作需要显式确认（force），退出码 2，HTTP 400——永久删除 / 跨 Bucket 删除 /
    //   非空桶删除缺 force 时用它，不再复用 FMT-001；
    //   FMT-017 RouteNotFound 提交 4ddb515 追加：没有这个接口，退出码 3，HTTP 404——
    //   只由 HTTP 兜底路由产生，管道入口没有「路由」概念；
    //   FMT-018 Unauthorized 提交 4b812b5 追加：缺少 / 无效的访问 token，退出码 5，
    //   HTTP 401——由 httplib 的 pre-routing 钩子一处拒绝，/api/ping 与分享下载除外）
    InvalidArgument,
    ConfirmRequired,
    RouteNotFound,
    Unauthorized,
    FileNotFound,
    // ... 见架构文档附录 A 完整清单

    // FMT-100 ~ FMT-106：文件名校验（FMT-106 FileNameLikeFileId 提交 9c3d2cb 追加：
    //   名字与 file_id 同形 fmt-YYYYMMDD-N，退出码 2，属「保留形状」）
    // FMT-200 ~ FMT-203：Bucket
    // FMT-300 ~ FMT-305：上传下载
    // FMT-400 ~ FMT-403：Trash（403 = 合并回退撞上同名文件，见第 56.3 节）
    // FMT-500 ~ FMT-503：Share
    // FMT-600 ~ FMT-603：Service
    // FMT-700 ~ FMT-701：HTTP
};
```

编号到字符串（`"FMT-001"`）的转换由 `common/error` 集中实现，业务代码不写字符串。