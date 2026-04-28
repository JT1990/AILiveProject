#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "StoryScenarioDirector.generated.h"

class ATargetPoint;
class USightMemoryComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FStoryScenarioSimpleDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FStoryScenarioFailedDelegate, FString, Reason);

UCLASS(BlueprintType, Blueprintable)
class AILIVEPROJECT_API AStoryScenarioDirector : public AActor
{
	GENERATED_BODY()

public:
	AStoryScenarioDirector();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Input")
	bool bEnableKeyTrigger = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Input")
	FKey StartKey;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Actors")
	TSubclassOf<AActor> NPC1Class;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Actors")
	TSubclassOf<AActor> NPC2Class;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Movement")
	float MeetingDistance = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Movement")
	float ArrivalAcceptanceRadius = 180.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Movement")
	float MovementTimeoutSeconds = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Movement")
	float ArrivalSettleSeconds = 0.75f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Dialogue")
	float DialogueLineDelaySeconds = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Dialogue")
	float PostDialogueHoldSeconds = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Dialogue")
	TArray<FString> DialogueLines;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Dialogue")
	FString NPC1VoiceId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Dialogue")
	FString NPC2VoiceId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Dialogue")
	FString Endpoint;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Dialogue")
	FName A2FProviderName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Dialogue")
	bool bRequireSpeechPlayback = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Debug")
	bool bDebugPrintScreen = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|StoryScenario|Debug")
	bool bEnableNPC1SightScreenDebug = true;

	UPROPERTY(BlueprintAssignable, Category="AILiveProject|StoryScenario")
	FStoryScenarioSimpleDelegate OnArrived;

	UPROPERTY(BlueprintAssignable, Category="AILiveProject|StoryScenario")
	FStoryScenarioSimpleDelegate OnLeft;

	UPROPERTY(BlueprintAssignable, Category="AILiveProject|StoryScenario")
	FStoryScenarioFailedDelegate OnFailed;

	UFUNCTION(BlueprintCallable, Category="AILiveProject|StoryScenario")
	bool BeginSceneFromConfiguredClasses();

	UFUNCTION(BlueprintCallable, Category="AILiveProject|StoryScenario")
	bool BeginScene(AActor* InNPC1, AActor* InNPC2, float InMeetingDistance);

	UFUNCTION(BlueprintCallable, Category="AILiveProject|StoryScenario")
	bool EndScene();

	UFUNCTION(BlueprintCallable, Category="AILiveProject|StoryScenario")
	void CancelScene();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="AILiveProject|StoryScenario")
	bool IsSceneRunning() const { return bSceneRunning; }

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	enum class EStoryScenarioState : uint8
	{
		Idle,
		MovingToMeeting,
		Dialogue,
		Returning
	};

	static constexpr float MovementPollInterval = 0.2f;

	bool ResolveDefaultClasses();
	AActor* ResolveActorOfClass(TSubclassOf<AActor> ActorClass) const;
	bool EnsureTargetActors();
	ATargetPoint* SpawnScenarioTarget(FName TargetName) const;
	bool InvokeMoveAndLookAt(AActor* Mover, AActor* MoveTarget, AActor* LookTarget) const;
	bool HasNPC2ReachedTarget(AActor* Target) const;
	bool IsNPC2MoveIdle() const;
	bool ReadNPC2BoolProperty(FName PropertyName, bool& bOutValue) const;

	void PollMoveToMeeting();
	void PollReturn();
	void HandleArrivedSettled();
	void StartDialogue();
	void SpeakDialogueLine();
	void StartReturn();
	void CompleteScene();
	void FailScene(const FString& Reason);
	void ClearScenarioTimers();

	void EnableNPC1SightDebug();
	void RestoreNPC1SightDebug();
	void DebugMessage(const FString& Message, const FLinearColor& Color = FLinearColor::White) const;

	UPROPERTY(Transient)
	TObjectPtr<AActor> NPC1Actor;

	UPROPERTY(Transient)
	TObjectPtr<AActor> NPC2Actor;

	UPROPERTY(Transient)
	TObjectPtr<ATargetPoint> MeetingTarget;

	UPROPERTY(Transient)
	TObjectPtr<ATargetPoint> HomeMoveTarget;

	UPROPERTY(Transient)
	TObjectPtr<ATargetPoint> HomeLookTarget;

	TWeakObjectPtr<USightMemoryComponent> NPC1SightMemory;
	bool bHadNPC1SightMemoryOriginalDebug = false;
	bool bNPC1SightMemoryOriginalDebug = false;

	FVector NPC2OriginalLocation = FVector::ZeroVector;
	FRotator NPC2OriginalRotation = FRotator::ZeroRotator;
	FString CachedApiKey;
	int32 DialogueIndex = 0;
	float MoveWaitElapsedSeconds = 0.0f;
	bool bSceneRunning = false;
	EStoryScenarioState SceneState = EStoryScenarioState::Idle;

	FTimerHandle MovementPollTimer;
	FTimerHandle SettleTimer;
	FTimerHandle DialogueTimer;
};
