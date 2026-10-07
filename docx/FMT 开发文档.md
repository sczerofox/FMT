# FMT 开发文档

> 项目名称：FMT
> 项目类型：Windows 文件管理系统
> 当前版本：V1
> 对应架构文档：`FMT 项目架构.md`
> 开发平台：Windows
> 构建工具：CMake + Ninja + Visual Studio Build Tools
> 主程序：`fmt.exe`（单一可执行文件，三种形态：CLI 形态 / Service 形态 / 提权短命副本）
> 文档状态：V1 开发规范（`arch-restart` 重构版）
> 本次重构差异与决策索引：`FMT 重构设计.md`

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
cpp-httplib         HTTP Server（Service 侧）
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

---

# 5. 运行目录

编译完成后：

```text
<数据根>/
├── fmt.exe
├── repository/
├── trash/
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

数据根（`FMT_ROOT`）**由 CLI 声明**：CLI 连接服务时用 `hello` 帧带上自己 exe 所在目录
（`GetModuleFileNameW` 取父目录），服务把该目录作为「当前数据根」。切换数据根时**不删除旧根数据**，
只对新根做幂等初始化（见第 91～94 节）。服务的 `hello` 响应会回填本次是否发生了切换：

```json
{ "id":1, "ok":true, "data":{ "root":"D:/FMT2", "switched":true, "previous_root":"D:/FMT" } }
```

`switched` 为真表示这次声明**导致服务切换了数据根**，此时才带 `previous_root`；CLI 打印
`数据根切换：旧 -> 新`。未切换时不含这两个字段（`switched` 缺省视为 false）。

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
清理    Service 启动时删除 temp/ 下以 fmt- 开头的遗留文件（上次异常退出留下的提权结果等），
        用户手放进去的其它文件一律不动；删除数量记一行 INFO。
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

只有临时文件写入成功并验证通过后，才替换正式配置。

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

单例数据（`config.json`、`server.json`）：

```json
{
    "version": 1,
    ...
}
```

集合数据（`file.json`、`share.json`、`trash.json`、`user.json`）：

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
user.json   -> users
```

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
  "is_trash": false
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
```

---

# 17. Trash 数据结构

V1 文件 Trash 记录暂定：

```json
{
  "file_id": "fmt-20261005-0",
  "file_name": "test.txt",
  "original_path": "repository/小谷/工作/2026/10/05/test.txt",
  "trash_path": "trash/小谷/工作/2026/10/05/test.txt",
  "deleted_at": "2026-10-05T20:00:00",
  "type": "file"
}
```

Bucket Trash 后续可以扩展：

```text
type = bucket
```

Bucket 不生成 `file_id`。

---

# 18. User 数据

V1 不实现完整用户系统。

保留：

```text
data/user.json
```

作为未来扩展入口。

当前用户由：

```text
config.json
```

中的：

```text
current_user
```

确定。

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

---

# 21. Trash 路径

Trash 保持原有层级。

例如：

```text
trash/
└── 小谷/
    └── 工作/
        └── 2026/
            └── 10/
                └── 05/
                    └── test.txt
```

这样可以方便恢复。

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
Bucket 删除
Bucket 恢复
```

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
current_bucket = name
 ↓
保存 config.json
```

`bucket use` 不修改 Bucket 本身。

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

---

# 32. File Upload

命令：

```text
file upload <URL>
```

完整流程：

```text
1. 检查当前用户
2. 检查 current_bucket
3. 校验 URL
4. 请求远程资源
5. 创建临时文件
6. 下载数据
7. 检查下载是否完整
8. 检查文件大小
9. 计算 MD5
10. 检查 MD5 去重
11. 获取用户输入文件名
12. 检查文件名冲突
13. 生成 file_id
14. 移动临时文件
15. 写入 file.json
16. 完成
```

---

# 33. Upload URL

支持：

```text
http://
https://
```

不支持：

```text
file://
```

等非 HTTP/HTTPS URL。

URL 必须进行基本合法性检查。

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

---

# 37. MD5 去重

如果已经存在相同 MD5：

```text
上传终止
```

向用户提示：

```text
该文件已经存在。
```

不：

```text
创建新 file_id
复制文件
修改旧文件名
```

---

# 38. 文件名冲突

如果：

```text
同用户
+
正常文件
+
相同 filename
```

则：

```text
拒绝
```

提示用户修改文件名。

不能自动：

```text
test(1).txt
test_1.txt
```

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

---

# 43. File Delete

命令：

```text
file delete <file_id>
```

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
更新 trash.json
```

文件：

```text
file_id
```

不改变。

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
share create
share get
share list
share delete
```

---

# 47. Share Create

命令：

```text
share create <file_id>
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

---

# 52. Trash Service

负责：

```text
trash list
trash get
trash restore
trash delete
```

---

# 53. Trash List

命令：

```text
trash list
```

只显示：

```text
is_trash = true
```

对应对象。

可以区分：

```text
file
bucket
```

---

# 54. Trash Restore

恢复前：

```text
检查 Trash 记录
 ↓
检查原始位置
 ↓
检查目标 Bucket
 ↓
检查文件名冲突
```

无冲突：

```text
恢复实际文件
 ↓
is_trash = false
 ↓
更新 metadata
 ↓
更新 trash.json
```

---

# 55. 冲突恢复

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

---

# 56. Bucket 部分恢复

Bucket 恢复：

```text
检查 Bucket
 ↓
检查内部文件
 ↓
逐个判断
```

例如：

```text
a.txt → 无冲突 → 恢复
b.txt → 无冲突 → 恢复
c.txt → 冲突 → 保留 Trash
```

最终 Bucket 可以处于：

```text
部分恢复
```

状态。

---

# 57. Bucket 恢复的数据原则

Bucket 本身没有 `file_id`。

Bucket 下文件只修改：

```text
is_trash
```

例如：

```text
true
```

变为：

```text
false
```

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

如果：

```text
原 Bucket 已永久删除
```

此时：

```text
trash restore <file_id>
```

必须要求用户指定已有 Bucket。

不能：

```text
自动创建 Bucket
```

---

# 59. Permanent Delete

命令：

```text
trash delete <id>
```

这是永久删除。

执行前要求明确确认。

流程：

```text
用户确认
 ↓
检查 Trash
 ↓
删除实际数据
 ↓
删除 File metadata
 ↓
删除 Trash metadata
 ↓
清理相关 Share
```

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

中的关联记录进行清理。

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
├── fmt.log        全部日志
└── error.log      仅 ERROR 级
```

不放入 `data/`，因为日志不是业务数据。

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
`<数据根>/log/fmt.log`（`error.log` 仅 ERROR 级，同样追加），一次一行写入；
MSVC 文件流是共享模式，因此不再存在「两个进程争抢同一日志文件」的问题。

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
2. --help / --version / help / exit 不写日志、不创建任何目录（它们不该在磁盘上留下东西）。
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

V1 **不实现**异步日志、日志线程、日志队列、压缩、轮转、ELK 或复杂配置。
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

`help`（不带参数）只列命令、不加描述：

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

`help <组>` 支持 `service` / `bucket` / `file` / `share` / `trash` / `help` / `exit`
（`quit` 等同 `exit`）。`help service`：

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
「服务端尚未实现，现在返回 FMT-602」。

`help <未知组>` → stderr 打印 `没有 <组> 的帮助；输入 help 查看命令列表`，退出码 2（`FMT-001`）。
`help` / `--help` 全程**不提权、不连服务、不写日志**。

`service` 命令**不带 `--` 前缀**：旧写法 `fmt.exe --service install` 作废；
service 有 `install` / `uninstall` / `start` / `stop` / `status` 五条，**没有 pause，也没有 delete**
（旧 `delete` 已更名为 `uninstall`）；其中 `status` 是查询命令，不提权、不弹 UAC。
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
监听 HTTP 127.0.0.1:4122
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
五条命令都直连 SCM，不走命名管道、不走 HTTP
四条动作命令（install / uninstall / start / stop）一律走 UAC 提权
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
     switched=true 时记一行日志「数据根切换：旧 -> 新」（只进日志）
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
CLI 记一行日志「数据根切换：旧 -> 新」（只进日志，不刷控制台）
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
浏览器   ──HTTP/HTTPS 127.0.0.1:4122──────→ service 层
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
`switched` 为真时 CLI 记一行日志 `[Service] 数据根切换：旧 -> 新`（只进日志）。

响应信封与 HTTP 完全一致，一份信封两处复用；超时：连接时管道不存在最多重试 **5 秒**
（权限类错误不重试）、普通命令 30 秒。

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
 │        响应里置 switched=true 并回填 previous_root，CLI 记一行日志「数据根切换：旧 -> 新」（只进日志，不刷控制台）
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
127.0.0.1:4122
```

---

# 79. HTTP Host

如果：

```text
host = 127.0.0.1
```

只允许本机访问。

如果：

```text
host = 0.0.0.0
```

允许监听所有网络接口。

局域网访问功能以后正式开发时再增加相应安全控制。

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

内部错误。

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
data/user.json         {"version":1,"users":[]}
data/file.json         {"version":1,"files":[]}
data/share.json        {"version":1,"shares":[]}
data/trash.json        {"version":1,"trash":[]}
```

默认 JSON 一律「写 `.tmp` → 验证 → 替换」，已存在的文件不改、不删、不覆盖
（见第 11 节、第 13 节）。已存在的 JSON 会被**真正读一遍**（解析 + 版本检查）来确认完整性：
读不出来或版本不受支持的**只报告、绝不重置**——日志记一行
`[Cli] 数据根损坏（未自动修复）：data/file.json`，异常同时送到 stderr。

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

V1 可以要求用户先设置当前用户。

用户系统正式开发后，再替换为正式登录机制。

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
├── file
│   ├── upload <url>
│   ├── list
│   ├── get <file_id>
│   ├── get <filename>
│   └── delete <file_id>
│
├── share
│   ├── create <file_id>
│   ├── get <share_id>
│   ├── list <file_id>
│   └── delete <share_id>
│
├── trash
│   ├── list
│   ├── get <id>
│   ├── restore <id>
│   └── delete <id>
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
service 有 install / uninstall / start / stop / status 五条
没有 pause，也没有 delete（旧 delete 更名为 uninstall）
业务命令（bucket / file / share / trash）经命名管道交给服务执行
service 命令直连 SCM：install / uninstall / start / stop 走 UAC 提权，
                        status 是查询、不提权、不弹 UAC
help / --help 是本地命令：不连服务、不提权、不写日志
exit / quit 是交互循环里的正式命令（help exit 有说明）
```

帮助有两个入口：`--help` 打印带横幅与退出码表的完整用法（内含命令总览）；`help` 不带参数
只列命令、`help <组>` 打印该组详情（`service` / `bucket` / `file` / `share` / `trash` /
`help` / `exit`），交互式与一次性都支持；`help <未知组>` → stderr 一行 + 退出码 2（`FMT-001`）。
各子命令的详细说明必须与实际命令一致（见第 68 节）。

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
```

↓

```text
阶段 5
File
Upload
Trash
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
  并记一行日志「数据根切换：旧 -> 新」（只进日志）
```

---

# 100. 阶段 4：Bucket

实现：

```text
bucket create
bucket list
bucket get
bucket use
bucket delete
```

首先确保：

```text
current_bucket
```

逻辑稳定。

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

---

# 102. 阶段 5：Upload

实现：

```text
file upload <URL>
```

完成：

```text
HTTP/HTTPS 下载
临时文件
大小限制
MD5
文件名冲突
File ID
metadata
```

---

# 103. 阶段 5：File 操作

实现：

```text
file list
file get <file_id>
file get <filename>
file delete <file_id>
```

验证：

```text
正常文件
+
当前 Bucket
+
文件名唯一性
```

---

# 104. 阶段 5：Trash

实现：

```text
trash list
trash get
trash restore
trash delete
```

重点测试：

```text
文件删除
文件恢复
文件名冲突
Bucket 删除
Bucket 部分恢复
Bucket 永久删除
```

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

HTTP 作用于**当前数据根**，默认只监听 `127.0.0.1:4122`；CLI 不依赖 HTTP（见第 76 节）。

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

---

# 112. Trash 测试

必须测试：

```text
删除文件
恢复文件
恢复冲突
永久删除
删除 Bucket
恢复 Bucket
Bucket 部分恢复
Bucket 永久删除
```

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
  hello 回执 switched=true 且 CLI 记一行日志「数据根切换：旧 -> 新」（只进日志，不刷控制台））
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
HTTPS
复杂 API 鉴权
```

后续版本根据实际需求增加。

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
├── HTTPS
├── 用户系统
├── 权限系统
└── 更完善的 Preview
```

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
9. Bucket 下文件通过 is_trash 管理
10. 冲突恢复不覆盖、不改名
11. current_bucket 无效时不自动选择
12. max_download_count 属于 Share
13. Share 默认最大下载次数为 20
14. File 本身不保存下载限制
15. JSON 损坏不能静默重置
16. 关键文件操作必须具备事务式处理
17. Windows Service V1 保持简单（五条命令 + SCM + Recovery，不自建 watchdog）
18. CLI（命名管道）与 HTTP 使用统一业务核心
19. 单一 fmt.exe，三种形态：CLI / Service / 提权短命副本
20. manifest 为 asInvoker，绝不 requireAdministrator
21. service 有 install / uninstall / start / stop / status 五条，没有 pause、没有 delete
22. service 的四条动作命令（install / uninstall / start / stop）一律走 UAC 提权，不做免提权优化；
    status 是查询命令，不提权、不弹 UAC
23. CLI 走命名管道，HTTP 只给浏览器（CLI 不走 HTTP）
24. 数据根由 CLI 声明，服务维护当前数据根，切换不删旧数据；
    hello 响应回填 switched / previous_root，CLI 据此记一行日志「数据根切换：旧 -> 新」（只进日志）
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

为上层设计依据；本次重构的差异说明与决策索引见：

```text
FMT 重构设计.md
```

本次更新已并入的冻结决策：

```text
单一 fmt.exe 三种形态（manifest 为 asInvoker）
service install / uninstall / start / stop / status（无 pause、无 delete）
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

CLI 启动时打印（`banner_text()` 产出，与 `--version` 同一串）：

```text
File Manager Tool  v1.0  ( build  2026.10.08 )
Service Running...
```

服务未运行时第二行改为：

```text
File Manager Tool  v1.0  ( build  2026.10.08 )
Service Stopped...
```

| 项 | 约定 |
| --- | --- |
| 程序名 | 固定 `File Manager Tool`（文档名/工程名仍叫 FMT，只有横幅用这个名字） |
| 版本部分 | `v<MAJOR>.<MINOR>`，当前 `v1.0`；工程版本仍是 `1.0.0` |
| 构建日期 | CMake 配置时生成（`FMT_BUILD_DATE`，`%Y.%m.%d` 本地时间），每次重新配置都会变 |
| 单一来源 | 横幅与 `--version` 调同一个 `banner_text()`，不各写一份；源码里不得硬编码版本号或日期 |
| 用法标题 | `--help` 里是 `用法：fmt.exe [命令]`，不再用 `FMT 1.0.0 - Windows 文件管理系统` 这类旧标题 |

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

**换根这种例行状态变化也只进日志、不刷控制台**（要看就问 `service status`，
它会打印「服务数据根」）：

```text
[Service] 数据根切换：D:/FMT -> D:/FMT2        ← 仅当 hello 回执 switched=true
[Service] 服务数据根已经是：D:/FMT2            ← 未切换时
```

这样归位守的是第 65 节那条老原则：**控制台负责用户交互与重要异常，日志文件负责完整运行记录**。

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

`help` 的输出：

```text
fmt> help
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
`--help` / `--version` / `help` / `exit` 不写日志、不创建任何目录。
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

---

# 附录 A. 已冻结决策（第二阶段开工前）

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
| 第三方依赖 | vendor `nlohmann/json` + `cpp-httplib` 到 `third_party/`，`/MT` 静态链接 CRT，产物只有一个 `fmt.exe` |
| 单一可执行文件 | `fmt.exe` 三种形态：CLI 形态 / Service 形态 / 提权短命副本；manifest 为 `asInvoker` |
| service 命令 | 有 `install` / `uninstall` / `start` / `stop` / `status` 五条（提权副本另有 `reinstall` 组合操作），无 `pause`、无 `delete`，命令不带 `--` 前缀 |
| service 提权 | 四条动作命令（`install` / `uninstall` / `start` / `stop`）含 `reinstall` 一律走 UAC 提权，结果经结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 回传父进程打印（命令行 `--elevated <op> --result "<路径>"`，op 五种），见第 126 节；数据根不可写时退回 `%TEMP%` 并记一行 WARN。**`status` 不提权**：只读查询，不弹 UAC、不生成结果文件 |
| CLI 通道 | 命名管道 `\\.\pipe\fmt.control`（帧 = 4 字节长度 + JSON）；HTTP `127.0.0.1:4122` 只给浏览器 |
| 数据根 | 由 CLI 用 `hello` 帧声明（自身 exe 所在目录）；服务维护当前数据根，切换不删旧数据；响应回填 `switched` / `previous_root`，CLI 记一行日志「数据根切换：旧 -> 新」（只进日志，不刷控制台） |
| 初始化归属 | Service 与 CLI **共用同一套幂等规则**（`ensure_root` / `check_root`）：六个目录 + 六个默认 JSON；Service 在启动/换根时执行、CLI 在双击时对自己数据根执行；两边都只补缺失，已有的读一遍确认，损坏 JSON 只报告不重置（不删除、不覆盖、不改名），见第 91～92 节 |
| 服务启动失败上报 | `ServiceMain` 初始化失败时把 FMT 编号写进 `dwServiceSpecificExitCode` 并置 `dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR`；CLI 通过统一查询接口读回并打印「服务启动失败：FMT-008 配置错误」，见第 75、99、126 节 |
| 状态查询接口 | `service::State` + `service::StatusInfo{state, wait_hint_ms, win32_exit_code, service_exit_code}` + `query_status()` / `query_state()` / `installed_binary_path()` / `last_start_failure()`；「未安装」是**正常结果**（`State::NotInstalled`）而非错误，旧的「`QueryServiceStatus` + `map_state`」写法作废。注意 `State` 是状态枚举，`ServiceState` 是 `service.json` 结构体，见第 70、126 节 |
| 等待落定 | 不写死时长：按 SCM 的 `dwWaitHint` 自适应，夹在 100 ms – 2000 ms（`kMinStepMs` / `kMaxStepMs`），兜底上限 30 秒（`kSettleCapMs`），状态一旦不是等待类就立即结束；实现是 CLI 内的 `settle_state()`。旧口径的「约 8 秒」作废，见第 70、75 节 |
| 结构化输出 | **V1 不做**：没有 `--json`，也不预留参数名；机器可读通道是命令退出码（`0` / 未安装 `FMT-601` → `8`），人类可读通道是固定顺序的几行文本，见第 70、127.4 节 |
| 双击引导 | 单实例 → 数据根体检与补齐 → 查 SCM（走 `query_status()`；未安装 install / 运行中不动 / 已安装未运行提权 start 并用 `settle_state()` 等它落定）→ 仍没起读失败编号，命中数据根/配置类错误码（FMT-005/006/007/008/009/011/013/014）不重装、其余提权 reinstall 一次 → 宿主 exe 丢失询问 reinstall → hello 声明数据根 → 横幅与提示符，见第 75 节 |
| 服务状态文件 | `%ProgramData%\FMT\service.json`（当前数据根 + 安装信息），不属于业务数据 |
| 横幅与版本 | 程序名固定 `File Manager Tool`，横幅与 `--version` 共用 `banner_text()`：`File Manager Tool  v1.0  ( build  <CMake 配置时生成的日期> )`；日期来自 `FMT_BUILD_DATE`（`%Y.%m.%d` 本地时间）→ `version.hpp` 的 `BUILD_DATE`；用法标题 `用法：fmt.exe [命令]`，见第 127.1 节 |
| 控制台约定 | 控制台只留交互与异常：横幅、提示符、命令结果、stderr 上的异常。双击时的数据根体检结果（新建目录 / 新建文件 / 完整）、服务当前状态、**以及换根通知**都只进日志（模块 `Cli` / `Service`），见第 65、127.1 节 |
| help 命令 | `--help` 打印带横幅与退出码表的完整用法（内含命令总览）；`help` 不带参数只列命令、`help <组>` 打印该组详情（`service` / `bucket` / `file` / `share` / `trash` / `help` / `exit`），交互式与一次性都支持，不提权、不连服务、不写日志；`help <未知组>` → stderr 一行 + `FMT-001` / 退出码 2；`exit` / `quit` 是正式命令，见第 68、95、127.2 节 |
| CLI 界面 | 横幅 `File Manager Tool  v1.0  ( build  2026.10.08 )` + `Service Running...`，提示符 `fmt> `（打印前先输出一个空行，空命令不重复空行），正常 → stdout / 错误 → stderr，见第 127 节 |

## 附录 A.1 错误码枚举

`ErrorCode` 定义在 `common/error`，枚举名与编号的对应关系以
`FMT 项目架构.md` 附录 A 的表格为准。声明形式：

```cpp
enum class ErrorCode {
    // FMT-001 ~ FMT-015：通用、JSON、路径
    InvalidArgument,
    FileNotFound,
    // ... 见架构文档附录 A 完整清单

    // FMT-100 ~ FMT-105：文件名校验
    // FMT-200 ~ FMT-203：Bucket
    // FMT-300 ~ FMT-305：上传下载
    // FMT-400 ~ FMT-402：Trash
    // FMT-500 ~ FMT-503：Share
    // FMT-600 ~ FMT-603：Service
    // FMT-700 ~ FMT-701：HTTP
};
```

编号到字符串（`"FMT-001"`）的转换由 `common/error` 集中实现，业务代码不写字符串。
