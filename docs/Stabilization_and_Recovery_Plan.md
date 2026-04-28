# PathOverlay Stabilization and Recovery Plan

本文定义 vNext 原型后的稳定化阶段边界，重点覆盖 commit/discard 中断恢复、discard 后台清理恢复、只读诊断命令、诊断包收集、dry-run 和备份恢复限制。实现任务以 `task.json` 的 T037 到 T044 为准；本文只定义第一阶段要保证的语义，不把 PathOverlay 扩展为完整事务文件系统或安全沙箱。

## 目标

- 服务启动后能识别上次未完成的 commit/discard，并给出可诊断、可重试的状态。
- discard 的后台删除不再是不可见的 fire-and-forget 操作，服务重启后能继续处理或报告失败。
- CLI 提供只读 `status`、`doctor` 和 `diagnostics collect`，默认不修改 metadata、shadow 或真实 source。
- `changes --rule`、`commit --dry-run` 和 `discard --dry-run` 在执行前展示影响范围，降低误操作风险。
- 备份恢复先提供可查询、可手工恢复的边界，不承诺完整事务 rollback。

## 非目标

- 不实现自动 repair 或自动 restore。
- 不在启动恢复中删除 pending changes、failed changes 或仍可能有用的 shadow 数据。
- 不把 commit 设计成跨文件系统和 SQLite 的原子事务。
- 不恢复进程关闭前的应用状态，不自动杀进程。
- 不支持跨 rule、跨卷、目标已存在的 rename/move 恢复。
- 不解决硬链接、ADS、完整 ACL 继承、reparse point 和注册表虚拟化。

## Operation 状态机

稳定化阶段新增持久 operation 记录，用于描述一次 commit 或 discard 的整体状态。推荐表结构可以独立于旧 `commits` 表，也可以由 `commits` 表迁移而来：

```sql
CREATE TABLE operations_v1 (
  operation_id TEXT PRIMARY KEY,
  rule_id TEXT NOT NULL,
  action TEXT NOT NULL,
  status TEXT NOT NULL,
  phase TEXT NOT NULL,
  started_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  finished_at TEXT NOT NULL DEFAULT '',
  backup_root TEXT NOT NULL DEFAULT '',
  error TEXT NOT NULL DEFAULT '',
  FOREIGN KEY(rule_id) REFERENCES rules(id)
);
```

`action` 允许值：

- `commit`
- `discard`

`status` 允许值：

- `running`：服务已开始执行，可能已暂停规则或处理部分 change。
- `done`：全部步骤完成，规则状态已恢复，驱动缓存已同步。
- `failed`：执行失败或启动恢复无法证明安全完成，保留 metadata 和 shadow。
- `recoverable`：启动恢复确认没有继续执行的风险，但需要用户显式重试 commit/discard。

`phase` 允许值：

- `created`：operation 已持久化，尚未暂停规则。
- `rule_paused`：目标 rule 已暂停并同步驱动。
- `prechecked`：占用、冲突、shadow 存在性和真实路径基线检查已完成。
- `applying`：正在写回真实 source 或正在迁移 discard shadow。
- `cleanup`：真实写回或 discard 元数据更新已完成，正在清理 shadow 或后台队列。
- `restoring_rule`：正在恢复 rule 状态并同步驱动。
- `finished`：终态。

### Commit 流程

1. 创建 `operations_v1(status='running', phase='created')`。
2. 暂停目标 rule，刷新驱动规则缓存，更新 `phase='rule_paused'`。
3. 执行占用检测、冲突检测和 shadow 存在性检查，更新 `phase='prechecked'`。
4. 对每个 change 标记 `committing`，写入 item 级日志和备份路径，更新 `phase='applying'`。
5. 写回真实 source，失败时把对应 change 标记 `failed`，operation 标记 `failed`，不得删除 shadow。
6. 全部成功后归档或删除 change，进入 `phase='cleanup'`。
7. 清理可立即删除的 shadow，恢复 rule 状态，同步驱动，operation 标记 `done`。

### Discard 流程

1. 创建 `operations_v1(status='running', phase='created')`。
2. 暂停目标 rule，刷新驱动规则缓存。
3. 将当前 active `store\drive` 原子移动到 cleanup staging 目录。
4. 删除该 rule 的 pending/failed change，operation 进入 `phase='cleanup'`。
5. 恢复 rule 状态并同步驱动，operation 标记 `done`。
6. staging 目录由 cleanup queue 后台删除；删除失败不回滚 discard，只记录 cleanup 失败。

discard 不修改真实 source。若步骤 3 之后服务中断，启动恢复必须优先确认 active `store\drive` 已经不再指向旧 shadow，再根据 metadata 判断是否需要继续删除 change 或仅报告 recoverable。

## 启动恢复策略

服务启动时，在加载规则和同步驱动前扫描 `operations_v1`、change 状态和 cleanup queue。

对 `status='running'` 的 operation：

- `phase='created'`：未暂停规则也未处理文件，标记 `failed` 或 `recoverable`，保留所有 changes。
- `phase='rule_paused'` 或 `phase='prechecked'`：恢复 rule 到原启用状态，标记 `recoverable`，允许用户重试。
- `phase='applying'`：逐条检查 item log、真实路径、备份路径和 shadow。能证明已完成的 item 标记完成；不能证明的 item 标记 `failed`，operation 标记 `failed`。
- `phase='cleanup'`：如果 change 已清空但 shadow staging 仍存在，转入 cleanup queue；如果 change 未清空，则标记 `failed` 并保留 shadow。
- `phase='restoring_rule'`：重新恢复 rule 状态并同步驱动，成功后按 operation 结果标记 `done` 或 `failed`。

恢复过程的保守原则：

- 不能证明真实 source 已安全写回时，不删除 change。
- 不能证明 shadow 已无用途时，不删除 shadow。
- 启动恢复不自动执行新的 commit 写回。
- 启动恢复不自动覆盖真实 source。
- 恢复结束后，`status` 和 `doctor` 必须能展示 operation、rule、change 和 cleanup 的状态。

## Discard Cleanup Queue

discard 成功路径中，用户可见的语义是 pending changes 立即清空、active shadow 不再参与重定向。实际删除大目录由持久 cleanup queue 处理：

```sql
CREATE TABLE cleanup_queue_v1 (
  cleanup_id TEXT PRIMARY KEY,
  rule_id TEXT NOT NULL,
  path TEXT NOT NULL,
  status TEXT NOT NULL,
  attempts INTEGER NOT NULL,
  last_error TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
```

`status` 允许值：

- `pending`：等待后台删除。
- `running`：本服务进程正在删除。
- `done`：目录已删除或确认不存在。
- `failed`：多次失败或遇到权限、占用、路径异常。

服务启动时：

1. 将旧的 `running` cleanup 重置为 `pending`。
2. 校验 cleanup 路径必须位于对应 rule 的 store 或 `%ProgramData%\PathOverlay` 已知 staging 根目录下。
3. 对 `pending` 项按创建时间删除。
4. 删除成功标记 `done`；失败递增 `attempts` 并记录 `last_error`。

cleanup queue 绝不能删除真实 source，也不能根据用户输入拼接任意递归删除路径。删除前必须解析绝对路径并确认位于允许根目录内。

## 诊断命令范围

### `pathoverlay status`

`status` 是只读命令，默认输出：

- 服务连接状态和服务版本。
- 驱动连接状态、驱动协议版本和规则同步摘要。
- 规则数量、enabled/disabled 数量。
- 每条 rule 的 id、enabled、source、store 和 pending change 数。
- 最近 operation 的 action、status、phase、error。
- cleanup queue 的 pending/running/failed 数量。

`status` 不扫描完整 shadow 目录树，只读取 metadata 和轻量驱动状态。

### `pathoverlay doctor`

`doctor` 是只读一致性检查，默认发现但不修复：

- operation 卡在 `running`、`failed` 或 `recoverable`。
- cleanup queue 指向不存在、越界或删除失败的路径。
- change 指向缺失 shadow，或 shadow 存在但 metadata 缺失。
- rule 的 source/store 不存在或出现 source/source、source/store 重叠。
- driver 已加载但规则缓存同步失败。

`doctor` 输出建议使用 `OK`、`WARN`、`ERROR` 等级。第一阶段不提供自动 `--fix`；未来如增加修复，也必须逐项确认。

### `pathoverlay diagnostics collect`

`diagnostics collect` 生成诊断目录或 zip，内容包括：

- `rule show`、`changes`、`status`、`doctor` 输出。
- 最近服务日志。
- metadata 摘要，不默认包含用户文件内容。
- cleanup queue、operation、commit log 摘要。
- `sc queryex PathOverlaySvc`、`sc queryex PathOverlayFlt`、`fltmc filters` 或等价状态。
- 测试机 E2E 失败时的脚本日志和包内文件哈希。

诊断包默认不得复制真实 source 文件内容和 shadow 文件内容；如未来支持内容采样，必须显式 opt-in。

## Dry-run 和 Changes 过滤

`changes --rule <id>` 只显示指定 rule 的待处理变更，并保留现有不带 `--rule` 的全量分组输出。

`commit --dry-run --rule <id>` 输出：

- 将写入或创建的真实路径。
- 将删除的真实路径。
- 将 rename/move 的 source 和 target。
- 将创建的备份根目录和预估备份项。
- 会阻塞提交的占用、冲突、缺失 shadow 或目标已存在错误。

`discard --dry-run --rule <id>` 输出：

- 将清除的 change 数量。
- 将从 active store 移入 cleanup queue 的 shadow 根。
- 不会修改的真实 source 范围。

dry-run 不暂停 rule、不修改 metadata、不创建备份、不移动 shadow、不删除任何文件。

## Repair、Restore 和 Rollback 边界

第一阶段只承诺诊断和手工恢复辅助：

- `doctor` 不自动修复。
- 启动恢复不自动 restore 备份。
- `restore` 设计只从已有 backup index 中复制用户选择的备份项。
- `restore` 不承诺恢复完整事务顺序，也不自动解决用户在 commit 后继续修改真实 source 的冲突。
- 没有 backup index 或备份文件缺失时，只报告错误。
- 恢复前如目标 rule 存在 pending changes，默认拒绝。

备份索引和手动恢复命令由 T042 细化。文档和 CLI 文案不得把第一阶段描述为完整 rollback。

## 验收映射

- T037：实现 operation 状态、启动扫描和保守恢复。
- T038：实现持久 cleanup queue 和重启续删。
- T039：实现 `status`、`doctor` 和只读诊断接口。
- T040：实现 `diagnostics collect` 和日志字段补充。
- T041：实现 `changes --rule`、commit/discard dry-run。
- T042：设计 backup index、`backup list` 和手工 `restore`。
- T043：扩展路径与属性兼容性测试。
- T044：把稳定化命令和发布检查清单写入用户文档。
