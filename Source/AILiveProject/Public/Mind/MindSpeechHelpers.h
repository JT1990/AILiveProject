#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MindSpeechHelpers.generated.h"

/**
 * P0-B+ 强约束统一入口：Speak 必须驱动可见 MetaHuman child actor，不是壳 Pawn。
 * 否则 ACE 组件挂壳 Pawn 但 Face AnimBP 在 child 上，curve 读不到——口型静默失败。
 *
 * T02 仅落骨架：cpp 默认返 owner（让 T05 / T05.5 编译期跑通）；
 * T06 实现完整解析（VisualOverride child actor → 否则 owner → 否则 nullptr 警告）。
 */
UCLASS()
class AILIVEPROJECT_API UMindSpeechHelpers : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind")
	static AActor* ResolveSpeechActor(AActor* MindOwner);
};
