# FMT 开发文档

> 项目名称：FMT
> 项目类型：Windows 文件管理系统
> 当前版本：V1
> 对应架构文档：`FMT 项目架构.md`
> 开发平台：Windows
> 构建工具：CMake + Ninja + Visual Studio Build Tools
> 主程序：`fmt.exe`
> 文档状态：V1 开发规范

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
* Windows Service
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
FMT/
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

`log/` 与业务数据分离（日志不是业务数据），见第 65 节。

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
fmt.exe --service --help
```

帮助内容应该：

```text
简洁
准确
与实际命令一致
```

---

# 69. Service 模块

Service 模块负责 Windows Service。

主要功能：

```text
install
start
stop
delete
```

---

# 70. Service Install

命令：

```text
fmt.exe --service install
```

流程：

```text
检查管理员权限
 ↓
检查 Service 是否已经存在
 ↓
不存在 → 创建
 ↓
设置自动启动
 ↓
配置 Recovery
```

如果已经存在：

```text
不要重复创建
```

---

# 71. Service Start

```text
fmt.exe --service start
```

检查：

```text
Service 是否存在
```

存在：

```text
启动
```

不存在：

```text
报错
```

---

# 72. Service Stop

```text
fmt.exe --service stop
```

正常停止 Service。

停止过程中应该：

```text
停止 HTTP Server
 ↓
等待正在进行的关键操作完成
 ↓
退出 Service
```

---

# 73. Service Delete

```text
fmt.exe --service delete
```

要求：

```text
Service 已停止
```

然后：

```text
删除 Windows Service
```

不能删除：

```text
repository
trash
data
config
```

---

# 74. Service Recovery

使用 Windows Service Recovery：

```text
服务异常退出
 ↓
Windows 检测
 ↓
重新启动 fmt.exe
```

V1 不实现独立 watchdog。

---

# 75. fmt.exe 双击行为

第一次双击：

```text
检查 Service
 ↓
不存在
 ↓
请求管理员权限
 ↓
安装
 ↓
启动
```

再次双击：

```text
检查 Service
 ↓
存在
 ↓
检查状态
 ↓
必要时启动
```

不重复安装。

---

# 76. Service 与 CLI

V1 保持简单。

`fmt.exe` 同时承担：

```text
CLI
+
Service
```

启动参数决定运行模式。

例如：

```text
普通 CLI：
fmt.exe file list
```

Service：

```text
fmt.exe --service ...
```

Windows Service 启动时进入后台服务模式。

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

第一次运行：

```text
fmt.exe
 ↓
检查运行目录
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
创建默认 JSON
 ↓
加载配置
```

不得删除用户已经存在的数据。

---

# 92. 初始化目录

必要目录：

```text
repository/
trash/
config/
data/
```

不存在则创建。

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
└── --service
    ├── install
    ├── start
    ├── stop
    └── delete
```

---

# 96. 开发顺序

V1 不建议一次性开发全部功能。

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
Config
Storage
JSON
目录初始化
```

↓

```text
阶段 3
Bucket
```

↓

```text
阶段 4
File
file_id
文件名
MD5
```

↓

```text
阶段 5
Upload
```

↓

```text
阶段 6
File Get
File List
File Delete
```

↓

```text
阶段 7
Trash
Restore
Permanent Delete
```

↓

```text
阶段 8
Share
```

↓

```text
阶段 9
Windows Service
```

↓

```text
阶段 10
HTTP Server
```

↓

```text
阶段 11
Preview
```

---

# 97. 第一阶段：项目骨架

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

# 98. 第二阶段：基础存储

实现：

```text
目录初始化
Config
JSON Storage
Path
Logger
Error
```

完成后：

```text
FMT/
├── repository/
├── trash/
├── config/
└── data/
```

能够自动创建。

---

# 99. 第三阶段：Bucket

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

# 100. 第四阶段：File 基础

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

# 101. 第五阶段：Upload

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

# 102. 第六阶段：File 操作

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

# 103. 第七阶段：Trash

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

# 104. 第八阶段：Share

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

# 105. 第九阶段：Windows Service

实现：

```text
--service install
--service start
--service stop
--service delete
```

并配置：

```text
自动启动
异常自动恢复
```

---

# 106. 第十阶段：HTTP Server

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
第一次安装
重复安装
启动
停止
删除
重启电脑
异常退出
Service Recovery
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
17. Windows Service V1 保持简单
18. CLI 与 HTTP 使用统一业务核心
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
V1 开发规范初稿
```

本文件以：

```text
FMT 项目架构.md
```

为上层设计依据。

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
| 命名空间 | `fmt` 保持不变；V1 不引入第三方库，不存在重名问题 |

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
