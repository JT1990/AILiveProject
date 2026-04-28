#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AILiveProjectPerceptionLogger.generated.h"

class AAIController;

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
};
