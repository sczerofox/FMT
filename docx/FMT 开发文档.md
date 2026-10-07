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
└── log/
    ├── fmt.log
    └── error.log
```

数据根（`FMT_ROOT`）**由 CLI 声明**：CLI 连接服务时用 `hello` 帧带上自己 exe 所在目录
（`GetModuleFileNameW` 取父目录），服务把该目录作为「当前数据根」。切换数据根时**不删除旧根数据**，
只对新根做幂等初始化（见第 91～94 节）。

`log/` 与业务数据分离（日志不是业务数据），见第 65 节。

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

或者系统临时目录。

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

模块短名：`Main`、`Config`、`File`、`Share`、`Trash`、`Bucket`、`Storage`、`Http`。

原则：

> **控制台负责用户交互和重要异常；日志文件负责完整运行记录。**

**只有 Service 写日志文件**；CLI 不写 `fmt.log`，只输出到控制台——避免两个进程争抢
同一日志文件。

日志目录位于**当前数据根**下（`FMT_ROOT/log/`），由服务在初始化时创建（见第 92 节）。

CLI 与服务之间的通道是命名管道（见第 76 节）：CLI 把命令发给服务，服务执行后把
`{ok, code, message}` 回传，CLI 只把结果打印到控制台，
**既不写日志文件，也不直接读写数据根下的任何 JSON**。

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

必须支持：

```text
fmt.exe --help
```

以及：

```text
fmt.exe bucket --help
fmt.exe file --help
fmt.exe share --help
fmt.exe trash --help
fmt.exe service --help
```

`service` 命令**不带 `--` 前缀**：旧写法 `fmt.exe --service install` 作废；
service 只有 `install` / `uninstall` / `start` / `stop` 四条，**没有 pause，也没有 delete**
（旧 `delete` 已更名为 `uninstall`），见第 70 节、第 95 节。

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

service 命令只有四条：

```text
fmt.exe service install
fmt.exe service uninstall
fmt.exe service start
fmt.exe service stop
```

固定规则：

```text
不带 -- 前缀（旧写法 fmt.exe --service install 作废）
没有 pause，也没有 delete（旧 delete 更名为 uninstall）
四条命令都直连 SCM，不走命名管道、不走 HTTP
四条命令一律走 UAC 提权
```

为什么必须直连 SCM：服务可能尚未安装或尚未运行，此时走任何进程间通道都会形成引导死锁。

**每条命令都提权**，即使目标状态已经满足（例如服务已安装仍执行 `install`）也照常弹 UAC，
不做「已满足状态就免提权」的优化。

各命令提权后做什么：

| 命令 | 提权后执行 | 典型结果 |
| --- | --- | --- |
| `service install` | 创建服务 + 配置 Recovery + 启动 | 已存在 → `FMT-600` / 退出码 8 |
| `service uninstall` | 先 stop，再 `DeleteService` | 未安装 → `FMT-601` / 退出码 8 |
| `service start` | `StartServiceW` | 未安装 → `FMT-601` / 退出码 8 |
| `service stop` | `ControlService(SERVICE_CONTROL_STOP)` | 未安装 → `FMT-601` / 退出码 8 |

提权的完整流程、四种命令各自何时提权、用户取消 UAC 的处理与固定输出格式，见第 126 节。

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
2. 查 SCM
     未安装        → 提权 install + start（一次 UAC）
     已安装未运行  → 提权 start（一次 UAC）
     已运行        → 不动，不提权
3. 比较服务 binPath 与自身路径
     相同              → 正常继续
     不同但文件存在    → 作为客户端继续
     不同且文件已丢失  → 提示重新安装服务（uninstall + install）
4. 连命名管道，用 hello 帧声明 root = 自身 exe 所在目录
5. 打印横幅与 Service Running... → 进入交互循环
```

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
服务宿主仍是首次安装时注册的那个 exe
 ↓
管道 hello 帧声明 root = D:\FMT2
 ↓
服务在 D:\FMT2 下幂等初始化
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

响应信封与 HTTP 完全一致，一份信封两处复用；超时：连接 3 秒、普通命令 30 秒。

管道安全：服务以 `LocalSystem` 运行，必须显式授权交互用户（IU）：

```text
D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)
```

并设置强制完整性标签（MIC），否则中完整性的普通 CLI 连接会报 `ERROR_ACCESS_DENIED`：

```text
S:(ML;;NW;;;ME)
```

CLI 形态**不碰 core、不建目录、不写 JSON**：只解析命令行、走管道、打印结果。

## 76.2 数据根由 CLI 声明

CLI 连接时的首帧是 `hello`，其中 `root` = CLI 自身 exe 所在目录（`GetModuleFileNameW` 取父目录）。

```text
root != 当前数据根 ?
 ├─ 是 → 切换当前数据根（旧根数据原样保留），对新根做幂等初始化
 └─ 否 → 直接进入命令循环
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
所以提权副本以 `SW_HIDE` 启动，结果经命名管道回传（见第 126 节）。

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

初始化**由服务执行，CLI 只读**：CLI 不创建目录、不写默认 JSON、不直接读写数据根下的文件。
（这条统一了旧文档「CLI 只读不建目录」与「首次运行创建目录」的口径冲突，口径唯一：
**服务建，CLI 只读**。）

触发时机：CLI 连接服务时用 `hello` 帧声明 `root`（自身 exe 所在目录），服务发现该根与当前数据根
不同（或该根从未初始化）时，对该根做一次幂等初始化。

```text
CLI 声明 root
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
写入默认 JSON（.tmp 原子替换）
 ↓
加载配置
 ↓
把该根记为当前数据根
```

幂等要求：

```text
不存在则创建
已存在 → 保持原样，不修改、不清空、不覆盖
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
```

由**服务**在数据根下创建，不存在则创建，已存在不动。

对应默认文件：

```text
config/config.json     {"version":1,...}
config/server.json     {"version":1,...}
data/user.json         {"version":1,"users":[]}
data/file.json         {"version":1,"files":[]}
data/share.json        {"version":1,"shares":[]}
data/trash.json        {"version":1,"trash":[]}
```

默认 JSON 一律「写 `.tmp` → 验证 → 替换」，已存在的文件不改、不删、不覆盖
（见第 11 节、第 13 节）。

`log/` 由服务独占写入，CLI 不写日志文件（见第 65 节）。

CLI 只读：不建目录、不写 JSON，只把命令经命名管道交给服务执行。

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
    └── stop
```

规则：

```text
service 命令不带 -- 前缀（旧写法 fmt.exe --service install 作废）
service 只有 install / uninstall / start / stop 四条
没有 pause，也没有 delete（旧 delete 更名为 uninstall）
业务命令（bucket / file / share / trash）经命名管道交给服务执行
service 命令直连 SCM，且每条都走 UAC 提权
```

各子命令支持 `--help`（如 `fmt.exe file --help`、`fmt.exe service --help`），
帮助内容必须与实际命令一致。

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

完成后，服务在 CLI 声明的数据根下自动创建：

```text
<数据根>/
├── repository/
├── trash/
├── config/
├── data/
└── log/
```

并写入默认 JSON（`.tmp` 原子替换，已存在不动）。

验收：

```text
能在指定数据根建出五个目录 + 默认 JSON
JSON 损坏报配置错误（退出码 7），且不修改原文件
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
    ServiceMain + HandlerEx
    Recovery
    %ProgramData%\FMT\service.json
ipc
    命名管道 \\.\pipe\fmt.control
    帧格式 [4 字节小端长度][UTF-8 JSON]
    安全描述符（授权 IU）+ MIC 标签
cli
    交互循环与横幅
    单实例互斥体 Local\FMT.CLI.v1
    单实例窗口激活
    UAC 提权与结果回显
    双击行为（首次 / 再次 / 换目录）
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
数据根由 CLI 用 hello 帧声明
初始化由服务执行，CLI 只读
```

验收：

```text
全新环境双击 → 一次 UAC → 服务装好且开机自启
service stop / service start 各弹一次 UAC，输出与第 126 节样例一致
复制 exe 到新目录双击 → 在新目录建出数据，旧目录数据保留
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
提权（四条命令各弹一次 UAC，输出与第 126 节样例一致）
用户取消 UAC（ERROR_CANCELLED 1223 → FMT-004 / 退出码 5，CLI 继续循环不退出）
提权等待超时（FMT-602 / 退出码 8）
提权副本不新开控制台窗口，结果经管道回到原窗口
开机自启（重启电脑后服务自动运行，横幅显示 Service Running...）
Service Recovery（异常退出后按 5 秒 / 10 秒 / 30 秒重启，失败计数 1 天重置）
uninstall 后 repository / trash / data / config / log 仍在
换目录声明新数据根（复制 exe 到新目录双击 → 新根完成初始化，旧根数据保留）
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
17. Windows Service V1 保持简单（四条命令 + SCM + Recovery，不自建 watchdog）
18. CLI（命名管道）与 HTTP 使用统一业务核心
19. 单一 fmt.exe，三种形态：CLI / Service / 提权短命副本
20. manifest 为 asInvoker，绝不 requireAdministrator
21. service 只有 install / uninstall / start / stop，没有 pause、没有 delete
22. service 四条命令一律走 UAC 提权，不做免提权优化
23. CLI 走命名管道，HTTP 只给浏览器（CLI 不走 HTTP）
24. 数据根由 CLI 声明，服务维护当前数据根，切换不删旧数据
25. 初始化由服务执行，CLI 只读不建目录、不写 JSON
26. 服务自身状态写在 %ProgramData%\FMT\service.json，不属于业务数据
27. 同时只有一个 CLI 窗口，因此同时只有一个数据根
28. 服务宿主为首次安装时注册的 exe 绝对路径，移动请用复制
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
service install / uninstall / start / stop（无 pause、无 delete）
service 四条命令一律 UAC 提权 + 结果经管道回传
CLI 走命名管道 \\.\pipe\fmt.control，不走 HTTP
数据根由 CLI 声明，服务侧幂等初始化，CLI 只读
服务状态文件 %ProgramData%\FMT\service.json
CLI 单实例与固定界面输出（横幅、提示符、stdout / stderr）
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

service 四条命令（`install` / `uninstall` / `start` / `stop`）**每条都走 UAC 提权**，
即使目标状态已经满足也照常弹 UAC，不做免提权优化。

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

```text
打印「需要管理员权限」        ← FMT-603 AdminRequired
打印「正在提权...」
 ↓
ShellExecuteExW(
    lpVerb = L"runas",
    fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
    nShow  = SW_HIDE)
 ↓
WaitForSingleObject(hProcess, 60000)
 ↓
GetExitCodeProcess
```

`runas` 启动的控制台程序会**另开一个控制台窗口**，所以提权副本必须：

```text
以 SW_HIDE 无窗口启动
只做 SCM 操作，短命，不进入命令循环
结果 {ok, code, message}
    经命名管道 \\.\pipe\fmt.elev.<pid>（或临时文件）回传父进程
由父进程打印
```

这是「只留一个窗口」的前提：整条命令始终只有一个可见控制台。

## 126.3 四种命令各自何时提权

| 命令 | 提权时机 | 提权后执行 |
| --- | --- | --- |
| `service install` | 命中该命令即提权，不检查服务是否已存在 | `CreateServiceW` + Recovery + `StartServiceW` |
| `service uninstall` | 命中该命令即提权，不检查服务是否在运行 | `ControlService(STOP)` + `DeleteService` |
| `service start` | 命中该命令即提权，不检查服务是否已在运行 | `StartServiceW` |
| `service stop` | 命中该命令即提权，不检查服务是否已停止 | `ControlService(SERVICE_CONTROL_STOP)` |

## 126.4 用户取消 UAC 与超时

```text
用户取消 UAC（ERROR_CANCELLED，1223）  → FMT-004 / 退出码 5
提权等待超时（60 秒）                  → FMT-602 / 退出码 8
SCM 操作失败                           → FMT-602 / 退出码 8
服务不存在                             → FMT-601 / 退出码 8
服务已存在（重复 install）             → FMT-600 / 退出码 8
```

用户取消 UAC 或提权失败**不退出 CLI**：打印错误后回到提示符继续等待输入（见第 127 节）。

## 126.5 固定输出

成功：

```text
FMT v1.0.0
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
FMT v1.0.0
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
```

---

# 127. CLI 界面约定

## 127.1 横幅

CLI 启动时打印：

```text
FMT v1.0.0
Service Running...
```

服务未运行时第二行改为：

```text
FMT v1.0.0
Service Stopped...
```

横幅之后打印提示符，等待用户输入。

## 127.2 提示符与交互循环

提示符为：

```text
fmt> 
```

规则：

```text
空行忽略：不报错、不退出
正常结果 → stdout
错误 → stderr
命令失败后继续循环，不退出
exit 或 quit 退出
```

命令按 `fmt> ` 提示符逐条输入，例如：

```text
fmt >service stop
fmt >file list
```

## 127.3 退出

```text
exit
quit
```

两种写法都结束 CLI 循环并以退出码 `0` 退出；直接关闭窗口同样只结束 CLI，
**不影响后台服务**（服务由 SCM 托管）。

## 127.4 输出通道

```text
stdout   命令的正常结果、横幅、提示符、提权过程提示（需要管理员权限 / 正在提权...）
stderr   错误码与错误消息（执行失败：FMT-NNN <消息>、错误码：N）
```

CLI 只输出控制台，**不写日志文件**（见第 65 节）。

## 127.5 CLI 与服务的边界

```text
CLI 不碰 core、不建目录、不写 JSON
CLI 只解析命令、走管道、打印结果
业务命令经命名管道交给服务执行（见第 76 节）
service 命令直连 SCM + UAC 提权（见第 126 节）
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
| service 命令 | 只有 `install` / `uninstall` / `start` / `stop`，无 `pause`、无 `delete`，命令不带 `--` 前缀 |
| service 提权 | 四条命令一律走 UAC 提权，结果经 `\\.\pipe\fmt.elev.<pid>` 回传父进程打印，见第 126 节 |
| CLI 通道 | 命名管道 `\\.\pipe\fmt.control`（帧 = 4 字节长度 + JSON）；HTTP `127.0.0.1:4122` 只给浏览器 |
| 数据根 | 由 CLI 用 `hello` 帧声明（自身 exe 所在目录）；服务维护当前数据根，切换不删旧数据 |
| 初始化归属 | 服务创建 `repository`/`trash`/`config`/`data`/`log` 与默认 JSON；CLI 只读，见第 91～92 节 |
| 服务状态文件 | `%ProgramData%\FMT\service.json`（当前数据根 + 安装信息），不属于业务数据 |
| CLI 界面 | 横幅 `FMT v1.0.0` + `Service Running...`，提示符 `fmt> `，正常 → stdout / 错误 → stderr，见第 127 节 |

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
