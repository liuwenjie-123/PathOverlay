# Repository Guidelines for Codex

本文件面向 Codex 和其他自动化开发代理。普通读者、新贡献者和操作者应优先阅读 `README.md`；测试机、驱动 E2E 和清理细节见 `docs/Testing.md`；当前任务状态以 `task.json` 为准，历史完成任务按 `task.json` 的 `archive` 字段读取归档。

## 文档入口

- `README.md`：项目概览、MVP 能力边界、环境要求、构建、测试、安装和基础 CLI 用法。
- `docs/Testing.md`：用户态测试、服务集成测试、驱动 E2E、test-signing 和清理流程。
- `PathOverlay_Design.md`：原始设计说明。
- `docs/Design_Review_and_Revisions.md`：设计修订和 MVP 边界。
- `docs/MVP_Development_Plan.md`：MVP 开发计划。
- `task.json`：当前任务列表、状态、交付物、验收标准、notes 引用和历史归档引用。
- `docs/task-notes.md`：活跃和近期任务的长 notes；`task.json` 只保留 `notes_ref`。
- `docs/task-archive.json`：已完成历史任务详情归档，当前归档范围见 `task.json` 的 `archive` 字段。
- `docs/task-notes-archive.md`：已完成历史任务 notes 归档。

## Project Structure

```text
driver/   Windows minifilter driver code
service/  user-mode service, metadata, commit/discard logic
cli/      command-line interface
common/   shared path, metadata, protocol, and overlay logic
docs/     design notes and operational documentation
tests/    unit and integration tests
scripts/  build, test, packaging, and cleanup scripts
```

Keep driver, service, CLI, and shared common logic separated. Put shared protocol or path-handling code in `common/` unless a narrower component owns it.

## Codex Workflow

1. 回答和最终总结使用中文。
2. 每次回复最后输出 `xzx`。
3. 开始实现任务前，先查看 `task.json`。如果任务不存在，先补充任务；如果任务存在且未开始，先标记为 `in_progress`。
4. 只有在该任务的 `test_criteria` 全部验证通过后，才能把状态改为 `done`。
5. 每次任务完成并把状态更新为 `done` 后，应提交一次 git commit；提交时只包含本任务相关改动，不要混入无关或用户已有改动。
6. 如果缺少 WDK、管理员权限、test-signing 环境或设计决策未确定导致无法继续，将任务标记为 `blocked`，并在 `docs/task-notes.md` 记录阻塞原因，同时在任务中保留或新增 `notes_ref`。
7. 不要回退用户已有改动。修改前先读取相关文件，按现有结构小范围变更。
8. 不要把长篇执行记录直接写入 `task.json` 的 `notes` 字段；新增或更新长 notes 时写入 `docs/task-notes.md`，并让任务使用 `notes_ref` 指向对应章节。
9. 构建、测试、安装和驱动操作命令不要在本文件重复维护，更新时应同步检查 `README.md` 和 `docs/Testing.md`。
10. 为降低上下文消耗，读取任务状态或 notes 时优先定位目标片段，例如使用 `Select-String`、`rg` 或等效命令读取目标任务和对应 notes 章节；避免为小范围状态更新完整读取 `task.json` 或 `docs/task-notes.md`。需要历史任务时再读取 `docs/task-archive.json` 或 `docs/task-notes-archive.md`。

## Coding Style

Use clear domain names from the design: `RealPath`, `RedirectPath`, `CopyPath`, `Box`, `Rule`, `Tombstone`, `Commit`, and `Discard`. Prefer explicit path helpers over ad hoc string manipulation.

For C/C++ code, use four-space indentation, descriptive function names, and defensive error handling. For scripts, use PowerShell approved verbs where practical.

## Testing Expectations

Run the narrowest useful validation for the change. Documentation-only changes should at least validate `task.json` when it was edited and check that documented script names match the repository.

For code changes, prefer:

```powershell
.\scripts\build.ps1
.\scripts\test.ps1
```

For driver E2E packaging or VM validation, follow `docs/Testing.md`.

## Security Notes

Filesystem redirection is high risk. Do not default to whole-system drive overlays. Preserve documented exclusions, rollback behavior, metadata compatibility notes, and warnings around operations that can modify or delete real files.
