#pragma once

// =============================================================================
// 中文教学：AILiveProjectScatterMover.h —— 「批量散布 NPC 到指定区域」工具
//
// 这是什么：
//   一个 BP 静态函数库。Act01 / Act02 用它在「让 10 个 NPC 围着电视站好」时
//   一次性派发：跑 EQS 查询取一组散布点 → 每个 NPC 调它自己的 BP 函数
//   MoveAndLookAtLocation(Loc, LookTarget) 走过去。
//
// 关键 UE 概念：
//
//   1) UEnvQuery
//      EQS（Environment Query System）查询模板。在编辑器里用图形化界面定义
//      「找点的规则」（生成器：Donut / Grid / Circle；测试器：距离、可见性、
//      Trace 等过滤打分）。运行时调用 RunEQSQuery 触发，回调取结果。
//
//   2) NPCs[i] 调自己的 BP 函数
//      本工具不假设 NPC 是哪个具体类，只要它的 BP 实现了 MoveAndLookAtLocation
//      就能驱动。这种「duck typing」做法在 UE 很常见 —— 用反射查函数 +
//      ProcessEvent 调用。比 cast 到具体类灵活。
// =============================================================================

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AILiveProjectScatterMover.generated.h"

class UEnvQuery;

UCLASS()
class AILIVEPROJECT_API UAILiveProjectScatterMover : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/**
	 * 围绕 CenterActor 跑 EQS 查询，把返回点按索引派发给 NPCs，每个 NPC 调用其
	 * BP 函数 MoveAndLookAtLocation(Loc, LookTarget) 走过去。EQS 异步，调用方
	 * 触发一次即可，回调里完成派发。
	 *
	 * NPCs 数大于 EQS 返回点数时，超出部分用最后一个点重复填（记 warning）。
	 * NPCs 数小于 EQS 返回点数时，丢掉尾部点。
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Scatter",
		meta = (WorldContext = "WorldContextObject",
			DisplayName = "Scatter NPCs Around Target"))
	static void ScatterNPCsAroundTarget(
		UEnvQuery* QueryAsset,
		AActor* CenterActor,
		const TArray<AActor*>& NPCs,
		AActor* LookTarget,
		UObject* WorldContextObject);
};
