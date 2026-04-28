#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "Perception/AIPerceptionTypes.h"
#include "UObject/WeakObjectPtr.h"
#include "SightMemoryComponent.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSightMemory, Log, All);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSightEnterDelegate, AActor*, Other, FVector, Location);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSightExitDelegate, AActor*, Other, FVector, LastSeenLocation);

UCLASS(ClassGroup=(AILiveProject), meta=(BlueprintSpawnableComponent), Blueprintable)
class AILIVEPROJECT_API USightMemoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USightMemoryComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|SightMemory")
	bool bDebugPrintScreen = false;

	UPROPERTY(BlueprintAssignable, Category="AILiveProject|SightMemory")
	FOnSightEnterDelegate OnSightEnter;

	UPROPERTY(BlueprintAssignable, Category="AILiveProject|SightMemory")
	FOnSightExitDelegate OnSightExit;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="AILiveProject|SightMemory")
	bool GetLastSeenLocation(AActor* Target, FVector& OutLocation) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="AILiveProject|SightMemory")
	bool IsCurrentlyVisible(AActor* Target) const;

	UFUNCTION(BlueprintCallable, Category="AILiveProject|SightMemory")
	void GetCurrentlyVisibleActors(TArray<AActor*>& OutActors) const;

	UFUNCTION(BlueprintCallable, Category="AILiveProject|SightMemory")
	void GetAllLastSeenLocations(TMap<AActor*, FVector>& OutLocations) const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void OnPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus);

	void AttemptBindPerceptionDelegate();

private:
	TMap<TWeakObjectPtr<AActor>, FVector> LastSeenLocations;
	TSet<TWeakObjectPtr<AActor>> CurrentlyVisibleActors;

	bool bDelegateBound = false;
	int32 BindRetryCount = 0;
	static constexpr int32 MaxBindRetry = 30;
	static constexpr float BindRetryInterval = 0.1f;
	FTimerHandle BindRetryHandle;
};
