#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SmartObjectRuntime.h"
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
