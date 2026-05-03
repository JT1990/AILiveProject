# T0 — 启用 SQLiteCore + 加 OpenSSL 依赖 + UBT 全量 + 最小编译验证

> 总览：[00_overview.md](00_overview.md)
> 依赖：—
> 改动量：**小**（< 1h）
> §11 验收：impl §1 实施前验证 4 步

## 预检

`mcp__monolith__monolith_status` 确认 Monolith MCP 在线（用于 T5/T6/T7 的 Blueprint/Actor 状态读取）；若离线，先重启 UE Editor（按 CLAUDE.md「UE 编辑器进程管理」段）。本步纯 .uproject + Build.cs 改动，C++ 状态用 Read/Grep 直接查文件。

## 目标

在投入任何类型工作前确认 SQLiteCore + OpenSSL 在本机 UE 5.7 安装下可 link、API 可用。

## 涉及文件

- 修：`AILiveProject.uproject`（`Plugins` 数组加 `{"Name":"SQLiteCore","Enabled":true}`）
- 修：`Source/AILiveProject/AILiveProject.Build.cs`（`PublicDependencyModuleNames` 加 `"SQLiteCore"` + `"OpenSSL"`）
- 临时：随便一个 `.cpp` 内加 `#include "SQLiteDatabase.h"` + `#include "openssl/evp.h"` + `FSQLiteDatabase Db; Db.Open(TEXT("test.db"), ESQLiteDatabaseOpenMode::ReadWriteCreate);`，验证完后立即删除

## 依赖

— （这是 P0 前置零号任务，无 blockedBy）

## 验收方式

1. Editor → Edit → Plugins 搜 SQLiteCore，状态 = Enabled
2. UBT 全量 `Build.bat AILiveProjectEditor Win64 Development` 通过
3. 编辑器能加载 `L_prison.umap`（既有功能不退化的烟测）
4. 重启后 `mcp__monolith__monolith_status` 仍能返回 online

## 风险点

- SQLiteCore 在某些 5.7 小版本 EnabledByDefault 状态有差异，必须显式 .uproject 声明（impl §1 已警告）
- 改 .uproject Plugins 列表 → CLAUDE.md 关键规则「`.Build.cs/.Target.cs/.uproject plugin 列表` 改了必须全量重建」

## 完成定义

UBT 全量构建通过 + Editor PIE 能加载关卡 + MCP 在线。**不**意味着任何业务代码就绪。
