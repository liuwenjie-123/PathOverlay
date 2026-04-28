# PathOverlay

PathOverlay 是一个 Windows 文件系统覆盖层原型，用 `minifilter driver + 用户态服务 + CLI` 实现指定本地目录的 copy-on-write 重定向。它的目标是在 commit 前保护真实目录：写入进入隔离目录，删除记录为 tombstone，用户可以选择提交或丢弃变更。

当前仓库是从 MVP 向 vNext 演进中的实现，不是生产级沙箱。设计背景见 `PathOverlay_Design.md`，MVP 边界以 `docs/Design_Review_and_Revisions.md`、`docs/MVP_Development_Plan.md` 和 `task.json` 为准。

## 当前能力

- 支持多条互不重叠的本地目录规则，每条规则有独立 rule id 和 store。
- 读操作优先读取已有 shadow 文件，否则读取真实文件。
- 写入真实文件前准备 shadow copy，commit 前不直接修改真实文件。
- 新建文件记录为 created。
- 删除文件记录 tombstone，commit 前不删除真实文件。
- 目录枚举合并 real/shadow，并隐藏 tombstone。
- 支持 `commit` 写回变更，写回或删除前创建备份。
- 支持 `discard` 丢弃隔离数据，不修改真实文件。
- 服务和 store 路径排除重定向，避免递归影响。

## 已知限制

- 多规则 source 不能相同或互相包含，任一 source 和任一 store 不能互相嵌套。
- 不支持整盘覆盖、盘符根目录、UNC 路径、网络路径和 reparse point 作为 source。
- source 和 store 不能互相嵌套。
- rename/move 在 MVP 中返回不支持。
- 不覆盖硬链接、alternate data streams、完整 ACL 继承和复杂安全语义。
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
.\pathoverlay.exe commit --rule <id>
.\pathoverlay.exe discard --rule <id>
.\pathoverlay.exe rule disable --rule <id>
.\pathoverlay.exe rule enable --rule <id>
```

## 覆盖层日常使用

设置覆盖规则：

```powershell
.\pathoverlay.exe rule add C:\Temp\PathOverlaySource
.\pathoverlay.exe rule add C:\Temp\AnotherSource --store D:\PathOverlayStore\AnotherSource
```

每次 `rule add` 成功都会返回自动生成的 rule id。未指定 `--store` 时，服务会为该规则生成独立 store；指定 `--store` 时，该路径会持久化并参与重叠校验。

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
```

按 rule id 丢弃该规则的覆盖层变更，不修改真实目录：

```powershell
.\pathoverlay.exe discard --rule rule-20260427-120000-1234-5678
```

按 rule id 应用该规则的覆盖层变更到真实目录：

```powershell
.\pathoverlay.exe commit --rule rule-20260427-120000-1234-5678
```

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
- `driver status` 失败：确认 `PathOverlayFlt` 已加载，驱动签名可信，test-signing 已启用并重启。
- 驱动无法加载：确认管理员权限、WDK 构建产物、测试证书导入和 `bcdedit /set testsigning on`。
- `rule add` 失败并提示规则重叠：检查新 source 是否与已有 source 互相包含，或 source/store 是否互相嵌套。
- `rule enable` 或 `rule disable` 失败：确认命令包含 `--rule <id>`，并用 `pathoverlay rule show` 查看有效 id。
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
