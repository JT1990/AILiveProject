#include "StoryScenarioDirector.h"

#include "AIController.h"
#include "MinimaxACELibrary.h"
#include "SightMemoryComponent.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Navigation/PathFollowingComponent.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogStoryScenario, Log, All);

AStoryScenarioDirector::AStoryScenarioDirector()
{
	PrimaryActorTick.bCanEverTick = true;

	StartKey = EKeys::O;
	NPC1VoiceId = TEXT("male-qn-qingse");
	NPC2VoiceId = TEXT("male-qn-jingying");
	Endpoint = TEXT("https://api.minimaxi.com/v1/t2a_v2");
	A2FProviderName = FName(TEXT("LocalA2F-James"));

	DialogueLines =
	{
		TEXT("我们两个结盟吧。"),
		TEXT("好的。"),
		TEXT("一言为定。"),
		TEXT("一言为定。"),
		TEXT("回见。"),
		TEXT("待会见。")
	};
}

void AStoryScenarioDirector::BeginPlay()
{
	Super::BeginPlay();
	UMinimaxACELibrary::PrewarmA2F(A2FProviderName);
}

void AStoryScenarioDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bEnableKeyTrigger || bSceneRunning)
	{
		return;
	}

	APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0);
	if (!PC)
	{
		return;
	}

	const bool bStartPressed = StartKey.IsValid() && PC->WasInputKeyJustPressed(StartKey);
	if (bStartPressed)
	{
		BeginSceneFromConfiguredClasses();
	}
}

void AStoryScenarioDirector::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearScenarioTimers();
	RestoreNPC1SightDebug();
	Super::EndPlay(EndPlayReason);
}

bool AStoryScenarioDirector::BeginSceneFromConfiguredClasses()
{
	if (!ResolveDefaultClasses())
	{
		FailScene(TEXT("NPC classes are not configured"));
		return false;
	}

	AActor* ResolvedNPC1 = ResolveActorOfClass(NPC1Class);
	AActor* ResolvedNPC2 = ResolveActorOfClass(NPC2Class);
	return BeginScene(ResolvedNPC1, ResolvedNPC2, MeetingDistance);
}

bool AStoryScenarioDirector::BeginScene(AActor* InNPC1, AActor* InNPC2, float InMeetingDistance)
{
	if (bSceneRunning)
	{
		DebugMessage(TEXT("[Scene] already running"), FLinearColor::Yellow);
		return false;
	}

	if (!IsValid(InNPC1) || !IsValid(InNPC2))
	{
		FailScene(FString::Printf(TEXT("invalid NPCs: NPC1=%s NPC2=%s"), *GetNameSafe(InNPC1), *GetNameSafe(InNPC2)));
		return false;
	}

	if (!EnsureTargetActors())
	{
		FailScene(TEXT("failed to create scenario target actors"));
		return false;
	}

	NPC1Actor = InNPC1;
	NPC2Actor = InNPC2;
	NPC2OriginalLocation = NPC2Actor->GetActorLocation();
	NPC2OriginalRotation = NPC2Actor->GetActorRotation();

	const float ClampedDistance = FMath::Max(50.0f, InMeetingDistance);
	const FVector MeetingLocation = NPC1Actor->GetActorLocation() + NPC1Actor->GetActorForwardVector() * ClampedDistance;
	MeetingTarget->SetActorLocation(MeetingLocation);
	MeetingTarget->SetActorRotation((NPC1Actor->GetActorLocation() - MeetingLocation).Rotation());

	HomeMoveTarget->SetActorLocation(NPC2OriginalLocation);
	HomeMoveTarget->SetActorRotation(NPC2OriginalRotation);
	HomeLookTarget->SetActorLocation(NPC2OriginalLocation + NPC2OriginalRotation.Vector() * 200.0f);
	HomeLookTarget->SetActorRotation(NPC2OriginalRotation);

	CachedApiKey.Reset();
	DialogueIndex = 0;
	MoveWaitElapsedSeconds = 0.0f;
	bSceneRunning = true;
	SceneState = EStoryScenarioState::MovingToMeeting;

	EnableNPC1SightDebug();

	if (!InvokeMoveAndLookAt(NPC2Actor, MeetingTarget, NPC1Actor))
	{
		FailScene(TEXT("MoveAndLookAt arrival failed"));
		return false;
	}

	DebugMessage(FString::Printf(TEXT("[Scene] NPC2 moving to NPC1 front: %s"), *MeetingLocation.ToString()), FLinearColor::Green);
	GetWorldTimerManager().SetTimer(
		MovementPollTimer,
		this,
		&AStoryScenarioDirector::PollMoveToMeeting,
		MovementPollInterval,
		true);
	return true;
}

bool AStoryScenarioDirector::EndScene()
{
	if (!bSceneRunning)
	{
		return false;
	}

	StartReturn();
	return true;
}

void AStoryScenarioDirector::CancelScene()
{
	if (!bSceneRunning)
	{
		return;
	}

	ClearScenarioTimers();
	RestoreNPC1SightDebug();
	SceneState = EStoryScenarioState::Idle;
	bSceneRunning = false;
	DebugMessage(TEXT("[Scene] cancelled"), FLinearColor::Yellow);
}

bool AStoryScenarioDirector::ResolveDefaultClasses()
{
	if (!NPC1Class)
	{
		NPC1Class = LoadClass<AActor>(
			nullptr,
			TEXT("/Game/Blueprints/NPCs/BP_NPC_MH_Character_1.BP_NPC_MH_Character_1_C"));
	}

	if (!NPC2Class)
	{
		NPC2Class = LoadClass<AActor>(
			nullptr,
			TEXT("/Game/Blueprints/NPCs/BP_NPC_MH_Character_2.BP_NPC_MH_Character_2_C"));
	}

	return NPC1Class != nullptr && NPC2Class != nullptr;
}

AActor* AStoryScenarioDirector::ResolveActorOfClass(TSubclassOf<AActor> ActorClass) const
{
	if (!ActorClass)
	{
		return nullptr;
	}

	return UGameplayStatics::GetActorOfClass(this, ActorClass);
}

bool AStoryScenarioDirector::EnsureTargetActors()
{
	if (!MeetingTarget)
	{
		MeetingTarget = SpawnScenarioTarget(TEXT("StoryScenario_MeetingTarget"));
	}
	if (!HomeMoveTarget)
	{
		HomeMoveTarget = SpawnScenarioTarget(TEXT("StoryScenario_HomeMoveTarget"));
	}
	if (!HomeLookTarget)
	{
		HomeLookTarget = SpawnScenarioTarget(TEXT("StoryScenario_HomeLookTarget"));
	}

	return MeetingTarget != nullptr && HomeMoveTarget != nullptr && HomeLookTarget != nullptr;
}

ATargetPoint* AStoryScenarioDirector::SpawnScenarioTarget(FName TargetName) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = const_cast<AStoryScenarioDirector*>(this);
	SpawnParams.Name = TargetName;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ATargetPoint* Target = World->SpawnActor<ATargetPoint>(
		ATargetPoint::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParams);
	if (Target)
	{
		Target->SetActorHiddenInGame(true);
		Target->SetActorEnableCollision(false);
		Target->SetActorTickEnabled(false);
	}
	return Target;
}

bool AStoryScenarioDirector::InvokeMoveAndLookAt(AActor* Mover, AActor* MoveTarget, AActor* LookTarget) const
{
	if (!IsValid(Mover) || !IsValid(MoveTarget) || !IsValid(LookTarget))
	{
		return false;
	}

	UFunction* MoveFunction = Mover->FindFunction(TEXT("MoveAndLookAt"));
	if (!MoveFunction)
	{
		UE_LOG(LogStoryScenario, Warning, TEXT("MoveAndLookAt function missing on %s"), *GetNameSafe(Mover));
		return false;
	}

	FObjectPropertyBase* MoveTargetProperty = FindFProperty<FObjectPropertyBase>(MoveFunction, TEXT("MoveTarget"));
	FObjectPropertyBase* LookTargetProperty = FindFProperty<FObjectPropertyBase>(MoveFunction, TEXT("LookTarget"));
	FBoolProperty* SucceededProperty = FindFProperty<FBoolProperty>(MoveFunction, TEXT("bSucceeded"));
	if (!MoveTargetProperty || !LookTargetProperty || !SucceededProperty)
	{
		UE_LOG(LogStoryScenario, Warning,
			TEXT("MoveAndLookAt signature mismatch on %s. Expected MoveTarget, LookTarget, bSucceeded."),
			*GetNameSafe(Mover));
		return false;
	}

	uint8* Params = static_cast<uint8*>(FMemory_Alloca(MoveFunction->ParmsSize));
	FMemory::Memzero(Params, MoveFunction->ParmsSize);
	MoveTargetProperty->SetObjectPropertyValue_InContainer(Params, MoveTarget);
	LookTargetProperty->SetObjectPropertyValue_InContainer(Params, LookTarget);

	Mover->ProcessEvent(MoveFunction, Params);
	return SucceededProperty->GetPropertyValue_InContainer(Params);
}

bool AStoryScenarioDirector::HasNPC2ReachedTarget(AActor* Target) const
{
	if (!IsValid(NPC2Actor) || !IsValid(Target))
	{
		return false;
	}

	const float Distance2D = FVector::Dist2D(NPC2Actor->GetActorLocation(), Target->GetActorLocation());
	if (Distance2D > ArrivalAcceptanceRadius)
	{
		return false;
	}

	bool bAligningLook = false;
	if (ReadNPC2BoolProperty(TEXT("bAligningLook"), bAligningLook) && bAligningLook)
	{
		return true;
	}

	return IsNPC2MoveIdle();
}

bool AStoryScenarioDirector::IsNPC2MoveIdle() const
{
	const APawn* NPC2Pawn = Cast<APawn>(NPC2Actor);
	if (!NPC2Pawn)
	{
		return true;
	}

	const AAIController* AIC = Cast<AAIController>(NPC2Pawn->GetController());
	if (!AIC)
	{
		return true;
	}

	return AIC->GetMoveStatus() == EPathFollowingStatus::Idle;
}

bool AStoryScenarioDirector::ReadNPC2BoolProperty(FName PropertyName, bool& bOutValue) const
{
	if (!IsValid(NPC2Actor))
	{
		return false;
	}

	const FBoolProperty* BoolProperty = FindFProperty<FBoolProperty>(NPC2Actor->GetClass(), PropertyName);
	if (!BoolProperty)
	{
		return false;
	}

	bOutValue = BoolProperty->GetPropertyValue_InContainer(NPC2Actor);
	return true;
}

void AStoryScenarioDirector::PollMoveToMeeting()
{
	MoveWaitElapsedSeconds += MovementPollInterval;
	if (MoveWaitElapsedSeconds > MovementTimeoutSeconds)
	{
		FailScene(TEXT("arrival timeout"));
		return;
	}

	if (HasNPC2ReachedTarget(MeetingTarget))
	{
		GetWorldTimerManager().ClearTimer(MovementPollTimer);
		GetWorldTimerManager().SetTimer(SettleTimer, this, &AStoryScenarioDirector::HandleArrivedSettled, ArrivalSettleSeconds, false);
	}
}

void AStoryScenarioDirector::PollReturn()
{
	MoveWaitElapsedSeconds += MovementPollInterval;
	if (MoveWaitElapsedSeconds > MovementTimeoutSeconds)
	{
		FailScene(TEXT("return timeout"));
		return;
	}

	if (HasNPC2ReachedTarget(HomeMoveTarget))
	{
		GetWorldTimerManager().ClearTimer(MovementPollTimer);
		CompleteScene();
	}
}

void AStoryScenarioDirector::HandleArrivedSettled()
{
	OnArrived.Broadcast();
	StartDialogue();
}

void AStoryScenarioDirector::StartDialogue()
{
	SceneState = EStoryScenarioState::Dialogue;
	DialogueIndex = 0;
	CachedApiKey = UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv();

	if (CachedApiKey.IsEmpty())
	{
		const FString Reason = TEXT("MiniMax API key missing; continuing without speech playback");
		if (bRequireSpeechPlayback)
		{
			FailScene(Reason);
			return;
		}
		DebugMessage(FString::Printf(TEXT("[Scene] %s"), *Reason), FLinearColor::Yellow);
	}

	SpeakDialogueLine();
}

void AStoryScenarioDirector::SpeakDialogueLine()
{
	if (!bSceneRunning || SceneState != EStoryScenarioState::Dialogue)
	{
		return;
	}

	if (!DialogueLines.IsValidIndex(DialogueIndex))
	{
		GetWorldTimerManager().SetTimer(DialogueTimer, this, &AStoryScenarioDirector::StartReturn, PostDialogueHoldSeconds, false);
		return;
	}

	AActor* Speaker = (DialogueIndex % 2 == 0) ? NPC2Actor.Get() : NPC1Actor.Get();
	const FString VoiceId = (DialogueIndex % 2 == 0) ? NPC2VoiceId : NPC1VoiceId;
	const FString Line = DialogueLines[DialogueIndex];
	DebugMessage(FString::Printf(TEXT("[Scene] %s: %s"), *GetNameSafe(Speaker), *Line), FLinearColor::White);

	if (!CachedApiKey.IsEmpty())
	{
		const bool bSpeechStarted = UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnWithNoise(
			this,
			Speaker,
			Line,
			CachedApiKey,
			VoiceId,
			Endpoint,
			A2FProviderName);
		if (!bSpeechStarted)
		{
			const FString Reason = FString::Printf(TEXT("speech playback failed for %s"), *GetNameSafe(Speaker));
			if (bRequireSpeechPlayback)
			{
				FailScene(Reason);
				return;
			}
			DebugMessage(FString::Printf(TEXT("[Scene] %s; continuing"), *Reason), FLinearColor::Yellow);
		}
	}

	++DialogueIndex;
	GetWorldTimerManager().SetTimer(DialogueTimer, this, &AStoryScenarioDirector::SpeakDialogueLine, DialogueLineDelaySeconds, false);
}

void AStoryScenarioDirector::StartReturn()
{
	if (!bSceneRunning)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(DialogueTimer);
	GetWorldTimerManager().ClearTimer(SettleTimer);
	GetWorldTimerManager().ClearTimer(MovementPollTimer);

	SceneState = EStoryScenarioState::Returning;
	MoveWaitElapsedSeconds = 0.0f;

	if (!InvokeMoveAndLookAt(NPC2Actor, HomeMoveTarget, HomeLookTarget))
	{
		FailScene(TEXT("MoveAndLookAt return failed"));
		return;
	}

	DebugMessage(TEXT("[Scene] NPC2 returning home"), FLinearColor::Green);
	GetWorldTimerManager().SetTimer(
		MovementPollTimer,
		this,
		&AStoryScenarioDirector::PollReturn,
		MovementPollInterval,
		true);
}

void AStoryScenarioDirector::CompleteScene()
{
	ClearScenarioTimers();
	OnLeft.Broadcast();
	DebugMessage(TEXT("[Scene] complete; expect SightMemory EXIT for NPC2"), FLinearColor::Green);
	RestoreNPC1SightDebug();
	SceneState = EStoryScenarioState::Idle;
	bSceneRunning = false;
}

void AStoryScenarioDirector::FailScene(const FString& Reason)
{
	ClearScenarioTimers();
	OnFailed.Broadcast(Reason);
	DebugMessage(FString::Printf(TEXT("[Scene] failed: %s"), *Reason), FLinearColor::Red);
	RestoreNPC1SightDebug();
	SceneState = EStoryScenarioState::Idle;
	bSceneRunning = false;
}

void AStoryScenarioDirector::ClearScenarioTimers()
{
	if (UWorld* World = GetWorld())
	{
		FTimerManager& TimerManager = World->GetTimerManager();
		TimerManager.ClearTimer(MovementPollTimer);
		TimerManager.ClearTimer(SettleTimer);
		TimerManager.ClearTimer(DialogueTimer);
	}
}

void AStoryScenarioDirector::EnableNPC1SightDebug()
{
	if (!bEnableNPC1SightScreenDebug)
	{
		return;
	}

	APawn* NPC1Pawn = Cast<APawn>(NPC1Actor);
	if (!NPC1Pawn)
	{
		return;
	}

	AController* Controller = NPC1Pawn->GetController();
	if (!Controller)
	{
		return;
	}

	USightMemoryComponent* SightMemory = Controller->FindComponentByClass<USightMemoryComponent>();
	if (!SightMemory)
	{
		UE_LOG(LogStoryScenario, Warning, TEXT("SightMemoryComponent missing on NPC1 controller %s"), *GetNameSafe(Controller));
		return;
	}

	NPC1SightMemory = SightMemory;
	bHadNPC1SightMemoryOriginalDebug = true;
	bNPC1SightMemoryOriginalDebug = SightMemory->bDebugPrintScreen;
	SightMemory->bDebugPrintScreen = true;
}

void AStoryScenarioDirector::RestoreNPC1SightDebug()
{
	if (bHadNPC1SightMemoryOriginalDebug)
	{
		if (USightMemoryComponent* SightMemory = NPC1SightMemory.Get())
		{
			SightMemory->bDebugPrintScreen = bNPC1SightMemoryOriginalDebug;
		}
	}

	NPC1SightMemory.Reset();
	bHadNPC1SightMemoryOriginalDebug = false;
	bNPC1SightMemoryOriginalDebug = false;
}

void AStoryScenarioDirector::DebugMessage(const FString& Message, const FLinearColor& Color) const
{
	UE_LOG(LogStoryScenario, Log, TEXT("%s"), *Message);
	if (bDebugPrintScreen)
	{
		UKismetSystemLibrary::PrintString(this, Message, true, false, Color, 4.0f);
	}
}
