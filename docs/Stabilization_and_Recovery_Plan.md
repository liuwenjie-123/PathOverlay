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

文档和 CLI 文案不得把第一阶段描述为完整 rollback。

### Backup Index

commit 成功或部分失败时，服务应把实际创建的备份写入可查询索引。索引只描述已落盘的备份内容，不反向生成用户继续修改后的差异，也不保证能重建一次 commit 的完整事务顺序。

推荐表结构：

```sql
CREATE TABLE backup_sets_v1 (
  backup_set_id TEXT PRIMARY KEY,
  operation_id TEXT NOT NULL,
  rule_id TEXT NOT NULL,
  backup_root TEXT NOT NULL,
  source_root TEXT NOT NULL,
  created_at TEXT NOT NULL,
  status TEXT NOT NULL,
  error TEXT NOT NULL DEFAULT '',
  FOREIGN KEY(operation_id) REFERENCES operations_v1(operation_id),
  FOREIGN KEY(rule_id) REFERENCES rules(id)
);

CREATE TABLE backup_items_v1 (
  backup_item_id TEXT PRIMARY KEY,
  backup_set_id TEXT NOT NULL,
  real_path TEXT NOT NULL,
  backup_path TEXT NOT NULL,
  kind TEXT NOT NULL,
  reason TEXT NOT NULL,
  original_size INTEGER NOT NULL DEFAULT -1,
  original_last_write_time INTEGER NOT NULL DEFAULT 0,
  restore_status TEXT NOT NULL DEFAULT '',
  restore_error TEXT NOT NULL DEFAULT '',
  FOREIGN KEY(backup_set_id) REFERENCES backup_sets_v1(backup_set_id)
);
```

`backup_sets_v1.status` 允许：

- `available`：索引和备份根存在，至少有一项可尝试手工恢复。
- `partial`：commit 中断或备份项不完整，只能逐项检查和恢复。
- `missing`：索引存在但备份根或全部备份文件缺失，只能报告错误。

`backup_items_v1.kind` 允许 `file` 或 `directory`。`reason` 记录创建备份的原因，例如 `overwrite`、`delete` 或 `rename-source`。第一版只索引 commit 写回前已经存在且被复制到 backup root 的真实内容；新建文件没有旧内容，不生成 backup item。

### `backup list`

命令形式：

```powershell
pathoverlay backup list [--rule <id>] [--operation <operation-id>] [--json]
```

输入语义：

- 不带参数时列出所有 backup set 摘要，按 `created_at` 倒序输出。
- `--rule <id>` 只列出指定 rule 的 backup set。
- `--operation <operation-id>` 只列出指定 operation/commit 对应的 backup set 和 item。
- `--json` 输出稳定字段，供诊断工具或测试脚本消费；文本输出保持面向人工检查。

文本输出应包含：

- backup set：`backup_set_id`、`operation_id`、`rule_id`、`status`、`created_at`、`source_root`、`backup_root`、item 数量。
- backup item：`backup_item_id`、`kind`、`reason`、`real_path`、`backup_path`、`original_size`、`original_last_write_time`、`restore_status`。
- 缺失项诊断：当 `backup_path` 不存在或类型不匹配时输出 `WARN missing backup item`，但不修改索引。

### `restore`

命令形式：

```powershell
pathoverlay restore --backup <backup-set-id> --item <backup-item-id> [--target <path>] [--overwrite]
pathoverlay restore --backup <backup-set-id> --all [--overwrite]
```

恢复语义：

- 第一版只从已有 `backup_items_v1.backup_path` 复制内容到目标路径；备份文件或目录不存在时失败。
- 默认目标是 item 的 `real_path`。`--target <path>` 只允许单 item 恢复，用于把备份恢复到用户指定位置；`--all` 不允许改写目标根。
- 恢复前必须确认目标 rule 没有 pending/failed changes；存在 pending/failed changes 时默认拒绝，要求用户先 `commit`、`discard` 或另行导出备份。
- 默认不覆盖现有目标。如果目标已存在且未指定 `--overwrite`，返回冲突并列出目标路径。
- 指定 `--overwrite` 时仍要做类型检查：文件只能覆盖文件，目录只能覆盖目录；类型不匹配时拒绝。
- `restore` 不暂停其他 rule，不尝试关闭占用进程；目标被占用、权限不足或父目录不可创建时失败并保留索引。
- 成功后只更新 item 的 `restore_status` 和 `restore_error`，不删除 backup 文件，也不删除 operation、commit 或 change 历史。

风险限制：

- `restore --all` 是按 item 顺序的批量复制，不是事务；中途失败时已经复制的目标不会自动回滚。
- `restore` 不根据 commit item 顺序重放删除、rename 或目录操作；它只恢复索引中已有的旧内容。
- `restore` 不判断 commit 后用户对真实 source 的业务语义变更，只通过目标存在性、类型和可选 `--overwrite` 做保守冲突处理。
- `restore` 不跨 rule 恢复；backup set 的 `rule_id` 必须仍存在，且目标路径必须位于该 rule 的 `source_root` 下，除非使用单 item `--target` 导出到 source 外的用户指定路径。
- CLI 帮助、README 和诊断输出只能称为 `backup restore` 或 `manual restore`，不得称为完整 `rollback`。

## 验收映射

- T037：实现 operation 状态、启动扫描和保守恢复。
- T038：实现持久 cleanup queue 和重启续删。
- T039：实现 `status`、`doctor` 和只读诊断接口。
- T040：实现 `diagnostics collect` 和日志字段补充。
- T041：实现 `changes --rule`、commit/discard dry-run。
- T042：设计 backup index、`backup list` 和手工 `restore`。
- T043：扩展路径与属性兼容性测试。
- T044：把稳定化命令和发布检查清单写入用户文档。
