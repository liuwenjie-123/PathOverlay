# PathOverlay vNext Metadata and Compatibility Plan

本文定义 vNext 元数据设计，覆盖多规则、目录变更、rename/move、按规则 commit/discard 和占用处理。MVP 的运行行为仍以现有实现和 README 为准；本文是后续 T020 及之后任务的实现约束。

## 设计目标

- 旧 `metadata.db` 可原地初始化或迁移，已有单规则数据不丢失。
- 所有变更都显式归属一个 `rule_id`，多规则之间不能共享 pending change。
- 元数据能表达文件、目录、文件 rename 和目录 rename。
- commit/discard 失败后保留足够状态，允许用户检查、重试或显式丢弃。
- 暂不支持排除路径、注册表虚拟化和完整系统盘保护。

## Schema 版本

新增 `schema_info` 表保存数据库版本：

```sql
CREATE TABLE IF NOT EXISTS schema_info (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
```

`schema_info('schema_version')` 的值从 `1` 开始。现有 MVP 数据库没有该表，初始化时视为 `0`，迁移到 `1`。迁移必须在事务中执行：

```sql
BEGIN IMMEDIATE;
-- inspect old schema
-- apply additive migration
INSERT OR REPLACE INTO schema_info(key, value) VALUES('schema_version', '1');
COMMIT;
```

迁移失败必须回滚并保持旧数据库可由旧版本继续读取。服务启动时若发现高于当前程序支持的版本，应拒绝启动并记录清晰错误，不能尝试降级写入。

## vNext Tables

### rules

保留现有字段，并增加创建时间、更新时间和规则状态：

```sql
CREATE TABLE rules_v1 (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  enabled INTEGER NOT NULL,
  source TEXT NOT NULL,
  store TEXT NOT NULL,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  state TEXT NOT NULL DEFAULT 'active'
);
```

`state` 允许值：

- `active`：可参与 CLI 和驱动同步。
- `paused`：commit/discard 期间临时暂停，不同步到驱动。
- `disabled`：用户禁用。
- `retired`：保留历史记录但不再允许启用。

实现可以继续使用表名 `rules`，通过 `ALTER TABLE` 添加字段；上面的 `rules_v1` 仅表达目标结构。

### changes

现有 `changes` 以 `(rule_id, real_path)` 为主键，不能表达 rename 目标和目录树语义。vNext 采用独立 `change_id`，并把源路径、目标路径和对象类型分开：

```sql
CREATE TABLE changes_v1 (
  change_id TEXT PRIMARY KEY,
  rule_id TEXT NOT NULL,
  sequence INTEGER NOT NULL,
  op TEXT NOT NULL,
  item_type TEXT NOT NULL,
  source_real_path TEXT NOT NULL,
  target_real_path TEXT NOT NULL,
  source_shadow_path TEXT NOT NULL,
  target_shadow_path TEXT NOT NULL,
  original_exists INTEGER NOT NULL,
  original_size INTEGER NOT NULL,
  original_last_write_time TEXT NOT NULL,
  current_size INTEGER NOT NULL,
  current_last_write_time TEXT NOT NULL,
  status TEXT NOT NULL,
  last_error TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  FOREIGN KEY(rule_id) REFERENCES rules(id) ON DELETE CASCADE
);

CREATE UNIQUE INDEX changes_rule_sequence_idx ON changes_v1(rule_id, sequence);
CREATE INDEX changes_rule_status_idx ON changes_v1(rule_id, status);
CREATE INDEX changes_source_idx ON changes_v1(rule_id, source_real_path);
CREATE INDEX changes_target_idx ON changes_v1(rule_id, target_real_path);
```

`op` 允许值：

- `create`：新建文件或目录。`target_real_path` 为最终真实路径，`source_real_path` 为空字符串。
- `modify`：修改已有文件。源和目标路径相同。
- `delete`：删除文件或目录。`source_real_path` 为被删除路径，`target_real_path` 为空字符串。
- `rename`：同一 rule 内 rename/move。`source_real_path` 是旧路径，`target_real_path` 是新路径。

`item_type` 允许值：

- `file`
- `directory`

`status` 允许值：

- `pending`：等待 commit 或 discard。
- `committing`：本次 commit 已开始处理此 change。
- `committed`：已写回真实 source，等待最终清理或日志归档。
- `discarding`：本次 discard 已开始处理此 change。
- `discarded`：shadow 和相关元数据已清理。
- `failed`：操作失败，`last_error` 记录失败原因，数据保留以便重试。

服务恢复时如果看到 `committing` 或 `discarding`，必须按保守恢复处理：重新扫描真实路径、shadow 路径和 commit log，能证明完成的标记为 `committed` 或 `discarded`，不能证明完成的标记为 `failed` 并保留 shadow 数据。

### commit_logs

现有 `commits` 可继续保留，但 vNext 需要可追踪每条 change 的处理结果：

```sql
CREATE TABLE commit_logs_v1 (
  commit_id TEXT PRIMARY KEY,
  rule_id TEXT NOT NULL,
  action TEXT NOT NULL,
  status TEXT NOT NULL,
  started_at TEXT NOT NULL,
  finished_at TEXT NOT NULL,
  backup_root TEXT NOT NULL,
  error TEXT NOT NULL,
  FOREIGN KEY(rule_id) REFERENCES rules(id)
);

CREATE TABLE commit_log_items_v1 (
  commit_id TEXT NOT NULL,
  change_id TEXT NOT NULL,
  status TEXT NOT NULL,
  backup_path TEXT NOT NULL,
  error TEXT NOT NULL,
  PRIMARY KEY(commit_id, change_id),
  FOREIGN KEY(commit_id) REFERENCES commit_logs_v1(commit_id),
  FOREIGN KEY(change_id) REFERENCES changes_v1(change_id)
);
```

`action` 允许 `commit` 或 `discard`。`status` 允许 `running`、`done`、`failed`。失败时不得删除 pending/failed change，也不得删除 shadow 数据。

### locks

占用检测结果不应混入 `changes`。vNext 可增加短生命周期表：

```sql
CREATE TABLE operation_locks_v1 (
  lock_id TEXT PRIMARY KEY,
  rule_id TEXT NOT NULL,
  action TEXT NOT NULL,
  path TEXT NOT NULL,
  process_id INTEGER NOT NULL,
  process_name TEXT NOT NULL,
  status TEXT NOT NULL,
  created_at TEXT NOT NULL,
  error TEXT NOT NULL,
  FOREIGN KEY(rule_id) REFERENCES rules(id)
);
```

该表只记录一次 commit/discard 前的占用诊断和确认关闭结果。成功完成后按 rule 清理；失败时保留，便于 CLI 输出上一次失败原因。

## 旧数据库迁移策略

现有 MVP 表：

- `rules(id, name, enabled, source, store)`
- `changes(rule_id, real_path, shadow_path, state, original_exists, original_size, original_last_write_time, current_size, last_write_time)`
- `commits(id, rule_id, start_time, status, operations, backup_path, error)`

迁移到 vNext 时：

1. 创建 `schema_info`。
2. 给 `rules` 增加 `created_at`、`updated_at`、`state` 字段；旧行使用当前时间，`enabled=1` 映射为 `active`，`enabled=0` 映射为 `disabled`。
3. 创建 `changes_v1`，把旧 `changes` 复制进去：
   - `state='created'` 映射为 `op='create'`、`item_type='file'`。
   - `state='modified'` 映射为 `op='modify'`、`item_type='file'`。
   - `state='deleted'` 和 `state='tombstone'` 映射为 `op='delete'`、`item_type='file'`。
   - `source_real_path=real_path`；`target_real_path` 对 create/modify 使用 `real_path`，对 delete 使用空字符串。
   - `source_shadow_path=shadow_path`；`target_shadow_path=shadow_path`。
   - `status='pending'`。
4. 创建 `commit_logs_v1`，从旧 `commits` 复制摘要。旧 `operations` 保留为日志兼容字段或写入 `error` 附加信息，不反向生成精确 item log。
5. 迁移完成前不删除旧表。实现稳定后可把旧表重命名为备份表，例如 `changes_mvp_backup`。

现有单规则 id `default` 必须保持不变，已有 source、store 和 change 记录必须可被 vNext CLI 查询和 commit/discard。

## 目录变更语义

目录创建：

- 记录 `op='create'`、`item_type='directory'`。
- shadow 中创建目录，真实 source 在 commit 前不创建。
- commit 时先创建父目录，再创建目标目录，然后处理子项。

目录删除：

- 记录 `op='delete'`、`item_type='directory'`。
- 枚举时隐藏该目录及其子树。
- commit 时自底向上删除真实目录，删除前对将被删除的真实文件和目录元数据做备份。
- discard 只删除对应 shadow 目录视图和 pending change，不修改真实目录。

目录 rename：

- 记录 `op='rename'`、`item_type='directory'`，同时保存源和目标路径。
- shadow 层按目标路径物化目录树。
- 枚举时隐藏源子树，显示目标子树。
- commit 时先确认目标不存在，再按目录树顺序移动或复制后删除源；失败时保留 `failed` 状态和 shadow 数据。

## 文件 Rename 语义

文件 rename/move 只允许同一 rule 内、同一卷内、目标不存在时执行。元数据记录：

- `op='rename'`
- `item_type='file'`
- `source_real_path` 为旧路径
- `target_real_path` 为新路径
- `source_shadow_path` 为旧 shadow 路径，可能为空
- `target_shadow_path` 为新 shadow 路径

如果源文件此前已有 `create` 或 `modify` change，rename 应合并成一条可提交的最终状态，或者把前置 change 标为 superseded。vNext 第一阶段建议采用合并策略，避免 commit 顺序复杂化：

- created 文件 rename：仍为 `create`，只更新 target。
- modified 文件 rename：变为 `rename`，target shadow 保留修改后的内容。
- tombstone 后 rename：拒绝。

## Commit/Discard 可重试状态

commit 必须以事务更新元数据状态，但文件系统操作不能放进 SQLite 事务里长期持锁。推荐流程：

1. 按 rule 暂停驱动规则，记录 `commit_logs_v1(status='running')`。
2. 将本次 change 从 `pending` 或 `failed` 标记为 `committing`。
3. 对每条 change 执行预检、备份和写回，并写 `commit_log_items_v1`。
4. 单条失败时标记该 change 为 `failed`，写 `last_error`，整个 commit log 标记 `failed`。
5. 全部成功后删除或归档 change，清理 shadow，commit log 标记 `done`。
6. 恢复规则状态并同步驱动缓存。

discard 使用同样的状态机，但不修改真实 source。任何失败都必须保留 change 和 shadow 数据，CLI 后续可再次执行 discard。

## vNext 边界

vNext 仍不支持：

- 排除路径或 per-rule include/exclude pattern。
- 注册表虚拟化。
- 完整系统盘保护或系统根目录覆盖。
- 跨 rule、跨卷或目标已存在的 rename/move。
- 硬链接、ADS、完整 reparse point 语义和完整 ACL 继承。
- 把 PathOverlay 宣称为安全沙箱。

