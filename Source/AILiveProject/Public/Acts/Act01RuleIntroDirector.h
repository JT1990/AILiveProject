#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "Act01RuleIntroDirector.generated.h"

class AMediaPlate;
class USceneComponent;
class UEnvQuery;
struct IConsoleCommand;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAct01CompletedDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAct01FailedDelegate, FString, Reason);

UENUM(BlueprintType)
enum class EAct01State : uint8
{
	Idle,
	OpeningDoors,
	NPCsMovingToTV,
	PlayingVideo,
};

UCLASS(BlueprintType, Blueprintable)
class AILIVEPROJECT_API AAct01RuleIntroDirector : public AActor
{
	GENERATED_BODY()

public:
	AAct01RuleIntroDirector();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Input")
	bool bEnableKeyTrigger = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Input")
	FKey StartKey;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSubclassOf<AActor> NPCMoverClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSoftObjectPtr<AActor> NavTargetActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSoftObjectPtr<AActor> NavLookTargetActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSoftObjectPtr<AMediaPlate> MediaPlateActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSubclassOf<AActor> NavTargetClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSubclassOf<AActor> NavLookTargetClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	FName NavTargetActorLabel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	FName NavLookTargetActorLabel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Doors")
	TArray<FName> CellDoorActorNames;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Doors")
	FName CellDoorMeshComponentName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Doors")
	FRotator DoorOpenRelativeRotation;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Doors")
	float DoorAnimationSeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Movement")
	TObjectPtr<UEnvQuery> ScatterQueryAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Movement")
	float MovementTimeoutSeconds = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Movement")
	float ArrivalAcceptanceRadius = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Movement")
	float ArrivalSettleSeconds = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Movement")
	float ScatterDispatchDelaySeconds = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Video")
	float VideoFallbackTimeoutSeconds = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Debug")
	bool bDebugPrintScreen = true;

	UPROPERTY(BlueprintAssignable, Category = "AILiveProject|Act01")
	FAct01CompletedDelegate OnAct01Completed;

	UPROPERTY(BlueprintAssignable, Category = "AILiveProject|Act01")
	FAct01FailedDelegate OnAct01Failed;

	UFUNCTION(BlueprintCallable, Category = "AILiveProject|Act01")
	bool BeginAct01();

	UFUNCTION(BlueprintCallable, Category = "AILiveProject|Act01")
	void CancelAct01();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "AILiveProject|Act01")
	bool IsAct01Running() const { return SceneState != EAct01State::Idle; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "AILiveProject|Act01")
	EAct01State GetSceneState() const { return SceneState; }

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	struct FCachedDoor
	{
		TWeakObjectPtr<USceneComponent> Component;
		FRotator ClosedRelativeRotation = FRotator::ZeroRotator;
		FRotator ClosedWorldRotation = FRotator::ZeroRotator;
	};

	void CacheInitialNPCTransforms();
	void CacheCellDoorComponents();
	USceneComponent* FindCellDoorComponent(AActor* CellActor) const;
	AActor* ResolveActorByClassAndLabel(TSubclassOf<AActor> InClass, FName Label) const;
	void ResolveTargetActors(bool bLogResolution);

	void RegisterDebugConsoleCommands();
	void UnregisterDebugConsoleCommands();
	void BeginAct01Console();

	void StartOpeningDoors();
	void TickOpenDoors(float DeltaSeconds);
	void ResetDoorsToClosed();

	void StartNPCMovement();
	void TickNPCMovement(float DeltaSeconds);
	bool AreAllNPCsIdle() const;

	void StartVideo();
	void TickVideoFallback(float DeltaSeconds);
	void UnbindVideoDelegate();

	UFUNCTION()
	void HandleVideoEnded();

	void CompleteAct01();
	void FailAct01(const FString& Reason);
	void ResetToInitialPositions();

	void DebugMessage(const FString& Message, const FLinearColor& Color = FLinearColor::White) const;

	UPROPERTY(Transient)
	TObjectPtr<AActor> NavTargetCached;

	UPROPERTY(Transient)
	TObjectPtr<AActor> NavLookTargetCached;

	UPROPERTY(Transient)
	TObjectPtr<AMediaPlate> MediaPlateCached;

	TMap<TWeakObjectPtr<AActor>, FTransform> InitialNPCTransforms;
	TArray<FCachedDoor> CachedDoors;

	float DoorAnimationElapsed = 0.0f;
	float MovementElapsed = 0.0f;
	float MovementSettleElapsed = 0.0f;
	float VideoElapsed = 0.0f;
	float HeartbeatElapsed = 0.0f;
	bool bArrivalSettling = false;
	bool bVideoDelegateBound = false;
	EAct01State SceneState = EAct01State::Idle;

	IConsoleCommand* StartCommand = nullptr;
	IConsoleCommand* CancelCommand = nullptr;
};
