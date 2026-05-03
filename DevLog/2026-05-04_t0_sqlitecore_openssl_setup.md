# T0 — 启用 SQLiteCore + 加 OpenSSL 依赖 + UBT 全量验证

日期：2026-05-04

## 目标

对抗博弈记忆系统（`Tasks/00_overview.md` + `Docs/memory_implementation_ue57.md`）落地的 P0 前置零号任务。在投入任何类型工作（T1 起）前，独立验证 SQLiteCore + OpenSSL 在本机 UE 5.7 安装下可 link、API 可用，避免后续 T2 起所有任务（DDL / EventStore / 哈希链 / Resume 协议）撞 link 阶段才发现根因。

## 改动清单

| 文件 | 改动 |
| --- | --- |
| `AILiveProject.uproject` Plugins 数组末尾 | 追加 `{"Name":"SQLiteCore","Enabled":true}` |
| `Source/AILiveProject/AILiveProject.Build.cs` PublicDependencyModuleNames | 末尾加 `"SQLiteCore"` 与 `"OpenSSL"` |

`Source/AILiveProject/Private/SQLiteCoreLinkProbe.cpp` 作为临时 link probe 走过整个流程后整文件删除。

## OpenSSL `UI` 符号冲突（执行中遇坑）

第一次 UBT 全量构建报错：

```
ossl_typ.h(144,22): error C2365: 'UI': redefinition; currently defined as a 'namespace'
typedef struct ui_st UI;
ObjectMacros.h(923,11): note: see 'UI' definition
namespace UI
```

OpenSSL 1.1.1t 的 `typedef struct ui_st UI` 与 UE CoreUObject `ObjectMacros.h` 的 `namespace UI` 冲突。UE 自家 `Runtime/Online/SSL` 模块没有这个问题，因为其 `SSL.Build.cs` 仅依赖 `Core`、不引入 `CoreUObject`。我们的模块依赖整套 `Core / CoreUObject / Engine`，PCH 会将 `namespace UI` 注入所有 .cpp，导致直接 include `<openssl/evp.h>` 必撞。

解：在 OpenSSL 头 include 前 `#define UI OpenSSL_UI`，include 后 `#undef UI`。OpenSSL 头里所有 `UI` 用法被宏改名为 `OpenSSL_UI`，但 .lib 二进制符号（`EVP_sha256` 等）与 `UI` 类型无关、不受影响。

probe 文件最终形态：

```cpp
#include "SQLiteDatabase.h"

#define UI OpenSSL_UI
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#undef UI

namespace AILive_T0_LinkProbe
{
    void Probe()
    {
        FSQLiteDatabase Db;
        Db.Open(TEXT("test.db"), ESQLiteDatabaseOpenMode::ReadWriteCreate);
        const EVP_MD* Md = EVP_sha256();
        (void)Md;
    }
}
```

> **后续 T3 落 ComputeEventHash 时复用同一模式**：业务代码不要直接在 .cpp 里 include `<openssl/...>`，要走 `Util/` 下封装层（`Sha256Fingerprint` 函数已在 implementation §5.2bis 计划），`#define UI OpenSSL_UI` 这层 hack 集中在封装层一处。

## 验收实证

| # | 用例 | 实证 |
| --- | --- | --- |
| 1 | `.uproject` Plugins 数组含 SQLiteCore Enabled | 文件第 156–159 行追加成功；UBT link 通过即证 plugin 启用 + 二进制完整 |
| 2 | UBT 全量 `Build.bat AILiveProjectEditor Win64 Development` | `Result: Succeeded`，`[2/4] Link UnrealEditor-AILiveProject.lib` + `[3/4] Link UnrealEditor-AILiveProject.dll` 全通过 = OpenSSL `EVP_sha256` 与 SQLite `FSQLiteDatabase::Open` 符号解析成立 |
| 3 | Editor 加载 `Content/MyAssets/Levels/L_prison.umap` | `EditorStartupMap` 已配该关卡，Editor 自动加载；`get_scene_statistics` 返回 `BP_NPC_MH_Character_1_C..10_C` 各 1 个、`BP_Act01Director_C` `BP_Act02Director_C` `StoryScenarioDirector` 全在场，`navmesh_status: built` |
| 4 | 重启后 `mcp__monolith__monolith_status` 在线 | `{"version":"0.14.7","server_running":true,"server_port":9316,"total_actions":1160,"namespaces":14,"engine_version":"++UE5+Release-5.7-CL-51494982","project_name":"AILiveProject"}` + 18 个 Monolith module 全 `loaded:true` |
| 5 | probe 删除后增量构建仍通过 | `Invalidating makefile (source file removed)` → `[1/3] Link` `[2/3] Link` `Result: Succeeded`；删除 `EVP_sha256()` 调用后无 LNK 残留，证明依赖图正确建立 |

## 既有功能不退化

`get_scene_statistics` 返回（节选）：

- StaticMeshActor 3458 / GroupActor 442 / DecalActor 1314 — 关卡几何完整
- 10 个 BP_NPC_MH_Character_X_C 全部 spawn
- BP_Act01Director_C / BP_Act02Director_C / StoryScenarioDirector 各 1，AC_VisualOverrideManager 链路（NPC 子类）未受 Build.cs 改动影响
- navmesh built，PlayerStart 1 个，AbstractNavData 在场

CLAUDE.md 三条硬规则全部未触碰：未改 `bTickPhysicsAsync`、未改 `DefaultBuildSettings`、未动既有 GASP / Mover / VisualOverride / NPC BP 链路。

## 完成定义达成

- [x] `.uproject` Plugins 显式声明 SQLiteCore=Enabled
- [x] `Build.cs` 加 SQLiteCore + OpenSSL 依赖
- [x] UBT 全量构建通过（`Result: Succeeded`）
- [x] 临时 link probe 删除后增量构建仍通过
- [x] Editor PIE 能加载 `L_prison.umap`，10 NPC + Director 全在场
- [x] 重启后 `mcp__monolith__monolith_status` 返回 online

T0 完成。**不**意味着任何业务代码就绪——T1 起进入 `Memory/` 类型骨架与 `FNPCAgentConfig` 三层组合重整。

## 给 T1 / T3 的提示

1. `Source/AILiveProject/Public/Memory/` 目录在本任务**未**新建（implementation §8 step 1 的"空骨架"按 00_overview.md 偏离表归到 T1）。T1 第一个动作是新建该目录 + 三层 USTRUCT + `Memory/` 私有 .cpp 骨架。
2. T3 落 `ComputeEventHash` / `Sha256Fingerprint` 时，**统一封装到 `Util/Sha256Fingerprint.h+cpp`**，OpenSSL include 走 `#define UI OpenSSL_UI` 这层 hack 仅在该 .cpp 内，业务代码 only 见 `FString Sha256Fingerprint(const FString&)` API。implementation §5.2bis 提示该函数下沉到通用工具库——按那个位置落即可。
3. 工程内 grep `Sha1` / `FSHA1` 当前已为零（无遗留 SHA-1 引用），T9 §11 L2 验收用例可直接跑。
