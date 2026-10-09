# FMT 项目架构

> FMT：Windows 平台的文件管理系统。**单一 `fmt.exe`、三种形态**：CLI 形态、被 SCM 启动的
> Service 形态、提权短命副本。CLI 与浏览器都只是客户端，直连同一个常驻 Service 进程。
>
> **文档状态：重构版（分支 `arch-restart`，基线 `bdbe33d`）。** 本版按已冻结的重构决策重写了
> 架构描述：`fmt.exe` 多形态与 `asInvoker` manifest、CLI↔服务的命名管道、浏览器↔服务的 HTTP、
> `service` **六条子命令**（`install` / `uninstall` / `start` / `stop` / **`reinstall`** / `status`；除 `status` 外都提权；`reinstall` 提交 `c573f14` 起正式化）与提权语义、数据根由 CLI 声明、
> 阶段顺序前移（Windows Service 提到阶段 2/3）；CLI 双击时对自己数据根做幂等体检与补齐。
> 旧版《项目架构》中「`FMT_ROOT` = exe 所在目录」「`fmt.exe --service install`」「CLI 走
> HTTP」「`service` 有 `pause` / `delete`」「`service` 只有四条命令」「初始化只由服务执行、
> CLI 只读不建目录」等描述**已作废**，与本文冲突时以本文为准。
>
> 文档关系：
>
> | 文档 | 定位 |
> |---|---|
> | `FMT 开发文档.md` | **规范主体**：模块职责、数据结构、命令、操作流程、测试要求 |
> | `FMT 技术文档.md` | 技术实现：技术栈、接口设计、实现方式，依据开发文档编写 |
> | `FMT 项目架构.md`（本文档） | 总体架构：对象关系、分层、设计决策、错误码清单 |
> | `FMT 重构设计.md` | 本次重构的差异说明与冻结决策索引，**架构改动的源头文档** |
> | `项目基本提示词.txt` | 项目背景说明（项目类型、架构方向），**不是规范依据** |
>
> 冲突时以 `FMT 开发文档.md` 为准；**进程形态、进程间通道与本文第 2 节的冻结决策以本文为准**。
> 本文档描述**当前代码的真实状态**。

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
                       （桶级身份记录的唯一权威是 `trash/<user>/.original`，见第 5.4.1 节）
```

V1 用 **JSON + 文件系统**满足需求，不使用数据库、Redis、MQ、微服务或复杂任务调度器。**常驻进程
只有一个**：由 SCM 托管的 Windows Service（`fmt.exe` 的 Service 形态，见第 4 节）。CLI 不是常驻
进程，只是连上它的一次性客户端。

采用分层架构，CLI 与 HTTP Server **不直接操作底层文件**，都经同一套业务核心，避免出现两套文件处理逻辑。
两条入口在进程上落在**同一个 Service 进程**里：通道不同，业务实现只有一份（第 4 节）。

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

V1 开发期间以下规则视为核心规则（`FMT 开发文档.md` 第 122 节）。其中第 **16～40** 项是
`arch-restart` 分支新增的**重构冻结项**，逐条对应 `FMT 重构设计.md` 第 2 节的决策索引：

| # | 规则 |
|---|---|
| 1 | Bucket **没有**独立 `bucket_id`，用 `bucket_name` 标识 |
| 2 | File 用 `file_id` 标识，格式 `fmt-YYYYMMDD-N` |
| 3 | Share 用 `share_id` 标识，必须唯一且不可预测 |
| 4 | File 用 MD5 去重，重复不上传物理文件、不生成新 ID |
| 5 | 同一用户的**正常**文件名唯一；Bucket 不构成命名空间 |
| 6 | Trash 不占用正常文件名空间 |
| 7 | `file_id` 不因删除、恢复而改变 |
| 8 | Bucket 删除不创建 `file_id`，其下文件只改 `is_trash`（同时写 `trash_reason = "bucket"`）；桶级身份写到 `trash/<user>/.original`，**不写 `data/trash.json`** |
| 9 | 冲突恢复**不覆盖、不改名**，文件留在 Trash（**文件级**恢复的规则）；**桶级回退是整单判定**：原位置已有同名 Bucket 就整单拒绝 `FMT-401`，不覆盖、不改名、不做部分恢复 |
| 10 | `current_bucket` 无效时**不自动**选择其他 Bucket |
| 11 | `max_download_count` 属于 Share，File 本身不保存下载限制 |
| 12 | Share 默认最大下载次数 20，与过期时间相互独立 |
| 13 | JSON 损坏**不能静默重置** |
| 14 | 关键文件操作必须具备事务式处理 |
| 15 | CLI 与 HTTP 使用统一业务核心 |
| 16 | 单一 `fmt.exe` 承载三种形态：CLI 形态、Service 形态（SCM 启动）、提权短命副本（内部参数 `--elevated`） |
| 17 | manifest 固定 `asInvoker`，**绝不** `requireAdministrator`；提权只发生在提权副本里 |
| 18 | Service 是**全局单例**，也是唯一写 `data/*.json` 的进程；CLI 只是客户端，不碰 core 业务操作、不写任何业务 JSON。唯一例外是**双击时对自己所在数据根做一次体检与补齐**（只补缺失的目录与默认 JSON，见第 3.2 节），以及建写日志用的 `log/` 与提权前要用的 `temp/`（第 7.2 节） |
| 19 | CLI 单实例：命名互斥体 `Local\FMT.CLI.v1`，已有窗口则激活、不新建——因此**同时只有一个 CLI 窗口，也就只有一个数据根** |
| 20 | **数据根由 CLI 声明**：CLI 用 hello 帧上报自身 exe 所在目录，服务维护「当前数据根」，切换**不删**旧根数据；响应用 `switched` / `previous_root` 回执本次是否发生切换（第 3.1 节） |
| 21 | 初始化规则**两边共用同一套幂等实现**（`ensure_root` / `check_root`）：六个目录（`repository/`、`trash/`、`config/`、`data/`、`log/`、`temp/`）+ 默认 JSON，`.tmp` 原子替换，**只补缺失、已存在不改、不删、不覆盖**；服务在启动/换根时执行，CLI 在双击时对自己的数据根执行 |
| 22 | 双入口直连服务：CLI 走命名管道 `\\.\pipe\fmt.control`（**提交 `a340d1e` 起可被环境变量 `FMT_PIPE` 覆盖**——只为端到端测试隔离，线上默认名不变，见 `FMT 技术文档.md` 第 13.9.1 节），浏览器走 `localhost:4122`（cpp-httplib），两者进同一个 service 层 |
| 23 | `service` 命令有 `install` / `uninstall` / `start` / `stop` / **`reinstall`** / `status` **六条**（`reinstall` 提交 `c573f14` 起正式化），**无 `pause`、无 `delete`**（`delete` 更名为 `uninstall`），命令**不带 `--` 前缀**；其中 `status` 是**只读查询**，见第 24 项 |
| 24 | **四条动作命令**（`install` / `uninstall` / `start` / `stop`）都走 UAC 提权，不做「目标状态已满足就免提权」的优化；`status` **不提权、不弹 UAC**，它是查询命令 |
| 25 | 服务自身状态写 `%ProgramData%\FMT\service.json`（当前数据根 + 安装信息），**不属于业务数据** |
| 26 | vendor `nlohmann/json` + `cpp-httplib` 到 `third_party/`（头文件入库），`/MT` 静态链接 CRT，产物仍然只有一个 `fmt.exe` |
| 27 | **Bucket 名称统一小写（提交 `9c3d2cb`）**：`create` 先按 **ASCII 折叠**转小写再校验、再建目录（`WORK` 建成 `work`，`renamed` 为真时回 `note` 提示用户）；`use` / `get` / `delete` 用 `canonical_name()` 规范化到**磁盘上的实际名字**，所以 `current_bucket`、`file.json` 的 `bucket`、`.original` 的 `original` **只留一份拼写**。理由：Windows 目录不区分大小写，`WORK` 与 `work` 本来就是同一个目录，不统一拼写只会让记录之间各存一份、日后比对与恢复都踩坑 |
| 28 | **`fmt-YYYYMMDD-N` 是保留形状（提交 `9c3d2cb`）**：文件名与 `file_id` 同形 → 上传拒绝 `FMT-106 FileNameLikeFileId`（退出码 2，`looks_like_file_id()`，属 `FMT-1xx` 文件名校验、与 Windows 保留设备名同类）；旧数据里已有的这种名字，`file delete` 在两个索引命中**不同**记录时报 `FMT-001` 并点名两条记录、让用户用 `file_id`——**只给 delete 加**，`file get` 的两种查询范围保持不变（只读查询最坏是给用户看 id 命中的那条；破坏性操作不能猜）。`file get` 命中回收站记录时另回 `trash_path`（相对数据根、正斜杠），仓库里找不到时不回 `path` |
| 29 | **破坏性操作先检查、说清楚、再确认（提交 `711da4c`；缺口收尾 `6a40742`）**：`file.delete` / `trash.delete` / `bucket.delete` 在 CLI 侧先发一次**只读预检**（`args.dry_run = true` / HTTP `?dry_run=1`），把目标属于哪个 Bucket、哪两条记录撞车、永久删除会毁掉什么打印出来；交互窗口问 `y/N`、一次性命令要 `--yes`（本地开关，不进 `argv`）；**用户同意之前一个破坏性请求都不发出**。**歧义 / 同名冲突 / 随桶删除 / 数据缺失是 `blocked` 而不是 `needs_confirm`**——y/N 表达不了「删哪一个」。服务端**独立校验** `force`：永久删除、跨 Bucket 删除、非空桶删除缺 `force` → **`FMT-016 ConfirmRequired`**（退出码 2，HTTP 400；原口径「复用 `FMT-001`」作废）。跨 Bucket 成功时 `message` 与 `data.bucket` 都带归属 |
| 30 | **回收站是一份两级视图（提交 `0fc242b`；列表计数 `18f16ca`）**：`src/trash/` 的 `TrashService` **组合** `FileService` 与 `BucketService`，把文件级与桶级合成一份 `TrashEntry` 列表并做**跨命名空间的标识解析**（① 回收站目录名 → ② `file_id` → ③ 桶原名 → ④ 文件名；③④ 多条 → 候选）。权威：**文件级 = `file.json`**（`is_trash` / `trash_reason` / **`deleted_at`**），**桶级 = `trash/<user>/.original`**；**`data/trash.json` 不再写入**，只做只读兼容。`trash list` 标出 `[文件]` / `[桶]`，桶级条目的 `files`/`bytes` 在列表里就算出来（对每个 `present` 条目遍历一次目录，代价见第 13 节）；回退的三种硬拒绝（同名冲突 `FMT-401` / 随桶删除 `FMT-402` / 数据缺失 `FMT-002`）都是 `blocked` |

| 31 | **粘贴路径的污染要清掉、不可见字符不许进名字（提交 `a9af276`）**：`clean_user_path()` 清掉不可见格式字符（`U+00A0` / `U+00AD` / `U+200B`–`U+200F` / `U+202A`–`U+202E` / `U+2060`–`U+2064` / `U+2066`–`U+2069` / `U+FEFF`）与**成对**引号、首尾空白，中文不受影响；**一处收口**在 `service/commands.cpp` 的 `argument()`（所有位置参数的唯一入口，CLI 与 HTTP 共用），`prepare_upload()` 再清一次来源与显式文件名。清掉后仍找不到 → `FMT-002` 并**点名码位**；**名字里不允许**这些字符：`validate_file_name()` → `FMT-101`、`validate_bucket_name()` → `FMT-202`（理由：屏幕上看不出来、用户没法重敲一遍）。用户实测：`C:\...\头像\asdva.jpg` 被 `U+202A`/`U+202C` 包住 → 「文件不存在」，现在能正常上传 |

| 32 | **请求形状由同一个构造函数产出；进程间共写的两处纪律（提交 `2c841c8` + `5b316b3`，用户实测踩出来的）**：① `fmt::cli::argument_envelope(positional, dry_run, force)` 是「位置参数放 `argv`、开关放同级」的唯一产出点，预检与真实请求共用——原来把 `dry_run` 挂到位置参数**数组**上，nlohmann 抛 `type_error.305` 未捕获即 `abort()`（`file delete a7.jpg` 弹出的 Debug Error）；**测试不许自己拼形状**（旧测试照着服务端契约拼，所以全绿却挡不住崩溃）。② temp/ 的启动清理**只清十分钟以前**的 `fmt-*`：提权结果文件也叫 `fmt-elev-<pid>.json(.tmp)`，`service install` 会在同一次操作里启动服务，一律清掉会让父进程报「FMT-602 提权副本没有返回结果」——**服务其实装好了**（假失败）。③ 原子写的临时名必须唯一（`<目标>.<pid>.<序号>.tmp`）+ 进程内互斥 + 替换遇共享冲突短暂重试：固定 `<目标>.tmp` 时安装器与服务启动同时写 `service.json` 会互相踩，加上调用方忽略返回值 → `service.json` 静默不更新。④ `TrashEntry.present` 语义写死为「数据在不在磁盘上」，回退/删除的结果**不翻转它**（文件级 `5b316b3`、桶级 `8f0fd5c`——原来恢复成功后打印「状态：数据已不存在」；`8f0fd5c` 之后真机跑过建桶/删桶/回退/永久删除确认为实况）。见第 4.2、4.5、8 节与 `FMT 技术文档.md` 第 6.6、10.4、11.15、13.8.3、18.28、18.29、18.30 节 |

| 33 | **`version` 是正式命令，与横幅、`--version` 同源（提交 `d108c80`）**：三处都走 `cli::version_text()`（内部就是 `banner_text()`），输出一行 `File Manager Tool  v1.0  ( build  <CMake 配置日期> )`——窗口里 `version`、`fmt.exe version`、`fmt.exe --version` / `-v` 都能用。**不需要服务在运行**（不连管道、不查状态、不弹 UAC），**不写任何磁盘内容**（一次性分支在开日志器之前，`log/fmt.log` 不会因此多记录）。`help` 总览多一行 `(version)  version  打印版本与构建日期`，`help version` 有正文。原口径「窗口里敲 `version` → 未知命令」**已作废** |

| 34 | **数据根被搬走时要说出来（提交 `8f2fbc5` + `bb7a40f`，用户实测后选定）**：① **切换发生的那一刻**（hello 回执 `switched=true`）CLI 在 **stderr** 打印 `cli::root_switch_notice(previous, current)`——两个根都点名、说明原因是「数据根由 CLI 声明」、并给出「用数据根正确的那个 `fmt.exe` 再执行一条命令切回去」；**同时**照旧记一行日志（**原口径「换根只进日志、不刷控制台」已作废**）。② 交互窗口横幅区**永远多一行「数据根：…」**（`service::load_state()`），与本程序所在目录不一致时再补一行说明（触发条件：双击引导会先连上并声明本目录，所以服务在跑时通常已一致；这一行主要在服务不可用时出现）。③ **为什么「连接那一刻报警」就够**：服务一次只接受一条连接（第 3.1 节与 `FMT 技术文档.md` 第 15.1 节①），交互窗口握着管道时别的 CLI 拿 `ERROR_PIPE_BUSY` → `FMT-601`，所以「会话中途被搬走」不可能发生。④ 文本收进 `root_switch_notice()` 是为了**能断言**（控制台输出第一次有测试覆盖） |

| 35 | **`service reinstall` 是正式命令（提交 `c573f14`）**：`is_user_service_command()` 加入 `reinstall`（该函数从匿名命名空间移到 `cli.hpp`，便于测试），两处用法提示、命令总览、`help service` 正文都更新。**service 子命令是六条**（`install` / `uninstall` / `start` / `stop` / **`reinstall`** / `status`），**除 `status` 外每条都提权**；原口径「只有四条命令」「`reinstall` 只在引导流程内部使用」**已作废**。语义是**一次 UAC** 里「卸载 → 按**当前这个 exe** 重新注册 → 启动」，因此是**更新 exe 的正确路径**（不必先复制、只要一次 UAC；用户以前要 `uninstall` + `install` 两次），也用于修复宿主 exe 被移动/删除。**三个「不变」**：业务数据不变、`C:\ProgramData\FMT\service.json` 不变（`current_root` 保留）、数据根仍由 CLI 声明。见第 4.5 节与 `FMT 技术文档.md` 第 13.8.2、13.8.4、18.34 节 |

| 36 | **端到端冒烟测试：真实 exe 走真实管道（提交 `a340d1e`）**：新增 `tests/cli_e2e_test.cpp`（套件 `CliE2e`）——进程内起 `ServerRuntime` → 把 `fmt.exe` **复制到临时数据根**（「数据根 = CLI 所在目录」）→ `CreateProcessW` 拉起**真实 exe**、喂 stdin、合并收 stdout+stderr → 断言**退出码与用户看到的文字**。覆盖 `version`、bucket 小写归一、上传/去重/重名、`file get` 按名与按 id、软删除 → `trash list`/`get`/`restore`、跨桶删除（无 `--yes` → `FMT-016` 且**文件仍在**）、永久删除、删桶（空桶成功但仍提醒「只能整体恢复」、非空桶要 `--yes`）、**交互式确认**（喂 `n` 不删、喂 `y` 才删——此前完全没有自动化覆盖）。**隔离靠 `FMT_PIPE`**（`ipc::pipe_name()`，第 22 项）：测试与它拉起的 CLI 共用私有管道名，**正在运行的真服务不受影响**（实测：跑完服务仍运行、数据根不变、桶与回收站完全未变）。**为什么要有它**：`file delete` 弹 `abort()` 那次崩溃，单元测试全绿却没挡住——测试自己拼请求形状、CLI 拼的是另一种形状；只有真实 exe 走一遍用户走的路才会当场露出来（那次崩溃对所有破坏性操作都生效）。全量 150 项、约 17 秒（端到端约 5 秒） |

| 37 | **日志轮转（提交 `821aba3`）**：`fmt.log` 超过 **5 MB** 轮转成 `fmt.log.1`（**只留一代**），`error.log` 同理；`Logger::Options::max_log_bytes` **0 表示不轮转**（默认 5 MB）。每写 **64 行**检查一次大小（不用时间节流：按行计数便宜、确定、测试可预期）；改名失败（另一个进程正在写）不报错、下次再试；轮转后在新文件里写一行「日志超过 N 字节，已轮转：fmt.log -> fmt.log.1」。**前置条件是日志「每行开-写-关」**——两个进程共用同一个文件，长期持有的句柄既挡改名、又会让另一个进程写进已改名的文件。顺带对齐口径：`error.log` **仅 ERROR 级**（`logger.hpp` 原注释写「WARN 也进」与第 65 节冲突，被既有用例当场抓住后改回代码）。见第 7.4 节与 `FMT 技术文档.md` 第 14.2、14.6、18.36 节 |

| 38 | **零碎三项：`trash empty` / `file list --sort` / `config list`·`set`（提交 `674d0b0`）**：`trash empty` 一次清空两级（dry_run 预检回 `{files,buckets,bytes,needs_confirm,blocked:false,message}`、缺 `force` → `FMT-016`、**空站不打扰**；实现上**每删一项重新 list**、**先文件级后桶级**、单条失败跳过 + `kMaxRounds` 兜底；**没加 HTTP 路由**）；`file list --sort name\|size\|id`（默认 name 不区分大小写、size 大的在前、id 是入库顺序，**乱写 → `FMT-001`**，响应回显 `sort`，`--sort` 是本地开关）；`config list` / `config set max_upload_size <大小>`（**只让改这一项**，其余只读 → `FMT-001`；1KB=1024 字节、下限 1KB、上限 100GB；落盘失败**回滚内存值**）。另：**`FMT-203 BucketInUse` 写成「保留（V1 未使用）」**——没有代码会产生它 | 
| 39 | **share 数据面（提交 `d5779db`）**：`src/share/` 真实落地，`share create/get/list/delete` 可用；`share_id` = **12 位随机十六进制**（`BCryptGenRandom`，撞号重摇、生成失败当错误返回）；默认 **20 次 + 7 天**（两条件**相互独立**）；`share get` **如实报状态**（未知 id 才是 `FMT-500`；已过期 / 次数用尽 / 已撤销 / 关联文件在回收站都是成功 + `state`/`available`/`message`）；检查顺序按开发文档 §49（**过期在次数之前**，§50 图的顺序以 §49 为准）；`expire_time` 解析失败按已过期；文件进回收站 → share 立刻不可用 + 阻断 create（`FMT-503`）；新增 **`share.download`** op，`register_download()` 在**业务锁内**一次完成检查+计数+落盘，**写不进去就拒绝**。**HTTP 路由一个都没加**（用户决定，等定开放哪几个）。**原口径「share 未实现、返回 `FMT-602`」已作废**——`FMT-602` 现在只对应 `server.*` 与未实现的 HTTP 路由 |

| 40 | **HTTP 接口第 1、2 步 + 账号/令牌（提交 `bfd89f7` / `4b812b5` / `ff237d5` / `22c3c3e`）**：① HTTP **已开启**，监听 **`localhost:4122`**（`ServerConfig::host` 代码默认值从 `127.0.0.1` 改成 `localhost`；线上 `server.json` 为 `enabled: true`）；**仍是缺口**：安装流程不会自动打开 `enabled`，新数据根要手改配置。② **所有 `/api/*` 都要 token**，唯一例外是 `/api/ping` 与 `/api/share/<id>/download`（分享链接本身就是凭证）；缺 / 错 → **401 + `FMT-018 Unauthorized`**；认证**只在一处**（httplib pre-routing 钩子，「漏给某条路由加认证」是这类代码最典型的事故），且**没有校验器时一律 401**（fail-closed）；请求头收 `X-FMT-Token` 与 `Authorization: Bearer`，比较常量时间；token 从 `config list` 拿。③ **桶的 HTTP 接口已全部删除**（用户明确「桶不要」）：`/api/bucket*` → **404 + `FMT-017`**（故意不要，不是 501），「已知模块」列表里**没有 bucket**。④ 分享路由四条已落地（都要 token），`GET /api/share/<id>/download` 公开但**流式下载第 3 步才做**（现在 501 + `FMT-602`）。⑤ `data/user.json` 定稿（**没有 `buckets` 字段**，桶以磁盘为准；密码只存 PBKDF2-SHA256 哈希；token 永久有效；默认账号在数据根初始化时创建，老的空 `users` 也会补建、已存在不覆盖）。⑥ **测试隔离教训**：`FMT_PIPE` 管不到 SCM，新增 **`FMT_NO_SERVICE=1`** 让引导完全不碰服务管理（提交 `ff237d5`，出过一次真事故：测试把真服务注册指向临时目录） |

系统明确**禁止自动**执行：覆盖文件、修改用户文件名、选择其他 Bucket、创建恢复目标
Bucket、绕过下载限制、删除文件、清空损坏 JSON、**删除旧数据根的数据**、**把服务宿主的
`fmt.exe` 复制到别处**（宿主就是首次安装时注册的那个路径）。

优先级：**数据安全 > 数据一致性 > 功能正确 > 性能 > 代码简洁**。

依赖约束（**重构后已放宽**）：V1 允许 vendor **两个**第三方库——`nlohmann/json`（JSON 读写，
业务数据、IPC 帧与响应信封共用）与 `cpp-httplib`（**HTTP 服务端**；URL 下载自提交 `a2b6cd1`
起改用系统的 **WinHTTP + Schannel**），以头文件形式
放进 `third_party/`，不用包管理器、不用 FetchContent、不产生额外 DLL。CRT 用 `/MT` 静态链接，
产物仍然只有一个 `fmt.exe`，可完全离线构建。除此之外只使用 C++17 标准库与 Windows API
（单元测试用仓库内自带的极简运行器，不引入任何测试框架）。第三方库只在 `storage` 与 `server`
等少数模块内可见，业务模块不直接包含它们的头文件。命名空间 `fmt` 仍不存在与外部库重名的
问题，保持不变，后续也不再讨论改名。

> **换 WinHTTP 正是为了守住这条约束（提交 `a2b6cd1`）**：`cpp-httplib` 的 `Client` 要 https
> 就得 OpenSSL，而自己编 OpenSSL 需要 **Perl + NASM**，会破坏「可完全离线构建」；
> 静态链接 OpenSSL 虽不引 DLL，但 `/MT` 与常见 `/MD` 静态包混用 CRT 会出问题。
> WinHTTP 是 **Windows 系统组件**（`target_link_libraries(fmt_core PRIVATE winhttp)`，
> 不算第三方依赖），TLS 走 **Schannel**、证书用系统证书库，**不分发任何 DLL**，
> 还自动使用系统代理（`WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`，失败退回 `DEFAULT_PROXY`）。
> 因此 `https://` 在 V1 可用（第 4.4、8、14 节，`FMT 开发文档.md` 第 33 节）。

## 3. 运行目录

### 3.1 数据根由 CLI 声明

`FMT_ROOT`（数据根）**不再固定为 exe 所在目录**，而是**由 CLI 声明**：

```text
CLI 启动
  → GetModuleFileNameW 取自身路径 → 取父目录 = 自己声明的 root（如 D:\FMT2）
  → 先对自己这个 root 做一次体检与补齐（第 3.2 节，只补缺失、不碰业务数据）
  → 连上服务后首帧就是 hello：{"id":1,"op":"hello","root":"D:\\FMT2","pid":1234}
  → 服务比对：
       root != 当前数据根 ?
         ├─ 是 → 切换数据根（旧根数据原样保留），对新根做幂等初始化，
         │        记 INFO「数据根切换: D:\FMT → D:\FMT2」，
         │        在 hello 响应里置 switched=true 并回填 previous_root
         └─ 否 → 直接进入命令循环，响应不含 switched/previous_root
  → 该连接上的所有业务命令都在该数据根下执行
```

`hello` 的响应体（复用第 4.3 节的信封，`data` 部分）：

```json
{ "id":1, "ok":true, "data":{ "root":"D:/FMT2", "switched":true, "previous_root":"D:/FMT" } }
```

| 字段 | 含义 |
|---|---|
| `root` | 服务处理后确认的当前数据根（等于 CLI 声明的 root） |
| `switched` | 这次声明是否**导致服务切换了数据根**；未切换时该字段不出现（或缺省为 false） |
| `previous_root` | 切换前的旧数据根；**只在 `switched` 为真时出现** |

`switched` 为真时，CLI **在 stderr 打印换根提示**（提交 `8f2fbc5`，`cli::root_switch_notice(previous, current)`：两个根都点名 + 原因 + 怎么切回去）**并**记一行日志 `[Service] 数据根切换：旧 -> 新`——**原口径「只进日志」已作废**，所以双击一个新目录就能看见
服务跟过来了；未切换时保持原来的静默。

数据根的取值优先级：

| 场景 | 数据根 |
|---|---|
| CLI 已连接（收到 hello 帧） | CLI 声明的目录，即 **CLI 自己 exe 所在目录** |
| 服务开机自启、无 CLI 连接 | `%ProgramData%\FMT\service.json` 里记录的上一次数据根 |
| 从未记录过 | 服务宿主 exe 所在目录 |

因为**同时只有一个 CLI 窗口**（第 2 节第 19 项、第 4.7 节），所以**同时只有一个数据根**，不会出现
两个根互相打架。换数据根必须先关闭旧 CLI 窗口，再启动新目录下的 `fmt.exe`。

### 3.2 数据根目录布局

```text
<数据根>/                   ← 由 CLI 声明，通常等于 CLI 的 fmt.exe 所在目录
├── fmt.exe
├── repository/           正式文件存储
│   └── <user>/<bucket>/YYYY/MM/DD/<file_name>
├── trash/                回收站，保持原有层级便于恢复；**顶层只放桶级条目**
│   └── <user>/
│       ├── .files/<bucket>/YYYY/MM/DD/<file_name>   文件级条目（阶段 5 起）
│       │                                        提交 4fee290：中间那层点开头的 .files
│       │                                        是刻意的，见下
│       ├── <bucket>_<YYYYMMDDHHMMSS>/           桶删除的落点：整个桶搬进来，
│       │                                        目录名一律带时间戳（同秒冲突加 _2）
│       └── .original                            桶级身份记录的唯一权威
│                                                （trashed / original / deleted_at）
├── config/
│   ├── config.json       version、current_user、current_bucket、max_upload_size、size_unit、language
│   └── server.json       version、enabled、host、port（默认 localhost:4122）
├── data/
│   ├── user.json         {"version":1,"users":[…]}（**提交 `bfd89f7` 定稿**：账号 + 密码哈希 +
│   │                      token + current_bucket；**没有 buckets 字段**，桶以磁盘为准）
│   ├── file.json         {"version":1,"files":[]} 文件元数据（含 is_trash / trash_reason）
│   ├── share.json        {"version":1,"shares":[]} 分享记录
│   └── trash.json        {"version":1,"trash":[]} 回收站管理信息——**只服务文件级条目**
│                         （阶段 5 起）；Bucket 级条目（旧 type=bucket）作废，
│                         桶级身份改记 trash/<user>/.original
├── log/                  运行日志（正式约定，与业务数据分离）
│   ├── fmt.log           全部日志，Service 与 CLI 追加同一个文件（第 7.2 节）
│   └── error.log         仅 ERROR 级（超过 5 MB 轮转成 error.log.1，只留一代）
└── temp/                临时文件（既不是业务数据、也不是日志，随时可以清空）
    └── fmt-elev-<父进程 pid>.json   提权结果文件，父进程读完立刻删除（第 4.5 节）
```

> **`trash/<user>/` 顶层归属（提交 `4fee290`）**：顶层只放**桶级条目**
> （`<名字>_<14 位时间戳>`，可带 `_<1-3 位序号>`），文件级条目收在点开头的
> `trash/<user>/.files/<bucket>/YYYY/MM/DD/` 下（原口径「文件级直接落在
> `trash/<user>/<bucket>/`」**已作废**）。分成两层是必须的：阶段 5 落地 `file delete`
> 之后，桶级扫描如果看到 `trash/<user>/<bucket>/` 就会把它误当成「孤儿桶条目」列出来；
> 现在扫描跳过点开头的条目（`.files` / `.original`），另有一道形状检查兜底（第 5.4 节）。

初始化是**幂等**的，且**两边共用同一套规则**（同一份 `ensure_root` / `check_root` 实现）：
**服务在启动/换根时对自己的数据根执行，CLI 在双击时对自己的数据根执行**；两边都只补不缺、
都不碰业务数据内容。规则细节：

- 六个目录（`repository/`、`trash/`、`config/`、`data/`、`log/`、`temp/`）不存在则创建，已存在则保持原样，
  **绝不删除、清空或覆盖**已有内容。
- **`current_user` 由初始化补上占位名 `user`**（阶段 4 起）：这一步在**服务侧**的
  `initialize_root()`（启动或 hello 换根时）里做——写出默认 `config.json` 之后读一次配置，
  发现 `current_user` 为空就置为 `user` 并保存。CLI 双击时调的是共用的
  `ensure_root` / `check_root`（只补缺失的目录与默认 JSON），**不写配置内容**。
  因此磁盘上的是 `repository/user/<bucket>/…`，用户不需要先「设置当前用户」；
  `FMT-604 NoCurrentUser` 只在用户被显式清空时出现（`FMT 开发文档.md` 第 93 节、
  `FMT 技术文档.md` 第 4.4.1 节）。
- 默认 JSON（`config/config.json`、`config/server.json`、`data/file.json`、`data/share.json`、
  `data/trash.json`、`data/user.json`）只在缺失时写入；关键 JSON 一律先写 `.tmp` 再原子替换，
  避免异常退出写出半文件。**临时名必须每个进程、每次调用都不同**
  （`<目标>.<pid>.<序号>.tmp`，提交 `5b316b3`）：同一个目标会有两个写者（`service install`
  时安装器与服务启动都写 `service.json`），固定 `<目标>.tmp` 会互相踩——先完成的一方把它
  rename 走，另一方读回校验时报「无法打开文件」，而调用方忽略返回值 →
  `service.json` 静默不更新；另外进程内对原子写加互斥、替换遇
  `ACCESS_DENIED` / `SHARING_VIOLATION` / `LOCK_VIOLATION` 短暂重试（40 × 5 ms）。
- **已存在的 JSON 会被真正读一遍**（解析 + 版本检查）来确认完整性；读不出来或版本不受支持的，
  **只报告、绝不重置**——沿用第 2 节第 13 项「JSON 损坏不能静默重置」。日志记一行
  `[Cli] 数据根损坏（未自动修复）：data/file.json`，同时把异常送到 stderr。
- CLI 侧在**打开日志器之前**执行这一步，所以 `log/` 也由它创建、并出现在同一条「新建目录」清单里。
  CLI 仍然**不改任何业务数据**（不写 `data/*.json` 的内容、不删文件），它只是把「缺的目录和默认文件」补齐。

CLI 双击时这一步的结果**只进日志**（`log/fmt.log`，模块 `Cli`），**控制台一行都不打**：

```text
[Cli] 数据根检查：D:/FMT2
[Cli] 数据根新建目录：repository, trash, config, data, log, temp
[Cli] 数据根新建文件：D:/FMT2/config/config.json, ...
```

第二次及以后双击（什么都没缺）只是日志里换成一行：

```text
[Cli] 数据根检查：D:/FMT2
[Cli] 数据根完整
```

**只有异常才走 stderr**：数据根无法补齐（如 `FMT-013`）、以及
`数据根文件损坏（未自动修复）：data/file.json`。理由见第 7.5 节：
控制台负责用户交互与重要异常，日志负责完整运行记录。

CLI 曾经的两处「例外」（写日志用的 `log/` 与执行提权命令前要用的 `temp/`）现在都已经被这一步
统一覆盖：CLI 建目录这件事不再有特例，只有**补缺失**这一条规则（第 4.3 节、第 7.2 节）。

**`temp/` 的定位与规则**：

```text
定位      临时文件目录：不是业务数据、也不是日志，内容随时可以清空
放什么    ① 提权结果文件 temp/fmt-elev-<父进程 pid>.json（父进程读完立刻删除，第 4.5 节）
          ② 以后上传时的暂存文件（旧文写「temp/ 或者系统临时目录」，现在明确为 temp/）
谁创建    CLI 在执行提权类命令前会尝试创建 <数据根>/temp；创建不出来（例如 exe 放在只读位置）
          就退回系统临时目录 %TEMP%，并写一行 WARN 说明原因与改用后的路径。
          提权副本在写入结果文件前也会确保目录存在——它自己有权限，
          所以调用方数据根只读时它仍然能建出来
清理      Service 启动时删除 temp/ 下**十分钟以前**的、以 fmt- 开头的遗留文件
          （上次异常退出留下的提权结果等），用户手放进去的其它文件一律不动；
          删除数量记一行 INFO（第 4.6 节 ServiceMain 序列）。
          **按年龄过滤的理由（提交 `5b316b3`）**：提权结果文件的临时文件也叫
          `fmt-elev-<pid>.json(.tmp)`，而 `service install` 会在同一次操作里启动服务——
          一律清掉会把父进程正在收的结果文件删掉，父进程报「FMT-602 提权副本没有返回结果」，
          **服务其实装好了**（假失败，见第 4.5 节）。结果文件寿命只有几十毫秒
```

`temp/` 跟着 exe 走：它在数据根下，用户一眼能找到、随时可清。它按定义就是**可清空**的目录，
与 `repository/`、`data/`、`config/` 分属不同职责，因此把短暂的提权结果或上传暂存放进去，
不会污染业务数据。

### 3.3 服务自身状态文件（不属于业务数据）

```text
%ProgramData%\FMT\service.json     ← 只存「当前数据根路径」与安装信息
```

它描述「服务怎么跑」，不是任何数据根里的业务数据，因此**不放进数据根，也不放进 `log/`**。
数据根被删除或移动后，服务仍能读到这里记录的路径并报出明确错误，而不是自己猜一个目录。
服务开机自启且没有 CLI 连接时，数据根取这里的记录值；从未记录过则取服务宿主 exe 所在目录
（见第 3.1 节优先级表）。

## 4. 进程与通道

### 4.1 `fmt.exe` 的三种形态

同一个 `fmt.exe`（**只有一个可执行 target**，绝不拆成 `fmt_cli` / `fmt_svc`），靠**启动方式与内部
参数**区分三种形态：

| 形态 | 触发方式 | 生命周期 | 职责 |
|---|---|---|---|
| CLI 形态 | 用户双击或命令行启动 | 用户关窗口为止 | 交互界面、声明数据根、经命名管道发请求；双击时对自己所在数据根做一次幂等体检与补齐（只补缺失，见第 3.2 节），此外**不碰 core 业务操作、不写业务 JSON**（写日志用的 `log/` 与提权前要用的 `temp/` 见第 7.2 节） |
| Service 形态 | SCM 按注册的 `binPath` 启动 | 常驻，开机自启 | 服务入口、初始化数据根、承载业务核心与 HTTP 入口；**唯一写 `data/*.json` 的进程** |
| 提权短命副本 | CLI 用 `ShellExecuteExW(lpVerb=L"runas", ...)` 拉起自己，带内部参数 `--elevated` | 秒级，做完一次操作即退出 | 只执行一次 `service install/uninstall/start/stop/reinstall`，把结果写进结果临时文件回传给父进程。`service status` **不在这里**——它是查询命令，不提权、不进提权副本 |

入口分发靠一个 API 完成，不需要额外的模式参数：

```text
wmain
 ├─ StartServiceCtrlDispatcherW({ L"FMT", ServiceMain })
 │     ├─ 成功                                            → Service 形态
 │     ├─ 失败且 ERROR_FAILED_SERVICE_CONTROLLER_CONNECT  → 用户启动的 → CLI 形态
 │     └─ 其它错误                                        → FMT-602 / 退出码 8
 └─ CLI 形态若带内部参数 --elevated → 提权短命副本分支（不进命令循环）
```

- SCM 只给 **30 秒**连接窗口，所以 `wmain` 必须**尽早**调用 `StartServiceCtrlDispatcherW`。
- `ServiceMain` 里初始化慢时先 `SetServiceStatus(SERVICE_START_PENDING)` 并上报 `dwCheckPoint`，
  否则 SCM 报 1053。
- `ServiceMain` 里初始化**失败**时，把 FMT 编号（`FMT-NNN` 解析出的数字部分，**不是退出码**）
  写进 `SERVICE_STATUS::dwServiceSpecificExitCode`，并置 `dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR`、
  `dwCurrentState = SERVICE_STOPPED`。CLI 用 `QueryServiceStatusEx` 读回这个编号、映射成错误码，
  于是能打印 `服务启动失败：FMT-008 配置错误`，而不是只会说「启动失败」（第 4.8 节引导流程第 3 步）。
- manifest **固定 `asInvoker`**（`resources/fmt.manifest`）：CLI 形态与 Service 形态都不需要管理员
  权限，只有提权副本需要，而它由 UAC 临时授权，所以**绝不**写 `requireAdministrator`。

```text
                        fmt.exe（唯一产物，asInvoker）
        ┌───────────────────────┼───────────────────────┐
   双击 / 命令行              SCM 启动              ShellExecuteExW "runas"
        │                       │                        │
   CLI 形态（唯一窗口）     Service 形态（常驻）      提权短命副本（SW_HIDE，无窗口）
```

### 4.2 通道总图

```text
  ┌────────────────┐  命名管道 \\.\pipe\fmt.control   ┌──────────────────────────────┐
  │  CLI 窗口       │ ──────────────────────────────→ │  fmt.exe（Service 形态）      │
  │  互斥体单例     │ ←────────────────────────────── │  全局唯一，开机自启           │
  └────────┬───────┘  [4B 长度][UTF-8 JSON]          │                              │
           │                                         │   ipc ──┐                    │
           │ ShellExecuteExW "runas"                 │         ├─→ service 层        │
           ▼                                         │ server ─┘         │           │
  ┌────────────────┐  结果临时文件                    │                   ▼           │
  │ 提权短命副本    │ ──────────────────────────────→ │              core 层          │
  │ 写入 {ok, code, │ ←────────────────────────────── │                   │           │
  │ message, exit}  │  temp\fmt-elev-<pid>.json       │              storage 层        │
  └────────┬───────┘                                 └─────────── 当前数据根 ──────────┘
           │ Advapi32（OpenSCManagerW / CreateServiceW / ControlService / DeleteService）
           ▼
  ┌────────────────┐
  │ 服务控制管理器  │
  └────────────────┘

  浏览器 ── HTTP localhost:4122（cpp-httplib，进程内线程）──→ Service 形态的 server 模块
```

三条路径互相独立，不能混用：

| 路径 | 谁走 | 通道 | 为什么不能混 |
|---|---|---|---|
| 业务命令 | CLI → 服务 | 命名管道 | HTTP 可关闭、端口可被占，CLI 不应随之失效 |
| 业务命令 | 浏览器 → 服务 | HTTP `localhost:4122` | 浏览器只会 HTTP |
| 服务生命周期 | CLI → SCM | **直连 SCM + UAC 提权** | 服务尚未安装时走任何进程间通道都是死锁 |
| 提权结果回传 | 提权副本 → CLI 父进程 | **结果文件** `<数据根>\temp\fmt-elev-<父进程 pid>.json`（数据根不可写时退回 `%TEMP%`） | 提权副本是高完整性进程，它创建的命名管道会被 MIC「禁止向上写」挡住（4.5 节） |

### 4.3 CLI ↔ 服务：命名管道（已冻结）

```text
管道名   \\.\pipe\fmt.control
类型     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT
实例数   PIPE_UNLIMITED_INSTANCES
帧       [4 字节小端长度][UTF-8 JSON]，一请求一响应，用 id 配对
```

请求：

```json
{"id": 1, "op": "hello", "root": "D:\\FMT2", "pid": 1234}
{"id": 2, "op": "bucket.list"}
```

响应**复用 HTTP 信封**——同一份信封在两个通道上复用，错误码用同一个函数还原：

```json
{"ok": true,  "data": { }}
{"ok": false, "error": {"code": "FMT-305", "message": "未设置当前 Bucket"}}
```

`id` 由 CLI 生成、服务原样回填；`root` 是 CLI 声明的数据根；`pid` 供服务诊断。

两个必须处理的 Windows 坑：

1. **DACL**：服务以 `LocalSystem` 运行，管道默认只有 SYSTEM / Administrators 能连。必须用
   `ConvertStringSecurityDescriptorToSecurityDescriptorW` 显式授权交互用户（IU）：
   `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)`
2. **强制完整性级别（MIC）**：高完整性进程创建的对象带高完整性标签，中完整性的普通 CLI 会被
   「禁止向上写」挡住。必须再给管道加 MIC 标签 `S:(ML;;NW;;;ME)`，否则非提权 CLI 连接报
   `ERROR_ACCESS_DENIED`。

超时：连接时管道不存在最多重试 **5 秒**（权限类错误不重试）、普通命令 **30 秒**
（`ipc::kCommandTimeoutMs`）、**`file.upload` 30 分钟**（`ipc::kUploadTimeoutMs`，
提交 `a2b6cd1`；到点 CLI 会提示「服务端可能仍在处理」，见第 12 节）。
连不上服务（`ERROR_FILE_NOT_FOUND`）时输出
「无法连接 FMT Service，请先执行 service install」，错误码 `FMT-601`、退出码 8。

### 4.4 浏览器 ↔ 服务：HTTP

```text
http://localhost:4122        cpp-httplib，编译进 fmt.exe，只在 Service 形态里监听
```

- 开关与地址由**数据根**下的 `config/server.json` 控制（`enabled`、`host`、`port`，默认
  `localhost:4122`），默认只绑本机回环，不监听外部网卡。
- HTTP 请求作用于**当前数据根**。
- 浏览器入口与 CLI 入口**进入同一个 service 层**（第 1 节分层图），业务行为天然一致，不存在
  第二套文件处理逻辑。
- 响应体与第 4.3 节的信封完全一致。**浏览器入口 V1 只实现 HTTP**；HTTPS 监听仍列在
  V1 范围之外（第 14 节）。
- **注意别与「上传来源」混起来（提交 `a2b6cd1`）**：`file upload <来源>` 的 **`https://`
  来源已经支持**，走的是 `common/http_client`（WinHTTP + Schannel，系统组件，
  不引 OpenSSL、不分发 DLL，自动使用系统代理）。浏览器入口的 HTTPS 监听与它是两件事。

### 4.5 `service` 命令：直连 SCM + UAC 提权（已冻结）

```text
fmt> service install     提权 → OpenSCManagerW → CreateServiceW（AUTO_START、LocalSystem）
                              → 配 Recovery → StartServiceW；已存在 → FMT-600，不重复创建
fmt> service uninstall   提权 → ControlService(STOP) → DeleteService
                              （不得删除 repository / trash / data / config / log / temp）
fmt> service start       提权 → 存在则 StartServiceW；不存在 → FMT-601
fmt> service stop        提权 → ControlService(SERVICE_CONTROL_STOP) → 停止接受新请求
                              → 等在途事务 → 停 HTTP → 退出
fmt> service status      **不提权** → service::query_status()（第 4.6.1 节；「未安装」是正常结果）
                              → 打印状态 / 宿主 / 数据根 / 错误码；未安装 → FMT-601、退出码 8
```

`service status` 的输出样例（服务已安装且运行中）：

```text
服务状态：运行中
服务宿主：D:/FMT2/fmt.exe
服务数据根：D:/FMT2
错误码：0
```

- 状态名由 `service::StatusInfo::state` 映射（`service::state_name()` 一处提供）：
  `NotInstalled` → `未安装`（`OpenServiceW` 报 `ERROR_SERVICE_DOES_NOT_EXIST`）/
  `Stopped` → `已停止` / `StartPending` → `正在启动` / `StopPending` → `正在停止` /
  `Running` → `运行中` / `ContinuePending` → `正在继续` / `PausePending` → `正在暂停` /
  `Paused` → `已暂停` / `Unknown` → `未知`（拿到服务但状态码不认识，不猜测）。
- `服务宿主` 取 `service::installed_binary_path()`（`QueryServiceConfigW` 的 `lpBinaryPathName`，
  去掉引号）；`服务数据根` 取 `%ProgramData%\FMT\service.json` 里记录的当前数据根
  （`service::load_state()`，第 3.3 节）。
- 未安装时 stdout 打印 `服务状态：未安装`，退出码 **8**（`FMT-601 ServiceNotInstalled`）；
  已安装但未运行时照常打印（如 `已停止`），退出码 **0**——「服务没在跑」不是错误，
  `status` 只回答「它现在什么样」。
- `query_status()` 自身**只报「查询失败」**（打不开服务控制管理器等 → `FMT-602` / 8）；
  「未安装要返回 `FMT-601` / 8」是命令层的判断（第 4.6.1 节）。

- **只有这五条**：`install` / `uninstall` / `start` / `stop` / `status`。没有 `pause` / `continue`，
  也没有 `delete`（旧文档的 `delete` 已更名为 `uninstall`）。命令**不带 `--` 前缀**：写
  `fmt.exe service install`，旧写法 `fmt.exe --service install` 作废。
- 服务**不声明** `SERVICE_ACCEPT_PAUSE_CONTINUE`；`HandlerEx` 只处理 `SERVICE_CONTROL_STOP`、
  `SERVICE_CONTROL_SHUTDOWN`、`SERVICE_CONTROL_INTERROGATE`。
- **四条动作命令都提权**：`install` / `uninstall` / `start` / `stop` 即使目标状态已经满足也照常提权，
  不做免提权优化。已满足状态由命令自身返回业务错误码（例如重复 `install` → `FMT-600`），
  而不是靠跳过提权绕过。
- **`status` 是查询，不提权**：它不走提权副本、不弹 UAC，也不需要管理员权限。这是它与其他
  那几条动作命令的唯一区别；它同样不经命名管道，直连 SCM。

提权流程（提权副本不是独立入口分支，而是 CLI 形态收到内部参数 `--elevated` 后的短命分支）：

命令行形状（**已定稿**）：

```text
结果文件：<数据根>\temp\fmt-elev-<父进程 pid>.json
          固定名、跟着 exe 走，用户一眼能找到、随时可清（第 3.2 节）
          数据根不可写时退回 %TEMP%\fmt-elev-<父进程 pid>.json，并记一行 WARN
命令行  ：fmt.exe --elevated <operation> --result "<结果文件的绝对路径>"
operation ∈ install | uninstall | start | stop | reinstall
            （五种，全部是「动作」命令；`service status` 是查询、不提权，因此**不在**这个清单里）
```

`reinstall` = 先卸载（服务未安装时忽略该错误）再安装并启动，用于 4.8 节「服务宿主 exe 已丢失」
那一行，**一次 UAC 做完**。

> **提交 `c573f14` 起 `reinstall` 也是给用户敲的正式命令**（原来只在双击引导内部用）：
> `is_user_service_command()` 加入它，两处用法提示、命令总览、`help service` 正文都更新。
> 于是**用户能敲的 service 子命令 = 上面这五个 operation + `status` = 六条**，
> 其中**除 `status` 外每条都提权**。原口径「service 只有四条命令」「`reinstall` 只在引导
> 流程内部使用」**已作废**。
> **三个「不变」**：① 业务数据不变（`uninstall()` 只 `DeleteService`，不删
> `repository / trash / config / data / log / temp`）；② `C:\ProgramData\FMT\service.json`
> 不变（卸载不删状态目录 → `current_root` 保留，重装后数据根仍是原来那个，直到某个 CLI
> 连上来重新声明）；③ 数据根仍由 CLI 声明，与「宿主 exe 是谁」无关。
> **为什么这是更新 exe 的正确路径**：服务在跑时旧宿主 exe 被占用、无法覆盖；
> `reinstall` 先停旧宿主解开占用，再把注册指向**你运行的那一份**新 exe——
> **不需要先复制，也只要一次 UAC**（`FMT 技术文档.md` 第 13.8.2、13.8.4、18.34 节）。

```text
CLI 形态（未提权）
  │ 1. 判断是否已提权：OpenProcessToken + GetTokenInformation(TokenElevation)
  │ 2. 未提权 → 打印「需要管理员权限」（FMT-603 AdminRequired）→ 打印「正在提权...」
  │ 3. 先确保 <数据根>\temp 存在（建不出来 → 退回 %TEMP% + 一行 WARN），
  │    再删除可能残留的结果文件 <数据根>\temp\fmt-elev-<父进程 pid>.json
  │ 4. ShellExecuteExW(lpVerb=L"runas",
  │                    lpFile=<具名 self 变量>.c_str(),   ← 不能直接用临时对象，见下
  │                    fMask=SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI,
  │                    nShow=SW_HIDE)
  ▼
提权短命副本（--elevated，无窗口，不进命令循环）
  │ 5. 执行一次 service operation（五种之一）
  │ 6. 先确保 temp/ 存在，再把结果 {ok, code, message, exit}
  │    写进结果文件 <数据根>\temp\fmt-elev-<父进程 pid>.json
  ▼
CLI 形态
  │ 7. WaitForSingleObject(hProcess, 60000) + GetExitCodeProcess
  │ 8. 读结果文件（读完即删除），打印第 6 步写下的结果，并按退出码退出
```

- `runas` 启动的控制台程序**会另开一个控制台窗口**，所以提权副本必须 `SW_HIDE` 无窗口，结果写
  结果文件、由父进程读回打印——这是「只留一个窗口」的前提。
- **为什么回传不用命名管道**：提权副本是**高完整性进程**，它创建的命名管道带高完整性标签，
  中完整性的父进程受 MIC「禁止向上写」限制，**连接和读取都会被拒**（与技术文档 13.9.2 的
  「坑 2」同一机制）。结果文件写在数据根下的 `temp/` 里，父子是同一个用户、只是令牌不同，
  两边都能正常读写，不需要放宽任何安全描述符；只有数据根不可写时才退回 `%TEMP%`。
- 提权副本在写入结果文件前会**自己确保 `temp/` 存在**（它有权限，所以调用方数据根只读时仍能建出来）；
  **Service 启动时**会清理 `temp/` 下遗留的 `fmt-*` 文件（用户手放的其它文件不动），删除数量记一行 INFO。
- **结果文件是唯一通道**：命名管道方案作废，不再保留任何候选方案。
- `SHELLEXECUTEINFOW::lpFile` 必须指向具名变量：`info.lpFile = path_from_utf8(self).c_str();`
  会让指针指向语句结束即析构的临时 `std::wstring`，实测表现为 Win32 1155
  `ERROR_NO_ASSOCIATION`，看不出是提权的问题。
- 提权相关失败**不新增错误码**，全部复用附录 A 的已有编号：

| 情况 | 错误码 | 退出码 |
|---|---|---|
| 用户在 UAC 点「否」（`ShellExecuteExW` 失败且 `GetLastError() == ERROR_CANCELLED`，1223） | `FMT-004` | 5 |
| 非管理员账户 / 策略禁止提权（`ShellExecuteExW` 失败且 `ERROR_ACCESS_DENIED`，5） | `FMT-603` | 5 |
| `ShellExecuteExW` 其它失败 | `FMT-602` | 8 |
| 提权副本等待超时（60 秒）、SCM 操作失败 | `FMT-602` | 8 |
| 结果文件不存在（提权副本崩了） | `FMT-602` | 8 |
| **结果文件被 temp 清理误删**（提交 `5b316b3` 已修：清理只清十分钟以前的） | `FMT-602` | 8（**假失败**：提权副本报成功、服务其实已装好，父进程却读不到结果——见第 4.2 节与 `FMT 技术文档.md` 第 13.8.3 节） |
| 未提权提示（打印「需要管理员权限」） | `FMT-603` | 5 |

`FMT-603` 只在**最终无法提权、命令以失败告终**时作为退出码；正常提权流程里它只是提示行，
最终退出码以提权副本回传的结果为准（样例中就是 `0`）。

CLI 侧输出样例：

```text
fmt> service stop
需要管理员权限
正在提权...
执行成功...
错误码：0

fmt>
```

（提示符 `fmt> ` 之前先输出一个空行：横幅之后一次、每条命令之后一次；空命令不重复空行，见第 7.5 节。）

### 4.6 服务注册参数与恢复策略（已冻结）

| 项 | 值 |
|---|---|
| 服务名 | `FMT` |
| 显示名 | `FMT File Management Service` |
| 启动类型 | `SERVICE_AUTO_START`（开机自启） |
| 运行账户 | `LocalSystem` |
| 接受的控件 | `SERVICE_ACCEPT_STOP` + `SERVICE_ACCEPT_SHUTDOWN`（**不含** `SERVICE_ACCEPT_PAUSE_CONTINUE`） |
| 宿主路径 | 首次安装时注册的那个 `fmt.exe` 绝对路径（第 4.8 节） |
| Recovery | `ChangeServiceConfig2W(SERVICE_CONFIG_FAILURE_ACTIONS)`：第一次失败 5 秒、第二次 10 秒、后续 30 秒重启；失败计数 **1 天**重置 |
| watchdog | **不自建**，交给 SCM 的 Recovery |

`ServiceMain` 关键序列：

```text
RegisterServiceCtrlHandlerExW
  → SetServiceStatus(SERVICE_START_PENDING[, dwCheckPoint])
  → 读 %ProgramData%\FMT\service.json，确定数据根
  → 初始化存储与业务服务（幂等建目录 + 默认 JSON，含 temp/；与服务/CLI 共用的 ensure_root）
  → 清理 <数据根>/temp 下**十分钟以前**的、以 fmt- 开头的遗留文件
    （用户手放的其它文件不动；提交 5b316b3 起按年龄过滤，否则会把 service install
      正在等的提权结果文件删掉 → 假失败 FMT-602），删除数量记一行 INFO
  → 起 HTTP 线程
  → SetServiceStatus(SERVICE_RUNNING, ACCEPT_STOP | ACCEPT_SHUTDOWN)
  → 等停止事件
  → 停 HTTP → 等在途操作 → SetServiceStatus(SERVICE_STOPPED)
```

**初始化失败时的上报**（第 4.8 节引导流程第 3 步依赖它）：任一步失败都**不把异常抛给 SCM**，
而是走到 `SetServiceStatus`，把 FMT 编号写进 `dwServiceSpecificExitCode`、置
`dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR`、`dwCurrentState = SERVICE_STOPPED`，
让 CLI 能读回「到底哪儿错了」：

```text
失败（如 FMT-008 ConfigError）
  → SERVICE_STATUS {
        dwServiceType             = SERVICE_WIN32_OWN_PROCESS,
        dwCurrentState            = SERVICE_STOPPED,
        dwWin32ExitCode           = ERROR_SERVICE_SPECIFIC_ERROR,
        dwServiceSpecificExitCode = 8,        ← FMT-008 的数字部分
        dwCheckPoint = 0, dwWaitHint = 0 }
  → 记一行 ERROR 日志（含完整 FMT-NNN 与消息）
```

CLI 侧用 `QueryServiceStatusEx(..., SC_STATUS_PROCESS_INFO, ...)` 读回 `dwServiceSpecificExitCode`，
再用 `code_from_number` 还原成 `FMT-008`，打印 `服务启动失败：FMT-008 配置错误`。

### 4.6.1 查询接口与「等它落定」（已冻结）

**所有对 SCM 状态与失败编号的读取都走一组统一接口**（实现在 `service` 模块，
详见 `FMT 技术文档.md` 13.4.2）：

```text
service::State { NotInstalled, Stopped, StartPending, StopPending, Running,
                 ContinuePending, PausePending, Paused, Unknown }
service::state_name(State)    -> 中文状态名（唯一出处）
service::StatusInfo { state, wait_hint_ms, win32_exit_code, service_exit_code }
service::query_status()        -> Result<StatusInfo>
       「未安装」是**正常结果**（state = State::NotInstalled），不是错误；
       只有查询本身失败（打不开服务控制管理器等）才返回错误（FMT-602 / 退出码 8）
service::query_state()         只是 query_status() 的薄封装（取 state，失败给 Unknown）
service::installed_binary_path() -> Result<std::string>（服务宿主 binPath，去掉引号）
service::last_start_failure()   -> Result<ErrorCode>
       也基于 query_status()，读 win32_exit_code / service_exit_code，
       把 service_exit_code 还原成 FMT 编号；没有失败信息时返回错误（FMT-602）
```

> 名字别混：`State` 是**运行状态枚举**，`ServiceState` 是 `%ProgramData%\FMT\service.json`
> 的结构体（`current_root` / `host_path` / `installed_at`，由 `load_state()` 读取）。

旧的「`QueryServiceStatus` + `map_state`」写法已被取代；`service status`、`service start`
的落定判定、双击引导读失败编号，三处都只走这一个入口。

**等待落定不写死时长，按 SCM 的 `dwWaitHint` 自适应**（实现是 CLI 内的 `settle_state()`）：

```text
间隔来源   服务处于 State::StartPending / State::StopPending 时，每轮查询读 SCM 给的 dwWaitHint
夹取区间   100 ms – 2000 ms（kMinStepMs / kMaxStepMs）
兜底上限   整体 30 秒（kSettleCapMs）；最后一步不越过上限
提前结束   状态一旦不是等待类（Running / Stopped / Paused / NotInstalled …）就立即结束
返回值     落定后的 State；回来仍是等待类就说明「没起来」
```

理由：`ServiceMain` 启动时要建目录、读 JSON、扫 `data/file.json`、起 HTTP、等管道就绪，
慢机器、大目录、杀毒软件介入时完全可能超过 8 秒——此时服务**并没有失败**，只是还在
`StartPending`。写死 8 秒会把「还在启动」误判成「启动失败」，接着白弹一次 UAC 去做根本
解决不了问题的重装（而且重装之后同样慢，用户会被反复弹 UAC）。**等多久，由 SCM 自己说。**

### 4.7 CLI 单实例窗口（已冻结）

- 互斥体名 `Local\FMT.CLI.v1`；`GetLastError() == ERROR_ALREADY_EXISTS` 表示已有实例。
- 已有实例时**不新建窗口**，转去激活它：`EnumWindows` 找 `ConsoleWindowClass` 的窗口 →
  `SetForegroundWindow`；受前台锁定限制失败时退化为 `FlashWindowEx` 提醒用户。
- **UIPI**：高完整性进程的窗口无法被中完整性进程置前，所以提权副本必须无窗口（第 4.5 节）。
- 推论：**同时只有一个 CLI 窗口，同时只有一个数据根**；换数据根必须先关掉旧窗口。

### 4.8 服务宿主 exe：移动请用复制（已冻结）

Service 形态的宿主 = **首次 `service install` 时注册的那个 exe 的绝对路径**（`CreateServiceW` 的
`lpBinaryPathName`），**不复制到别处**：注册的就是用户手上那份 `fmt.exe`。

CLI 双击时会比较服务的 `binPath` 与自身路径：

| 比较结果 | 行为 |
|---|---|
| 与自身路径**相同** | 正常作为 CLI 客户端运行 |
| **不同**但该文件**存在** | 仍然作为 CLI 客户端运行（连服务并声明自己的数据根） |
| **不同**且该文件**已丢失** | 提示重新安装服务（提权副本的 `reinstall` operation：先卸载、再安装并启动，服务未安装时忽略卸载阶段的 `FMT-601`，**一次 UAC 完成**，见第 4.5 节） |

双击时的完整引导流程：

```text
双击 D:\FMT2\fmt.exe
  1. 单实例互斥体：旧窗口还开着 → 激活旧窗口，不开第二个（换目录前必须先关掉旧窗口）
  2. 数据根体检与补齐：对自己所在数据根执行幂等 ensure_root/check_root
       （六个目录 + 六个默认 JSON，只补缺失；已有 JSON 读一遍确认，损坏只报告不重置，第 3.2 节）
       这一步在打开日志器之前完成，所以 log/ 也在这条「新建目录」清单里
       结果**只进日志**（[Cli] 数据根检查 / 数据根新建目录 / 数据根新建文件 / 数据根完整），
       **控制台一行都不打**；只有异常走 stderr（无法补齐、文件损坏）
  3. 查 SCM 状态（走统一的 `service::query_status()`，见技术文档 13.4.2；「未安装」是正常结果）
       ├─ 未安装        → 提权 install（装 + 启动，一次 UAC）
       ├─ 运行中        → 不动、不弹 UAC
       └─ 已安装未运行  → 提权 start → **等它落定**（第 4.6.1 节：按 SCM 的 `dwWaitHint` 自适应）
             ├─ 起来了       → 完成
             └─ 仍然没起     → 读服务留下的失败编号 dwServiceSpecificExitCode（第 4.6 节）
                   ├─ 数据根/配置类问题 → **不重装**（重装也解决不了），打印原因让用户先处理
                   └─ 其它             → 提权 reinstall 一次（卸载 + 安装，一次 UAC），仍失败则报错
  4. 比较服务 binPath 与自身路径（见上表）；不同且该宿主 exe 已丢失 → 询问是否 reinstall 指向当前目录
  5. 服务在运行 → 连管道发 hello 声明 root=D:\FMT2（第 3.1 节）；
       switched=true 时**在 stderr 打印换根提示**（提交 `8f2fbc5`）并记一行日志「数据根切换：旧 -> 新」（**原口径「只进日志」已作废**）
  6. 打印横幅（`File Manager Tool  v1.0  ( build  <日期> )` + Service Running.../Service Stopped...）
       → 空一行 → fmt> 提示符
```

第 3 步判定「数据根/配置类问题」的错误码集合（命中就**跳过重装**、直接打印原因让用户先处理）：

| 错误码 | 含义 |
|---|---|
| `FMT-006` | `JsonParseError` JSON 格式错误 |
| `FMT-007` | `JsonWriteError` JSON 写入失败 |
| `FMT-008` | `ConfigError` 配置错误 |
| `FMT-011` | `JsonUnsupportedVersion` JSON 数据版本不受支持 |
| `FMT-013` | `DirectoryCreateFailed` 目录创建失败 |
| `FMT-009` | `StorageError` 存储操作失败 |
| `FMT-005` | `IoError` 磁盘/IO 错误 |
| `FMT-014` | `PathEscape` 路径穿越 |

这些都不是「服务注册坏了」，重装服务解决不了；报给用户、由用户先处理数据根或配置。
集合之外的失败（例如 `FMT-602 ServiceOperationFailed`）才走「提权 reinstall 一次」。

**移动请用复制**：复制出一份新的 `fmt.exe` 不影响已有服务；而**剪切或删除**已注册的宿主
`fmt.exe` 会让 SCM 无法按 `binPath` 启动服务（报 1053），必须重新 `service install`。

## 5. 数据模型

### 5.1 JSON 文件格式（已冻结）

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

### 5.2 file.json

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
      "is_trash": false,
      "trash_reason": ""
    }
  ]
}
```

`file.json` **禁止**保存 `max_download_count`、`download_count`、`expire_time`
——这些属于 Share（`FMT 开发文档.md` 第 15 节）。

**`trash_reason`（阶段 4 起，`is_trash = true` 时同时写）**：取值 `"bucket"`（桶被删）
或 `"file"`（文件自己删的，阶段 5 起）；不在回收站时为空串。它存在的唯一理由是**区分
「谁把这条记录送进回收站」**：

- 桶删除：只给 `is_trash` 为 false 的记录置位，写 `trash_reason = "bucket"`；
- 桶回退：**只翻回 `trash_reason == "bucket"` 的那些**并把 `trash_reason` 清空，
  用户单独删过的文件（`"file"`）保持不动。

没有这个字段，回退一个桶就会把用户自己删过的文件一起放出来（`FMT 技术文档.md` 第 7.2 节）。

**提交 `188e85d` 已落地 `"file"` 那一半**：`file delete` 只改**自己那一条**记录
（`FileService::remove()`：`is_trash = true` + `trash_reason = "file"`），
不动桶级那套 `set_bucket_files_trash_flag()`；桶回退时 `"file"` 的记录一律不动。
**提交 `0ad9efc` 补一句**：`remove()` 的参数是 `<file_id|文件名>`（签名
`remove(std::string_view file_id_or_name)`），定位走私有 `locate_record()`（与 `file get`
同一套规则：先 `file_id`、再文件名；`get` 侧仍是 `get_by_id()` + `get_by_name()`），**命中之后落点用的是记录自己的 `bucket`**，
与「用户现在站在哪个 Bucket」无关——在桶 B 里按名字删桶 A 的文件，
文件进 `trash/<用户>/.files/<A 的桶名>/…`（第 5.5 节与
`FMT 技术文档.md` 第 10.2.4、18.21 节）。

**提交 `5bf2c1f` 补第二条（数据损坏修复）**：`file.json` 的**唯一性口径是
「不区分大小写」**——`doc.txt` 与 `DOC.TXT` 不能共存（上传时 `commit_upload()` 用
`iequals()` 判重名 → `FMT-105`）。按精确比较放过时，两条记录会指向**同一个磁盘文件**，
第二次上传**覆盖**第一个文件的字节而第一条记录的 `size`/`md5` 还留在旧值上——
**同一磁盘文件被两条记录指向 + 字节被覆盖 + 元数据失真**。`locate_record()` 与
`get_by_name()` 的同名比较、桶侧的 `is_current`/`was_current`/回收站定位同样改成
`iequals()`（第 5.5 节、`FMT 技术文档.md` 第 18.22 节）。

**提交 `9c3d2cb` 再补第三条（保留形状 + 歧义）**：`fmt-YYYYMMDD-N` 被定为**保留形状**——
`validate_file_name()` 在「Windows 保留设备名」之后加一条 `looks_like_file_id()`（最短 14 个字符：
4 + 8 + 1 + 1；`fmt-` 前缀按 ASCII 折叠比较，日期段恰好 8 位数字，序号段全数字且非空），
命中即 `FMT-106 FileNameLikeFileId`（退出码 2，属 `FMT-1xx` 文件名校验一组），
显式名与从来源推断的名字走同一道校验。放行的反例：`fmt-20261008-0.txt`、
`my-fmt-20261008-0`、`fmt-20261008`（没有序号）、`fmt-2026100-0`（日期 7 位）。
理由是定位规则本身：`file delete` 先按 `file_id` 查、查不到再按名字查，
若一个文件就叫 `fmt-20261008-0` 而另一个文件的 `file_id` 恰好是它，按名字提交的删除会删错对象。
旧数据里已经存在的这种名字不静默处理：`locate_record()` 现在把两条索引分别记下，
**两者命中不同记录**时报 `FMT-001`「有歧义：… 请直接用 file_id 指定要删哪一个」，
消息里点名两条记录。**只有 `file delete` 加这一步**（`locate_record()` 只被 `remove()` 用），
`file get` 的两种查询范围保持不变——只读查询最坏是把 id 命中的那条给用户看，破坏性操作不能猜。
`file get` 另有一处更新：命中回收站记录时**增加** `trash_path`（相对数据根、正斜杠），
仓库里找不到该文件时**不返回** `path`（设计如此，不是缺失）。

**这条记录里不存路径、也不存时间（第 3.2 节与 `FMT 技术文档.md` 第 7.6 节的纪律）**：
上表十个字段就是全部，文件落在哪一天由 `file_id` 的 `fmt-YYYYMMDD-N` 片段推出——
`repository/<user>/<bucket>/YYYY/MM/DD/<file_name>`（`resolve_path()`），
软删除后是 `trash/<user>/.files/<bucket>/YYYY/MM/DD/<file_name>`（`trash_path_of()`）。
推出来的路径不存在时，在桶的日期树里找同名文件兜底，**唯一命中才用**，
多个命中报 `FMT-015 ConsistencyError`（有歧义，交给人处理）。

### 5.3 share.json

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

### 5.4 trash.json（**只读兼容**，不再是文件级条目的权威）

字段定义以 `FMT 开发文档.md` 第 17 节为准，实现细节见 `FMT 技术文档.md` 第 7.3 节。

> **口径（提交 `0fc242b`）：文件级条目的权威是 `file.json`**
> （`is_trash` / `trash_reason` / **`deleted_at`**），路径由 `file_id` 与记录推出；
> **`data/trash.json` 不再写入**——它保留为**只读兼容**（老数据缺 `deleted_at` 时从它补，
> 回退/永久删除时顺手清掉那条老记录）。桶级身份记录的唯一权威仍是
> `trash/<user>/.original`（5.4.1）。原口径「`trash.json` 只服务文件级条目、写入侧已落地」
> （commit c2d545d / 188e85d）**已被 `0fc242b` 取代**。

下面是**老数据**里的形状，仅供兼容读取对照（新装数据根里它始终是空的）：

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

> **读取侧已落地（提交 `0fc242b`）**：`trash list` / `get` / `restore` / `delete` 两级
> 都处理，`TrashService`（`src/trash/`）把文件级（`file.json`）与桶级（`.original`）
> 合成一份视图；`trash list` 会标出 `[文件]` / `[桶]`。原口径「文件级读取侧属阶段 7」
> **已作废**。
>
> **`trash_path` 的写入口径（提交 `4fee290` 定落点、`9c3d2cb` 补齐 `file get`）**：
> 中间那一段 `.files` 是**必须**的——`trash/<user>/` 顶层只留给桶级条目，
> 文件级条目全部落在 `trash/<user>/.files/<bucket>/YYYY/MM/DD/<file>`（上面的 JSON 样例
> 已按此更正；原口径 `trash/小谷/工作/…` 作废）。`file get` 命中的记录在回收站时会把
> 同一个相对路径回给用户（`data.trash_path`），仓库里找不到该文件时**不回** `path`。

#### 5.4.1 桶级回收站记录：`trash/<user>/.original`（阶段 4 已落地并冻结）

Bucket 级记录不使用 `file_id`，用 Bucket 信息管理；**唯一权威就是这张表**（形状见
`FMT 技术文档.md` 第 7.3 节）：

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

| 字段 | 说明 |
|---|---|
| `version` | 固定 `1`；不认识就拒绝，不猜不降级（第 5.1 节） |
| `buckets[]` | 桶级条目的唯一权威表 |
| `trashed` | 回收站里的目录名（**带时间戳**） |
| `original` | 原桶名；为空表示没有身份记录 |
| `deleted_at` | 本地时间 ISO 8601（无时区） |

四条硬规则：

1. **回收站目录名一律带删除时间戳**：`<原桶名>_<YYYYMMDDHHMMSS>`（例如
   `lazy-fox_20261008012233`），**无论有没有重名**；同一秒内删两次、或目录恰好同名时
   再加序号 `<原名>_<时间戳>_2`。因此同一个名字删多少次都不会互相覆盖
   （旧口径「用原名、重名才加时间戳」已作废：那样两条条目会抢同一个回退位置）。
2. **绝不靠剥离时间戳反推原名**（`x_2026...` 也可能本来就叫这个），一律查这张表。
3. **`.original` 损坏时 `bucket delete` 直接拒绝**（`FMT-006 JsonParseError`），不搬动目录——
   否则会留下一个「没有身份记录、原名永久丢失」的条目。索引写不进去时目录**搬回原位**；
   回退方向同理（索引没减掉就把目录退回去）。**磁盘与索引不允许不一致。**
4. **回退是整单判定**（这条推翻旧「Bucket 部分恢复」口径）：目标
   `repository/<user>/<原名>` 已存在就**整单拒绝**——`FMT-401 RestoreConflict`、退出码 4、
   提示「回退失败：Bucket 已存在：<原名>」，不覆盖、不改名、不把不冲突的文件先塞进去；
   目标不存在就整个目录一次 `std::filesystem::rename` 搬回，因为整棵树一次搬走，
   **不存在文件级冲突**。「部分恢复」（逐个文件判断、冲突的留在回收站）属于**文件级恢复**
   （`trash restore <file_id>`，阶段 7），第 54/55 节的逐个文件、冲突不覆盖不改名在
   文件级路径上仍然适用。

回退的**定位方式**：先用**回收站里的名字**（精确匹配）；再用**原桶名**，但同名多条时
必须唯一，否则报 `FMT-001` 并列出候选的 `trashed` 名。

对外的接口形状（`include/fmt/bucket/bucket.hpp`，已冻结）：

```cpp
inline constexpr const char* kOriginalIndexName = ".original";

struct BucketRemoval { std::filesystem::path moved_to; std::string trashed_name;
                       std::size_t files_affected; bool was_current; };
// 提交 9c3d2cb：create 的名称统一转小写，返回值带着「本来敲的是什么」给调用方提示
struct BucketCreation { std::string requested;   // 用户原本敲的
                        std::string name;        // 实际创建的名字（小写）
                        bool renamed;            // requested != name
                        bool became_current; };  // 是不是顺手设成了当前 Bucket
struct TrashBucket { std::string trashed_name; std::string original_name;
                     std::string deleted_at; bool directory_present; };
struct TrashBucketDetail { TrashBucket bucket; std::filesystem::path directory;
                           std::size_t file_count; std::uintmax_t byte_count; };
struct TrashPurge { std::string trashed_name; std::string original_name;
                    std::size_t removed_files; std::size_t removed_records; };

Result<BucketCreation>           create(std::string_view name);              // 9c3d2cb（原 Status）
Result<BucketRemoval>            remove(std::string_view name);
Result<std::vector<TrashBucket>> list_trashed();
Result<TrashBucket>              restore(std::string_view identifier);
Result<TrashBucketDetail>        get_trashed(std::string_view identifier);  // 4fee290
Result<TrashPurge>               purge(std::string_view identifier);        // 4fee290
// list / get / use / directory_of / refresh_current_bucket 签名不变；
// get / use / remove 内部改用私有 canonical_name()（不区分大小写地找磁盘上的实际桶名，
// 找不到才退回 to_lower()），落进 current_bucket / file.json / .original 的都是它；
// 私有新增 find_trashed()（restore / get_trashed / purge 共用同一套定位）
// 与 remove_bucket_file_records()（只清 trash_reason == "bucket" 的记录）
```

两类响应形状（与源码一致）：`bucket delete` 的 `data` 是
`{bucket, moved_to, trashed_name, files_affected, was_current, current_bucket, message}`，
`message` 为 `Bucket 已删除（移入回收站）：<原名>  ->  <回收站名>（之后只能整体恢复这个桶）`
（提交 `0fc242b` 补了括号里那句）；
**`bucket create` / `bucket use`（提交 `9c3d2cb`）**的 `data` 里 `bucket` 是**规范化后的名字**
（`create` 是实际建成的 `work`，`use` 是磁盘上的实际名字），名称被改过时**增加 `note`**
（`"Bucket 名称统一使用小写：已把 WORK 转为 work"`），CLI 把 `note` 打成单独一行 `提示：…`；
**`trash` 四条的 `data` 在提交 `0fc242b` 统一成一份条目形状**（`TrashEntry`）：
`trash list` 回 `{entries:[{type,id,name,bucket,deleted_at,bytes,files,present,restorable,trash_path?,reason?}], count, files, buckets}`，
`trash get` 回 `{entry}`，`trash restore` / `trash delete` 回 `{entry, message}`，
两者的 `dry_run` 预检回 `{needs_confirm, blocked, entry, message?}`；
**原口径的 `deleted_buckets`、顶层 `trashed` / `original` / `restored_to` / `removed_files` /
`removed_records` 形状全部作废**（`FMT 技术文档.md` 第 12.3.2.1 节有完整表）。
`files` 是目录里的实际文件数，`bytes` 是总字节数——**提交 `18f16ca` 起 `trash list`
也会给每个 `present` 的桶级条目遍历一次目录把它们算出来**（`trash get` 与预检同样遍历）。
**代价（如实记录）**：`trash list` 因此不是纯索引查询（每个桶条目多遍历一次目录），
但它是用户显式敲的命令；`BucketService::list_trashed()` 那层索引仍不遍历目录。
原口径「只有单条查询遍历目录、列表不做」**已作废**。

**`present` 的语义 = 「数据在不在磁盘上」（提交 `5b316b3` + `8f0fd5c`）**：`restore` / `purge`
的**结果条目一律不翻转它**（文件级 `5b316b3`、桶级 `8f0fd5c`——两级四种结果都是）：
恢复成功后数据在仓库里，它就是 `true`；拿它当「还在不在回收站」用会打印出
「状态：数据已不存在」这种误导信息（实测报上来的就是这条）。结果一律由 `message` 说明。
`8f0fd5c` 之后对着已安装的服务实测过「建桶 → 删桶 → 回退 → 永久删除」，打印里不再出现
那句话（`FMT 技术文档.md` 第 18.30 节）。

**`trash delete` 的三步与确认（提交 `4fee290`；`0fc242b` 起两级通用）**：

```text
① 删磁盘数据   文件级：删 .files/ 下那个文件；桶级：remove_all 整个回收站目录
              失败就什么都没变（记录/索引还在，可以重来）
② 清 metadata  文件级：从 file.json 删掉那条记录；
              桶级：只清 user + bucket 匹配、is_trash == true 且 trash_reason == "bucket"
              的记录，"file" 的记录绝不动
③ 摘索引       桶级：最后从 trash/<user>/.original 里删掉对应那条；
              文件级的记录在第 ② 步已经删掉
```

顺序刻意如此：先删数据，中途失败不会留下「索引没了、目录还在」的幽灵条目。
**幽灵条目（索引有、目录没了）也能永久删掉**（否则永远清不掉）。
**必须显式确认**：服务端要求请求带 `force == true`，否则 **`FMT-016 ConfirmRequired`**
（退出码 2 / HTTP 400；提交 `711da4c` 起，**原来记的是 `FMT-001`**，
消息「永久删除不可恢复，需要确认（force = true）」）；CLI **先发只读预检**
（`args.dry_run`）打印条目详情与「不可恢复」那一行，再问「确认执行？(y/N)」，
答 n → 「已取消」+ 退出码 0 且不发请求；一次性命令必须 `--yes` / `-y`，
否则本地拒绝、退出码 2；HTTP `DELETE /api/trash/<标识>` 要 `?force=1`
（或请求体 `{"force":true}`），预检用 `?dry_run=1`（第 5.5 节、`FMT 技术文档.md` 第 11.15 节）。
**已知缺口**：永久删除时**没有**清理 `share.json`（share 模块属阶段 6），属阶段 6 待办。

异常状态**如实报告、不擅自修**：

| 情况 | `trash list` | `trash restore` |
|---|---|---|
| 回收站里有目录、`.original` 里没有记录（手工拷进来的、旧版本留下的） | 照实列出，`original` 为空，CLI 显示「原名称未记录，无法回退」；**形状不符的目录根本不会被列出**（提交 `4fee290`） | `FMT-001`「回收站条目缺少原桶名记录（.original），无法回退」——**不猜名字**；`trash get` 能按目录名查到它（`original` 为空、`present: true`），`trash delete` 也能按目录名删掉 |
| `.original` 有记录、目录没了 | 标 `present: false` 如实报告，**不擅自清理** | `FMT-400 TrashEntryNotFound`「回收站目录已不存在：\<trashed\>」；`trash get` **不报错**，返回 `present: false`（提交 `4fee290`），`trash delete` 照样能把它永久删掉 |

**桶级扫描的形状检查（提交 `4fee290`）**：扫 `trash/<user>/` 时**跳过点开头的条目**
（`.files` / `.original`），并且**只认 `<名字>_<14 位时间戳>` 或
`<名字>_<14 位时间戳>_<1-3 位序号>` 形状的目录**——别的东西不会被误当成桶级条目。

删除 Bucket 是**移入回收站**，数据不丢：`repository/<user>/<bucket>/` 整体移到
`trash/<user>/<bucket>_<时间戳>/`，同时 `.original` 追加一条身份记录。同一名字删两次
得到两份数据、两条身份记录（而不是旧口径里的「两条记录抢同一个回退位置」）：

```text
user/lazy-fox（空桶） → 删除 → lazy-fox_20261008012233
再建 lazy-fox、放文件 → 删除 → lazy-fox_20261008020304
回退第一个（空桶） → repository/user/lazy-fox 重建，放新文件
回退第二个（带文件） → 目标 lazy-fox 已存在 → FMT-401 整单拒绝，不碰第一个
```

### 5.5 对象生命周期

```text
File:   上传 → 正常 → 删除 → Trash → 恢复 → 正常
                                 └→ 永久删除 → 结束        file_id 全程不变

Bucket: 创建 → 正常 → 删除 → Trash → 恢复 → 正常
                                 └→ 永久删除 → 结束        无独立 ID

Share:  创建 → 有效 → 访问（计数 +1）→ 过期 / 次数耗尽 / 文件永久删除 → 失效
```

**Bucket 这一行的阶段 4 细节**（commit c2d545d，收尾 `4fee290`）：`删除 → Trash` 落成
`trash/<user>/<原名>_<删除时间戳>/`，身份记在 `trash/<user>/.original`；
`Trash → 恢复` 是**整单判定**——目标 `repository/<user>/<原名>` 已存在就整单拒绝
（`FMT-401`，不覆盖、不改名、不做部分恢复），不存在就整个目录一次搬回；
`Trash → 永久删除` 已经落地（提交 `4fee290`）：先删目录、再清 `trash_reason="bucket"` 的
`file.json` 记录、最后摘 `.original`，**必须显式确认**（`force` / `--yes` / 交互 y），
文件级（`"file"`）记录绝不动；
`file_id` 全程不变（`is_trash` / `trash_reason` 翻回），桶本身**无独立 ID**。

**File 这一行的阶段 5 细节（提交 `188e85d`，`0ad9efc` 补一条）**：`上传 → 正常` 与
`正常 → 删除 → Trash` 都已经落地——`file delete` 是**软删除**：`file_id` 不变、文件搬到
`trash/<用户>/.files/<桶>/YYYY/MM/DD/`、`file.json` 置 `is_trash=true` /
`trash_reason="file"` / **`deleted_at` = 删除时间**；
顺序是「搬文件 → 写 `file.json`（失败搬回）」——**提交 `0fc242b` 起不再写 `trash.json`**
（`file.json` 是唯一权威，第 5.4 节），每条失败路径都能退回去
（`FMT 技术文档.md` 第 10.2.4 节）。

> **参数与定位（提交 `0ad9efc`）**：`file delete <file_id|文件名>` 与 `file get` 收
> 同一套参数、用同一套定位规则——先当 `file_id`（全局唯一、形状固定 `fmt-YYYYMMDD-N`），
> 再当文件名（当前用户 + 正常文件，同用户跨 Bucket）；**名字在、但已经在回收站** →
> `FMT-001` 并给出 `file_id`（**不能报成 `FMT-002` 文件不存在**），都没有才 `FMT-002`
> （退出码 3）。落点里的 `<桶>` 取的是**记录自己的 `bucket`**，不是当前 Bucket——
> 在桶 B 里按名字删桶 A 的文件，文件进 `trash/<用户>/.files/<A 的桶名>/…`
> （第 5.2 节 `trash_path_of()`、`FMT 技术文档.md` 第 10.2.4 节）。
>
> **提交 `9c3d2cb` 三分这一套定位**：① 文件名与 `file_id` 同形（`fmt-YYYYMMDD-N`）**上传即拒**
> （`FMT-106`，属保留形状），所以新数据不会再有这种名字；② 旧数据里若已经存在，
> `file delete` 在两个索引命中**不同**记录时报 `FMT-001`「有歧义：<key> 既是 <file_id> 的文件标识，
> 又是另一个文件的文件名（file_id <file_id>）」——**不猜**，请用户用 `file_id` 指定；
> ③ `file get` **不加**②这一步：按 `file_id` 查是全局含回收站、按文件名查只查当前用户的正常文件，
> 这是**有意的差异**（get 只读，最坏是把 id 命中的那条给用户看；delete 破坏性，不能猜）。
> `file get` 命中回收站记录时另回 `trash_path`（相对数据根、正斜杠，形如
> `trash/user/.files/工作/2026/10/08/test.txt`），仓库里找不到时不回 `path`。
>
> **比较一律不区分大小写（提交 `5bf2c1f`，数据损坏修复）**：上面三步的比较都用
> `iequals()`（ASCII 折叠），不是 `==`。按精确比较时，先传 `doc.txt` 再传 `DOC.TXT`
> 会被当成两个名字放过，而两者在 Windows 上落到**同一个磁盘路径**——第二次上传
> **覆盖**第一个文件的字节，`file.json` 里第一条记录还写着旧的 `size`/`md5`，即
> **同一个磁盘文件被两条记录指向 + 字节被覆盖 + 元数据失真**。根因防线是第 5.2 节那条
> 「同用户范围内名字唯一」**（不区分大小写）**；同一提交还把桶侧的 `is_current` /
> `was_current` / 回收站定位改成不区分大小写（`FMT 技术文档.md` 第 10.1、18.22 节）。
> **提交 `9c3d2cb` 把 `iequals()` 实现收紧成只折叠 ASCII**（`>= 0x80` 的字节原样比较）：
> 交给 `std::tolower` 在 C locale 下虽是恒等，但一旦有人调 `setlocale` 就会把 UTF-8
> 名字改坏——这是「比较一律不区分大小写」那条口径的**实现约束**，中文不受影响。
> **提交 `9c3d2cb` 又给 `file delete` 加了歧义判定**：两个索引命中**不同**记录
> （一个按 `file_id`、另一个按文件名）时报 `FMT-001` 并点名两条记录，让用户直接用
> `file_id`；`file get` 不加这一步（有意的差异，第 5.2 节）。
> 由此**待决清单已清空的一条**——「按名字删除可能跨 Bucket」**已在提交 `711da4c` 定稿**：
> **跨桶要确认**（只读预检 + `FMT-016` + y/N 或 `--yes`），既不是静默执行，也不改成
> 「按名字只在当前桶里找」（那会让跨桶同名重新变成「文件不存在」，正是 `0ad9efc` /
> `5bf2c1f` 修掉的误导诊断）。详见下面两条与 `FMT 技术文档.md` 第 10.2.4、11.15 节。
>
> **预检与确认（提交 `711da4c`）**：`file delete` 在 CLI 侧先发一次只读预检
> （`args.dry_run = true`），把「目标属于哪个 Bucket、当前是哪个 Bucket」说清楚；
> **歧义不是 `needs_confirm` 而是 `blocked`**（y/N 表达不了删哪一个，只能让用户用
> `file_id` 重发）。用户同意后才带 `force` 发真实请求；服务端**独立校验**：跨 Bucket
> 且缺 `force` → **`FMT-016 ConfirmRequired`**（退出码 2，提交 `711da4c` 起不再复用
> `FMT-001`）。成功后 `message` 与 `data.bucket` 都带归属
> `文件已移入回收站：a.txt（Bucket：工作）`。软删除也不再往 `trash.json` 追加记录
> （提交 `0fc242b`，第 5.4 节）。

**`Trash → 恢复` 与 `→ 永久删除` 两跳（文件级）已经实现（提交 `0fc242b`）**：
`trash list` / `get` / `restore` / `delete` 两级都处理，`src/trash/` 的 `TrashService`
把 `file.json`（文件级）与 `trash/<user>/.original`（桶级）合成一份视图，
列表标出 `[文件]` / `[桶]`；回退的三种「确认解决不了」情况（同名冲突 / 随桶删除 /
数据缺失）是 `blocked`。原口径「读取侧属阶段 7」**已作废**（`FMT 技术文档.md` 第 10.4、18.25 节）。

## 6. 源码模块

```text
include/fmt/<模块>/     公共头文件，按模块分子目录
src/
├── CMakeLists.txt      汇总各模块
├── main.cpp            wmain：入口分发（Service / CLI / 异常兜底）
├── common/             公共类型与工具：Error、Result、Time、String、Path、Hash、Validation、Logger、
│                        Http Client（**提交 a2b6cd1**：WinHTTP + Schannel 的流式 GET 下载，
│                        只用系统库 winhttp；浏览器服务端仍用 cpp-httplib）
├── config/             config.json / server.json 的加载、校验、保存
├── storage/            底层数据访问：JSON Storage、File Storage、Trash Storage
├── core/               数据根（FMT_ROOT）解析与切换、运行目录、程序生命周期
├── ipc/                进程间通道：命名管道服务端/客户端、帧编解码、管道 DACL 与 MIC、超时
├── cli/                命令行解析、控制台输出、交互循环、CLI 单实例窗口、提权引导
├── bucket/             Bucket 逻辑（**桶级回收站也在这里**：`list_trashed` / `restore` /
│                        `get_trashed` / `purge`、`trash/<user>/.original` 索引，
│                        提交 c2d545d 与 4fee290）
├── file/               文件逻辑（**提交 188e85d 已落地**：`FileService` 的
│                        commit_upload / list / get_by_id / get_by_name / remove /
│                        check_remove / list_trashed / check_restore / restore / purge /
│                        resolve_path / trash_path_of，以及 `prepare_upload()`；
│                        `file.json` 读写也在这里——技术文档第 2 节的目录细分
│                        把 File 的存储与规则并进了这一个模块）
├── share/              Share 逻辑（**尚未创建**，阶段 5 剩下的部分）
├── trash/              **已创建（提交 0fc242b）**：`TrashService` 组合 `FileService`
│                        与 `BucketService`，把文件级（`file.json`）与桶级（`.original`）
│                        合成一份视图 + 跨命名空间的标识解析（不存状态）
├── service/            Windows Service：SCM 生命周期、服务模式主循环、数据根切换
└── server/             HTTP Server（cpp-httplib，浏览器侧）

third_party/            vendored 单头文件库：nlohmann/json、cpp-httplib（cpp-httplib 只服务
                        HTTP 服务端；URL 下载自 a2b6cd1 起走系统 winhttp，不再依赖它）
resources/              图标、resources/fmt.manifest（requestedExecutionLevel = asInvoker）
```

> `ipc` 与 `cli` 分属通道两端，但**共用同一份帧格式定义**（`include/fmt/ipc/protocol.hpp`），
> 保证「写帧」与「读帧」不会各自漂移。该头文件必须放在 `include/fmt/ipc/` 而不是 `src/` 下的
> 私有头，否则 CLI 侧只能重复实现一遍。
>
> 目录细分以 `FMT 技术文档.md` 第 2 节为准：Share 的存储与规则并入 `file`（共用同一把锁与同一份
> 元数据读取路径）；**`trash` 已在提交 `0fc242b` 建目录**（`include/fmt/trash/trash.hpp` +
> `src/trash/trash.cpp`，第 13 节与 `FMT 技术文档.md` 第 10.4 节）。上表是**当前布局**，
> 阶段状态见第 15、16 节。

依赖方向：`CLI → IPC → Service/Business → Core → Storage → 文件系统`。
`server` 走同一条业务路径（`server → Service/Business`），不绕过 Service 层。
`common` 不依赖任何业务模块；业务模块之间不得直接调用对方的底层实现。
第三方库只在 `storage`（`nlohmann/json`）与 `server`（`cpp-httplib`）等少数模块内可见，
业务模块不直接包含它们的头文件。

```text
CLI (管道客户端)              HTTP (浏览器)
       │                            │
       ▼                            ▼
   IPC \\.\pipe\fmt.control      server
       │                            │
       └──────────┬─────────────────┘
                  ▼
            Service 层            bucket, file, share, trash, config
                  │
                  ▼
            Core 层               数据根解析与切换、生命周期
                  │
                  ▼
            Storage 层            JSON、文件操作、目录初始化
                  │
                  ▼
              文件系统（当前数据根）
```

### 6.1 模块职责边界

| 模块 | 负责 | 不负责 |
|---|---|---|
| `common` | 公共类型与工具 | 任何业务规则 |
| `config` | 配置读写与校验 | 业务数据 |
| `storage` | 读 JSON、写 JSON、建目录、移动/删除/检查文件、幂等 `ensure_root`/`check_root`（补缺失目录与默认 JSON） | 业务规则（如「文件能否删除」由 Service 决定）；决定何时调用幂等检查 |
| `core` | 数据根（`FMT_ROOT`）解析与切换、运行目录、程序生命周期 | 业务对象 |
| `ipc` | 命名管道服务端/客户端、帧编解码、请求/响应信封、管道 DACL 与 MIC、超时 | 业务规则（只把 `op` 交给 Service 层）；决定数据根的内容 |
| `cli` | 参数解析、控制台输出、`help` 总览与分组详情、交互循环、CLI 单实例窗口、提权引导、双击时对自身数据根调用幂等体检与补齐 | 直接操作文件系统业务；写业务 JSON、改业务数据内容 |
| `bucket`/`file`/`share`/`trash` | 各自的业务规则 | 越过 `storage` 直接操作文件 |
| `service` | Windows Service 生命周期（SCM 注册、状态机、停止流程、失败编号上报）、服务模式主循环、数据根切换、`service` 五命令（`install`/`uninstall`/`start`/`stop`/`status`） | 业务逻辑 |
| `server` | HTTP 请求解析、参数校验、调用 Service、返回响应 | 直接改 `file.json`、`repository`、`trash` |

### 6.2 头文件引用

公共头文件一律用带 `fmt/` 前缀的形式引用：

```cpp
#include "fmt/core/path_manager.hpp"
```

新增模块只需三步：建 `src/<模块>/` 与 `include/fmt/<模块>/`、在 `src/CMakeLists.txt`
加 `add_subdirectory`、在本模块 `CMakeLists.txt` 用 `target_sources(fmt_core PRIVATE ...)`
登记源文件。顶层构建脚本不需要改动。细节见 `src/README.md`。

## 7. CLI 命令结构

命令集以 `FMT 开发文档.md` 第 95 节为准，实现细节见 `FMT 技术文档.md` 第 11 节：

```text
fmt.exe
├── --help
├── --version | -v
├── version            打印版本与构建日期（提交 d108c80；一次性与窗口里都行，
│                      与横幅、--version 共用 version_text()，不需要服务、不写磁盘）
├── help    [组]       命令总览（不带参数）/ 某一组详情（help service）
├── exit | quit        交互循环内退出
├── bucket  create <name> | list | get <name> | use <name> | delete <name>
├── file    upload <来源> [文件名] | list | get <file_id|文件名> | delete <file_id|文件名>
│           （**四条都已落地**：提交 188e85d；`delete` 的参数在提交 0ad9efc 从
│             只收 `file_id` 扩成两种，与 `get` 同一套定位规则，见第 5.5 节与
│             `FMT 技术文档.md` 第 10.2.4、18.21 节。`upload` 的来源可以是
│             http:// 或 https:// URL，或本机路径；第二个位置参数是**可选**的文件名，
│             省略时从来源推断）
├── share   create <file_id> | get <share_id> | list <file_id> | delete <share_id>
│           （**数据面已落地**：提交 `d5779db`；HTTP 下载端点未做，见第 5.4 节）
├── config  list | set max_upload_size <大小>
│           （**提交 `674d0b0` 落地**：只让改 max_upload_size，其余只读）
├── trash   list | get <标识> | restore <标识> | delete <标识> | empty
│           （**两级都已落地**：桶级 list / restore 提交 c2d545d，get / delete 提交 4fee290；
│             文件级读取侧提交 0fc242b；`empty` 一次清空两级、提交 `674d0b0`；
│             标识可以是 file_id / 回收站目录名 / 原名，
│             list 标出 [文件] / [桶]；delete / restore / empty 有预检与确认，第 5.5 节）
└── service install | uninstall | start | stop | reinstall | status
            （**六条子命令**：提交 c573f14 起 reinstall 从引导内部用法变成正式命令；
              除 status 外每条都提权，第 4.5 节）
```

> **命令集口径更正（阶段 4 与提交 `0fc242b`）**：冻结命令集里 `trash` 组原本整组排在阶段 7，
> 桶级先完成（`4fee290`），**文件级在提交 `0fc242b` 落地**——两级现在由
> `TrashService` 合成一份视图，`list` 标出 `[文件]` / `[桶]`。
> 阶段 7 保留的只剩「永久删除时的 `share.json` 清理」（share 模块属阶段 6）
> 与「文件级」的收尾实测；原口径「文件级 trash list/get/restore/delete、文件级永久删除
> 属阶段 7」**已作废**。`bucket.delete` / `trash.delete` / `file.delete` 都进了
> 「先检查 → 说清楚 → 再确认」那条流程（提交 `711da4c`，第 5.5 节）。

帮助有两个入口，内容一致：`--help` 一次性打印带横幅与退出码表的完整用法（内含命令总览）；
`help` 不带参数只列命令、`help <组>` 打印该组详情（`service` / `bucket` / `file` /
`share` / `trash` / `help` / `exit`），交互式与一次性都支持。`help <未知组>` → stderr
一行 + 退出码 2（`FMT-001`）。**帮助文案已随阶段 4 同步（提交 `8a5e554`），并随桶级回收站
再同步两次（提交 `c2d545d` 与收尾 `4fee290`），`188e85d` 又一次**：`bucket` 五条命令已经可用，因此 `(bucket)`
列在「可用命令」组；
**`(trash) list get restore delete` 四条两级命令都可用，同样列在「可用命令」组**；
**`(file) upload list get delete` 四条也已在提交 `188e85d` 落地，同样在「可用命令」组**；
「服务端尚未实现，现在返回 FMT-602」这句现在**只对 `share` 一组成立**（原口径
「与 `file` / `share` 两组」、以及更早的「与 `trash get` / `trash delete`」**均已作废**）。
`help file` 的正文照源码写进 `FMT 开发文档.md` 第 68 节与 `FMT 技术文档.md` 第 11.4 节
（标题是「文件（当前用户在当前 Bucket 里的文件）」；`get` / `delete` 都写 `<file_id|文件名>`，
`delete` 那两行在提交 `0ad9efc` 改过；`upload` 那行现在写明来源可以是 `http://` 或
`https://` 的 URL，文件名省略时取来源的最后一段，
重名不改名、相同内容（MD5 相同）会被拒绝）。

> **⚠ 那句 `upload` 文案曾经过时（提交 `a2b6cd1`），现在源码已经改掉**：原来源码里
> 写着「v1 不支持 https（需要 OpenSSL）；同时只支持 http」，与行为相反（https 现在是
> 合法来源，见第 14 节与 `FMT 开发文档.md` 第 33 节）。**后续提交已经把这句改写为**
> 「来源可以是 http:// 或 https:// 的 URL，也可以是本机路径。」+「网络下载走系统组件
> （WinHTTP + Schannel），支持 https，不需要 OpenSSL。」，所以这条残留**已结清**；
> `help file` 的正文与 `FMT 技术文档.md` 第 11.4 节、`FMT 开发文档.md` 第 68 节同注。
>
> **`file delete` 那两行也在提交 `0ad9efc` 改过**：原「`delete <file_id>` 软删除进回收站，
> `file_id` 不变（可用 trash 查回）」→「`delete <file_id|文件名>` 软删除进回收站，
> `file_id` 不变 / （文件级回收站目前只能写、还不能从 trash 查回，待阶段 7）」。
> **括号里那句「只能写、还不能从 trash 查回」已在提交 `0fc242b` 被推翻**——文件级条目
> 现在能列出、回退、永久删除，`help trash` 的正文也同步改过（`FMT 技术文档.md` 第 11.4 节）；
> 「可用 trash 查回」原本跑在实现前面，后来一度删掉，现在成了事实（第 5.4、5.5 节）。
>
> **按名字/标识定位一律不区分大小写（提交 `5bf2c1f`）**（文件名、`file_id`、桶名、回收站条目名）。
> `file get report.txt` 找得到 `Report.txt`，`file delete REPORT.TXT` 可用；桶的
> `is_current` / `was_current` 与回收站定位同样按不区分大小写匹配。理由是 Windows 的文件系统与
> 路径本身就不区分大小写（第 5.2、5.5 节与 `FMT 技术文档.md` 第 18.22 节）。
> **提交 `9c3d2cb` 之后这条有两处更新**：① 桶名不再各存一份拼写——`bucket use WORK` 存进
> `current_bucket` 的是**磁盘上的实际名字** `work`（`canonical_name()`），`bucket get` 显示的
> 也是它，`bucket create WORK` 直接建成 `work` 并回 `note` 提示；② **帮助文案现在提这一条了**
> （`help file` 末行「名字与 file_id 的比较都不区分大小写（Windows 习惯）」、`help bucket`
> 新增三行小写与按实际名字处理，提交 `9c3d2cb`）。
`help bucket` 写明五条子命令的真实行为（第一个 Bucket 自动成为当前 / `use` 只改
`current_bucket` / `delete` 移入回收站且当前置空不自动切换），`help trash` 写明桶级
`list` / `get` / `restore` / `delete` 的真实行为（`get` 报原桶名 / 删除时间 / 目录 / 文件数 /
占用；`restore` 时名称可用回收站里的名字或唯一的原桶名，原位置已有同名
Bucket 就整单拒绝，不覆盖、不改名、不做部分恢复；`delete` 是永久删除，交互窗口问一次、
一次性命令要 `--yes`）。帮助内容必须与实际命令一致
（见第 4.5 节与本表）；输出样例见 `FMT 开发文档.md` 第 95、127.6 节与
`FMT 技术文档.md` 第 11.14 节。

`help` 的命令总览（**只列命令、不加描述**，与 `src/cli/cli.cpp` 的输出一致）：

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
  (server)   （`server.*` 是唯一还剩的空壳）
```

> 这是提交 `188e85d` 之后的逐字输出（`src/cli/cli.cpp` 的 `print_command_list()`），
> 并已同步到 `d5779db`：**`d108c80` 起 `(version)` 一行在列**；**`674d0b0` 起 `trash`
> 多了 `empty`、新增 `(config) list set`**；**`d5779db` 起 `(share)` 从「尚未实现」组
> 移进「可用命令」组**。上面标题里的「现在会返回 FMT-602」是**字面输出**，
> 现在它只对应 `server.*` 这个空壳与**未实现的 HTTP 路由**（兜底路由的 501）——
> 原口径「`share.*` 被前缀表认得但没实现，拿到 FMT-602」**已作废**。

> **`version` 命令（提交 `d108c80`）**：窗口里原来敲 `version` 得到「未知命令」
> （只有 `--version` 旗标实现了）。现在三处——横幅、`--version` / `-v`、`version` 命令
> ——**共用同一份文本** `cli::version_text()`，输出一行
> `File Manager Tool  v1.0  ( build  2026.10.09 )`；**不需要服务在运行**，
> 也**不写任何磁盘内容**（一次性分支在开日志器之前）。
> 上面那张「本地处理」表已把它与 `--help` / `--version` 并列。

三类命令的走向完全不同，实现时不能混：

| 命令 | 走向 | 说明 |
|---|---|---|
| `bucket` / `file` / `share` / `trash` | **命名管道 → 服务** | CLI 不碰文件系统；服务未运行则 `FMT-601`、退出码 8。**`bucket` 已在阶段 4 落地**（`bucket.*` 五种 op + 两条入口），**桶级 `trash.list` / `trash.get` / `trash.restore` / `trash.delete` 同阶段落地**（`get` / `delete` 提交 `4fee290`；`file.json` 的 `trash_reason`、`trash/<user>/.original` 见第 5.4.1 节）；**`file` 四条也在提交 `188e85d` 落地**（`file.upload` 走两段式；`file.json` 的读写、`trash.json` 的文件级条目写入都在 `src/file/`）；**只剩 `share` 仍返回 `FMT-602`**（原口径「`file` / `share` 两组」「与 `trash.get` / `trash.delete`」均已作废） |
| `service install` / `uninstall` / `start` / `stop` / **`reinstall`** | **直连 SCM + UAC 提权** | 不经管道、不经 HTTP，见第 4.5 节；**提交 `c573f14` 起 `reinstall` 也是给用户敲的正式命令**（原来只在引导流程内部用） |
| `service status` | **直连 SCM，不提权** | 只读查询：不需要管理员权限、不弹 UAC、不走提权副本；未安装时 `FMT-601`、退出码 8 |
| `--help` / `--version` / `-v` / `version` / `help` / `exit` | 本地处理 | 不连服务、不弹 UAC、不写日志（`version` 命令提交 `d108c80` 新增） |

`service` 命令的硬性约束（第 2 节第 23 项、第 4.5 节）：

- **六条子命令，不带 `--` 前缀**：写 `fmt.exe service install`。旧写法 `fmt.exe --service install`
  已作废，`fmt.exe --service --help` 同样作废，改用 `fmt.exe service --help`。
- **没有 `pause`**（服务不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`），**没有 `delete`**
  （旧文档的 `delete` 已更名为 `uninstall`）。
- **四条动作命令**（`install` / `uninstall` / `start` / `stop`）都走 UAC 提权，提权与否不改变命令名；
  用户取消 UAC → `FMT-004` / 退出码 5。
- **`service status` 是查询，不提权**：不弹 UAC、不要管理员权限，直接读 SCM 状态并打印
  （第 4.5 节）。它出现在帮助文本与 `fmt.exe service --help` 里。

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

### 7.1 错误与返回值（已冻结）

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

### 7.2 日志（已冻结）

日志目录为**当前数据根**下的 `log/`（第 3.2 节），两个文件：

```text
log/
├── fmt.log        全部日志
└── error.log      仅 ERROR 级（快速排查；超过 5 MB 轮转成 error.log.1，只留一代）
```

与它平行的还有**临时文件目录 `temp/`**（第 3.2 节）：两者都不是业务数据，但性质不同——
`log/` 要长期保留、用于回答「谁在什么时候做了什么」，`temp/` 按定义**随时可以清空**。

每行格式：

```text
时间 [级别] [模块] 消息
2026-10-05 23:34:21 [INFO] [File] 文件上传开始: example.txt
2026-10-05 23:36:12 [ERROR] [File] 文件删除失败: example.txt
```

| 级别 | 写入日志文件 | 输出到控制台 |
|---|---|---|
| INFO | ✅ | 视 CLI 场景 |
| WARN | ✅ | ✅ |
| ERROR | ✅ | ✅ |

V1 **不设 DEBUG 级别**，也不实现异步日志、日志线程、日志队列或压缩。
**轮转已经做了**（提交 `821aba3`，第 7.4 节）——原口径「也不实现……轮转」**已作废**。

**Service 与 CLI 都写日志文件，两个进程追加同一个文件**：两个进程都以「追加」方式打开
同一份 `<数据根>/log/fmt.log`（`error.log` **仅 ERROR 级**，同样追加），
**每行开-写-关**（这也是轮转能做成的前提，第 7.4 节）；
MSVC 文件流是共享模式，因此不存在两个进程争抢同一日志文件的问题。

旧口径是「只有 Service 写日志文件，CLI 只输出控制台」，已被推翻：用户在 CLI 里敲
`service stop`，服务随即被停掉，旧口径下这次操作在日志里**一个字都没有**——日志跟不上
用户做过什么。日志要能回答「谁在什么时候对服务做了什么、结果如何」，因此两个进程都写。

CLI 侧的三条边界（详见 `FMT 技术文档.md` 第 11.12 节）：

```text
1. CLI 双击启动时，先对自己所在数据根执行一次与服务共用的幂等体检与补齐
   （六个目录 + 六个默认 JSON，只补缺失、已存在不动、绝不重置损坏 JSON，见第 3.2 节）；
   这一步在打开日志器之前完成，因此 log/ 也在同一条「新建目录」清单里；
   它的结果攒成若干行、等日志器开好后写进 log/fmt.log（模块 Cli），**控制台一行都不打**，
   只有异常（无法补齐、文件损坏）走 stderr。见第 3.2、7.5 节。
   除此之外 CLI 只允许创建 <数据根>/log/ 与执行提权命令前要用的 <数据根>/temp/ 这两个目录
   （都不属于业务数据：日志不是业务数据，temp/ 按定义随时可以清空）；
   CLI 仍然不改任何业务数据——不写 data/*.json 的内容、不删文件、不改名。
2. `--help` / `--version` / `-v` / `version` / `help` / `exit` 不写日志、不创建任何目录。
3. 两个进程的数据根可能不同（服务可能被别人启动在另一个目录）：各写各自数据根下的
   log/fmt.log；这种情况下 CLI 会额外写一行 WARN，指明服务当前数据根与服务侧日志的位置。
4. 控制台只留横幅、提示符、命令结果与异常；服务的当前状态、数据根体检结果只进日志；**换根通知是例外**（提交 `8f2fbc5`：切换时 stderr 报一次 + 横幅常驻「数据根：…」）
   （「控制台负责用户交互与重要异常，日志负责完整运行记录」）。
```

CLI 侧的模块短名为 `Cli`（CLI 本体）与 `Elevated`（提权副本）。

原则：**控制台负责用户交互与重要异常，日志文件负责完整运行记录。**

### 7.3 路径管理（已冻结）

第二阶段引入 `PathManager`，持有 `FMT_ROOT`，提供 `repository()`、`trash()`、`config()`、
`data()`、`log()`、`temp()` 以及 `file_data()`、`share_data()`、`trash_data()` 等具体路径。

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

数据根由 CLI 在 hello 帧里声明（第 3.1 节），因此 `PathManager` 必须支持**切换数据根**：
切换只替换持有的根路径，旧根的数据原样留在磁盘上，新根重新做一次幂等初始化（第 3.2 节）。

### 7.4 日志轮转

**已定并落地（提交 `821aba3`）**：`fmt.log` 超过 **5 MB** 轮转成 `fmt.log.1`
（**只留一代**），`error.log` 同理；**0 表示不轮转**（`Logger::Options::max_log_bytes`，
默认 5 MB）。每写 **64 行**检查一次大小；改名失败（另一个进程正好在写）不报错、
下一次再试；轮转后在新文件里写一行说明「日志超过 N 字节，已轮转：fmt.log -> fmt.log.1」。

> **前置条件是「每行开-写-关」**（`append_line()`）：CLI 与服务共用同一个日志文件，
> 长期持有的 `ofstream` 既会挡住改名，也会让另一个进程继续往**已改名**的文件里写——
> 没有这一步，轮转做不成。**原口径「单文件大小上限与轮转规则尚未确定」已作废**
> （`FMT 技术文档.md` 第 14.2、14.6、18.36 节）。

### 7.5 CLI 界面与输出（已冻结）

启动横幅要告诉用户「服务在不在」：

```text
File Manager Tool  v1.0  ( build  2026.10.09 )
Service Running...
数据根：D:/Data/Temp/JMT/fmt          ← 提交 8f2fbc5 起常驻（service::load_state()）

fmt>
```

```text
服务已在运行  → 第二行 Service Running...
服务未运行    → 第二行 Service Stopped...
数据根行      → 第三行「数据根：<服务记录的数据根>」（提交 8f2fbc5，永远显示；
                服务记的根为空时不打）；它与本程序所在目录不一致时再补一行说明
横幅名称版本  → File Manager Tool  v1.0  ( build  <配置时的日期> )，与 --version、version 同一串
提示符        → fmt> （fmt 后紧跟 > 和一个空格）
提示符前的空行 → 打印 fmt> 之前先输出一个空行：横幅之后一次、每条命令执行完之后一次，
                 这样输出不会和提示符挤在一起；用户只敲回车（空命令）时不再重复空行
输入          → 空行忽略；exit / quit 退出；help 看命令；命令失败后继续循环，不退出
输出去向      → 正常结果 → stdout；错误 → stderr（不与结果混流）
```

**控制台只留交互，体检与状态的常规结果全部进日志。** 双击时控制台就这几行：

```text
需要管理员权限        ← 仅当需要装/启服务时才有这两行
正在提权...
执行成功...
错误码：0

File Manager Tool  v1.0  ( build  2026.10.09 )
Service Running...
数据根：D:/Data/Temp/JMT/fmt

fmt> 
```

数据根体检的「建了什么 / 完不完整」、以及服务的当前状态，**都不打印**，只写进
`log/fmt.log`（模块 `Cli` / `Service`，第 7.2 节）：

```text
[Cli] 数据根检查：D:/FMT2
[Cli] 数据根新建目录：repository, trash, config, data, log, temp
[Cli] 数据根新建文件：D:/FMT2/config/config.json, ...
[Cli] 数据根完整                      ← 第二次双击
[Service] 当前状态：运行中
```

**只有异常才走 stderr**：`数据根无法补齐：FMT-013 …`、
`数据根文件损坏（未自动修复）：data/file.json`。

**换根通知是个例外：现在也上控制台（提交 `8f2fbc5`，原口径「只进日志、不刷控制台」已作废）**
——切换的那一刻在 **stderr** 打印 `cli::root_switch_notice(previous, current)`：

```text
注意：服务的数据根已切换
  原来：D:/Data/Temp/JMT/fmt
  现在：D:/Data/CLionProjects/FMT/cmake-build-debug/bin
原因是「数据根由 CLI 声明」：谁连上服务，服务就用谁的目录。
如果这不是你想要的，请用数据根正确的那个 fmt.exe 再执行一条命令切回去。
```

同时照旧写日志（完整运行记录不变；也可以问 `service status`，它会打印「服务数据根」）：

```text
[Service] 数据根切换：D:/FMT -> D:/FMT2        ← 仅当 hello 回执 switched=true
[Service] 服务数据根已经是：D:/FMT2            ← 未切换时
```

**交互窗口横幅区还常驻一行「数据根：…」**（提交 `8f2fbc5`），与本程序所在目录不一致时
再补一行说明；**触发条件**与服务只接受一条连接、所以「中途被搬走」不可能发生这条论据，
见 `FMT 技术文档.md` 第 11.9、13.10、15.1、18.33 节。

这样归位正是本工程的老原则：**控制台负责用户交互与重要异常，日志文件负责完整运行记录**
（第 7.2 节）——换根通知属于「重要异常」那一类：数据、桶、回收站会整体换成另一个目录的
内容，用户必须立刻知道。双击时数据根体检的「建了什么」仍然只进日志，不该把它淹掉。

`service status` 的输出也走 stdout（它是查询命令，成功与「未安装」两种结果都是它的正常输出）：

```text
fmt> service status
服务状态：运行中
服务宿主：D:/FMT2/fmt.exe
服务数据根：D:/FMT2
错误码：0

fmt>
```

输出格式固定（`FMT-NNN` 与退出码分属两层，见第 7 节退出码表与附录 A）：

```text
成功：                          失败：
执行成功...                     执行失败：FMT-601 服务未安装
错误码：0                       错误码：8
```

- 错误行的 `FMT-NNN` 之后跟**错误消息**；同一错误码的文案可以改（第 7.1 节）。
- **控制台编码已冻结为 UTF-8**：`wmain` 最开始调用 `SetConsoleOutputCP(CP_UTF8)`，同时置
  `SetConsoleCP(CP_UTF8)`，保证中文不乱码。旧的「控制台编码待定」描述作废。
- **V1 不做结构化输出（`--json` 等），也不预留参数名**（第 14 节）：机器可读通道是
  **命令退出码**（`0` 成功；未安装 `FMT-601` → `8`；查询失败 → `8`），人类可读通道就是上面
  那几行固定顺序的文本。脚本判断「服务在不在」读退出码即可；需要 `wait_hint_ms` 这类
  结构化字段时再加。

## 8. 并发与事务

V1 即使主要单机使用也必须考虑并发，重点保护：`file_id` 分配、JSON 写入、File metadata、
Share `download_count`、`current_bucket`、文件移动。

**重构后并发模型变简单了**：Service 是唯一写 `data/*.json` 的进程，CLI 只发命令、不改业务数据内容
（它只在自己双击时对自身数据根做一次幂等补齐，见第 3.2 节），所以不存在「CLI 与服务同时改同一个
JSON」的跨进程竞争。但服务**进程内部**依然有并发，两条入口都会进 Service 层，所以下面的锁策略
一条都不能省。

**阶段 4 实现的准确形态**（旧文写的「命名管道连接线程 + 每连接一个线程」不成立——
实现里没有为连接起线程）：

```text
① 命名管道：连接级严格串行
   服务端只有一条 accept 循环（ServerRuntime::run()）：accept 一次只建立一条连接，
   然后在这条连接上「读一个请求 → 处理 → 写一个响应」，客户端断开才回到 accept。
   所以同一时刻只有一条 CLI 连接（配套「只留一个 CLI 窗口」，第 4.7 节）；
   第二个客户端拿到 ERROR_PIPE_BUSY，客户端等 3 秒重试，最终归为 FMT-601。

② HTTP：线程池并发
   cpp-httplib 默认 max(8, hardware_concurrency - 1) 个线程，请求并发进入；
   /api/ping、/api/status 不碰业务锁，是真并发。

③ 业务命令：共用运行体的同一把互斥锁
   管道请求与 HTTP 业务处理器都在业务调用之前 lock 同一把 mutex，
   所以两条入口的 bucket 命令**互斥串行**——同一时刻只有服务在写数据根。
   这是**互斥（mutex），不是队列（queue）**：不保证先来先服务、没有优先级、
   没有排队长度上限、没有排队超时。

④ 代价（阶段 5 必须处理）—— **已在提交 `188e85d` 落地解决**
   一个慢业务命令会卡住两条入口的所有业务命令。
   上传/下载可能持续几十秒到几分钟，阶段 5 必须二选一——**收细锁粒度**
   （按 JSON 文件 / 按 file_id 分锁）或**把长任务移出锁**（登记任务 + 后台线程 + 轮询状态）。
   在选完之前，「上传期间其他命令一起等」是既定限制，不是 bug。
   —— 这四行描述的是**决策前**的状态；决策与落地见下面一段（提交 188e85d）
```

**选的是「把长任务移出锁」的简化形态：「长任务不持锁」（提交 `188e85d`）。**
不加任务登记、不加进度查询 op，只把上传拆成两段：

```text
ServerRuntime::run_upload(args)      ← 管道与 HTTP 共用这一份
  ① 锁下取快照    paths / logger / size_limit，随即放锁
  ② 锁外 prepare  prepare_upload()：下载或复制到 temp/、边写边算 MD5、边判大小上限
  ③ 锁内 commit   commit_upload()：去重 → 重名 → file_id → 搬到仓库 → 写 file.json
```

第 ② 段几十秒到几分钟、**全程不持业务锁**，所以「上传时 `bucket list` 卡住」不再是事实；
第 ③ 段是毫秒级的一次锁。`file.upload` 是**唯一**走这条路径的 op（`commands.cpp` 的
`file_command()` 里没有它的分支），其余业务命令仍是「取锁 → `execute_business()`」。
**换根不会插进第 ② 段**：换根只由 `hello` 触发，而管道 accept/serve 是串行的（①），
上传期间不会再处理第二个请求，所以第 ① 段的快照在整段下载期间都成立。
**没有新增锁**：仍然只有 `ServerRuntime::mutex_` 一把，按 JSON 分锁是**目标形态**，
等真的需要「上传与下载并发」时再细分（`FMT 技术文档.md` 第 15.1 ④、15.3、18.16、18.19 节）。

- 用 Mutex/Lock 保护同一 JSON 数据的「读取—修改—写入」全过程，避免互相覆盖。
- 保护同一文件、同一 `file_id`、同一 Share 的操作，避免「删除+下载」「恢复+永久删除」
  「两个上传生成同一 ID」「两个下载突破 Share 次数」。
- 正式文件必须经「临时文件 → 完整完成 → 安全移动」才成为正式文件，避免下载读到半成品。
  **提交 `188e85d` 已落地**：下载/复制阶段只写 `<数据根>/temp/fmt-upload-<随机>-<序号>.tmp`
  （`repository/` 一个字节都不动），完整性（大小上限边写边判、`Content-Length` 必须对上、
  MD5 边写边算）通过后才 `rename` 进 `repository/<user>/<bucket>/YYYY/MM/DD/`；
  失败路径都删临时文件，`file.json` 写不进去就删掉刚提交的仓库文件。
- 优先同文件系统内移动；跨盘时用「复制 → 校验 → 删除原文件」并做一致性保护。
- 数据一致性检查覆盖四种异常：metadata 有而文件无、文件有而 metadata 无、size 不符、
  md5 不符。发现异常只记录并停止危险操作，**不自动删除或重建**。
- **数据根切换**（hello 帧触发并回填 `switched` / `previous_root`，第 3.1 节）必须与在途业务操作
  互斥，否则会出现一半命令落在旧根、一半落在新根的撕裂状态。切换只改根路径，不动旧根数据。

## 9. 编译与构建

环境：Windows x64、MSVC（Visual Studio Build Tools）、CMake ≥ 3.20、Ninja、C++17。

构建目录只有 **`cmake-build-debug`** 一个，与 CLion 默认配置一致。

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

产物：`cmake-build-debug/bin/fmt.exe`（**唯一产物**，三种形态是同一个 target 的运行期分支，
不拆成 `fmt_cli` / `fmt_svc` 两个 exe）。

Ninja 需要 MSVC 环境变量，请在 *Visual Studio 开发者命令行* 中执行。

`fmt.exe` 的第三方依赖只有 vendor 进 `third_party/` 的 `nlohmann/json` 与 `cpp-httplib`
（**头文件入库，不用包管理器、不用 FetchContent**），CRT 用 `/MT` 静态链接
（`CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded`），因此可完全离线构建、不产生额外 DLL。
**URL 下载另链系统库 `winhttp`**（提交 `a2b6cd1`）：它属于 Windows 本身而非第三方依赖，
同样不产生额外 DLL——这也是换掉 `cpp-httplib` 的 `Client` 的直接原因（第 2 节）。
单元测试用**仓库内自带的极简运行器**（`tests/fmt_test.cpp`），不引入 Catch2、不需要联网。
运行器在**每条用例开始前**打印 `[开始] <套件>.<名称>`，并把 `std::cout` 设成 `unitbuf`
逐行刷新——某条卡住时，屏幕上的最后一行就是它（提交 `a2b6cd1`；
`FMT 技术文档.md` 第 17.2 节）。需要跳过测试时：

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

CI 见 `.github/workflows/ci.yml`，**只在 `windows-latest` 上**构建并测试：Windows Service
（`advapi32`）、`ShellExecuteEx` 提权与命名管道安全描述符都无法在 Linux 上构建。
分支：`main` 为稳定分支只接受 PR，`dev` 为集成分支，本次重构在 `arch-restart` 上进行。

## 10. 当前实现状态

**本分支（`arch-restart`）回到架构初始骨架**：只保留一个可编译、可运行的最小 `fmt.exe`，
阶段 2 / 阶段 3 按本文档的新架构**重新开始**。旧分支（`dev`）上按「CLI 直接操作本地目录 +
`--service` 参数 + CLI 走 HTTP」写出的实现**不迁移、不复用**。

**阶段状态（截至 commit `0fc242b`「feat(trash): make the file level readable, and warn
before a bucket goes」；
阶段 4 主体是 32249ea，回收站形状 c2d545d，桶级 trash 收尾 `4fee290`，
阶段 5 主体 `188e85d`、下载改 WinHTTP `a2b6cd1`、`file delete` 收文件名 `0ad9efc`、
大小写口径 `5bf2c1f`、保留形状与回收站路径 `9c3d2cb`、预检与确认 `711da4c`（缺口收尾 `6a40742`）、
回收站读侧 `0fc242b`）**：

| 阶段 | 状态 |
|---|---|
| 1 骨架（CMake / Ninja / MSVC / `fmt.exe` / `--help`） | ✅ 完成并验收 |
| 2 基础设施（`common` / `config` / `storage` / `core` 初始化） | ✅ 完成 |
| 3 服务与通道（`service` / `ipc` / `cli`） | ✅ 完成 |
| 4 Bucket（create / list / get / use / delete + `current_bucket` + 名称校验 + 两条入口 + **桶级回收站四条命令** + 删桶前预检） | ✅ 完成（明细见 `FMT 技术文档.md` 第 18.15、18.17、18.25 节） |
| 5 File / Upload / Trash（**两级**条目）/ Share | 🟡 **`file` / `trash` / `share` 数据面 / `config list·set` 都完成；只剩 share 的 HTTP 下载端点**：`file upload/list/get/delete`（+ **提交 `674d0b0` 的 `--sort name|size|id`**）；**文件级与桶级回收站合成一份视图**（`src/trash/` 的 `TrashService`，提交 `0fc242b`），`trash list/get/restore/delete` 两级都能用、`list` 标出 `[文件]` / `[桶]`，**`trash empty` 一次清空两级**（`674d0b0`）；**破坏性操作走「先检查 → 说清楚 → 再确认」**（提交 `711da4c`：只读预检 + `FMT-016` + y/N 或 `--yes`）；**`share` 数据面**（`d5779db`：`create/get/list/delete` + 20 次 / 7 天 + `share.download` 记账）；**`config list` / `config set max_upload_size`**（`674d0b0`）。明细见 `FMT 技术文档.md` 第 18.19～18.38 节。**剩下的**：share 的 HTTP 下载端点（**等用户决定开放哪几个接口**）与永久删除时的 `share.json` 清理 |
| 6 HTTP Server + Preview | 🟡 **HTTP 接口第 1、2 步已完成**（提交 `bfd89f7` / `4b812b5` / `22c3c3e`）：**监听 `localhost:4122`、`enabled: true`**、**全部 `/api/*` 要 token**（401 + `FMT-018`，`/api/ping` 与分享下载除外）、`/api/file` 四条、`/api/trash` 四条、**`/api/share` 四条**；**`/api/bucket*` 已删除**（404 + `FMT-017`）。**还没做**：**分享的流式下载端点**（第 3 步，现在 501 + `FMT-602`）、下载/预览路由与浏览器页面 |
| 7 收尾（永久删除时的 share 清理 + 真机实测） | ⏳ 未开始（**当前只剩一个缺口**：删桶 / 永久删除时**没有**联动清理 `share.json` 记录。原缺口「文件级 trash 只写不读」**已在 `0fc242b` 关闭**；**share 数据面已在 `d5779db` 落地**，只剩它的 HTTP 下载端点等用户定接口） |

当前代码里**已经存在**的部分（阶段 2～5）：

| 已实现 | 位置 |
|---|---|
| 程序入口 `wmain`、控制台 UTF-8、`--help` / `--version` / 未知参数退出码 2 | `src/main.cpp` |
| 统一错误码基础设施：`ErrorCode`、`code_string` / `code_from_string`、`exit_code`、`make_error` | `include/fmt/common/error.hpp`、`src/common/error.cpp` |
| `common`：`string`（UTF-8 转换、`url_encode` / `url_decode`）、`time`、`logger`、`validation` | `include/fmt/common/`、`src/common/` |
| `config` / `storage` / `core`：数据根解析、幂等初始化、`PathManager`、JSON 原子读写 | `src/config/`、`src/storage/`、`src/core/` |
| `ipc`：命名管道帧、安全描述符 + MIC、`hello` 的 `switched` / `previous_root`、**`pipe_name()`（`FMT_PIPE` 覆盖，提交 `a340d1e`）** | `src/ipc/` |
| `service`：SCM **六条子命令**（`reinstall` 提交 `c573f14` 起正式化）、`ServiceMain`、统一查询接口、按 `dwWaitHint` 落定、`Runtime`（锁 + 业务分发 + **启动/换根时校验 `current_bucket`**，失效置空） | `src/service/` |
| `cli`：交互循环、横幅、`help`、单实例、UAC 提权引导、双击体检与补齐 | `src/cli/` |
| `server`：cpp-httplib 监听 **`localhost:4122`**、`/api/ping` / `/api/status`、`/api/file` 四条、`/api/trash` 四条、`/api/share` 四条（提交 `4b812b5`）、**token 认证（pre-routing 钩子，401 + `FMT-018`）**、`/api/bucket*` **已删除**（404 + `FMT-017`）、路由、`/api/file` 四条路由、`delete_args()`（`?dry_run=1` / `?force=1` / 请求体 `{"force":true}`，`/api/trash` 与 `/api/file` 的 `DELETE` 共用）、错误码 → 状态码映射 | `src/server/` |
| `bucket`：`BucketService`（无独立 ID；桶级 `list_trashed` / `restore` / `get_trashed` / `purge` / **`check_remove`**） + 业务分发 `bucket.*` | `src/bucket/`、`src/service/commands.cpp` |
| `file`（**提交 `188e85d`**）：`FileService`（`commit_upload` / `list` / `get_by_id` / `get_by_name` / `remove` / `resolve_path` / `trash_path_of`）+ 自由函数 `prepare_upload()` / `file_name_from_source()` / `extension_of()`；业务分发 `file.list` / `file.get` / `file.delete`。**提交 `0ad9efc`**：签名改为 `remove(std::string_view file_id_or_name)`，私有新增 `locate_record(records, key)`。**提交 `9c3d2cb`**：`locate_record()` 命中不同记录时报 `FMT-001` 歧义；`file.get` 增加 `trash_path`。**提交 `711da4c`**：新增 `check_remove()`（`FileDeleteCheck`）与 `bucket` 消息归属。**提交 `0fc242b`**：新增 `list_trashed()` / `check_restore()` / `restore()` / `purge()` 与 `deleted_at` 字段，**不再写 `trash.json`** | `src/file/`、`src/service/commands.cpp`、`src/service/runtime.cpp` |
| **`trash`（提交 `0fc242b`）**：`TrashService`（`include/fmt/trash/trash.hpp` / `src/trash/trash.cpp`）**组合** `FileService` 与 `BucketService`，合并两级视图 + 跨命名空间标识解析 + 两级预检（`TrashCheck`）；**提交 `674d0b0`** 再加 `trash.empty`（`empty()`：每删一项重新 list、先文件级后桶级、单条失败跳过 + `kMaxRounds`） | `src/trash/`、`src/service/commands.cpp` |
| **`share`（提交 `d5779db`，数据面）**：`ShareService`（`include/fmt/share/share.hpp` / `src/share/share.cpp`）——`create` / `get` / `list` / `remove` + **`register_download()`**（业务锁内一次完成检查+计数+落盘）；`share_id` 由 `BCryptGenRandom` 生成 12 位十六进制；顺带给 `server` 那条服务行加上 `config.list` / `config.set` 的分发 | `src/share/`、`src/service/commands.cpp` |
| `common/hash`（**提交 `188e85d`**）：`Md5`（Windows CNG / bcrypt 增量接口）+ `md5_hex()`；`bcrypt.lib` 在 `src/common/CMakeLists.txt` 链接 | `include/fmt/common/hash.hpp`、`src/common/hash.cpp` |
| `core`：`PathManager`（`trash_file` 落点 `trash/<user>/.files/<bucket>/YYYY/MM/DD/`，提交 `4fee290`） | `src/core/` |
| `common/http_client`（**提交 `a2b6cd1`**）：`is_remote_url()` + `http_download()`——WinHTTP + Schannel 的流式 GET（跟随重定向、连接/发送/接收超时、`Accept-Encoding: identity`、只有 2xx 交给 sink、sink 返回 false 即中止、证书失败给准提示）；`src/common/CMakeLists.txt` 链 `winhttp`，`src/file/CMakeLists.txt` 不再链 cpp-httplib | `include/fmt/common/http_client.hpp`、`src/common/http_client.cpp` |
| 单元测试（错误码、校验、配置、路径、存储、管道、服务、CLI、Bucket、File、**Trash**、Hash、HTTP 下载…；**156 个**：提交 `9c3d2cb` 由 129 增到 134（`Validation.与file_id同形的文件名被拒` / `File.与file_id同形的名字不能上传` / `File.标识与名字同时命中时报歧义` / `Bucket.创建时大写会转成小写` / `Bucket.use大写规范化到磁盘上的名字`），提交 `0fc242b` 再由 136 增到 140（`Trash.文件级条目能列出并回退` / `Trash.回退遇同名冲突要拦住` / `Trash.随桶删除的文件不能单独回退` / `Trash.永久删除文件级条目`），提交 `a9af276` 再由 140 增到 142（`String.清理粘贴带进来的路径污染` / `File.粘贴路径里的不可见字符会被清掉`），提交 `2c841c8` 增到 143（`Cli.位置参数的信封形状`），提交 `5b316b3` 增到 144（`Storage.两个写者同时写同一个文件不会互相踩`，并把 `Service.启动时清理temp里的遗留临时文件` 改成「新的留着、旧的清掉」），提交 `d108c80` 增到 145（`Cli.版本文本只有一个来源`），提交 `bb7a40f` 增到 146（`Cli.数据根切换提示要把两个根都说清楚`——**控制台输出第一次有测试覆盖**），提交 `c573f14` 增到 147（`Cli.service子命令集合`——**命令集合第一次有断言**），提交 `a340d1e` 增到 150（`CliE2e.核心链路走真实exe与真实管道` / `CliE2e.交互式确认答n不删答y才删` / `Ipc.管道名可被FMT_PIPE覆盖`——**第一次让真实 exe 走真实管道**），提交 `821aba3` 增到 152（`Logger.超过上限会轮转出一代` / `Logger.上限为零时不轮转`），提交 `674d0b0` 增到 153（`Service.列表排序配置与清空回收站`），提交 `d5779db` 增到 154（`Service.分享的创建查看列出撤销与计数`），提交 `bfd89f7` / `4b812b5` 增到 156（`Server.管理接口要token且桶路由已下线` / `App.初始化数据根会建默认账号与token` / `App.已有空users文件时也要补建默认账号`）；另有多条既有用例追加断言：`File.列表与查询`、`File.软删除进回收站`、`Service.管道能执行Bucket命令`、`Service.破坏性操作先预检再确认`、`Service.管道能上传与操作文件`、`Server.Bucket路由与状态码`、`Server.File路由与上传`） | `tests/` |
| vendored 第三方库 | `third_party/nlohmann/json.hpp`、`third_party/cpp-httplib/httplib.h`（**只服务 HTTP 服务端**；URL 下载走系统 `winhttp`） |
| 版本号的单一来源（CMake 生成头） | `cmake/version.hpp.in` |
| 构建辅助脚本 | `tools/build.ps1` |

**尚不存在**：`server.*`（唯一还剩的业务空壳；HTTP 路由也还没开）。
**原列在这里的 `share` 业务模块已落地**（提交 `d5779db`：`src/share/` 真实存在、
`share create/get/list/delete` 可用，**HTTP 下载端点未做**——用户决定先不加接口）。
**还没做的一件事**：永久删除时的 `share.json` 清理（现在 share 撤销是删记录，
桶删除时**没有**联动清理 share——仍是阶段 6/7 待办）。
**另外还缺**：HTTP 的下载/预览路由与
浏览器页面、`resources/fmt.manifest`。
（`config get / set` 两条命令**已在提交 `674d0b0` 落地**——现在的形态是
`config list` / `config set max_upload_size <大小>`；原口径「还缺 config get / set」**已作废**。）
原列在这里的两条**都已关闭**：`src/trash/` 已建（提交 `0fc242b`）、
**文件级** Trash 的读取侧已落地（`trash list` / `get` / `restore` / `delete` 两级都能用，
`TrashService` 把 `file.json`（文件级）与 `.original`（桶级）合成一份视图）。

> **不要读错**：`file` 四条命令与 `trash` 四条命令**现在都能用**，见
> `FMT 开发文档.md` 第 101～103、127.7 节与 `FMT 技术文档.md` 第 18.19、18.25 节；
> 原口径「回收站里能看到并恢复这个文件才是尚未实现的那一步」**已作废**——
> 那一步就是提交 `0fc242b` 做的（`Trash.文件级条目能列出并回退` 钉着它）。

> 旧的 `FMT_ROOT` 实现（`src/core/path.cpp`、`src/core/app.cpp`、`src/cli/cli.cpp`、
> `include/fmt/core.hpp`、`src/app/`、`tests/core_test.cpp`）已从工作区移除：它们实现的是
> 「`FMT_ROOT` = exe 所在目录 + CLI 自己建四个目录」的旧口径，与第 3 节「数据根由 CLI 声明、
> 初始化走与 Service 共用的同一套幂等规则」冲突，不值得改造。注意新口径下 CLI **确实会建目录**，
> 但方式是调用共用的 `ensure_root`/`check_root`（只补缺失），而不是旧实现里那份独立的、
> 只认四个目录的 CLI 私有逻辑。
>
> **路径提醒**：`src/core/app.cpp`、`src/cli/cli.cpp`、`include/fmt/core/path.hpp` 这些文件名
> **后来按新架构重新出现了**（阶段 2～4 的实现），与上面被移除的那批旧实现**不是同一份代码**；
> 上表列的是当前存在的文件。旧实现里「CLI 独立建四个目录」的私有逻辑没有回来——
> 现在的 CLI 只调共用的 `ensure_root`/`check_root`。

上一分支阶段 1 的验收结果（**历史记录**，保留备查，不代表当前骨架的能力；其中 `FMT_ROOT` 与
「四个运行目录」属于旧口径，新口径见第 3 节）：

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

## 11. 开发顺序

阶段划分以 `FMT 开发文档.md` 第 96 节为准，实现细节见 `FMT 技术文档.md` 第 18 节；重构后的
阶段顺序见 `FMT 重构设计.md` 第 10 节。**Windows Service 从原来的阶段 9 提前到阶段 2 / 3**，
业务功能整体后移。

**每完成一个模块就走「开发 → 单元测试 → 集成测试 → 异常测试」，不允许所有模块做完
才开始测试。**

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 项目骨架：CMake、Ninja、MSVC、`fmt.exe`、`--help` | ✅ 完成 |
| 2 | 基础设施：`common`（Error / Result / Time / String / Path / Logger）、`config`、`storage`、`core` 初始化（数据根解析、幂等建六个目录 + 默认 JSON；**`ensure_root`/`check_root` 由 Service 与 CLI 共用**，损坏 JSON 只报告不重置） | ✅ 完成 |
| 3 | 服务与通道：`service`（SCM **六条子命令** `install`/`uninstall`/`start`/`stop`/**`reinstall`**/`status` + 统一查询接口 `query_status()`/`query_state()`/`last_start_failure()`（第 4.6.1 节）+ 按 `dwWaitHint` 自适应的落定等待 + `ServiceMain` + 失败编号上报 `dwServiceSpecificExitCode` + Recovery + `%ProgramData%` 状态文件）、`ipc`（命名管道 + 安全描述符 + 帧）、`cli`（双击幂等体检与补齐、交互循环、横幅、单实例、提权引导、落定判定与 reinstall 兜底） | ✅ 完成 |
| 4 | Bucket：create / list / get / use / delete，`current_bucket` 逻辑；`common/validation` 名称校验；管道与 HTTP 两条入口打通；**Bucket 删除/回退的回收站形状**（目录名一律带时间戳、`trash/<user>/.original` 是桶级身份唯一权威、回退**整单判定** `FMT-401`、`file.json` 增加 `trash_reason`）与桶级 `trash list` / `get` / `restore` / `delete`（`get` / `delete` 提交 `4fee290`，永久删除要显式确认，且只清 `trash_reason="bucket"` 的记录）；文件级条目落点定为 `trash/<user>/.files/<bucket>/YYYY/MM/DD/` | ✅ 完成（commit 32249ea；回收站形状 c2d545d；`.files` 落点与 `trash get` / `delete` 见 `4fee290`；明细见 `FMT 技术文档.md` 第 18.15、18.17 节） |
| 5 | File / Upload / Trash / Share：`file.json`、`file_id`、文件名、extension、file_type、size、md5、`is_trash` / `trash_reason` / **`deleted_at`**、list / get / get by name / delete；HTTP(S) 下载、临时文件、大小限制、MD5、文件名冲突；**两级** Trash（文件级 = `file.json`，桶级 = `.original`）与其 list / get / restore / delete；Share create / get / list / delete、20 次限制、过期、并发安全 | 🟡 **`file` 与 `trash` 都已完成，且 https 可用**（提交 `188e85d` + `a2b6cd1` + `0ad9efc` + `9c3d2cb` + `711da4c` + `0fc242b`）：`file upload/list/get/delete`（软删除；跨 Bucket 删除要确认 → `FMT-016`）+ 上传**两段式** + MD5 去重（`FMT-304`）+ 重名拒绝（`FMT-105`）+ 存储日期由 `file_id` 推出；**`trash` 四条两级通用**（`src/trash/` 的 `TrashService` 合成 `TrashEntry`，`list` 标出 `[文件]` / `[桶]`）。**下载走 `common/http_client`（WinHTTP + Schannel）**。**剩下的**：`share` 整组（第 8 节 ④「长任务不持锁」已落地）。**「https 不支持」不再是限制** |

| 6 | HTTP Server + Preview：文件查询、下载、Share 访问（浏览器入口）；图片预览 | ⏳ 未开始（`/api/bucket` 五条 + `/api/trash` 四条 + `/api/file` 四条路由已在阶段 4、5 落地；`DELETE` 共用 `delete_args()` 解析 `?dry_run=1` / `?force=1`；下载/预览路由与页面未开始） |
| 7 | 收尾：**永久删除时清理相关 Share**（`share.json`）+ 两级回收站口径的真机实测 | ⏳ 未开始（**当前只剩一个缺口**：share 模块属阶段 6，现在永久删除**没有**清理 `share.json` 的动作。原缺口「文件级条目只写不读、`trash list` / `get` / `restore` 只认桶级」**已在提交 `0fc242b` 关闭**：两级合成一份 `TrashEntry` 视图） |

阶段 2 + 3 完成后，「双击即用的服务 + CLI」闭环成立：全新环境双击 → 一次 UAC → 服务装好且
开机自启 → `service stop` / `service start` 各弹一次 UAC 且输出与样例一致 → 把 exe 复制到
新目录双击，即在新目录建出数据。

与旧顺序的对应关系（便于对照旧文档）：原阶段 3（Bucket）→ 新阶段 4；原阶段 4（File 基础）、
5（Upload）、6（File 操作）、7（Trash）、8（Share）→ 合并进新阶段 5；原阶段 9（Windows
Service）→ **提前**到新阶段 3；原阶段 10（HTTP Server）、11（Preview）→ 新阶段 6。
新阶段 2 基本对应原阶段 2，并新增 `core` 的数据根初始化。

## 12. 后续阶段需要先解决的问题

| 事项 | 说明 |
|---|---|
| `FilenameValidator` | **已完成，从待决清单移出**：阶段 4 以 `common/validation` 落地（`include/fmt/common/validation.hpp` / `src/common/validation.cpp`）——`is_windows_reserved_name`（`CON`/`PRN`/`AUX`/`NUL`/`COM1..`/`LPT1..`，含带扩展名形式）、`kMaxNameBytes = 255`、`validate_bucket_name`（失败一律 `FMT-202`）、`validate_file_name`（`FMT-100`～`FMT-104`，**外加提交 `9c3d2cb` 的 `FMT-106 FileNameLikeFileId`**，按原因分工；新增判定函数 `bool looks_like_file_id(std::string_view)`——`fmt-YYYYMMDD-N` 的保留形状）。文档第 19/25/26/118 节的要求全部覆盖，用例见 `tests/validation_test.cpp`；错误码分工表见 `FMT 开发文档.md` 第 25 节、`FMT 技术文档.md` 第 9.3 节 |
| 编译器矩阵 | 仅验证 MSVC；Clang 未验证 |
| 日志轮转策略 | **已定并落地（提交 `821aba3`，从待决清单移出）**：5 MB 上限、轮转成 `fmt.log.1`、只留一代、`max_log_bytes = 0` 表示不轮转；前置条件是「每行开-写-关」（第 7.4 节、`FMT 技术文档.md` 第 14.6、18.36 节）。原口径「单文件大小上限与轮转规则未定」**已作废** |
| 提权副本的结果通道细节 | **已定稿，从待决清单移出**：结果经结果文件 `<数据根>\temp\fmt-elev-<父进程 pid>.json` 回传（第 4.5 节），数据根不可写时退回 `%TEMP%` 同名文件并记一行 WARN；命令行 `--elevated <operation> --result "<路径>"`，operation 五种（`install` / `uninstall` / `start` / `stop` / `reinstall`）。命名管道方案作废——提权副本是高完整性进程，它创建的管道会被 MIC「禁止向上写」挡住（与技术文档 13.9.2「坑 2」同一机制） |
| 数据根切换的并发保护 | 目前依赖「只有一个 CLI 窗口」（第 4.7 节）；多窗口场景不在本次范围。**「会话中途被别的 CLI 把数据根搬走」不可能发生**：服务一次只接受一条连接（`FMT 技术文档.md` 第 15.1 节① 严格串行），交互窗口握着管道时别的 CLI 拿 `ERROR_PIPE_BUSY` → 重试 → `FMT-601`；切换只可能发生在某个 CLI 连上来的那一刻——**所以换根提示在那一刻报就够了**（提交 `8f2fbc5`，第 3.1 节） |
| **阶段 5 的锁粒度** | **已定，从待决清单移出**（提交 `188e85d`）：阶段 4 是「两条入口的业务命令共用运行体的一把互斥锁」（第 8 节 ③）——上传/下载持锁会把 `bucket list` 与浏览器请求一起卡住。**实现选的是「把长任务移出锁」的简化形态**：上传两段式，`prepare_upload()`（下载/复制到 `temp/`、边写边算 MD5）在**锁外**、`commit_upload()`（去重 → 重名 → `file_id` → 搬文件 → 写 `file.json`）在**锁内**，运行体 `ServerRuntime::run_upload()` 编排、管道与 HTTP 共用。**没有引入按 JSON / 按 `file_id` 的细分锁**（那仍是目标形态），仍然只有 `ServerRuntime::mutex_` 一把；「上传期间其他命令一起等」不再是既定限制（第 8 节 ④、`FMT 技术文档.md` 第 15.1 ④、15.3、18.16、18.19 节） |
| `file.upload` 的命令超时值 | **已定并落地（提交 `a2b6cd1`，从「待决」改为「已知边界 + 现有缓解」）**：`ipc::kCommandTimeoutMs = 30000` 仍是其余命令的超时；**`file.upload` 改用新增的 `ipc::kUploadTimeoutMs = 30 * 60 * 1000`（30 分钟）**（`include/fmt/ipc/protocol.hpp`，CLI 侧 `src/cli/cli.cpp:651` 按 operation 选值）。CLI 在等待超时时额外打印「提示：等待服务响应超时。服务端可能仍在处理，稍后用 file list 确认；也可以查看 log/fmt.log。」**残余风险如实保留**：30 分钟上限到了仍可能出现「用户看到失败、服务端已经入库」；彻底解法仍是进度/长任务语义（`FMT 技术文档.md` 第 19.1 节保留此条） |

| 双击引导的重装判定 | **已定稿，从待决清单移出**：等待时长**不写死**，按 SCM 的 `dwWaitHint` 自适应（夹在 100 ms – 2000 ms，兜底 30 秒，不是等待类就立即结束，第 4.6.1 节）；仍没起来时先读 `dwServiceSpecificExitCode` 还原 FMT 编号（第 4.6 节）：命中数据根/配置类错误码集合（第 4.8 节表）就**不重装**、只打印原因，其余才提权 `reinstall` 一次 |
| `service status` 是否要机器可读输出 | **已定稿，从待决清单移出**：**V1 不做 `--json`、也不预留参数名**。机器可读通道是**命令退出码**（`0` 成功 / 未安装 `FMT-601` → `8`），人类可读通道是固定顺序的那几行文本（第 4.5、7.5 节）；需要结构化输出时再加 |

> **已冻结、不再是待解决问题**：Service 与 CLI 的通信方式（CLI 走命名管道
> `\\.\pipe\fmt.control`，第 4.3 节；浏览器走 HTTP `localhost:4122`，第 4.4 节）、
> 提权副本的结果通道（**`<数据根>\temp\` 下的结果文件**，第 4.5 节；命名管道方案作废）、
> 控制台编码（UTF-8：`SetConsoleOutputCP(CP_UTF8)` + `SetConsoleCP(CP_UTF8)`，第 7.5 节）、
> `service` 六条子命令与提权语义（第 4.5 节；除 `status` 外都提权）、服务注册参数与恢复策略
> （第 4.6 节）、服务启动失败的编号上报（`dwServiceSpecificExitCode`，第 4.6 节）、
> 统一查询接口与按 `dwWaitHint` 自适应的落定等待（第 4.6.1 节；`--json` 不在 V1 范围）、
> CLI 单实例与窗口激活（第 4.7 节）、服务宿主规则（第 4.8 节）、双击引导六步流程（第 4.8 节）、
> 数据根由 CLI 声明与 `hello` 的 `switched`/`previous_root` 回执（第 3.1 节）、
> 初始化规则由 Service 与 CLI 共用（第 3.2 节）、程序横幅与构建日期（第 7、7.5 节）、
> `help` 命令（第 7.5 节）、控制台只留交互与异常（第 7.2、7.5 节）。

## 13. 暂定内容

以下属于 V1 暂定，编码前可继续确认，不影响总体架构：`file.json`、`share.json`、
`trash.json` 单条记录的最终字段；HTTP API 路由；Share ID 生成方式；Bucket Trash
元数据结构（**桶级已冻结，见下**）；最大上传大小默认值；HTTP 鉴权（目前只监听 `127.0.0.1`，
面向局域网访问的安全控制后续再做）。

**阶段 4 之后从暂定转为已冻结的三条**：

- **HTTP 兜底路由的三条口径（提交 `4ddb515`，2026-10-09 真机实测后定）**：已知模块
  （`bucket` / `file` / `trash` / `share` / `config` / `server` / `preview`）下没有这个接口 →
  **`501 + FMT-602`**「接口尚未实现：<path>」；完全打错的 `/api/...` →
  **`404 + 新错误码 FMT-017 RouteNotFound`**「没有这个接口」；已知路由但业务找不到对象 →
  `404 + FMT-002`。**原口径「兜底硬编码 500 + FMT-602 操作尚未实现」已作废**——
  500 等于告诉调用方「服务器坏了」（实测 `GET /api/nosuch` 就是这样）。
  `FMT-602` 的映射也从「落 `default: 500`」改成**显式 501**
  （`FMT 技术文档.md` 第 12.3、12.5、18.31 节）。
- **HTTP 已开启（提交 `22c3c3e`，真机验收 2026-10-09 21:29）**：代码默认 `host` 从
  `127.0.0.1` 改成 **`localhost`**，线上 `config/server.json` 为 `enabled: true`，
  日志「HTTP 监听 localhost:4122」，接口全部可用。**认证**：所有 `/api/*` 都要 token
  （401 + `FMT-018`），例外是 `/api/ping` 与分享下载；**桶的接口已整体删除**
  （`/api/bucket*` → 404 + `FMT-017`）。
  **仍是缺口**：`ServerConfig::enabled` 代码默认仍是 `false`，而 `src/` 里
  **没有任何代码**把它置为 `true`（安装流程不碰 `config/server.json`，CLI 也没有命令能开），
  所以**全新数据根**装完服务后 `localhost:4122` 不会监听。
  **待用户决策**：① 安装流程按文档置 `true`，还是 ② 加一条 CLI 命令（如 `config http on`）。
  （第 4.4 节、`FMT 技术文档.md` 第 5.2、12.1、12.2、18.39、19.1 节）。

- **Bucket 级 Trash 元数据结构**：**权威是 `trash/<user>/.original`**，形状
  `{"version":1,"buckets":[{"trashed","original","deleted_at"}]}`；回收站目录名一律带删除
  时间戳（同秒冲突加 `_2`）；**`data/trash.json` 不再记桶级条目**（旧的 `type:"bucket"` 作废），
  **提交 `0fc242b` 起也不再记文件级条目**——文件级权威是 `file.json`（第 5.4 节），
  `trash.json` 只做只读兼容。
- **~~`/api/bucket` 五条路由~~ 已删除（提交 `4b812b5`，用户明确「桶不要」）**：
  `/api/bucket*` 现在返回 **404 + `FMT-017`**——**故意不要**，不是「还没做」，
  所以**不是 501**；「已知模块」列表里没有 `bucket`。桶继续由 CLI 管，
  HTTP 客户端操作的是**当前桶**。原先冻结的那五条路由与参数形状**已作废**
  （`FMT 技术文档.md` 第 12.3.2 节）。
  请求体仍接受 `{"name":"工作"}` 或 `{"argv":["工作"]}`（**其它路由沿用**）；
  路径参数里的中文由服务端 `url_decode` 解码；
  错误码 → HTTP 状态码的映射表也一并冻结（`FMT 技术文档.md` 第 12.3.2.1、12.5 节）。
- **`/api/trash` 四条路由（两级，提交 `0fc242b` 收口）**：`GET /api/trash`（列条目，回
  `{entries:[{type,id,name,bucket,deleted_at,bytes,files,present,restorable,trash_path?,reason?}], count, files, buckets}`）、
  `POST /api/trash/<标识>/restore`（回退，回 `{entry, message}`；`?dry_run=1` 预检）、
  `GET /api/trash/<标识>`（条目详情，回 `{entry}`）、
  `DELETE /api/trash/<标识>`（永久删除，**需 `?force=1` 或请求体 `{"force":true}`，否则
  `400 + FMT-016`**（提交 `711da4c` 起，原来记的是 `FMT-001`），回 `{entry, message}`；
  `?dry_run=1` 只预检）；标识可以是 `file_id` / 回收站目录名 / 原名（解析见
  `FMT 技术文档.md` 第 10.4 节），路径参数百分号解码。原口径「`deleted_buckets` 形状、
  文件级路由随阶段 6、7 细化」**已作废**——只有 `/api/config` 还随阶段 6 细化。

**阶段 5 又追加了一条冻结（提交 `188e85d`，`0ad9efc` 把 `DELETE` 的参数名放宽）**：

- **`/api/file` 四条路由与上传请求体**：`GET /api/file`（`file.list`）、
  `POST /api/file`（`file.upload`，请求体 `{"url":"…"}` 或 `{"path":"…"}`，可带
  `"file_name"`；**`file_name` 可省略**，省略时服务端从来源推断文件名）、
  `GET /api/file/<id_or_name>`（`file.get`，路径参数百分号解码）、
  `DELETE /api/file/<file_id_or_name>`（`file.delete`，**同桶软删除不需要 `force`**；
  **跨 Bucket 需要 `force`**，否则 `400 + FMT-016`（提交 `711da4c`）；`?dry_run=1` 只预检；
  提交 `0ad9efc` 起路径参数与 `GET` 一样可以是文件名，**路由正则本身没改**，
  仍是 `([^/]+)` + `url_decode()`）；
  请求体为空 → 400 + `FMT-001`，不是合法 JSON → 400 + `FMT-006`，
  缺 `url|path` → 400 + `FMT-001`；`data` 形状见 `FMT 技术文档.md` 第 12.3.2.1 节。
  四条与管道 op 一一对应，`file.upload` 也走运行体的两段式（下载在锁外、登记在锁内）。
- **名字/标识的比较不区分大小写（提交 `5bf2c1f`，数据损坏修复）**：`file_id`、文件名、
  桶名、回收站条目名的定位一律用 `iequals()`（ASCII 折叠）。这不是接口形状的改动
  （路由与 `args.argv` 都没变），而是**语义冻结**：上传时 `doc.txt` 与 `DOC.TXT`
  必须被判为同名并拒绝（否则两条记录指向同一个磁盘文件、字节被覆盖、元数据失真），
  按名字查询/删除、桶的 `is_current`/`was_current`、回收站定位同理
  （第 5.2、5.5 节与 `FMT 技术文档.md` 第 10.1、10.2.4、18.22 节）。
  **提交 `9c3d2cb` 的三点补充**：① `iequals()` 的实现**只折叠 ASCII**（`>= 0x80` 原样比较），
  不再把 UTF-8 字节交给 `std::tolower`（`setlocale` 一被调用就会改坏中文名字）；
  ② 桶名从「比较时不区分」更进一步——`create` 统一转小写、`use`/`get`/`delete` 规范化到
  **磁盘上的实际名字**（`canonical_name()`），`current_bucket`、`file.json` 的 `bucket`、
  `.original` 的 `original` 只留一份拼写；③ 新错误码 `FMT-106 FileNameLikeFileId`（退出码 2）
  让「文件名与 `file_id` 同形」在上传时就被拒，旧数据里的这种名字在 `file delete` 报
  `FMT-001` 歧义（`GET` 侧不加，`file get` 的两种查询范围不变，另回 `trash_path`）。

> 已冻结、不再属于暂定：JSON 文件组织形式（第 5.1 节）、错误码编号（附录 A）、
> 日志分级与去向（第 7.2 节）、`PathManager` 形式（第 7.3 节）、`fmt.exe` 三形态与 manifest
> `asInvoker`（第 4.1 节）、双通道与「业务只有一份实现」（第 4.2 节）、`service` 六条子命令与
> 提权语义（第 4.5 节）、服务注册参数与恢复策略（第 4.6 节）、CLI 单实例（第 4.7 节）、
> 服务宿主规则（第 4.8 节）、CLI 界面与输出格式（第 7.5 节）、数据根由 CLI 声明（第 3.1 节）、
> `hello` 响应新增 `switched` / `previous_root`（第 3.1 节）、CLI 双击时的幂等体检与补齐
> （第 3.2 节）、`service status` 不提权（第 4.5 节）、统一查询接口与按 `dwWaitHint` 自适应的
> 落定等待（第 4.6.1 节）、程序横幅 `File Manager Tool  v1.0  ( build  <日期> )`
> 与 `help` 命令（第 7、7.5 节）、控制台只留交互与异常（第 7.2、7.5 节）。

## 14. 不在 V1 范围

分块上传、断点续传、上传取消、复杂用户认证、权限系统、文件夹系统、文件逻辑对象、
物理对象引用计数、复杂搜索引擎、数据库、复杂 API 鉴权。

> **`HTTPS` 已从这一行里删掉（提交 `a2b6cd1`）**：**上传来源的 `https://` 在 V1 已经支持**
> ——下载走 `common/http_client` 的 **WinHTTP + Schannel**（TLS 用系统证书库），
> **不需要 OpenSSL、不分发任何 DLL**，还自动使用系统代理，见第 4.4、8 节与
> `FMT 开发文档.md` 第 33 节。
> 仍然不在 V1 范围的是**浏览器入口自己用 HTTPS 监听**（要处理证书、端口与浏览器信任链），
> 如需保留在清单里，请写成「浏览器入口的 HTTPS 监听」而不是笼统的 `HTTPS`。

本次重构另外明确不做：`service pause` / `continue`（服务不声明 `SERVICE_ACCEPT_PAUSE_CONTINUE`，
也没有对应命令）、多个 CLI 窗口（因此也不支持同时挂多个数据根）、业务命令的 HTTP 客户端实现
（CLI 只走命名管道，HTTP 只留给浏览器）、服务宿主 exe 的自动搬迁（移动请用复制，第 4.8 节）、
**命令的结构化输出（`--json` 等，也不预留参数名）**——机器可读通道是**命令退出码**
（`0` 成功 / 未安装 `FMT-601` → `8`），人类可读通道是固定顺序的那几行文本（第 4.5、7.5 节）。

---

## 附录 A. 错误码清单

**规则：编号一旦发布，不复用、不修改语义。新增错误只能追加新编号。错误码稳定，错误**
**消息可修改。**

前 10 项为已确认的核心错误，其余为 V1 各阶段所需，在对应模块实现前即可使用该编号。

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-001 | `InvalidArgument` | 参数错误 | 2 |
| FMT-002 | `FileNotFound` | 文件不存在（**提交 `a9af276`**：上传时本地路径找不到、而原始输入里带不可见格式字符时，消息**点名码位**——「…有不可见字符 U+202A、U+202C，它会让路径对不上；已自动清掉，请检查路径是否还有别的问题」；只去掉引号/空白时另有一句说明） | 3 |
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
| FMT-016 | `ConfirmRequired` | 该操作需要显式确认（`force`）——**提交 `711da4c` 追加**：永久删除（两级 `trash delete`）、跨 Bucket 的 `file delete`、非空桶的 `bucket delete` 缺 `force` 时返回它；HTTP 400；**不再复用 `FMT-001`** | 2 |
| FMT-017 | `RouteNotFound` | **没有这个接口**——**提交 `4ddb515` 追加**：HTTP 兜底路由遇到完全打错的 `/api/...` 路径时返回它（HTTP 404），默认消息「没有这个接口」。**只由 HTTP 兜底路由产生**——管道入口没有「路由」概念（op 名写错走业务层错误）。**已知模块下没有这个接口是另一回事**：`501 + FMT-602`（见下表 FMT-602 行）。**提交 `4b812b5` 追加一种用法**：`/api/bucket*` 也返回它——那是**故意不要**（用户明确「桶不要」），不是「还没做」 | 3 |
| FMT-018 | `Unauthorized` | **缺少 / 无效的访问 token**——**提交 `4b812b5` 追加**：HTTP 401、退出码 5。`/api/*` 都要 token，唯一例外是 `/api/ping`（健康检查）与 `/api/share/<id>/download`（分享链接本身就是凭证）；由 httplib 的 **pre-routing 钩子一处拒绝**（不逐个路由判断），且**没有注入校验器时一律 401**（fail-closed）。请求头收 `X-FMT-Token: <token>` 与 `Authorization: Bearer <token>`，比较是**常量时间**；token 从 `config list` 拿 | 5 |

**文件名校验（阶段 5 前，`common/validation`）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-100 | `FileNameEmpty` | 文件名为空 | 2 |
| FMT-101 | `FileNameInvalidChar` | 含 Windows 非法字符 / 以点或空格结尾 / **含不可见格式字符**（`U+00A0` / `U+200B`–`U+200F` / `U+202A`–`U+202E` / `U+2060`–`U+2064` / `U+2066`–`U+2069` / `U+FEFF` 等，**提交 `a9af276`**，消息点名码位） | 2 |
| FMT-102 | `FileNameSeparator` | 含路径分隔符 | 2 |
| FMT-103 | `FileNameReserved` | Windows 保留设备名 | 2 |
| FMT-104 | `FileNameTooLong` | 文件名超长 | 2 |
| FMT-105 | `FileNameConflict` | 同用户正常文件重名 | 4 |
| FMT-106 | `FileNameLikeFileId` | 文件名与文件标识同形（`fmt-YYYYMMDD-N`），会与 `file_id` 混淆（**提交 `9c3d2cb`**，属 `FMT-1xx` 文件名校验一组，与 Windows 保留设备名同类） | 2 |

**Bucket（阶段 4）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-200 | `BucketNotFound` | Bucket 不存在 | 3 |
| FMT-201 | `BucketAlreadyExists` | Bucket 已存在 | 4 |
| FMT-202 | `BucketNameInvalid` | Bucket 名称非法（含**不可见格式字符**，**提交 `a9af276`**，消息点名码位） | 2 |
| FMT-203 | `BucketInUse` | Bucket 仍被引用，不能删除——**保留（V1 未使用）**：**没有代码会产生它**（提交 `674d0b0` 起口径写死） | 4 |

> **`FMT-203` 的准确口径（提交 `674d0b0`）**：**保留、V1 未使用**。`bucket delete`
> 不会返回它——V1 允许删除仍有文件的 Bucket（数据一并移入回收站，该桶的 `file.json`
> 记录置 `is_trash` 并写 `trash_reason = "bucket"`）。原本设想它用于「桶仍被引用」，
> 但实际路径分别被 **`FMT-401`（回退冲突）**、**`FMT-402`（原桶已删）**、
> **`FMT-016`（删非空桶要确认）** 覆盖了，**所以没有产生它的代码**。
> **编号语义冻结、不删行**，但也不要假装它会被返回。
> `FMT-200` / `FMT-201` / `FMT-202` 三条已在使用（`FMT 开发文档.md` 第 27～30 节）。

**上传与下载（阶段 5、6）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-300 | `UrlInvalid` | URL 非法或协议不被支持（**http/https 之外的协议，如 `ftp://`**；不是 `FMT-002`） | 2 |
| FMT-301 | `DownloadFailed` | 下载失败（域名/连接/TLS/响应异常；**证书类失败给出「根证书不受信任 / 证书主机名不符 / 证书已过期」**；服务器返回非 identity 的压缩内容也归它） | 1 |
| FMT-302 | `DownloadTimeout` | 下载超时（`ERROR_WINHTTP_TIMEOUT`；接收超时是**单次读取的空闲超时**，不是总时长） | 1 |
| FMT-303 | `SizeLimitExceeded` | 超过 `max_upload_size` | 2 |
| FMT-304 | `Md5Duplicate` | MD5 已存在，文件重复 | 4 |
| FMT-305 | `NoCurrentBucket` | 未设置当前 Bucket | 3 |

**Trash（桶级阶段 4、文件级阶段 5/7）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-400 | `TrashEntryNotFound` | Trash 条目不存在（回退时回收站目录也没了；**`trash get` / `trash delete` 找不到条目也返回它**） | 3 |
| FMT-401 | `RestoreConflict` | 恢复目标已存在：**桶级回退时目标 Bucket 已存在就整单拒绝**（提示「回退失败：Bucket 已存在：<原名>」），文件级恢复时同名文件不覆盖不改名 | 4 |
| FMT-402 | `RestoreBucketMissing` | 原 Bucket 已永久删除 | 3 |

> **阶段 4 的桶级口径（commit c2d545d，收尾 `4fee290`）**：`FMT-401` 已在使用——
> `bucket restore` 发现 `repository/<user>/<原名>` 已存在就**整单拒绝**，不覆盖、不改名、
> 不做部分恢复（第 5.4.1 节）。
> `FMT-400` 在「`.original` 有记录、回收站目录已不存在」时返回（`trash list` 同时标
> `present: false`）；**`trash get` / `trash delete` 找不到条目时同样返回 `FMT-400`**
> （提交 `4fee290`），`trash get` 对「索引有、目录没了」的条目**不报错**、只返回
> `present: false`。另外两条**没有专属编号**、按通用码返回：`.original` 损坏 →
> `FMT-006 JsonParseError`（`bucket delete` / `trash restore` 直接拒绝，不搬目录）；
> 同名多条需要指定、或条目缺原桶名记录 → `FMT-001 InvalidArgument`（并列出候选的
> 标识）。
>
> **提交 `0fc242b` 的文件级口径**：`FMT-401` 也用在文件级回退上（目标位置已有同名
> 正常文件，或磁盘上有 `file.json` 里没有的同名文件）；同一节还给 `FMT-402` 加了第二种
> 触发条件——**随桶一起删除的文件**（`trash_reason == "bucket"`）单独回退时返回它，
> 消息让用户整体恢复那个桶；数据缺失（`.files/` 下没有那个文件）返回 `FMT-002`
> （消息给出路径）。三种都是「确认解决不了」的 `blocked`，**不是** `needs_confirm`。
>
> **永久删除缺 `force` 确认不再走 `FMT-001`**（提交 `711da4c`）：改用独立编号
> **`FMT-016 ConfirmRequired`**（退出码 2，HTTP 400，默认消息「该操作需要显式确认
> （force）」）。同一条规则也覆盖**跨 Bucket 的 `file delete`** 与**非空桶的 `bucket delete`**
> （见下表 FMT-016 行与 `FMT 开发文档.md` 第 82 节）。
> `FMT-402` 保留给以后。

**Share（阶段 5；**数据面已落地，提交 `d5779db`**）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-500 | `ShareNotFound` | Share 不存在（**未知 share_id 才是真错误**；「已过期 / 次数用尽 / 已撤销 / 关联文件在回收站」都是**成功 + 状态**，不伪装成不存在；撤销是删记录，所以 delete 之后再 get 就是它） | 3 |
| FMT-501 | `ShareExpired` | Share 已过期（`expire_time` 解析失败也按**已过期**处理——安全侧默认可拒） | 5 |
| FMT-502 | `ShareDownloadLimitReached` | 下载次数耗尽 | 5 |
| FMT-503 | `ShareFileUnavailable` | 关联文件不可用（处于 Trash 或已删除）；**文件进回收站时，它的 share 立刻不可用，且阻断新的 create** | 5 |

**Service 与 HTTP（阶段 3、6）**

| 编号 | ErrorCode | 含义 | 退出码 |
|---|---|---|---|
| FMT-600 | `ServiceAlreadyInstalled` | Service 已存在 | 8 |
| FMT-601 | `ServiceNotInstalled` | Service 不存在（`service status` 在未安装时也用它） | 8 |
| FMT-602 | `ServiceOperationFailed` | Service 操作失败。**提交 `4ddb515` 起它也是 HTTP 501 的专属码**：兜底路由遇到「已知模块下没有这个接口」时返回 `501 + FMT-602`「接口尚未实现：<path>」（share 整组属于这一类）——原来落 `default: 500`，把「还没做」说成了「服务器坏了」。提权侧它还有「结果文件不存在」等含义（见提权章） | 8 |
| FMT-603 | `AdminRequired` | 需要管理员权限 | 5 |
| FMT-604 | `NoCurrentUser` | 未设置当前用户 | 7 |
| FMT-700 | `HttpRequestInvalid` | HTTP 请求参数错误 | 2 |
| FMT-701 | `PreviewUnsupported` | 该文件类型不支持预览 | 2 |

> **`FMT-604` 的阶段 4 口径**：数据根初始化会把空的 `current_user` 补成占位名 `user`
> （第 3.2 节），所以**正常首启不会**遇到它；它保留给「用户被显式清空」这种情况。
> 与 `FMT-305 NoCurrentBucket` 别混：前者是「没有用户」，后者是「没选当前 Bucket」，
> 后者由文件类命令（阶段 5）返回。

> **提权相关失败不新增编号**，全部复用上表已有语义：用户取消 UAC → `FMT-004`；提权副本等待
> 超时、`ShellExecuteExW` 其它失败、结果文件不存在（提权副本崩了）、SCM 操作失败 → `FMT-602`；
> 未提权提示（打印「需要管理员权限」）→ `FMT-603`（只有最终无法提权时它才是退出码——包括
> `ShellExecuteExW` 失败且 `ERROR_ACCESS_DENIED`（5）的情况，正常提权流程中它只是提示行，
> 见第 4.5 节）；CLI 连不上服务管道 → `FMT-601`；`service status` 发现服务未安装 → `FMT-601`
> （退出码 8，但**不属于提权失败**，`status` 全程不提权）；服务宿主 exe 丢失需重装 → 仍用 `FMT-601`
> （但提权副本的 `reinstall` operation 会**忽略**卸载阶段的服务不存在错误，继续安装并启动，
> 不作为失败）。
> 服务启动失败时上报的 `dwServiceSpecificExitCode` 用的也是**上表已有编号的数字部分**（第 4.6 节），
> 不新增编号；CLI 读回后按同一张表还原 `FMT-NNN` 并打印。
> 因此 `FMT-605` 起的 Service 段编号**仍然空闲**，留待出现真正的新语义时再追加。

**未映射错误码的退出码默认为 1（通用错误）。** 上表退出码一项即代码中 `ErrorCode` →
`ExitCode` 的映射依据。

> 编号分段预留：`FMT-0xx` 通用、`FMT-1xx` 文件名、`FMT-2xx` Bucket、`FMT-3xx` 上传下载、
> `FMT-4xx` Trash、`FMT-5xx` Share、`FMT-6xx`/`FMT-7xx` Service 与 HTTP、`FMT-9xx`
> 保留给后续扩展。
