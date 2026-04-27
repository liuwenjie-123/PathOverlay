# PathOverlay 设计审查与修订

## 结论

原设计的大方向合理：PathOverlay 应定位为 Windows 文件系统 overlay/copy-on-write 工具，而不是完整沙箱；正式透明重定向应使用 minifilter driver + 用户态服务 + CLI/UI 的三层架构。

需要修正的是 MVP 范围。原文同时覆盖盘符规则、rename、目录枚举、提交回滚、ACL、冲突处理等内容，作为最终方向可以保留，但不适合全部进入第一版。MVP 必须收窄到一个可验证、可回滚、风险可控的闭环。

## 保留的设计

- 使用 Windows minifilter driver 做透明文件访问拦截。
- 用户态服务负责规则、元数据、commit/discard、备份和日志。
- CLI 负责规则配置、变更查看、提交和丢弃。
- 写入使用 copy-on-write：真实文件存在且隔离副本不存在时，先复制到隔离目录，再写隔离副本。
- 删除不直接删除真实文件，而是记录 tombstone。
- 目录枚举需要合并真实目录、隔离目录和 tombstone。
- commit/discard 不放入驱动，必须由用户态服务执行。

## 必须修正的点

### MVP 只支持一个本地目录规则

第一版只支持一个本地目录 source，例如 `C:\Work`。不支持多个规则、盘符根目录、整个系统盘、网络路径或 UNC 路径。这样可以先验证路径匹配、copy-on-write、tombstone、目录枚举、commit/discard 这些核心语义。

### store 默认放在 ProgramData

原设计示例使用 `C:\PathOverlay\Boxes\Default`，适合作为说明，但实现默认值应改为：

```text
%ProgramData%\PathOverlay\Boxes\Default
```

这样更符合 Windows 服务数据位置，也避免在系统盘根目录创建固定业务目录。

### source/store 必须互相隔离

规则校验必须拒绝以下情况：

- `store` 位于 `source` 内。
- `source` 位于 `store` 内。
- `source` 等于 `store`。

否则驱动可能递归重定向自己的隔离数据，导致无限复制、枚举污染或提交错误。

### store 和服务进程必须排除重定向

驱动必须排除：

- PathOverlay 的 store 目录。
- PathOverlaySvc 服务进程自己的文件访问。
- 元数据库、日志、备份目录等内部路径。

这些路径不能被 overlay 规则再次命中。

### rename/move 不进入 MVP

rename/move 涉及源/目标关系、跨目录状态、覆盖冲突和回滚顺序。MVP 中遇到 rename/move 应返回不支持或保守失败，后续版本再单独设计。

### ACL 只做最低限度保留

MVP 可以复制文件内容、基础属性和时间戳。完整 ACL 继承、安全边界和权限模拟不属于 MVP，不应把 PathOverlay 描述为安全沙箱。

## MVP 明确不支持

- rename/move。
- 整盘覆盖或系统盘根目录覆盖。
- UNC 路径和网络盘。
- 硬链接。
- NTFS alternate data streams。
- reparse point、junction、symlink 的完整语义。
- 完整 ACL 继承和安全边界。
- 按进程规则。
- 内存映射写入一致性保证。

## 修订后的 MVP 边界

MVP 交付目标是：在 Windows 上通过 C++ minifilter driver、C++ Windows 服务和 C++ CLI，实现一个本地目录的透明写入覆盖层。

MVP 必须支持：

- 添加、启用、禁用一个本地目录规则。
- 普通文件 create/read/write/delete。
- copy-on-write。
- tombstone 删除标记。
- 目录枚举合并。
- 变更列表。
- commit 写回真实目录。
- discard 丢弃隔离数据。

