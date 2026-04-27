# PathOverlay MVP 开发计划

## 目标

第一版实现完整三层 MVP：C++ minifilter driver、C++ Windows 服务、C++ CLI。MVP 只覆盖一个本地目录规则，验证普通文件的 create/read/write/delete、copy-on-write、目录枚举合并、commit 和 discard。

## 技术边界

- 技术栈：C++ 全栈。
- 驱动：Windows minifilter，项目名 `PathOverlayFlt`。
- 服务：Windows 服务，服务名 `PathOverlaySvc`。
- CLI：命令名 `pathoverlay`。
- 元数据：SQLite，由服务独占写入。
- 默认数据目录：`%ProgramData%\PathOverlay`。

## 阶段 1：文档与工程骨架

交付：

- 设计审查与修订文档。
- MVP 开发计划。
- C++ Visual Studio 解决方案。
- `driver/`、`service/`、`cli/`、`common/`、`tests/`、`scripts/` 目录。

验收：

- MVP 范围和不支持项写入文档。
- 任务拆分写入 `task.json`。
- 构建脚本在缺少 WDK 或依赖时能输出清晰错误。

## 阶段 2：用户态核心

交付：

- 规则模型。
- 路径规范化。
- source/store 校验。
- RealPath 到 ShadowPath 映射。
- SQLite 元数据。
- commit/discard 的用户态规划和执行逻辑。

验收：

- MVP 只接受一个本地目录规则。
- rename、整盘覆盖、UNC、硬链接、ADS、完整 ACL 继承被明确拒绝或标记为不支持。
- source/store 互相包含时规则创建失败。
- 修改已有文件前生成 shadow copy。
- discard 不修改真实文件。
- commit 写回真实文件并保留日志。

## 阶段 3：服务与 CLI

交付：

- `PathOverlaySvc` 服务安装、启动、停止、卸载。
- CLI 到服务的命名管道 IPC。
- `rule add`、`rule show`、`changes`、`commit`、`discard` 命令。

验收：

- CLI 不直接写 SQLite。
- 服务不可用时 CLI 给出清晰错误。
- 规则和变更状态由服务统一管理。
- commit/discard 请求通过服务执行。

## 阶段 4：minifilter 驱动

交付：

- 驱动加载、卸载和过滤器注册。
- 服务通信端口。
- create/open 重定向。
- copy-on-write 请求。
- delete tombstone。
- 目录枚举合并。

验收：

- 未启用规则时不发生重定向。
- source 外路径不受影响。
- store 目录不会被递归重定向。
- 服务进程自己的文件访问不会被重定向。
- 读操作优先读取 shadow 文件。
- tombstone 文件表现为不存在。

## 阶段 5：端到端验证

交付：

- 单元测试。
- 用户态集成测试。
- 驱动 E2E 测试说明。
- 测试数据清理脚本。

验收：

- 在临时 source 目录内创建、修改、删除文件时，真实目录在 commit 前不被提前修改。
- 目录枚举能显示 shadow 新文件，隐藏 tombstone 文件。
- discard 清理隔离数据且不修改真实目录。
- commit 成功写回变更并清理 shadow。
- commit 失败时保留可检查或可重试的元数据。

## 开发顺序

开发顺序以 `task.json` 为准。每个任务开始前标记为 `in_progress`，完成对应 `test_criteria` 后标记为 `done`。如果缺少 WDK、管理员权限或 test-signing 环境导致无法继续，应标记为 `blocked` 并记录原因。

