#pragma once

// =============================================================================
// 中文教学：AILiveProjectPerceptionLogger.h —— AI 感知（视/听）信息收集 + BP 工具
//
// 这是什么：
//   一个 BP 函数库，给 AIController 暴露「我此时此刻看到/听到了什么」的查询入口。
//   底层走 UE 的 AIPerception 系统（UAIPerceptionComponent + AISense_Sight/Hearing）。
//   再额外封了 SmartObject 申领工具（解决 BP 里 Wildcard pin 不易用的问题）。
//
// 两个数据结构：
//   - FPerceivedAgentInfo ：视野感知到的另一 actor 的快照（距离/方位/相对偏航/年龄）
//   - FHeardSoundInfo     ：听到的噪声事件（位置/响度/方位/年龄）
//
// 关键 UE 概念：
//
//   1) UAIPerceptionComponent
//      AIController 上挂这个组件就能感知世界。组件订阅多种 AISense_*（Sight /
//      Hearing / Damage / Touch / Team / Prediction）。配置好 Config 资产即可工作。
//
//   2) FAIStimulus
//      一次感知刺激的快照：位置、年龄、是否当前可感知（被遮挡时为 false）。
//
//   3) RelativeYawDeg：[-180, 180]
//      相对 perceiver forward 方向的偏航角。0=正前，+90=正右，-90=正左，
//      ±180=正后。配合 NPC prompt「在你右侧 30 度有人发声」语义。
//
//   4) FSmartObjectClaimHandle
//      SmartObject 是 UE 的「场景智能交互点」框架：椅子、按钮、咖啡机等可被
//      NPC 使用的物件。claim handle 表示「我占用这个 slot」的凭据。
// =============================================================================

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SmartObjectRuntime.h"             // FSmartObjectClaimHandle
#include "AILiveProjectPerceptionLogger.generated.h"

class AAIController;
class UChildActorComponent;

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FPerceivedAgentInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	FName Identity;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	float DistanceCm = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	FRotator DirectionFromPerceiver = FRotator::ZeroRotator;

	/** 相对 Perceiver forward 的偏航角，[-180, 180]。0=正前，+90=正右，-90=正左，±180=正后。 */
	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	float RelativeYawDeg = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	bool bCurrentlySensed = false;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	float StimulusAge = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	FVector LastStimulusLocation = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FHeardSoundInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	FName SourceIdentity;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	float DistanceCm = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	FRotator DirectionFromPerceiver = FRotator::ZeroRotator;

	/** 相对 Perceiver forward 的偏航角，[-180, 180]。0=正前，+90=正右，-90=正左，±180=正后。 */
	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	float RelativeYawDeg = 0.f;

	/** 噪声事件发生时的世界坐标（不是 source actor 当前位置）。 */
	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	FVector StimulusLocation = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	float Loudness = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Perception")
	float StimulusAge = 0.f;
};

UCLASS()
class AILIVEPROJECT_API UAILiveProjectPerceptionLogger : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "AILive|Perception",
		meta = (WorldContext = "WorldContextObject",
			DisplayName = "Gather Sight Perception"))
	static TArray<FPerceivedAgentInfo> GatherSightPerception(
		AAIController* Perceiver,
		UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "AILive|Perception",
		meta = (WorldContext = "WorldContextObject",
			DisplayName = "Log Perception To Output"))
	static int32 LogPerceptionToOutput(
		AAIController* Perceiver,
		FString Tag,
		UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "AILive|Perception",
		meta = (WorldContext = "WorldContextObject",
			DisplayName = "Gather Hearing Perception"))
	static TArray<FHeardSoundInfo> GatherHearingPerception(
		AAIController* Perceiver,
		UObject* WorldContextObject);

	UFUNCTION(BlueprintCallable, Category = "AILive|Perception",
		meta = (WorldContext = "WorldContextObject",
			DisplayName = "Log Hearing Perception To Output"))
	static int32 LogHearingPerceptionToOutput(
		AAIController* Perceiver,
		FString Tag,
		UObject* WorldContextObject);

	/** UChildActorComponent::GetChildActor() 的 BP 包装（原 native getter 不是 UFUNCTION）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "AILive|Util",
		meta = (DisplayName = "Get Child Actor Of"))
	static AActor* GetChildActorOf(UChildActorComponent* Component);

	/**
	 * 在指定的 SmartObject Actor 上查找首个匹配的可用 slot 并申领它。
	 * 一次性完成 FindSmartObjectsInActor + MarkSmartObjectSlotAsClaimed，
	 * 绕开 BP Array_Get wildcard pin 在 MCP 下不可用的问题。
	 * 返回的 ClaimHandle 可直接传给 MoveToAndUseSmartObjectWithGameplayInteraction。
	 * 失败时返回默认（无效）句柄，可用 IsValidSmartObjectClaimHandle 检测。
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|SmartObject",
		meta = (WorldContext = "WorldContextObject",
			DisplayName = "Claim First Slot In Actor"))
	static FSmartObjectClaimHandle ClaimFirstSlotInActor(
		AActor* SmartObjectActor,
		AActor* UserActor,
		UObject* WorldContextObject);
};
