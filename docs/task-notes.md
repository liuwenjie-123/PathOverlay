# Task Notes

本文件保存任务执行过程中的长 notes，避免 `task.json` 过大。`task.json` 仅保留任务状态、验收标准和 `notes_ref` 引用。

<a id="T004"></a>

## T004 - 实现 SQLite 元数据存储

已通过用户态测试。测试运行时使用 SQLite 官方 Windows x64 sqlite3.dll，放置在 x64/Debug 输出目录。

<a id="T008"></a>

## T008 - 实现 minifilter 驱动骨架

2026-04-24 复查：WDK/VS 构建环境已可用，VS 2022 的 x64/Win32 PlatformToolsets 已出现 WindowsKernelModeDriver10.0，Windows Kits 10.0.26100.0 的 km include/lib、fltKernel.h、WindowsDriver.Common.targets 均存在，MSVC Spectre-mitigated libraries 已安装。已将驱动项目固定到 WindowsTargetPlatformVersion 10.0.26100.0，并关闭构建期自动签名；PathOverlayFlt.vcxproj 和 scripts/build.ps1 均可成功生成 PathOverlayFlt.sys。已新增 scripts/package-test-machine.ps1 和 test-machine-package/，可生成签名后的测试机包。已实现 PathOverlaySvc 通过 FilterConnectCommunicationPort 连接驱动通信端口，并新增 pathoverlay driver status 验证命令。虚拟机日志 test-machine-package/PathOverlay-Test-20260424-154645.log 显示：测试签名证书导入成功，PathOverlayFlt 可加载并在清理阶段卸载/删除，驱动通信端口可连接，PathOverlaySvc 可安装启动，pathoverlay rule show 通过命名管道返回 OK no rules，pathoverlay driver status 返回 OK driver connected，无规则时基础文件写入/删除直通正常，清理成功。

<a id="T009"></a>

## T009 - 实现驱动规则匹配与打开重定向

2026-04-24：已新增 common/path_overlay_protocol.h，服务启动和 rule add 后通过 FilterSendMessage 将默认规则同步到驱动；服务将 DOS 路径转换为 NT device path，并携带服务进程 PID。驱动新增单规则缓存、source/store 路径匹配、store 排除、服务进程排除，以及 IRP_MJ_CREATE 中的基础重定向：纯读请求仅在 shadow 已存在时重定向，写/创建意图请求重定向到 shadow 路径。Debug 全量构建通过，scripts/test.ps1 用户态测试通过。尚未在测试机验证真实驱动加载后的 source 外路径、禁用规则、shadow 读优先、store 排除和服务进程排除行为，因此任务保持 in_progress。2026-04-24 复查 test-machine-package/PathOverlay-Test-20260424-172542.log：已通过真实测试机自动化间接验证 source 外路径不受影响（无规则 passthrough）和 shadow 读优先/copy-on-write 行为。禁用规则不生效、store 目录不会被递归重定向、服务进程自身操作不会被重定向目前有代码支持或间接现象，但缺少显式 E2E 断言，因此 T009 继续保持 in_progress。后续风险：changes 当前会出现 modified <source目录>，建议在 T012/commit 前处理，避免目录记录影响 commit。2026-04-24 真实测试机自动化验证通过：test-machine-package/PathOverlay-Test-20260424-174426.log 明确执行并通过无规则 passthrough、启用规则后的 shadow 读优先/copy-on-write、disabled rule passthrough、store path passthrough、service process redirection exclusion；T009 验收项已满足。

<a id="T010"></a>

## T010 - 实现 copy-on-write 与删除拦截

2026-04-24：已实现驱动到服务的请求协议和服务侧 FilterGetMessage 监听线程。驱动在命中规则的 IRP_MJ_CREATE 中先查询 tombstone，若路径已删除则返回 STATUS_OBJECT_NAME_NOT_FOUND；写/创建意图打开前通过服务调用 PrepareCopyOnWrite，服务创建 shadow parent、复制真实文件到 shadow 或记录 created 后，驱动再将打开重定向到 shadow。IRP_MJ_SET_INFORMATION 已拦截 FileDispositionInformation/FileDispositionInformationEx 并通过服务记录 tombstone，同时完成请求以避免真实文件被删除；rename 相关 FileInformationClass 在 MVP 中返回 STATUS_NOT_SUPPORTED。2026-04-24 测试机日志 test-machine-package/PathOverlay-Test-20260424-164343.log 显示 rename 不支持符合预期，但删除后 Test-Path 仍可见 tombstone 路径；原因是 IRP_MJ_CREATE 将 DELETE 访问权限误判为写意图，先记录 tombstone 后又执行 PrepareCopyOnWrite，将 change 状态覆盖为 modified。已修正 create 阶段不再把 DELETE 权限当作写意图，也不在 create 阶段记录 tombstone，删除记录统一由 SetInformation 处理。2026-04-24 测试机日志 test-machine-package/PathOverlay-Test-20260424-164952.log 显示服务启动命令返回成功后 CLI IPC 立即不可用，日志缺少服务状态和 PathOverlaySvc.log，暂无法判断是服务退出、管道创建失败还是启动时序问题。已增强 PathOverlaySvc start：等待 SCM 进入 running 且命名管道可用；已增强测试脚本：CLI IPC 失败时输出 sc queryex、PathOverlaySvc.log 和近期 Service Control Manager 事件。2026-04-24 测试机日志 test-machine-package/PathOverlay-Test-20260424-165400.log 显示 IPC 恢复正常，但删除后 Test-Path 仍可见 tombstone 路径；已补充 IRP_MJ_NETWORK_QUERY_OPEN 回调，对 tombstone 路径返回 STATUS_OBJECT_NAME_NOT_FOUND，以覆盖 GetFileAttributes/Test-Path 这类可能绕过 IRP_MJ_CREATE 的存在性查询；同时服务记录 query/copy-on-write/record-delete 请求日志，测试脚本在 tombstone 可见失败时输出 changes 和服务诊断。2026-04-24 测试机日志 test-machine-package/PathOverlay-Test-20260424-165756.log 显示删除文件被记录为 modified，且无 record-delete 日志；原因是 Remove-Item 打开文件时携带 FILE_WRITE_ATTRIBUTES，驱动误判为写意图并先重定向到 shadow，后续删除发生在 store 内被排除。已将 create 写意图收窄到 FILE_WRITE_DATA/FILE_APPEND_DATA/FILE_WRITE_EA 或创建/覆盖 disposition，并补充 FileDispositionInformationEx 的 FILE_DISPOSITION_DELETE 标志检查。2026-04-24 测试机日志 test-machine-package/PathOverlay-Test-20260424-171018.log 显示 record-delete 已成功且 changes 输出包含 tombstone，服务查询也返回 state=tombstone，但 Test-Path 仍报告可见；已在驱动所有主动完成的失败/重解析路径补充 FltSetCallbackDataDirty，确保 STATUS_OBJECT_NAME_NOT_FOUND/STATUS_NOT_SUPPORTED/STATUS_REPARSE 等 IoStatus 修改向下传播；测试脚本在 tombstone 可见失败时增加 File.Exists、OpenRead、Get-Item 和父目录枚举诊断。2026-04-24：已新增 IRP_MJ_QUERY_OPEN 回调；tombstone 命中时 IRP_MJ_NETWORK_QUERY_OPEN 返回 FLT_PREOP_DISALLOW_FASTIO，IRP_MJ_QUERY_OPEN 返回 FLT_PREOP_DISALLOW_FSFILTER_IO，使存在性查询回退到 IRP_MJ_CREATE 的 STATUS_OBJECT_NAME_NOT_FOUND 权威判断；rename/move 拒绝已移到 source/store 路径过滤之后，source 外 rename 不再被规则误拦截；测试包脚本已将 Test-Path、File.Exists、Get-Item、OpenRead 都作为 tombstone 不可见性断言。Debug 全量构建通过，scripts/test.ps1 用户态测试通过，test-machine-package 已重新生成并签名。2026-04-24 真实测试机自动化验证通过：test-machine-package/PathOverlay-Test-20260424-172542.log 显示 copy-on-write 生效，commit 前真实文件不变，删除 source 内真实文件只生成 tombstone，Test-Path/Get-Item/OpenRead 对 tombstone 均表现为不存在，rename/move 在 MVP 中返回不支持，discard 后完成清理；T010 验收项已满足。

<a id="T011"></a>

## T011 - 实现目录枚举合并

2026-04-24：已实现目录枚举 MVP 路径。协议新增 PrepareDirectoryView；服务侧准备 shadow 目录视图，将真实目录项同步到 shadow、跳过 tombstone 且不覆盖已有 shadow 项；驱动在目录打开时请求服务准备目录视图并重定向到 shadow 目录。已补充用户态 PrepareDirectoryView 测试和测试机目录枚举断言。已通过 scripts/test.ps1、Debug build.ps1、Release package-test-machine.ps1。测试机日志 test-machine-package/PathOverlay-Test-20260424-175043.log 显示 T011 未通过：目录枚举仍暴露 tombstone 文件 delete-me.txt。原因判断为真实目录项可能先被目录视图物化到 shadow，后续 source 删除请求落到 shadow/store 路径时未记录 tombstone。已修复：PrepareDirectoryView 会清理 tombstone 对应的 stale shadow 项；驱动在删除请求命中规则 shadow/store 路径时反推 source 路径并调用 RecordDelete。修复后 scripts/test.ps1、Debug build.ps1、Release package-test-machine.ps1 均通过，test-machine-package 已重新生成并签名。2026-04-24 真实测试机自动化验证通过：test-machine-package/PathOverlay-Test-20260424-175531.log 显示目录枚举测试通过，tombstone 文件 delete-me.txt 对 Get-Item/OpenRead 表现为不存在，changes 输出包含 created.txt、existing.txt modified、delete-me.txt tombstone，最终 PathOverlay test package passed all automated checks；T011 验收项已满足。

<a id="T012"></a>

## T012 - 实现端到端 commit 与 discard

2026-04-24：已实现服务侧 commit/discard 期间暂停默认规则并清空驱动规则缓存，操作结束后按原状态恢复规则；CLI commit 使用唯一 commit id，避免重复提交日志主键冲突。公共库 commit 增加提交前预检：检测真实文件占用、shadow 缺失、真实文件与记录的 original_* 基线不一致等冲突，失败时记录 failed commit 日志并保留 changes/shadow 元数据以便检查或重试；成功路径在覆盖/删除前备份真实文件，清理 shadow drive 和 changes 后才记录 done。修正 changes upsert 不再覆盖 original_* 基线，避免多次写入掩盖外部冲突；修正路径规范化在目标文件尚不存在时展开最近存在父目录，保证 created 文件和 source/store 包含判断稳定。已补充用户态测试覆盖冲突失败、占用失败、失败后元数据保留、成功 commit 备份和清理；已补充测试机脚本的端到端 commit 验证，包括规则恢复、真实写回、备份、shadow 清理和 changes 清理。2026-04-24 复查 test-machine-package/PathOverlay-Test-20260424-180645.log：T012 失败是旧 PathOverlayRule-* 目录清理时规则仍启用，驱动重新记录旧 source tombstone，随后 rule add 替换 default 规则但继承旧 changes，commit 预检旧目录时 error=5。已修正 rule add 在当前规则存在 pending changes 时拒绝切换，切换无 pending changes 的规则前清理旧 shadow drive；测试脚本清理前禁用规则，并验证 pending changes 时 rule add 失败。公共库不再把目录 copy-on-write 记录为 modified，目录删除在 MVP 中显式失败，避免目录 tombstone 进入 commit。已通过 Debug build、Debug scripts/test.ps1、Release package-test-machine.ps1、Release scripts/test.ps1。2026-04-24 复查 test-machine-package/PathOverlay-Test-20260424-182607.log：pending changes 时 rule add alternate-source 已正确返回 ERROR，active source 也保持原规则；失败原因是测试脚本用 %TEMP% 产生的 8.3 短路径 C:\Users\ADMINI~1 与 rule show 返回的长路径 C:\Users\Administrator 做字符串匹配。已修正 test-machine-package/Test-PathOverlay.ps1：新增 canonical path 展开和 source 解析，rule replacement 断言改为比较 rule show 的 canonical source，并让 shadow/backup 路径计算共用规范化逻辑。已通过 PowerShell 解析、Debug scripts/test.ps1、Release package-test-machine.ps1、Release scripts/test.ps1。2026-04-24 真实测试机自动化验证通过：test-machine-package/PathOverlay-Test-20260424-183211.log 显示 pending changes 时 rule add alternate-source 被拒绝且 active source 保持原规则，commit 返回 OK commit completed，commit 后规则恢复 enabled=true，禁用规则后真实路径验证和 changes 清理通过，最终 PathOverlay test package passed all automated checks；T012 验收项已满足。

<a id="T013"></a>

## T013 - 补充自动化与手工验证套件

2026-04-24：已新增 docs/Testing.md，说明用户态测试、服务集成覆盖、驱动 E2E 流程、管理员权限、WDK/test-signing 和清理方式。已新增 scripts/cleanup-test-data.ps1，可清理本地临时测试数据，并可在管理员 PowerShell 下清理 ProgramData、卸载服务和驱动。scripts/test.ps1 运行前后会调用清理脚本，确保集成测试使用临时目录且不保留测试数据。scripts/package-test-machine.ps1 会把 cleanup-test-data.ps1 打入 test-machine-package，test-machine-package/README.md 已补充创建、修改、删除、枚举、discard、commit 和手工清理说明。验证通过：scripts/test.ps1、scripts/package-test-machine.ps1 -Configuration Release、scripts/test.ps1 -Configuration Release。

<a id="T014"></a>

## T014 - 更新贡献者与运维文档

2026-04-27：已新增 README.md，面向普通读者和新贡献者说明项目概览、MVP 能力边界、环境要求、构建、测试、测试机驱动包、手工安装、CLI 基础用法、停止卸载和故障排查。已重写 AGENTS.md，面向 Codex/自动化代理保留任务跟踪规则，并明确优先参考 README.md、docs/Testing.md 和 task.json。验证通过：task.json JSON 解析正常，README 中引用的 scripts/build.ps1、scripts/test.ps1、scripts/package-test-machine.ps1、scripts/cleanup-test-data.ps1 和 docs/Testing.md 均存在，scripts/test.ps1 通过。

<a id="T015"></a>

## T015 - 新增编译目录一键安装启动脚本

2026-04-27：已新增 scripts/install-start.ps1 和 scripts/Install-Start-PathOverlay.cmd；scripts/build.ps1 会在构建后复制到 x64/<Configuration> 编译输出目录，scripts/package-test-machine.ps1 也会打入测试机包。脚本从自身目录定位 PathOverlayFlt.sys、PathOverlaySvc.exe、pathoverlay.exe、sqlite3.dll，支持非管理员自动提权，检查 test-signing，在需要时用 WDK signtool.exe 创建/使用测试证书签名驱动，安装/加载 PathOverlayFlt，安装/启动 PathOverlaySvc，并执行 rule show 和 driver status 验证。已新增 -ValidateOnly 用于不加载驱动的安全验证。验证通过：PowerShell 脚本语法检查、task.json JSON 解析、scripts/build.ps1、x64/Debug/install-start.ps1 -ValidateOnly、scripts/test.ps1。未在当前会话实际加载驱动或安装服务。

<a id="T016"></a>

## T016 - 补充覆盖层日常使用说明

2026-04-27：已在 README.md 新增“覆盖层日常使用”章节，说明 rule add、rule show、source/store、shadow 路径格式、changes、discard、commit，以及典型操作流程。验证通过：task.json JSON 解析正常，README 包含 T016 验收点。

<a id="T017"></a>

## T017 - 新增编译目录一键卸载脚本

2026-04-27：已新增 scripts/uninstall.ps1 和 scripts/Uninstall-PathOverlay.cmd；scripts/build.ps1 会在构建后复制到 x64/<Configuration> 编译输出目录，scripts/package-test-machine.ps1 也会打入测试机包。卸载脚本从自身目录定位 PathOverlaySvc.exe，支持非管理员自动提权，停止并卸载 PathOverlaySvc，卸载 PathOverlayFlt 并删除驱动服务，支持 -RemoveData 清理 %ProgramData%\PathOverlay，并提供 -ValidateOnly 无副作用验证模式。README 已补充一键卸载说明。验证通过：PowerShell 脚本语法检查、task.json JSON 解析、scripts/build.ps1、x64/Debug/uninstall.ps1 -ValidateOnly、scripts/test.ps1。未在当前会话实际卸载服务或驱动。

<a id="T018"></a>

## T018 - 拆分 task notes 到独立文件

2026-04-27：任务创建，准备将 task.json 内联 notes 拆分到 docs/task-notes.md。

2026-04-27：已将 task.json 中所有内联 notes 迁移到 docs/task-notes.md，并为每个迁移任务保留 notes_ref。已更新 AGENTS.md，要求后续长 notes 写入 docs/task-notes.md。验证通过：task.json JSON 解析正常，task.json 不再包含 notes 字段，所有 notes_ref 均有对应锚点。

<a id="T019"></a>

## T019 - 规划 vNext 元数据与兼容策略

2026-04-27：任务开始，目标是先形成 vNext 元数据与兼容策略设计，再进入多规则实现。

2026-04-27：已新增 docs/vNext_Metadata_and_Compatibility.md，定义 schema_info 版本表、rules/changes/commit_logs/operation_locks 的 vNext 目标结构、旧 MVP metadata.db 迁移策略、目录 create/delete/rename 和文件 rename 记录方式，以及 commit/discard 失败后的 pending/committing/failed 可重试状态。文档明确 vNext 仍不支持排除路径、注册表虚拟化和完整系统盘保护。验证通过：task.json JSON 解析正常，T019 notes_ref 锚点存在，文档覆盖全部 T019 验收点。

<a id="T020"></a>

## T020 - 实现多规则模型与规则校验

2026-04-27：任务开始，目标是扩展公共规则模型和服务侧 rule add 校验，支持多条互不重叠规则、自动 rule id、默认独立 store 和自定义 store。

2026-04-27：已在 common 增加 GenerateRuleId、DefaultStoreRootForRule 和 ValidateOverlayRuleSet。服务侧 rule add 改为自动生成 rule id，未指定 store 时使用每条规则独立 store，指定 --store 时持久化自定义 store，并在保存前用已有规则做 source/source 与 source/store 包含校验。用户态测试覆盖多条非重叠规则保存/查询/启用禁用、相同 source 和父子 source 拒绝、跨规则 source/store 包含拒绝、默认独立 store 和自定义 store 持久化。验证通过：scripts/build.ps1、scripts/test.ps1。

<a id="T021"></a>

## T021 - 扩展 CLI 与服务规则管理

2026-04-27：已更新 pathoverlay CLI，支持 rule add <source> [--store <path>]、rule enable --rule <id>、rule disable --rule <id> 和多规则 rule show。PathOverlaySvc IPC 请求处理已按自动生成 rule id 保存规则，rule add 返回 id/source/store，rule show 列出所有规则 id、enabled、source 和 store，rule enable/disable 不再接受隐式 default，必须指定有效 rule id；无效 rule id、无效 source/store 或规则重叠会返回 ERROR。规则元数据操作成功但驱动暂未连接时返回 OK 并附带 driver sync warning，避免规则管理被驱动加载状态阻塞。README 已更新为多规则语义。验证通过：task.json JSON 解析正常，scripts/build.ps1，scripts/test.ps1，CLI usage 和缺少 --rule 时的本地参数校验。

<a id="T022"></a>

## T022 - 扩展驱动多规则缓存与匹配

2026-04-27：已将驱动协议扩展为携带 rule id，并把驱动侧单规则缓存改为最多 16 条启用规则的缓存。服务同步驱动时会清空驱动缓存，再按当前启用规则逐条推送；同步前会复用多规则校验，若启用规则出现 source/source 或 source/store 重叠则拒绝同步并清空驱动缓存。驱动匹配时先排除所有 store 路径和服务进程，再按 source 唯一命中规则构造 shadow 路径；QueryPath、PrepareCopyOnWrite、PrepareDirectoryView 和 RecordDelete 请求都会携带 rule id，服务按 rule id 查询规则和 changes。验证通过：scripts/test.ps1、scripts/build.ps1。当前会话未在启用 test-signing 的测试机上加载驱动验证多规则真实重定向、禁用规则、store 排除和服务进程排除，因此 T022 暂标记为 blocked，等待驱动 E2E 环境验证后才能改为 done。

2026-04-27：已新增 scripts/Test-PathOverlay.ps1、scripts/Run-PathOverlay-Test.cmd 和 scripts/test-machine-package-README.md，并更新 scripts/package-test-machine.ps1。测试机脚本会输出逐项 [PASS]/[FAIL] 日志，覆盖 no-rule passthrough、两个启用规则分别写入各自 store、禁用规则直通真实路径、source 外路径不受影响、store 路径不递归重定向、服务进程写入排除、重叠规则 add 被拒绝且不影响已有驱动同步；失败时会输出 PathOverlaySvc.log、sc queryex 和近期 Service Control Manager 事件。已重新编译 Release 测试机包并签名，输出目录为 test-machine-package；包内无旧 .log，Test-PathOverlay.ps1、Run-PathOverlay-Test.cmd 和 README.md 与 scripts 源文件哈希一致。验证通过：PowerShell 语法检查、scripts/package-test-machine.ps1 -Configuration Release、scripts/test.ps1 -Configuration Release、git diff --check。仍需在测试机运行 Run-PathOverlay-Test.cmd 并确认日志出现 “PathOverlay test package passed all automated checks.” 后，T022 才能从 blocked 改为 done。

2026-04-27：测试机在 Installing minifilter service 后远程断开并重启，判断为 fltmc load PathOverlayFlt 期间触发内核 bugcheck，而不是测试脚本主动重启。复查 T022 驱动改动发现回调函数在内核栈上保存完整 PATHOVERLAY_RULE_CACHE；16 条规则缓存约 35KB，超过常见内核栈预算，文件系统活动触发回调时可能栈溢出并导致系统重启。已修复：删除完整缓存快照，改为在 gRuleLock 下查找匹配规则，只把命中的单条 PATHOVERLAY_RULE_ENTRY 复制到栈上，并删除未使用的 PathOverlayRuleSnapshot。验证通过：scripts/build.ps1、scripts/test.ps1、scripts/package-test-machine.ps1 -Configuration Release、git diff --check；已重新生成并签名 test-machine-package，包内无旧 .log。

2026-04-27：复查测试机日志 test-machine-package/PathOverlay-Test-20260427-161611.log：驱动加载、服务启动、CLI IPC、driver status、no-rule passthrough、两个启用规则分别写入各自 store、禁用规则直通、source 外路径不受影响、store 排除和服务进程排除均通过。失败点为最后的 overlapping rule add 断言：脚本在 rule1 启用时创建 source-one\nested-overlap，目录创建被覆盖层重定向到 store，真实 source 下目录不存在，导致 rule add 返回 ERROR source path does not exist，而不是预期的重叠规则错误。已修正测试脚本：创建嵌套 overlap source 前临时禁用 rule1，创建真实目录后重新启用，再验证 rule add 被 overlap/contain 错误拒绝。验证通过：PowerShell 语法检查、scripts/package-test-machine.ps1 -Configuration Release。T022 继续保持 blocked，等待在测试机复跑通过。

2026-04-27：测试机复跑通过，结果为 “PathOverlay test package passed all automated checks.” T022 验收项已满足：source 外路径不受任何规则影响，多个启用规则分别重定向到各自 store，禁用规则不参与驱动匹配，store 目录不会递归重定向，服务进程自身操作不会被重定向，重叠规则被服务拒绝且不会同步到驱动。T022 状态改为 done。

<a id="T023"></a>

## T023 - 实现目录 create/delete 语义

2026-04-27：已扩展公共覆盖层操作：新建目录会在 shadow 中创建并记录 created，目录删除会记录 tombstone 并移除已有 shadow 目录项，目录视图刷新会隐藏 tombstone 目录及子项；commit 可创建真实目录、备份并删除真实目录树，discard 清理目录 shadow 和 metadata 且不修改真实目录。驱动侧调整目录 create/open 顺序：带写入或创建意图的目录打开走 copy-on-write 记录，新建目录不会直接落到真实 source；普通目录打开只在真实目录或已有 shadow 存在时准备合并视图，避免读打开不存在目录时制造 shadow。验证通过：scripts/build.ps1、scripts/test.ps1。

<a id="T024"></a>

## T024 - 实现文件 rename/move

2026-04-27：已实现文件 rename/move 的主体代码：metadata changes 增加 `target_path` 和 `renamed` 状态并支持旧库迁移；公共操作新增 `RecordRename`，同 rule 内文件 rename 会移动 shadow 文件、记录 source->target 元数据，created 文件 rename 会合并为 target created，目标已存在、跨 rule/跨卷或 tombstone 后 rename 会失败；commit 会备份并移除真实源文件、写入目标文件，discard 清理 shadow/metadata 且不修改真实 source。服务协议新增 `PathOverlayServiceCommandRecordRename` 和 target NT path，PathOverlaySvc 处理驱动 rename 请求并在 changes 输出 source->target。驱动侧 `IRP_MJ_SET_INFORMATION` 已从原先拒绝 rename 改为解析 `FileRenameInformation*` 目标路径，要求源和目标命中同一 rule，再调用服务记录 rename 并完成请求。测试机 E2E 脚本新增 T024 场景，覆盖源隐藏、目标可见、目录枚举、shadow 物化、目标已存在失败以及禁用规则后真实 source 未被提前修改。验证通过：`scripts/build.ps1`、`scripts/test.ps1`、`scripts/package-test-machine.ps1 -Configuration Release`，以及 `scripts/Test-PathOverlay.ps1` 语法检查。当前机器虽有管理员权限，但 BCD 当前项未启用 `testsigning`，本会话未直接加载测试签名驱动运行测试机 E2E；T024 暂标记 blocked，待在启用 test-signing 的测试机运行 `test-machine-package/Run-PathOverlay-Test.cmd` 并通过后再改为 done。

2026-04-27：测试机复跑通过，结果为 “PathOverlay test package passed all automated checks.” T024 文件 rename 场景已验证：同 rule 内 rename 后源路径隐藏、目标路径可见且内容可读，目录枚举隐藏源并显示目标，target shadow 已物化，rename 到已存在目标被拒绝，禁用规则后真实 source 仍保留原文件且真实目标未提前创建。结合用户态测试中 commit/discard、created rename 合并和目标冲突覆盖，T024 验收项已满足，状态改为 done。

<a id="T025"></a>

## T025 - 实现目录 rename/move

2026-04-27：已扩展 `OverlayOperations::RecordRename` 支持目录 rename/move：同 rule 内目录会先准备 source shadow 视图，再把整棵 shadow 目录树移动到 target shadow；目标已存在、跨 rule/跨卷、tombstone 后 rename、以及把目录移动到自身子树会保守失败。目录 rename 会记录 source->target 的 `renamed` 元数据，并删除 source 子树下已折叠进目录移动的子项 change，避免 commit 时重复处理；created 目录 rename 会合并为 target created。commit 已支持 renamed 目录递归写回真实 target 并 `remove_all` 真实 source，discard 继续只清理 shadow 和 metadata、不修改真实 source。测试补充了用户态目录 rename 场景：修改子文件后 rename 整个目录、target shadow 子文件可读、子项 metadata 被折叠、目标已存在失败、discard 保留真实 source、commit 写回 target 并删除 source。测试机 E2E 脚本新增 T025 目录 rename 场景，覆盖驱动拦截下源目录隐藏、目标目录可见、目标子文件可读、目录枚举、shadow 物化、目标已存在失败、禁用规则后真实 source 未提前修改。验证通过：`scripts/test.ps1`、`scripts/build.ps1`、`scripts/Test-PathOverlay.ps1` 语法检查、`scripts/package-test-machine.ps1 -Configuration Release`。当前 BCD 当前项未启用 `testsigning Yes`，本会话无法加载测试签名驱动运行测试机 E2E；T025 暂标记 blocked，待在启用 test-signing 的测试机运行 `test-machine-package/Run-PathOverlay-Test.cmd` 并通过后再改为 done。

2026-04-27：测试机复跑 T025 失败于目标目录可见但 `after-dir\child.txt` 不存在。原因是目录 rename 前只准备了浅层目录视图，遇到已存在但不完整的 source shadow 时没有递归补齐真实 source 子树；同时 QueryPath 只隐藏 renamed source 本身，没有隐藏 source 子路径。已修复：目录 rename 前递归复制真实 source 目录中 shadow 缺失的子项，保留已存在 shadow 修改；服务端 QueryPath 将 renamed 目录 source 子树视为隐藏；用户态和测试机脚本增加 nested 子文件断言。验证通过：`scripts/test.ps1`、`scripts/build.ps1`、`scripts/Test-PathOverlay.ps1` 语法检查、`scripts/package-test-machine.ps1 -Configuration Release`。T025 仍等待测试机用新包复跑。

2026-04-27：测试机再次复跑仍在 `after-dir\child.txt` 缺失处失败，说明仅在 rename 时递归物化 source shadow 还不足以覆盖驱动随后打开 target 子路径的时序。已新增 renamed target 懒物化：公共操作 `PrepareRenamedTargetPath` 会在 QueryPath 阶段匹配 `renamed` 记录的 target 子树，将缺失的 target shadow 子项从原 source 子树复制过来，且不覆盖已有 shadow 修改；服务端 QueryPath 对 normal 路径调用该逻辑并在日志中输出 `renamed-shadow=`；测试机脚本在读取 target child 前先断言 shadow child 已物化，失败时输出 changes、target shadow 递归列表和服务日志。用户态测试补充了删除 target shadow nested child 后按 target 路径懒物化恢复的回归断言。验证通过：`scripts/test.ps1`、`scripts/build.ps1`、`scripts/Test-PathOverlay.ps1` 语法检查、`scripts/package-test-machine.ps1 -Configuration Release`。当前本机仍无法直接加载测试签名驱动，T025 保持 blocked，等待测试机用新包复跑。

2026-04-27：测试机输出显示 `rename target child is materialized in shadow before read` 已通过，但随后 `Get-Content after-dir\child.txt` 仍按真实 target 路径失败；`rule show/disable` 是异常后的 finally 清理输出。判断原因为 QueryOpen/NetworkQueryOpen 只对 tombstone 强制回落，遇到只存在于 shadow 的 renamed target 子文件时，文件系统快速查询可能先按真实路径返回不存在，未进入后续 Create 重定向。已修复驱动 QueryOpen 处理：`PathOverlayShouldDeferQueryOpen` 会先向服务端执行 QueryPath（触发 renamed target 懒物化），再检查对应 shadow 路径；若路径是 tombstone 或 shadow 已存在，则拒绝快速查询，让后续 IRP_MJ_CREATE 走现有 shadow reparse。验证通过：`task.json` JSON 解析、`scripts/Test-PathOverlay.ps1` 语法检查、`scripts/test.ps1`、`scripts/build.ps1`、`scripts/package-test-machine.ps1 -Configuration Release`。已生成新的 `test-machine-package`，T025 仍 blocked，等待测试机复跑 `Run-PathOverlay-Test.cmd` 后确认。

2026-04-28：根据测试机新日志继续定位，确认失败点与 `ADMINI~1` 8.3 短路径相关：`after-dir\child.txt` 读取失败时服务日志没有对应子文件 query，说明请求在驱动规则匹配阶段未命中而直接落回真实路径。已修复规则短路径兼容链路：协议版本升级到 v2，驱动规则消息新增 `SourceAliasNtPath`；服务端下发规则时额外计算 source 短路径并转换为 NT alias；驱动 source 规则匹配支持 long/alias 双前缀，并在 `PreCreate`/`QueryOpen` 使用实际命中的前缀计算 shadow suffix，避免短路径下 suffix 偏移导致误判。另在 `PathOverlayBuildShadowPath` 增加 `RealPath->Length < Source->Length` 防御检查。测试机脚本补充 T025 短路径读取断言（可获取短路径时验证 `after-dir\\child.txt` 经短路径可读）。验证通过：`scripts/test.ps1`、`scripts/build.ps1`、`scripts/Test-PathOverlay.ps1` 语法检查、`scripts/package-test-machine.ps1 -Configuration Release`。由于当前会话未直接在测试机执行驱动 E2E，T025 继续保持 blocked，待测试机复跑 `test-machine-package\\Run-PathOverlay-Test.cmd`。

2026-04-28：读取测试机日志 `PathOverlay-Test-20260428-102632.log` 后确认仍失败于短路径访问 `after-dir\\child.txt`。进一步定位为“混合短长路径前缀”未覆盖：规则 long 前缀是 `...\\Users\\Administrator\\...`，服务生成的 alias 可能是全短路径，而实际访问路径常见为 `...\\Users\\ADMINI~1\\...\\PathOverlayDirRename-<long>\\...`，导致既不匹配纯 long 也不匹配纯 alias。已在驱动侧改为按路径组件逐段匹配：每一段允许命中 long 或 alias 组件，并把实际命中的 `Path` 前缀回填为 `matchedSourceNtPath` 参与 shadow suffix 计算，从而支持混合短长路径。验证通过：`scripts/test.ps1`、`scripts/build.ps1`、`scripts/package-test-machine.ps1 -Configuration Release`。新包 `test-machine-package\\PathOverlayFlt.sys` SHA256=`EBBEDEA33F2998376E16F3FF61F82FC1DB2800B41C4225FCB30CADD5D79D4C7C`。T025 仍保持 blocked，等待测试机复跑 `Run-PathOverlay-Test.cmd`。

2026-04-28：按测试建议补充 T025 读取诊断脚本：在读取 `after-dir\\child.txt` 时，除原始路径外新增 canonical 长路径读取（`ConvertTo-CanonicalPath`），并分别断言与输出错误信息，以区分“文件确实不存在”与“仅特定路径形态访问失败”。同步更新 `scripts\\Test-PathOverlay.ps1` 与当前 `test-machine-package\\Test-PathOverlay.ps1`。

2026-04-28：修复新增诊断断言中的 PowerShell 兼容问题。原实现将 `if (...) { ... } else { ... }` 直接作为 `Write-Check` 参数分组表达式，测试机运行时会报“无法将 if 项识别为 cmdlet”。已改为先计算 detail 字符串变量，再调用 `Write-Check`，避免该语法差异导致脚本提前中断。


## T031

2026-04-28：已修复 PathOverlaySvc 启动状态上报时序：服务不再在 runtime 初始化和 pipe server 创建前向 SCM 上报 SERVICE_RUNNING，而是在 pipe server 和 driver message thread 创建成功后上报 running。PathOverlaySvc.exe start 的 IPC ready 探测改为实际打开命名管道，超时时输出最后一个 Win32 LastError，并将 SCM running 等待窗口放宽到 10 秒。验证通过：task.json JSON 解析、scripts/build.ps1、scripts/test.ps1、scripts/package-test-machine.ps1 -Configuration Release、本机管理员环境下 test-machine-package/install-start.ps1 -SkipDriver -ResetData 可安装启动服务并通过 pathoverlay rule show，随后已停止并卸载服务。
2026-04-28：根据测试机日志继续修复 T025。先解释并修正诊断缺失：`Write-Check` 在 raw path 读取失败时立即 throw，导致后续 canonical long path 和 short path 读取诊断没有执行；测试脚本已改为先收集并打印 raw/canonical/short 三类读取结果，再统一断言失败。功能侧进一步修复 renamed target 子文件重定向：服务端 `QueryPath` 在 `PrepareRenamedTargetPath` 懒物化成功后，将实际 shadow DOS 路径转换为 NT 路径并通过 `PATHOVERLAY_SERVICE_RESPONSE.ShadowNtPath` 回传；驱动 `PreCreate` 收到该路径时优先使用服务回传的 shadow NT path 进行 reparse，避免 8.3 raw path 与 canonical shadow 路径推导不一致。验证通过：`task.json` JSON 解析、`scripts/build.ps1`、`scripts/test.ps1`、`scripts/Test-PathOverlay.ps1` 语法检查、`scripts/package-test-machine.ps1 -Configuration Release`。新包哈希：`PathOverlayFlt.sys` SHA256=`387C62B3ED206438CFC198D9623B5F183A8C4BCA3CD358C43266361B37D60A52`，`PathOverlaySvc.exe` SHA256=`5B46B65DDA3A78DB41770604FD41288915D6914AC24CF6824213D35C22D85FF7`，`Test-PathOverlay.ps1` SHA256=`5787903A6310B1B73CFBA317EA7AC6BE00714BD434C87B8D59FED1E59AA80D82`。当前本机 BCD 当前项未显示 `testsigning Yes`，无法直接完成驱动 E2E，T025 保持 blocked，等待测试机复跑。
2026-04-28：根据测试机复跑结果继续定位 T025：raw、canonical long、short 三种路径读取均失败，但 shadow child 已存在，说明失败不再是单一路径字符串形态问题，而是目标真实路径不存在时过滤路径没有进入服务查询/reparse。已修复驱动取名和 QueryOpen 链路：新增 `PathOverlayGetFileNameInformationWithFallback`，`PreCreate` 与 `PathOverlayShouldDeferQueryOpen` 在 `FLT_FILE_NAME_NORMALIZED` 失败时回退 `FLT_FILE_NAME_OPENED`，覆盖只存在于 shadow 的 renamed target 子文件；`PathOverlayShouldDeferQueryOpen` 也会识别服务端 `ShadowNtPath` 并据此检查 shadow 是否存在，存在时拒绝 fast query，让后续 create 走 shadow reparse。验证通过：`task.json` JSON 解析、`scripts/build.ps1`、`scripts/test.ps1`、`scripts/Test-PathOverlay.ps1` 语法检查、`scripts/package-test-machine.ps1 -Configuration Release`。新包哈希：`PathOverlayFlt.sys` SHA256=`C39AFF7BE1712653234FD417492711AF0D0AACD5658F6974772E4704E57F7B4C`，`PathOverlaySvc.exe` SHA256=`5B46B65DDA3A78DB41770604FD41288915D6914AC24CF6824213D35C22D85FF7`，`Test-PathOverlay.ps1` SHA256=`5787903A6310B1B73CFBA317EA7AC6BE00714BD434C87B8D59FED1E59AA80D82`。当前本机 BCD 当前项未显示 `testsigning Yes`，T025 保持 blocked，等待测试机复跑。
2026-04-28：测试机复跑通过，日志 `test-machine-package/PathOverlay-Test-20260428-111915.log` 显示 T025 目录 rename 场景全部通过：renamed source directory hidden、renamed target directory visible、target child shadow materialized、raw/canonical long/short 三种路径均可读取 `after-dir\child.txt`，nested child 可读，目录枚举隐藏 source 并显示 target，target 已存在时 rename 被拒绝，禁用规则后真实 source 未提前修改且真实 target 未创建。日志最终输出 `PathOverlay test package passed all automated checks.`；T025 验收项已满足，状态改为 done。

<a id="T026"></a>

## T026 - 按 rule id 执行 commit 和 discard

2026-04-28：已将 CLI commit/discard 改为必须显式传入 `--rule <id>`，服务端同步拒绝缺少 rule id 的 commit/discard 请求，并按指定 rule id 查询规则、暂停该规则、同步驱动规则缓存、执行 commit 或 discard、再恢复该规则；其他规则的启用状态和 pending changes 不参与本次操作。用户态测试新增双规则场景，验证 commit 只写回指定规则并保留其他规则变更，discard 只清理指定规则且不修改真实文件。验证通过：`scripts/build.ps1`、`scripts/test.ps1`。

<a id="T027"></a>

## T027 - 实现占用检测与确认关闭流程

2026-04-28：已在服务端 commit/discard 执行前使用 Windows Restart Manager 查询本规则 pending changes 涉及的现有文件占用进程。默认行为改为发现占用即返回 `ERROR occupied files detected`，输出 pid、进程名、应用类型和 protected 标记，并保留 change 元数据与 shadow 数据；CLI 新增 `--confirm-close`，确认后服务只会关闭非关键用户进程，遇到 system/critical/service/PathOverlaySvc 等受保护进程会整体拒绝关闭，不会先部分终止。关闭后会复查占用，确认清空后再暂停规则、执行 commit/discard 并恢复规则；SkipDriver 环境下 driver sync 缺失会降级为 warning，避免阻塞用户态服务集成验证。新增 `debug prepare-cow --rule <id> <path>` 用于服务集成测试制造 pending change。README 和安装脚本示例已更新为 `commit|discard --rule <id>` 与 `--confirm-close` 语义。验证通过：`scripts/build.ps1`、`scripts/test.ps1`、`x64/Debug/install-start.ps1 -SkipDriver -ResetData`，以及 SkipDriver 服务集成脚本覆盖 commit/discard 默认占用失败、占用进程列表输出、失败保留 shadow、`--confirm-close` 关闭持有文件句柄的用户进程后继续完成操作、discard 不修改真实文件；已卸载测试服务。
## T028 - 补充 vNext 自动化与驱动 E2E

2026-04-28：任务开始，目标是补齐多规则、目录语义、rename/move、按规则 commit/discard 和占用检测的用户态、服务集成与测试机 E2E 覆盖，并增强失败日志中的 rule id、source、store 和关键操作结果。

2026-04-28：已修正 `pathoverlay changes`，从旧的 `default` 单规则查询改为按所有规则输出 pending changes，并在每组变更前输出 rule id、enabled、source 和 store；commit/discard 成功、操作失败和占用预检失败响应均补充 rule/source/store 上下文。测试机 E2E 脚本新增目录 tombstone 与枚举、按 rule id commit/discard 隔离、占用 commit 默认失败并保留 metadata 的场景，失败断言详情包含 rule id、source、store、shadow/path 和关键输出。验证通过：`task.json` JSON 解析、`scripts/Test-PathOverlay.ps1` 语法检查、`scripts/test.ps1`、`scripts/build.ps1`、`scripts/package-test-machine.ps1 -Configuration Release`。当前会话具备管理员权限，但 `bcdedit /enum {current}` 未显示 `testsigning Yes`，无法本机加载测试签名驱动执行真实 E2E；T028 暂标记 blocked，等待在启用 test-signing 的测试机运行 `test-machine-package/Run-PathOverlay-Test.cmd` 并通过后再改为 done。

2026-04-28：用户在启用 test-signing 的测试机运行 `test-machine-package/Run-PathOverlay-Test.cmd`，结果为 “PathOverlay test package passed all automated checks.” T028 的用户态测试、测试机多规则隔离、文件与目录 rename/move、目录 tombstone/枚举、按 rule id commit/discard、占用失败日志验收均已满足，状态改为 done。

## T029 - 更新 vNext 文档与任务收尾

2026-04-28：任务开始，目标是同步 README、测试文档和设计边界，明确当前 vNext 已实现能力、CLI 命令、测试流程以及仍不支持的范围。

2026-04-28：已更新 README，明确当前 vNext 原型状态、多规则、目录和文件 rename/move、按 rule id commit/discard、占用检测和 `changes` 多规则输出；已更新 `docs/Testing.md`，补充测试机 E2E 通过标志和失败日志上下文；已更新设计审查文档，保留 MVP 历史边界并追加 vNext 已实现边界和仍不支持范围；已调整 vNext 元数据文档开头，说明当前用户可见行为以 README 和测试文档为准。验证通过：`task.json` JSON 解析、README/Testing 引用脚本存在性检查、T019-T028 done 任务 notes_ref 检查、`scripts/test.ps1`。T029 验收项已满足，状态改为 done。

<a id="T033"></a>

## T033 - 新增 rule 删除接口并保持初始无默认规则

2026-04-28：任务开始，目标是新增 `pathoverlay rule delete/del --rule <id>`，服务端按 rule id 删除无 pending changes 的规则并同步驱动，同时确认安装和 ResetData 后初始状态仍为 no rules，不创建默认 rule。

2026-04-28：已新增 `pathoverlay rule delete --rule <id>` 和 `pathoverlay rule del --rule <id>`；服务端删除前按 rule id 查询规则并拒绝仍有 pending changes 的规则，删除成功后重新同步驱动规则缓存。元数据层新增 `DeleteRule`，README 和安装脚本示例已更新。测试机 E2E 脚本新增 fresh install no rules、pending changes 删除拒绝、缺失 rule id 删除失败、`delete`/`del` 删除无 pending 规则、删除后 rule show 不再列出以及删除后驱动同步仍可写入现有规则的断言。验证通过：`task.json` JSON 解析、`scripts/build.ps1`、`scripts/test.ps1`、`scripts/test.ps1 -Configuration Release`、`scripts/Test-PathOverlay.ps1` 语法检查、`x64/Debug/install-start.ps1 -SkipDriver -ResetData` 显示 `OK no rules`、SkipDriver 服务集成手工验证 rule delete/del/缺失 id/pending changes 拒绝、`scripts/package-test-machine.ps1 -Configuration Release`。当前本机 BCD 当前项未显示 `testsigning Yes`，未在本机加载驱动跑完整 E2E；已生成新 `test-machine-package` 供测试机复跑。

2026-04-28：用户在测试机运行新 `test-machine-package`，结果为 `PathOverlay test package passed all automated checks.` T033 的真实驱动 E2E 验收已通过。

<a id="T034"></a>

## T034 - 校验自定义 store 路径必须为目录

2026-04-28：任务开始，目标是修正 `rule add --store` 可接受已有普通文件路径的问题；期望已有 store 路径必须是目录，不存在的 store 路径仍按目录语义允许后续创建。

2026-04-28：已在公共规则校验中新增已有 store 路径属性检查，若路径存在但不是目录则返回 `ERROR store path must be a directory`；不存在的 store 路径仍允许，已有目录仍允许。用户态测试覆盖 missing store 允许、已有普通文件 store 拒绝；README 已补充 `--store` 目录要求和故障排查。验证通过：`task.json` JSON 解析、`git diff --check`、`scripts/build.ps1`、`scripts/test.ps1`，以及 Debug SkipDriver 服务集成手工验证普通文件 store 拒绝、目录 store 和不存在 store 可添加规则；验证后已卸载 Debug 服务。

<a id="T035"></a>

## T035 - 将 discard shadow 删除改为后台清理

2026-04-28：任务开始，目标是降低 discard 后立刻访问 source 目录时的卡顿。计划将同步 `remove_all(store\drive)` 改为先快速 rename 到 store 下的 cleanup 目录，使驱动不再命中旧 shadow，再清理 metadata 并在后台删除 cleanup 目录。

2026-04-28：已将 `OverlayOperations::Discard` 改为先把 `store\drive` 快速 rename 到 `.discard-cleanup-<rule>-<pid>-<tick>`，再删除该 rule 的 changes；metadata 删除成功后用后台 detached 线程递归删除 cleanup 目录。若隔离 rename 失败则 discard 失败并保留 metadata，避免旧 shadow 仍在 active drive 路径时误报成功；若 metadata 删除失败会尝试把 cleanup 目录 rename 回 `store\drive`。README 已说明 discard 会先移出旧 shadow 并后台删除大目录；用户态测试新增 discard 后旧 active shadow 路径立即不可见的断言。验证通过：`task.json` JSON 解析、`git diff --check`、`scripts/build.ps1`、`scripts/test.ps1`，以及 Debug SkipDriver 服务集成验证 discard 后 changes 立即清空、旧 `store\drive` shadow 路径不可见、真实文件未修改；验证后已卸载 Debug 服务。

<a id="T036"></a>

## T036 - 规划稳定化与恢复策略

2026-04-28：已新增 `docs/Stabilization_and_Recovery_Plan.md`，定义稳定化阶段的 operation 状态机、commit/discard 中断恢复策略、discard cleanup queue 持久化和重启续删策略、`status`、`doctor`、`diagnostics collect` 输出范围、dry-run 和 `changes --rule` 语义，以及 repair、restore、rollback 第一阶段不自动执行的边界。README 已补充该稳定化计划入口，并明确当前 vNext 原型尚未完成 operation 恢复、cleanup 续删、诊断命令、dry-run 和备份恢复能力，第一阶段只做保守诊断和可重试状态记录，不宣称完整 rollback。验证通过：`task.json` JSON 解析、`docs/Stabilization_and_Recovery_Plan.md` 存在且覆盖 T036 验收点、README 引用该文档。

<a id="T037"></a>

## T037 - 实现 commit/discard 操作状态与启动恢复

2026-04-29：已新增 `operations` 元数据表和 `OperationRecord`，记录 operation id、rule id、action、status、phase、时间、backup root、错误和 operation 前 rule 启用状态。服务端 commit/discard 会在执行前创建 running operation，并在 prechecked、rule_paused、applying、cleanup、restoring_rule、finished 或 failed 阶段更新状态；commit 复用 operation id 作为 commit id，便于日志关联。服务启动时调用 `RecoverInterruptedOperations`，把遗留 `running` 的 applying/cleanup 操作标记为 `failed`，其他阶段标记为 `recoverable`，保留 pending changes 和 shadow 数据，并只在 operation 前 rule 原本启用时恢复 rule enabled 状态。新增用户态测试覆盖 running operation 恢复为 failed/recoverable、恢复状态可由 metadata 查询、pending changes 和 shadow 保留、恢复后同一 rule 可再次 commit。验证通过：`task.json` JSON 解析、`git diff --check`、`scripts/build.ps1`、`scripts/test.ps1`。

<a id="T038"></a>

## T038 - 持久化 discard 后台清理队列

2026-04-29：已新增 `cleanup_queue` 元数据表和 `CleanupRecord`，记录 cleanup id、rule id、detached shadow path、status、attempts、last_error 和时间字段。`OverlayOperations::Discard` 仍先把 active `store\drive` 快速 rename 到 `.discard-cleanup-*`，确保 discard 返回后旧 active shadow 不再参与重定向；随后删除该 rule 的 changes，并把 detached cleanup 目录以 `pending` 状态写入队列，不再使用不可追踪 detached thread。新增 `OverlayOperations::ProcessCleanupQueue`，服务启动时会处理 `pending` 或旧 `running` cleanup：删除成功标记 `done`，失败标记 `failed` 并保留错误；删除前校验 cleanup 路径必须是对应 rule store 的子路径，避免误删真实 source。用户态测试覆盖 discard 后 active drive 立即移走、队列处理删除 detached 目录并标记 done、越界 cleanup 路径标记 failed 且不删除真实 source 文件。验证通过：`task.json` JSON 解析、`git diff --check`、`scripts/build.ps1`、`scripts/test.ps1`。

<a id="T039"></a>

## T039 - 新增 status 和 doctor 诊断命令

2026-04-29：已新增 CLI/服务 IPC 命令 `pathoverlay status` 和 `pathoverlay doctor`。`status` 输出服务连接、驱动连接、规则总数和启用数量、pending changes 总数、每条 rule 的 changes 数、cleanup pending/running/failed/done 计数以及最近 operation 摘要。`doctor` 默认只读扫描 failed/recoverable/running operation、failed cleanup、缺失 cleanup 路径、缺失 rule source、非法 store 类型和 created/modified/renamed change 缺失 shadow，并以 `ERROR`/`WARN` 行输出；不修改 metadata、shadow 或真实 source。README 已补充 `status`/`doctor` 当前能力并从未完成能力中移除。验证通过：`task.json` JSON 解析、`git diff --check`、`scripts/build.ps1`、`scripts/test.ps1`，以及 Debug `install-start.ps1 -SkipDriver -ResetData` 后手工执行 `pathoverlay status` 和 `pathoverlay doctor`，输出分别包含 `service=connected`、`driver=not_connected`、规则/cleanup/operation 摘要和 `no issues`；验证后已卸载服务并清理数据。

## T040 - 增加诊断包收集与日志改进

2026-04-29：已新增 CLI 命令 `pathoverlay diagnostics collect [--output <directory>]`。命令会生成诊断目录，写入 `rule-show.txt`、`changes.txt`、`status.txt`、`doctor.txt`、`driver-status.txt`、`scm-service.txt`、`scm-driver.txt`、`manifest.txt`，并在服务日志存在时复制 `PathOverlaySvc.log`；即使服务 IPC 不可用也会保留对应命令失败信息，便于离线排查。服务日志补充本地时间戳和 pid 字段。测试机脚本 `Test-PathOverlay.ps1` 在失败分支自动执行诊断收集并输出目录路径；`scripts/test.ps1` 增加无服务环境的诊断收集冒烟验证。README 和 `docs/Testing.md` 已补充入口说明。验证通过：`scripts/build.ps1`、`scripts/test.ps1`、`git diff --check`。

## T041 - 增加 rule 级 changes 过滤与 dry-run

2026-04-29：已新增 `pathoverlay changes --rule <id>`，服务端按指定 rule id 输出 pending changes，缺失 rule 会返回错误；无 `--rule` 时保留全量按 rule 分组输出。CLI 和服务端新增 `commit --dry-run --rule <id>` 与 `discard --dry-run --rule <id>`；dry-run 在创建 operation 记录、暂停 rule、写真实 source、移动 shadow 或清理 metadata 之前返回。commit dry-run 输出 write/delete/rename、backup root、预估 backup path，以及占用、真实文件冲突、缺失 shadow、rename target 已存在等 blocker；discard dry-run 输出将清理的 change 数量、active shadow 根、metadata scope 和 source unchanged 范围。测试机 E2E 脚本在按 rule commit/discard 场景中覆盖 `changes --rule` 和 dry-run 不修改真实文件、shadow 或 metadata。README 已补充命令示例和语义。验证通过：`scripts/build.ps1`、`scripts/test.ps1`、`scripts/Test-PathOverlay.ps1` 语法检查、`git diff --check`，以及 Debug `install-start.ps1 -SkipDriver -ResetData` 后手工服务集成验证 `changes --rule`、`commit --dry-run --rule`、`discard --dry-run --rule`；验证后已卸载服务并清理数据。

## T042 - 设计备份索引与手动恢复能力

2026-04-29：已在 `docs/Stabilization_and_Recovery_Plan.md` 细化 backup index 与手动恢复边界：新增 `backup_sets_v1` 和 `backup_items_v1` 建议结构，定义 backup set/item 的状态、路径、原因和 restore 记录字段；明确 `pathoverlay backup list [--rule <id>] [--operation <operation-id>] [--json]` 的输入和输出字段；明确 `pathoverlay restore --backup <id> --item <id>`、`restore --all`、`--target` 和 `--overwrite` 的语义、冲突处理、pending/failed changes 限制和占用/权限失败行为。第一版只从已有 backup item 复制已落盘备份内容，不恢复新建文件的旧内容，不自动 repair，不按 commit 顺序重放删除或 rename，不宣称完整事务 rollback。验证通过：`task.json` JSON 解析、`git diff --check`，以及文档关键字检查覆盖 backup list、restore、pending changes、已有备份内容和非完整 rollback 边界。

## T043 - 扩展路径与属性兼容性测试

2026-04-29：已扩展用户态测试和测试机 E2E 脚本。用户态 `PathOverlayTests` 新增长路径片段与 Unicode 路径、大小写变体路径、可用时的 8.3 短路径、只读属性与 last-write timestamp 保留、空嵌套目录 commit 的覆盖；测试机 `Test-PathOverlay.ps1` 新增 T043 场景，覆盖长 Unicode 路径写入 shadow、大小写变体写入 canonical shadow、可用时 8.3 短路径写入 shadow、`debug prepare-cow` 保留只读属性和 last-write timestamp、空嵌套目录只进入 shadow 且未提前创建真实 source。`docs/Testing.md` 已补充用户态和驱动 E2E 兼容性覆盖范围。验证通过：`task.json` JSON 解析、`git diff --check`、`scripts/build.ps1`、`scripts/test.ps1`、`scripts/build.ps1 -Configuration Release`、`scripts/test.ps1 -Configuration Release`、`scripts/Test-PathOverlay.ps1` 语法检查、`scripts/package-test-machine.ps1 -Configuration Release`。最新测试机包已生成并签名：`PathOverlayFlt.sys` SHA256=`C39AFF7BE1712653234FD417492711AF0D0AACD5658F6974772E4704E57F7B4C`，`PathOverlaySvc.exe` SHA256=`9B5CF92F66C0132757C66A437A79A58567E528472DD54CE70E88FABEB7C520B9`，`Test-PathOverlay.ps1` SHA256=`DC31E74A2AB23C9A0D87DBA388B67BA69D0ECE323BE3C9087686695BC1590251`。当前会话具备管理员权限，但 `bcdedit /enum {current}` 未显示 `testsigning Yes`，无法本机加载测试签名驱动执行 `test-machine-package/Run-PathOverlay-Test.cmd`；T043 暂标记 blocked，待在启用 test-signing 的测试机复跑并确认输出 `PathOverlay test package passed all automated checks.` 后再改为 done。

2026-04-29 复查测试机日志 `test-machine-package/PathOverlay-Test-20260429-141218.log`：T043 失败点为 `compatibility changes include long unicode path`，前置长 Unicode 路径写入 shadow 已通过，服务日志也显示 `changes record[6] realPath_len=209`，说明 metadata 中完整保存了路径；实际失败来自 CLI/pipe 输出边界截断到 `source\unicode-`。已修正 CLI 输出统一走 `WriteConsoleW` 或 UTF-8 `WriteFile`，并保留 UTF-8 转换函数的缓冲区越界修复。验证通过：`scripts/build.ps1`、`scripts/test.ps1`、`scripts/Test-PathOverlay.ps1` 语法检查、SkipDriver 服务集成冒烟验证 PowerShell 捕获的 `changes --rule` 包含完整 `unicode-路径\...\数据-file.txt`、`scripts/package-test-machine.ps1 -Configuration Release`、`git diff --check`。当前启动项仍未显示 `testsigning Yes`，真实驱动 E2E 需在测试机复跑。

2026-04-29 再次复查测试机日志 `test-machine-package/PathOverlay-Test-20260429-142328.log`：`changes --rule` 已不再截断，但 Windows PowerShell 5.1 仍把 UTF-8 native 输出按默认代码页解码，同时脚本源码中的中文路径字面量也有无 BOM 读取风险，导致测试期望路径和 CLI 输出路径不相等。已在测试脚本入口设置 `[Console]::OutputEncoding` 与 `$OutputEncoding` 为 UTF-8，并将 T043 的 `路径`、`数据-file.txt` 改为 `[char]` 构造，避免源码编码影响。验证通过：`scripts/Test-PathOverlay.ps1` 语法检查、`scripts/test.ps1`、SkipDriver 服务集成冒烟验证 PowerShell 捕获的 `changes --rule` 可匹配 `[char]` 构造的中文路径、`scripts/package-test-machine.ps1 -Configuration Release`。新测试机包中 `Test-PathOverlay.ps1` SHA256=`A368BABEF3495BC28290C4261E2A541E6DF3F98C4CCE140EC626D06A6DC4F460`，`pathoverlay.exe` SHA256=`090397C6F75F5701FB556404017CF6049DB5FDC49E91DE4B141750F7F0C48885`，`PathOverlayFlt.sys` SHA256=`D933336D0C2BE33477C0C6BE37B76BEC62038D187A68BCCEA08DCA0B0BCDBA32`。真实驱动 E2E 仍需在启用 test-signing 的测试机复跑。

<a id="T044"></a>

## T044 - 更新稳定化文档与发布检查清单

2026-04-29：任务开始。目标是把稳定化阶段新增命令、诊断流程、恢复限制和测试流程补充到用户文档，并新增发布前检查清单。T036 到 T043 均已有 notes 记录；T043 的真实驱动 E2E 仍依赖启用 test-signing 的测试机复跑，T044 文档会保留该环境要求。

2026-04-29：已更新 README 稳定化说明，集中描述 `status`、`doctor`、`diagnostics collect`、`changes --rule`、commit/discard dry-run、诊断包内容和第一阶段不自动 repair/restore/rollback 的恢复边界。`docs/Testing.md` 已补充稳定化验证流程、兼容性测试覆盖范围、服务诊断检查步骤和测试机失败诊断包验收方式。新增 `docs/Release_Checklist.md`，覆盖发布前 scope 检查、Debug/Release 构建、用户态测试、服务诊断、测试机驱动 E2E、清理和最终 review。验证通过：`task.json` JSON 解析、`git diff --check`、关键字检查覆盖 T044 验收项、`scripts/test.ps1`。
