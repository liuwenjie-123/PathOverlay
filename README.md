# PathOverlay

PathOverlay 是一个 Windows 文件系统覆盖层原型，用 `minifilter driver + 用户态服务 + CLI` 实现指定本地目录的 copy-on-write 重定向。它的目标是在 commit 前保护真实目录：写入进入隔离目录，删除记录为 tombstone，用户可以选择提交或丢弃变更。

当前仓库已经完成 vNext 原型任务，不是生产级沙箱。设计背景见 `PathOverlay_Design.md`，MVP 历史边界见 `docs/Design_Review_and_Revisions.md` 和 `docs/MVP_Development_Plan.md`；当前能力、测试状态和后续限制以本 README、`docs/Testing.md`、`docs/vNext_Metadata_and_Compatibility.md`、`docs/Stabilization_and_Recovery_Plan.md`、`docs/Release_Checklist.md` 和 `task.json` 为准。

## 当前能力

- 支持多条互不重叠的本地目录规则，每条规则有独立 rule id 和 store。
- 读操作优先读取已有 shadow 文件，否则读取真实文件。
- 写入真实文件前准备 shadow copy，commit 前不直接修改真实文件。
- 新建文件记录为 created。
- 删除文件记录 tombstone，commit 前不删除真实文件。
- 目录 create/delete/rename/move 进入 shadow 和元数据，commit 前不提前修改真实目录。
- 文件 rename/move 支持同一 rule 内、目标不存在的路径，commit 前只更新 shadow 和元数据。
- 目录枚举合并 real/shadow，并隐藏 tombstone 和 renamed source。
- 支持按 rule id 执行 `commit`，写回变更，写回或删除前创建备份。
- 支持按 rule id 执行 `discard`，丢弃隔离数据，不修改真实文件。
- commit/discard 前检测占用文件；默认失败并列出占用进程，显式 `--confirm-close` 时可关闭非关键用户进程后继续。
- 支持只读 `status`、`doctor` 和 `diagnostics collect` 诊断命令，查看服务、驱动、规则、pending changes、operation、cleanup 和日志状态。
- 支持 `changes --rule <id>`、`commit --dry-run --rule <id>` 和 `discard --dry-run --rule <id>`，在提交或丢弃前预览影响范围。
- 服务和 store 路径排除重定向，避免递归影响。

## 已知限制

- 多规则 source 不能相同或互相包含，任一 source 和任一 store 不能互相嵌套。
- 不支持整盘覆盖、盘符根目录、UNC 路径、网络路径和 reparse point 作为 source。
- source 内部的 symlink、junction、mount point 等 reparse subtree 采用 passthrough 策略：PathOverlay 不跟随、不接管、不递归复制、不纳入 commit/discard；访问这些路径时由 Windows 默认处理，写入 link target 不受覆盖层隔离保护。
- source 和 store 不能互相嵌套。
- rename/move 只支持同一 rule 内、同一卷内、目标不存在的文件或目录；跨 rule、跨卷、目标已存在、tombstone 后 rename 会保守失败。
- 不支持 per-rule include/exclude pattern、排除路径和按进程规则。
- 不支持注册表虚拟化，也不提供完整系统盘保护或安全沙箱边界。
- 不覆盖硬链接、alternate data streams、完整 ACL 继承和复杂安全语义。
- 备份索引、`backup list` 和手动 `restore` 仍处于设计阶段；当前实现只在 commit 写回前创建备份，不提供完整事务回滚。
- 第一阶段恢复能力只做保守诊断、可重试状态记录和 cleanup 续删，不自动 repair、restore 或宣称完整 rollback。
- 真实驱动测试需要 Windows 测试机、管理员权限、test-signing 和签名证书。

## 环境要求

- Windows。
- Visual Studio 2022，包含 Desktop development with C++ 和 MSBuild。
- Windows SDK 和 WDK for Visual Studio 2022。
- x64 构建环境。
- `sqlite3.dll` 需要位于构建输出或测试脚本可发现的位置。
- 驱动安装、服务安装、清理 `%ProgramData%\PathOverlay`、启用 test-signing 等操作需要管理员权限。

## 快速开始

在仓库根目录运行：

```powershell
.\scripts\build.ps1
.\scripts\test.ps1
```

构建完成后，一键安装并启动当前 Debug 输出：

```powershell
cd .\x64\Debug
.\Install-Start-PathOverlay.cmd
```

构建 Release：

```powershell
.\scripts\build.ps1 -Configuration Release
.\scripts\test.ps1 -Configuration Release
```

安装并启动 Release 输出：

```powershell
cd .\x64\Release
.\Install-Start-PathOverlay.cmd
```

`scripts\test.ps1` 会验证 `task.json`、检查项目结构、在能找到 `PathOverlayTests.exe` 时运行用户态测试。它不要求安装驱动。

## 构建

默认构建 Debug x64：

```powershell
.\scripts\build.ps1
```

构建 Release x64：

```powershell
.\scripts\build.ps1 -Configuration Release -Platform x64
```

脚本会查找 Visual Studio 2022 MSBuild、Windows Kits 和 WDK targets。缺少依赖时会给出明确错误。

## 测试

用户态和服务逻辑测试：

```powershell
.\scripts\test.ps1
```

Release 测试：

```powershell
.\scripts\test.ps1 -Configuration Release
```

清理本地测试数据：

```powershell
.\scripts\cleanup-test-data.ps1
```

管理员 PowerShell 下清理服务数据：

```powershell
.\scripts\cleanup-test-data.ps1 -IncludeServiceData
```

管理员 PowerShell 下卸载测试服务和驱动并清理数据：

```powershell
.\scripts\cleanup-test-data.ps1 -IncludeDriverAndService
```

更多测试机和 E2E 细节见 `docs/Testing.md`。

## 测试机驱动包

在开发机生成测试机包：

```powershell
.\scripts\package-test-machine.ps1 -Configuration Release
```

脚本会构建项目、复制 `PathOverlayFlt.sys`、`PathOverlaySvc.exe`、`pathoverlay.exe`、`sqlite3.dll`、清理脚本和测试证书到 `test-machine-package\`，并用测试证书签名驱动。

将整个 `test-machine-package\` 目录复制到 Windows 测试 VM。VM 上需要管理员权限，并启用 test-signing 后重启：

```powershell
bcdedit /set testsigning on
```

然后在测试包目录运行：

```powershell
.\Run-PathOverlay-Test.cmd
```

建议在 VM 快照中运行文件系统驱动测试。

## 手工安装与基础用法

通常优先在编译输出目录运行一键脚本：

```powershell
cd .\x64\Release
.\Install-Start-PathOverlay.cmd
```

脚本会请求管理员权限，检查 `PathOverlayFlt.sys`、`PathOverlaySvc.exe`、`pathoverlay.exe` 和 `sqlite3.dll`，检查 test-signing，在需要时用 WDK `signtool.exe` 创建/使用测试证书签名驱动，安装并加载 `PathOverlayFlt`，安装并启动 `PathOverlaySvc`，然后执行 `pathoverlay rule show` 和 `pathoverlay driver status` 验证。

如果当前系统未启用 test-signing，先在管理员 PowerShell 中执行并重启：

```powershell
bcdedit /set testsigning on
```

如需清空 `%ProgramData%\PathOverlay` 后重新安装：

```powershell
.\install-start.ps1 -ResetData
```

通常优先使用 `test-machine-package\Run-PathOverlay-Test.cmd` 进行端到端验证。需要完全手工操作时，在包含构建产物的管理员 PowerShell 中执行：

```powershell
sc.exe create PathOverlayFlt type= filesys binPath= "<PathOverlayFlt.sys 的完整路径>" start= demand error= normal depend= FltMgr DisplayName= "PathOverlay minifilter driver"
reg.exe add HKLM\SYSTEM\CurrentControlSet\Services\PathOverlayFlt\Instances /v DefaultInstance /t REG_SZ /d "PathOverlayFlt Instance" /f
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\PathOverlayFlt\Instances\PathOverlayFlt Instance" /v Altitude /t REG_SZ /d 370030 /f
reg.exe add "HKLM\SYSTEM\CurrentControlSet\Services\PathOverlayFlt\Instances\PathOverlayFlt Instance" /v Flags /t REG_DWORD /d 0 /f
fltmc.exe load PathOverlayFlt
```

安装并启动用户态服务：

```powershell
.\PathOverlaySvc.exe install
.\PathOverlaySvc.exe start
```

CLI 示例：

```powershell
.\pathoverlay.exe driver status
.\pathoverlay.exe rule add C:\Temp\PathOverlaySource
.\pathoverlay.exe rule add C:\Temp\AnotherSource --store D:\PathOverlayStore\AnotherSource
.\pathoverlay.exe rule show
.\pathoverlay.exe changes
.\pathoverlay.exe changes --rule <id>
.\pathoverlay.exe status
.\pathoverlay.exe doctor
.\pathoverlay.exe diagnostics collect
.\pathoverlay.exe commit --dry-run --rule <id>
.\pathoverlay.exe commit --rule <id>
.\pathoverlay.exe discard --dry-run --rule <id>
.\pathoverlay.exe discard --rule <id>
.\pathoverlay.exe rule disable --rule <id>
.\pathoverlay.exe rule enable --rule <id>
.\pathoverlay.exe rule delete --rule <id>
```

## 覆盖层日常使用

设置覆盖规则：

```powershell
.\pathoverlay.exe rule add C:\Temp\PathOverlaySource
.\pathoverlay.exe rule add C:\Temp\AnotherSource --store D:\PathOverlayStore\AnotherSource
```

每次 `rule add` 成功都会返回自动生成的 rule id。未指定 `--store` 时，服务会为该规则生成独立 store；指定 `--store` 时，该路径会持久化并参与重叠校验；如果该 store 路径已经存在，必须是目录。

查看当前规则和隔离目录：

```powershell
.\pathoverlay.exe rule show
```

输出中会列出所有规则的 `id`、`enabled`、`source` 和 `store`。例如：

```text
OK
rule-20260427-120000-1234-5678 enabled=true source=C:\Temp\PathOverlaySource store=C:\ProgramData\PathOverlay\Boxes\rule-20260427-120000-1234-5678
```

按 rule id 启用或禁用规则：

```powershell
.\pathoverlay.exe rule disable --rule rule-20260427-120000-1234-5678
.\pathoverlay.exe rule enable --rule rule-20260427-120000-1234-5678
```

删除无 pending changes 的规则：

```powershell
.\pathoverlay.exe rule delete --rule rule-20260427-120000-1234-5678
.\pathoverlay.exe rule del --rule rule-20260427-120000-1234-5678
```

如果规则仍有 pending changes，删除会失败；先执行 `commit` 或 `discard` 后再删除。删除规则只移除规则元数据并同步驱动规则缓存，不递归删除自定义 `store` 目录。

规则启用后，写入 `source` 内的文件不会在 commit 前直接修改真实文件。新建或修改：

```powershell
Set-Content C:\Temp\PathOverlaySource\a.txt "hello"
```

对应内容会写入 `store` 下的 shadow 路径，形如：

```text
C:\ProgramData\PathOverlay\Boxes\rule-...\drive\C\Temp\PathOverlaySource\a.txt
```

删除 `source` 内文件时，commit 前不会删除真实文件，而是记录 tombstone：

```powershell
Remove-Item C:\Temp\PathOverlaySource\old.txt
```

查看当前待处理变更：

```powershell
.\pathoverlay.exe changes
.\pathoverlay.exe changes --rule <id>
```

`changes` 会按规则分组输出待处理变更，并包含 rule id、enabled 状态、source 和 store，便于在多规则场景下定位隔离数据。`changes --rule <id>` 只显示指定规则的 pending changes。

同一 rule 内可以 rename/move 文件或目录；源路径会在 overlay 视图中隐藏，目标路径从 shadow 读取。commit 前真实 source 不会被提前移动或删除。跨 rule、跨卷、目标已存在或 tombstone 后的 rename/move 会失败。

按 rule id 丢弃该规则的覆盖层变更，不修改真实目录：

```powershell
.\pathoverlay.exe discard --dry-run --rule rule-20260427-120000-1234-5678
.\pathoverlay.exe discard --rule rule-20260427-120000-1234-5678
```

`discard --dry-run --rule <id>` 只读输出将清理的 change 数量、active shadow 根和 metadata 范围，不会修改真实 source、shadow 或 metadata。`discard` 成功后会立即清空该 rule 的 pending changes，并先把旧 shadow 数据移出当前 `store\drive`，避免后续访问继续命中旧覆盖内容；较大的 shadow 目录会在后台继续删除。

按 rule id 应用该规则的覆盖层变更到真实目录：

```powershell
.\pathoverlay.exe commit --dry-run --rule rule-20260427-120000-1234-5678
.\pathoverlay.exe commit --rule rule-20260427-120000-1234-5678
```

`commit --dry-run --rule <id>` 只读输出将写入、删除、rename 和备份的路径，并列出会阻塞提交的占用、冲突、缺失 shadow 或目标已存在问题；不会暂停规则、创建 operation、创建备份、写真实 source、移动 shadow 或清理 metadata。

如果 commit 或 discard 检测到相关文件被用户进程占用，默认会失败并列出占用进程。确认可以关闭这些非关键用户进程时，显式追加：

```powershell
.\pathoverlay.exe commit --rule rule-20260427-120000-1234-5678 --confirm-close
.\pathoverlay.exe discard --rule rule-20260427-120000-1234-5678 --confirm-close
```

典型流程：

```powershell
.\pathoverlay.exe rule add C:\Temp\PathOverlaySource

Set-Content C:\Temp\PathOverlaySource\a.txt "hello"
Remove-Item C:\Temp\PathOverlaySource\old.txt

.\pathoverlay.exe changes
.\pathoverlay.exe commit --rule <returned-rule-id>
```

## 稳定化与诊断命令

稳定化阶段的完整设计边界见 `docs/Stabilization_and_Recovery_Plan.md`，发布前人工核对见 `docs/Release_Checklist.md`。这些命令默认都不读取或打包真实 source 文件内容，也不会复制 shadow 文件内容。

```powershell
.\pathoverlay.exe status
.\pathoverlay.exe doctor
.\pathoverlay.exe diagnostics collect --output C:\Temp\PathOverlayDiagnostics
.\pathoverlay.exe changes --rule <id>
.\pathoverlay.exe commit --dry-run --rule <id>
.\pathoverlay.exe discard --dry-run --rule <id>
```

`status` 是轻量只读状态摘要，输出服务连接、驱动连接、规则数量、启用规则数量、pending changes 数量、cleanup 队列计数和最近 operation 摘要。它不扫描完整 shadow 目录树。

`doctor` 是只读一致性检查，报告 failed/recoverable/running operation、failed cleanup、缺失 cleanup 路径、缺失 rule source、非法 store 类型、缺失 shadow 和 source 内 reparse passthrough 路径等问题。第一阶段没有自动 `--fix`，不会修改 metadata、shadow 或真实 source。

`diagnostics collect [--output <目录>]` 会生成诊断目录，包含 `rule-show.txt`、`changes.txt`、`status.txt`、`doctor.txt`、`driver-status.txt`、SCM 状态、manifest 和服务日志副本；服务不可用时也会记录失败输出，便于离线排查。诊断包可能包含本机路径、rule id、store 路径和错误消息，分享前应按需要脱敏。

dry-run 命令用于提交或丢弃前确认范围：`commit --dry-run --rule <id>` 列出将写入、删除、rename、备份的路径和 blocker；`discard --dry-run --rule <id>` 列出将清理的 change 数量、active shadow 根和 metadata 范围。dry-run 不暂停 rule、不创建 operation、不创建备份、不移动 shadow、不删除 metadata，也不修改真实 source。

如果不想保留这些变更，把最后一行换成：

```powershell
.\pathoverlay.exe discard --rule <returned-rule-id>
```

一键卸载服务和驱动：

```powershell
cd .\x64\Release
.\Uninstall-PathOverlay.cmd
```

如需同时删除 `%ProgramData%\PathOverlay` 数据：

```powershell
.\uninstall.ps1 -RemoveData
```

完全手工停止和卸载：

```powershell
.\PathOverlaySvc.exe stop
.\PathOverlaySvc.exe uninstall
fltmc.exe unload PathOverlayFlt
sc.exe delete PathOverlayFlt
```

## 故障排查

- `PathOverlaySvc is unavailable`：确认服务已安装并启动，或查看 `%ProgramData%\PathOverlay\PathOverlaySvc.log`。
- 需要集中定位问题：运行 `pathoverlay diagnostics collect [--output <目录>]`，诊断目录会包含规则、changes、status、doctor、driver status、SCM 状态和服务日志。
- `driver status` 失败：确认 `PathOverlayFlt` 已加载，驱动签名可信，test-signing 已启用并重启。
- 驱动无法加载：确认管理员权限、WDK 构建产物、测试证书导入和 `bcdedit /set testsigning on`。
- `rule add` 失败并提示规则重叠：检查新 source 是否与已有 source 互相包含，或 source/store 是否互相嵌套。
- `rule add` 失败并提示 `store path must be a directory`：检查 `--store` 是否指向已有普通文件；store 必须是目录路径。
- `rule enable`、`rule disable` 或 `rule delete` 失败：确认命令包含 `--rule <id>`，并用 `pathoverlay rule show` 查看有效 id；删除前还要确认该规则没有 pending changes。
- 测试数据残留：运行 `.\scripts\cleanup-test-data.ps1 -IncludeDriverAndService`。

## 仓库结构

```text
driver/   Windows minifilter driver code
service/  user-mode service, metadata, commit/discard logic
cli/      command-line interface
common/   shared path, metadata, protocol, and overlay logic
docs/     design notes and operational documentation
tests/    unit and integration tests
scripts/  build, test, packaging, and cleanup scripts
```

## 贡献说明

任务通过 `task.json` 跟踪。开始实现任务前先将对应任务标记为 `in_progress`；只有对应 `test_criteria` 全部验证通过后才能标记为 `done`。Codex 和自动化代理的额外规则见 `AGENTS.md`。
