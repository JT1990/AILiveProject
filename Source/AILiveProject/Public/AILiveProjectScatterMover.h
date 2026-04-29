#pragma once

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
