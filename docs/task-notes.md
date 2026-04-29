# Task Notes

本文件仅保存活跃和近期任务 notes。历史 T004-T044 notes 已归档到 `docs/task-notes-archive.md`；历史任务详情见 `docs/task-archive.json`。`task.json` 保留当前任务状态和归档引用。

<a id="T045"></a>

## T045 - 设计 reparse point passthrough 策略

2026-04-29：已明确 source 内部 symlink、junction、mount point 等 reparse subtree 的首版策略：source 根本身仍作为 reparse point 被 rule add 拒绝；source 内 reparse subtree 不由 PathOverlay 跟随、接管或递归处理，访问这些路径时完全直通 Windows 默认行为，因此写入 link target 不受 commit/discard 隔离保护。README 和 `docs/Design_Review_and_Revisions.md` 已补充该限制和风险边界。验证通过：`task.json` JSON 解析。

<a id="T046"></a>

## T046 - 实现服务层 reparse 防递归与 doctor 提示

2026-04-29：已在 common 增加 reparse 检测辅助函数，服务层和用户态测试复用同一判断。`PrepareDirectoryView` 会跳过 source 内 reparse child；rename 懒物化、copy-on-write、delete、rename 记录遇到 reparse subtree 会返回 `reparse passthrough path is outside overlay scope`，避免写入 shadow 或 metadata；递归复制、backup 和 commit 写回使用跳过 reparse entry 的目录复制路径，避免进入 junction/symlink target。`doctor` 新增 `WARN reparse passthrough`，只读列出 source 内 reparse entry。验证通过：`scripts/build.ps1`、`scripts/test.ps1`、SkipDriver 服务集成验证 `doctor` 对 source 内 junction 输出 `reparse passthrough`。

<a id="T047"></a>

## T047 - 实现驱动 reparse subtree 完全直通

2026-04-29：协议新增 `PathOverlayPathStatePassthrough`。服务端 `QueryPath` 在发现请求路径位于 source 内 reparse subtree 时返回 passthrough；驱动 `PreCreate`、QueryOpen/NetworkQueryOpen、delete 和 rename 链路收到 passthrough 后不执行 COW、不重定向、不记录 tombstone 或 rename，让 Windows 默认处理真实路径。测试机 E2E 脚本新增 source 内 junction 场景，验证 junction 写入直通 target、shadow 不创建 junction subtree、changes 不包含 junction 路径、doctor 输出 passthrough。验证通过：`scripts/build.ps1`、`scripts/build.ps1 -Configuration Release`、`scripts/package-test-machine.ps1 -Configuration Release`、`scripts/Test-PathOverlay.ps1` 语法检查。当前启动项未显示 `testsigning Yes`，本机会话无法加载测试签名驱动执行 `test-machine-package/Run-PathOverlay-Test.cmd`，真实驱动 E2E 需在启用 test-signing 的测试机复跑。

2026-04-29 复查测试机日志 `PathOverlay-Test-20260429-155428.log`：新增 junction 场景失败于 `junction subtree is not copied to shadow`。日志显示 `mklink /J` 创建 junction 时，驱动先对 `source\junction-out` 执行 copy-on-write，生成 shadow 目录；随后写入 `junction-out\outside.txt` 虽然直通到了 target，但 shadow 已存在导致断言失败。已修复驱动 `PreCreate`：带 `FILE_OPEN_REPARSE_POINT` 的打开/创建请求直接直通，不进入 COW 或目录视图逻辑，使 junction/symlink 自身创建不被 overlay 接管。真实驱动 E2E 需重新打包后在测试机复跑。

2026-04-29 复查测试机日志 `PathOverlay-Test-20260429-155856.log`：失败点变为旧目录 tombstone 场景 `tombstoned directory is hidden`，不是前一次 junction shadow 断言。服务日志显示 `delete-dir state=tombstone`，说明服务端 metadata 已记录 tombstone，但驱动此前对所有 `FILE_OPEN_REPARSE_POINT` create 直接直通，导致带该标志的存在性查询绕过 tombstone。已收窄修复：`PreCreate` 先执行 `QueryPath` 并优先处理 tombstone/passthrough；只有非 tombstone 且非服务端 passthrough 的 reparse-point open 才直通，避免破坏 tombstone 隐藏语义。

2026-04-29 复查测试机日志 `PathOverlay-Test-20260429-160943.log`：失败点为文件 rename 场景 `renamed target path is visible`。服务已成功记录 `before.txt -> after.txt`，且 QueryPath 对 target 返回 `renamed-shadow`，但更早日志显示驱动对 rule source 根目录执行了 copy-on-write，生成了 source 根目录级 shadow，干扰后续目录视图和 renamed target 可见性。已修复驱动 `PreCreate`：当命中路径就是 rule source 根本身时，即使 create 参数带写意图，也不执行 COW；source 根仍按目录视图逻辑处理。

2026-04-29 复查测试机日志 `PathOverlay-Test-20260429-161612.log`：失败点仍为文件 rename 场景 `renamed target path is visible`，但已不是 source 根 COW 问题。服务日志显示 `record-rename ... ok`，且随后 `QueryPath(after.txt)` 返回 `renamed-shadow=...after.txt`，说明服务 metadata 和 shadow materialization 已正确。剩余问题在驱动存在性查询路径：`Test-Path` 可通过 `IRP_MJ_NETWORK_QUERY_OPEN` 做快速属性查询，原逻辑只是 disallow fast I/O 并期待后续 create 重试，导致 renamed target 的 shadow 文件没有被稳定呈现。已修复驱动 `PreNetworkQueryOpen`：对 tombstone 直接返回 not found；对已存在的 shadow/renamed-shadow 直接查询 shadow 文件的 `FILE_NETWORK_OPEN_INFORMATION` 并完成请求。

<a id="T048"></a>

## T048 - 更新 reparse 兼容性测试与发布文档

2026-04-29：用户态测试新增 source-child directory symlink 场景，覆盖 reparse subtree 检测、目录视图不复制到 shadow、copy-on-write/delete/rename 不接管、metadata 不记录该 subtree；测试机脚本新增 source-child junction passthrough 场景。`docs/Testing.md` 和 `docs/Release_Checklist.md` 已加入 reparse passthrough 验收项。验证通过：`task.json` JSON 解析、`git diff --check`、`scripts/build.ps1`、`scripts/test.ps1`、`scripts/build.ps1 -Configuration Release`、`scripts/test.ps1 -Configuration Release`、`scripts/package-test-machine.ps1 -Configuration Release`。

<a id="T049"></a>

## T049 - 修复 162232 测试日志中的 tombstone 查询回归

2026-04-29 复查测试机日志 `PathOverlay-Test-20260429-162232.log`：rename target 可见性已继续前进，当前失败点回到 T023/T028 目录 tombstone 场景 `tombstoned directory is hidden`。服务日志显示删除流程最终记录 `record-delete ... delete-dir ok`，后续 `QueryPath(delete-dir)` 返回 `state=tombstone`，说明服务端 metadata 正确；但 `Test-Path` 仍看到目录。原因是上一轮只让 `IRP_MJ_NETWORK_QUERY_OPEN` 能直接从 shadow 或 tombstone 完成，目录存在性查询仍可能走 `IRP_MJ_QUERY_OPEN`，旧逻辑只是返回 `FLT_PREOP_DISALLOW_FSFILTER_IO`，没有把 tombstone 的 not found 结果直接交回调用方。已修复：抽出通用 shadow 属性查询 helper，`PreNetworkQueryOpen` 和 `PreQueryOpen` 都对 tombstone 直接完成为 `STATUS_OBJECT_NAME_NOT_FOUND`，对 shadow/renamed-shadow 直接查询 shadow 文件属性并完成请求。

验证通过：`scripts/build.ps1`、`scripts/test.ps1`、`scripts/build.ps1 -Configuration Release`、`scripts/test.ps1 -Configuration Release`、`scripts/package-test-machine.ps1 -Configuration Release`。最新测试机包已生成到 `test-machine-package`。

<a id="T050"></a>

## T050 - 归档历史任务与 notes 降低上下文读取成本

2026-04-29：`task.json` 约 43KB、`docs/task-notes.md` 约 63KB，绝对体量不大，但会让代理在每次查看任务时容易误读全量历史并占用上下文。已将 T001-T044 的任务详情归档到 `docs/task-archive.json`，将 T004-T044 的长 notes 归档到 `docs/task-notes-archive.md`；活跃 `task.json` 仅保留 T045 起的当前/近期任务，并通过 `archive.archived_task_ranges` 指向归档文件。`docs/task-notes.md` 也只保留 T045 起的 notes。`AGENTS.md` 已补充归档入口，后续只有需要历史任务时才读取归档文件。

归档后活跃 `task.json` 约 6KB，活跃 `docs/task-notes.md` 约 6KB。验证通过：`task.json` 和 `docs/task-archive.json` 均可被 JSON 解析；归档文件保留 T001-T044 的任务详情和 T004-T044 的 notes。
