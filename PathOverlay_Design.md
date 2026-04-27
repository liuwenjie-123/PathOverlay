# PathOverlay 文件系统重定向工具设计文档

> 设计修订与 MVP 边界以 `docs/Design_Review_and_Revisions.md` 为准。
> 第一版开发任务与验收标准见 `docs/MVP_Development_Plan.md` 和 `task.json`。

## 1. 项目目标

PathOverlay 是一个 Windows 文件系统重定向工具，用于对指定路径或盘符建立可回滚的写入覆盖层。

目标行为：

- 用户指定一个真实路径或盘符，例如 `C:\Work`、`D:\`。
- 工具将命中范围内的写入重定向到隔离目录，例如 `C:\PathOverlay\Boxes\Default\...`。
- 真实系统默认不被修改。
- 用户可以选择“应用变更”，将隔离目录中的新增、修改、删除同步回真实路径。
- 用户可以选择“丢弃变更”，删除隔离数据，使系统保持原状。

这类工具本质是一个文件系统 overlay/copy-on-write 层，而不是完整沙箱。它只关注文件系统变更的重定向、提交和丢弃。

## 2. 参考思路

Sandboxie 的文件重定向模型可以作为主要参考：

- 对每个访问路径计算两套路径：
  - `TruePath`：真实系统路径。
  - `CopyPath`：沙箱/隔离目录中的副本路径。
- 读操作优先读取隔离副本，否则读取真实文件。
- 写操作触发 copy-on-write：如果真实文件存在但隔离副本不存在，先复制真实文件到隔离目录，再写隔离副本。
- 删除不直接删除真实文件，而是记录删除标记。
- 目录枚举需要合并真实目录和隔离目录，并过滤删除标记。
- 恢复/提交由用户态管理程序执行，不建议在驱动内做复杂业务逻辑。

## 3. 推荐架构

推荐采用 `Windows minifilter driver + 用户态服务 + UI/CLI` 三层架构。

```text
UI / CLI
  - 配置重定向规则
  - 展示变更列表
  - 执行应用变更或丢弃变更

用户态服务
  - 保存规则和元数据
  - 与驱动通信
  - 执行 commit / discard
  - 冲突检测、备份、日志

Minifilter 驱动
  - 拦截文件创建、打开、写入、删除、重命名、目录枚举
  - 判断路径是否命中规则
  - 执行路径重定向
  - 维护运行时状态缓存
```

不推荐只做用户态 API Hook 作为正式方案。API Hook 只能影响被注入进程，不能可靠覆盖系统服务、未注入进程、原生 NT 调用、内存映射和部分安全软件场景。若目标是透明地重定向某个路径或盘符，应优先走 minifilter。

## 4. 核心概念

### 4.1 路径映射

示例：

```text
RealPath:
  C:\Users\Alice\Desktop\a.txt

RedirectPath:
  C:\PathOverlay\Boxes\Default\drive\C\Users\Alice\Desktop\a.txt
```

盘符路径建议统一映射为：

```text
C:\PathOverlay\Boxes\<BoxName>\drive\<DriveLetter>\...
```

例如：

```text
D:\Game\data.db
```

映射为：

```text
C:\PathOverlay\Boxes\Default\drive\D\Game\data.db
```

### 4.2 规则

基础规则结构：

```json
{
  "id": "rule-001",
  "name": "default-work-overlay",
  "enabled": true,
  "source": "C:\\Work",
  "store": "C:\\PathOverlay\\Boxes\\Default",
  "mode": "overlay"
}
```

规则需要支持：

- 指定目录，例如 `C:\Work`。
- 指定盘符，例如 `D:\`。
- 启用和禁用。
- 排除路径。
- 只读直通路径。
- 可选：按进程生效。

MVP 阶段可以先不做按进程过滤，默认全局生效。

### 4.3 元数据

隔离目录中需要维护元数据，不能只依赖文件树。

建议维护：

```json
{
  "realPath": "C:\\Work\\a.txt",
  "redirectPath": "C:\\PathOverlay\\Boxes\\Default\\drive\\C\\Work\\a.txt",
  "state": "created | modified | deleted | renamed",
  "originalExists": true,
  "originalSize": 12345,
  "originalLastWriteTime": "2026-04-24T12:00:00Z",
  "currentSize": 23456,
  "lastWriteTime": "2026-04-24T12:10:00Z"
}
```

删除标记可以存在数据库中，也可以落为 tombstone 文件。建议用 SQLite 或单独 journal 文件管理，便于提交时遍历。

## 5. 重定向语义

### 5.1 打开文件

命中规则后：

1. 计算 `RealPath`。
2. 计算 `RedirectPath`。
3. 如果路径存在删除标记，则表现为文件不存在。
4. 如果隔离副本存在，则打开隔离副本。
5. 如果隔离副本不存在：
   - 纯读打开：打开真实文件。
   - 写打开：执行 copy-on-write 后打开隔离副本。

### 5.2 新建文件

新建文件不写入真实路径，直接创建到 `RedirectPath`。

同时记录：

```text
state = created
originalExists = false
```

### 5.3 修改已有文件

如果真实文件存在但隔离副本不存在：

1. 创建隔离目录父路径。
2. 复制真实文件到隔离路径。
3. 复制基础元数据，例如时间戳、属性、ACL。
4. 后续写入都落到隔离副本。
5. 记录 `state = modified`。

### 5.4 删除文件

删除操作不能直接删除真实文件。

行为：

1. 如果隔离副本存在，可以删除隔离副本。
2. 记录删除标记。
3. 后续打开该路径时返回不存在。
4. 目录枚举时隐藏该路径。

状态：

```text
state = deleted
```

### 5.5 重命名文件

重命名是高风险场景，MVP 可以先限制为同一规则范围内的重命名。

建议行为：

1. 源文件若无隔离副本，先 copy-on-write。
2. 在隔离目录中执行 rename。
3. 对源路径记录删除或 renamed-from。
4. 对目标路径记录 created/renamed。

跨盘符、跨规则、硬链接相关 rename 可以先返回不支持或转为复制后删除。

### 5.6 目录枚举

目录枚举必须合并两棵树：

- 真实目录。
- 隔离目录。

合并规则：

- 隔离目录中的同名项覆盖真实项。
- 删除标记隐藏真实项。
- 新增项从隔离目录显示出来。
- 修改项显示隔离副本的元数据。

这是工具可用性的关键点。只重定向写入但不处理目录枚举，会导致用户看不到新建文件或看到已删除的真实文件。

## 6. 应用变更

应用变更即把隔离层写回真实系统。

建议由用户态服务执行，不要在驱动里实现复杂提交逻辑。

流程：

1. 暂停目标规则或阻止新的写入。
2. 等待相关文件句柄释放，或提示用户关闭占用进程。
3. 扫描元数据和隔离目录。
4. 做冲突检测：
   - 真实文件是否被外部修改。
   - 真实目标是否被删除。
   - 目标路径是否存在同名文件。
   - 权限是否允许写入。
5. 对会被覆盖或删除的真实文件做备份。
6. 按依赖顺序提交：
   - `created`：复制隔离文件到真实路径。
   - `modified`：替换真实文件。
   - `deleted`：删除真实文件。
   - `renamed`：移动或复制到真实目标，并处理源路径。
7. 提交成功后删除隔离数据和元数据。
8. 若失败，使用备份回滚或保留隔离层等待用户处理。

提交阶段必须有日志：

```text
commit-id
start-time
rule-id
operation-list
backup-path
status
error
```

## 7. 丢弃变更

丢弃变更比提交简单，因为真实系统默认未被修改。

流程：

1. 暂停目标规则。
2. 等待隔离目录内文件句柄释放。
3. 删除隔离目录。
4. 删除元数据。
5. 清理驱动缓存。
6. 恢复规则或保持禁用。

如果隔离文件仍被占用，需要提示用户关闭相关进程，或者标记为下次启动删除。

## 8. MVP 范围

第一阶段建议只做最小可用版本：

- 支持一个本地目录规则。
- 支持普通文件 create/read/write/delete。
- 支持 copy-on-write。
- 支持目录枚举合并。
- 支持提交和丢弃。
- 支持基础变更列表。

暂不支持：

- 整个系统盘 `C:\`。
- 网络盘和 UNC 路径。
- 硬链接。
- NTFS alternate data streams。
- reparse point / junction / symlink 完整语义。
- 内存映射写入一致性。
- 按进程规则。
- ACL 完整继承和安全边界。

第二阶段再扩展：

- 盘符级规则。
- 排除路径。
- 按进程生效。
- 冲突解决 UI。
- 快照。
- 变更预览。
- 提交前备份和回滚。

## 9. 关键技术点

### 9.1 Windows minifilter

驱动重点拦截：

- `IRP_MJ_CREATE`
- `IRP_MJ_READ`
- `IRP_MJ_WRITE`
- `IRP_MJ_SET_INFORMATION`
- `IRP_MJ_DIRECTORY_CONTROL`
- `IRP_MJ_CLEANUP`
- `IRP_MJ_CLOSE`

MVP 可以优先处理：

- create/open 路径重定向。
- write 前 copy-on-write。
- delete/rename 的 `SetInformation`。
- directory query 的合并。

### 9.2 路径规范化

必须统一处理：

- DOS 路径：`C:\A\b.txt`
- NT 路径：`\Device\HarddiskVolumeX\A\b.txt`
- `\??\C:\A\b.txt`
- 大小写不敏感比较。
- 结尾反斜杠。
- 短文件名。

路径规范化如果做不好，规则匹配会不稳定。

### 9.3 数据一致性

需要考虑：

- 文件仍被打开时不能提交。
- 外部进程可能绕过规则修改真实文件。
- 提交过程中失败要保留可恢复状态。
- 删除操作必须用 tombstone 记录。
- rename 必须记录源和目标关系。

## 10. 建议项目结构

```text
PathOverlay/
  driver/
    PathOverlayFlt/
  service/
    PathOverlaySvc/
  cli/
    pathoverlay/
  ui/
    PathOverlayUI/
  docs/
    design.md
    minifilter-notes.md
    commit-discard.md
  tests/
    integration/
```

## 11. 推荐名称

推荐项目名：

```text
PathOverlay
```

含义直接，能覆盖“路径”和“盘符”的重定向场景，也不强绑定完整沙箱概念。

可选名称：

- `RedirectBox`
- `ShadowPath`
- `WriteLayer`
- `RevertFS`
- `DiskOverlay`
- `SandboxFS`

## 12. 第一版实现路线

建议按这个顺序推进：

1. 建立 CLI 和服务配置模型。
2. 实现路径规则和元数据存储。
3. 写一个用户态 PoC 验证 `RealPath -> RedirectPath`、commit、discard 逻辑。
4. 开始 minifilter，先只拦截指定目录下的 create/open。
5. 加入 copy-on-write。
6. 加入 delete tombstone。
7. 加入目录枚举合并。
8. 加入 commit/discard。
9. 增加冲突检测和备份回滚。

不要一开始实现整盘透明重定向。先把单目录 overlay 做稳定，再扩展到盘符级别。
