#include "Acts/Act01RuleIntroDirector.h"

#include "AIController.h"
#include "AILiveProjectScatterMover.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "MediaPlate.h"
#include "MediaPlateComponent.h"
#include "MediaPlayer.h"
#include "MediaPlaylist.h"
#include "MediaSource.h"
#include "Navigation/PathFollowingComponent.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogAct01, Log, All);

AAct01RuleIntroDirector::AAct01RuleIntroDirector()
{
	PrimaryActorTick.bCanEverTick = true;

	StartKey = EKeys::One;
	NavTargetActorLabel = TEXT("BP_NavTarget_1");
	NavLookTargetActorLabel = TEXT("BP_NavLookTarget_TV");
	CellDoorMeshComponentName = TEXT("SM_Door");
	DoorOpenRelativeRotation = FRotator(0.f, -120.f, 0.f);
	CellDoorActorNames =
		{
			TEXT("SM_blocking_prison_Cube_270"),
			TEXT("SM_blocking_prison_Cube_269"),
			TEXT("SM_blocking_prison_Cube_268"),
			TEXT("SM_blocking_prison_Cube_259"),
			TEXT("SM_blocking_prison_Cube_285"),
			TEXT("SM_blocking_prison_Cube_286"),
			TEXT("SM_blocking_prison_Cube_351"),
			TEXT("SM_blocking_prison_Cube_352"),
			TEXT("SM_blocking_prison_Cube_271"),
			TEXT("SM_blocking_prison_Cube_353"),
		};
}

void AAct01RuleIntroDirector::BeginPlay()
{
	Super::BeginPlay();

	if (!NPCMoverClass)
	{
		NPCMoverClass = LoadClass<AActor>(
			nullptr,
			TEXT("/Game/Blueprints/SandboxCharacter_Mover.SandboxCharacter_Mover_C"));
	}
	if (!NavTargetClass)
	{
		NavTargetClass = LoadClass<AActor>(
			nullptr,
			TEXT("/Game/Blueprints/Markers/BP_NavTarget.BP_NavTarget_C"));
	}
	if (!NavLookTargetClass)
	{
		NavLookTargetClass = LoadClass<AActor>(
			nullptr,
			TEXT("/Game/Blueprints/Markers/BP_NavLookTarget.BP_NavLookTarget_C"));
	}
	if (!ScatterQueryAsset)
	{
		ScatterQueryAsset = LoadObject<UEnvQuery>(
			nullptr,
			TEXT("/Game/AI/EQS/EQS_ScatterAroundTarget.EQS_ScatterAroundTarget"));
	}

	CacheInitialNPCTransforms();
	CacheCellDoorComponents();
	ResolveTargetActors(/*bLogResolution=*/true);
	RegisterDebugConsoleCommands();

	UE_LOG(LogAct01, Log, TEXT("[Act01] BeginPlay cached %d NPC(s), %d cell door(s)"),
		   InitialNPCTransforms.Num(), CachedDoors.Num());
}

void AAct01RuleIntroDirector::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterDebugConsoleCommands();
	UnbindVideoDelegate();
	Super::EndPlay(EndPlayReason);
}

void AAct01RuleIntroDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	HeartbeatElapsed += DeltaSeconds;
	if (HeartbeatElapsed >= 5.0f)
	{
		HeartbeatElapsed = 0.0f;
		APlayerController *HBPC = UGameplayStatics::GetPlayerController(this, 0);
		UE_LOG(LogAct01, VeryVerbose, TEXT("[Act01] tick alive state=%d PC=%s"),
			   static_cast<int32>(SceneState), *GetNameSafe(HBPC));
	}

	if (bEnableKeyTrigger && SceneState == EAct01State::Idle)
	{
		APlayerController *PC = UGameplayStatics::GetPlayerController(this, 0);
		if (PC && StartKey.IsValid() && PC->WasInputKeyJustPressed(StartKey))
		{
			UE_LOG(LogAct01, Log, TEXT("[Act01] StartKey pressed via PC"));
			BeginAct01();
			return;
		}
	}

	switch (SceneState)
	{
	case EAct01State::OpeningDoors:
		TickOpenDoors(DeltaSeconds);
		break;
	case EAct01State::NPCsMovingToTV:
		TickNPCMovement(DeltaSeconds);
		break;
	case EAct01State::PlayingVideo:
		TickVideoFallback(DeltaSeconds);
		break;
	default:
		break;
	}
}

bool AAct01RuleIntroDirector::BeginAct01()
{
	if (SceneState != EAct01State::Idle)
	{
		DebugMessage(TEXT("[Act01] already running"), FLinearColor::Yellow);
		return false;
	}

	ResolveTargetActors(/*bLogResolution=*/false);

	if (!NavTargetCached || !NavLookTargetCached || !MediaPlateCached)
	{
		FailAct01(FString::Printf(
			TEXT("missing actor refs (NavTarget=%s NavLookTarget=%s MediaPlate=%s)"),
			*GetNameSafe(NavTargetCached),
			*GetNameSafe(NavLookTargetCached),
			*GetNameSafe(MediaPlateCached)));
		return false;
	}

	if (InitialNPCTransforms.Num() == 0)
	{
		CacheInitialNPCTransforms();
		if (InitialNPCTransforms.Num() == 0)
		{
			FailAct01(TEXT("no NPC instances found via NPCMoverClass"));
			return false;
		}
	}

	if (CachedDoors.Num() == 0)
	{
		CacheCellDoorComponents();
	}

	ResetToInitialPositions();
	StartOpeningDoors();
	return true;
}

void AAct01RuleIntroDirector::CancelAct01()
{
	if (SceneState == EAct01State::Idle)
	{
		return;
	}
	UnbindVideoDelegate();
	SceneState = EAct01State::Idle;
	DebugMessage(TEXT("[Act01] cancelled"), FLinearColor::Yellow);
}

void AAct01RuleIntroDirector::CacheInitialNPCTransforms()
{
	InitialNPCTransforms.Reset();
	if (!NPCMoverClass)
	{
		return;
	}

	TArray<AActor *> Found;
	UGameplayStatics::GetAllActorsOfClass(this, NPCMoverClass, Found);
	for (AActor *Actor : Found)
	{
		if (IsValid(Actor) && Cast<APawn>(Actor))
		{
			InitialNPCTransforms.Add(Actor, Actor->GetActorTransform());
		}
	}
}

void AAct01RuleIntroDirector::CacheCellDoorComponents()
{
	CachedDoors.Reset();
	UWorld *World = GetWorld();
	if (!World)
	{
		return;
	}

	TSet<FName> WantedNames(CellDoorActorNames);
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor *Actor = *It;
		if (!Actor)
		{
			continue;
		}
		const FName LabelName = FName(*Actor->GetActorNameOrLabel());
		if (!WantedNames.Contains(LabelName))
		{
			continue;
		}

		if (USceneComponent *Door = FindCellDoorComponent(Actor))
		{
			FCachedDoor Entry;
			Entry.Component = Door;
			Entry.ClosedRelativeRotation = Door->GetRelativeRotation();
			Entry.ClosedWorldRotation = Door->GetComponentRotation();
			CachedDoors.Add(Entry);
		}
		else
		{
			UE_LOG(LogAct01, Warning, TEXT("[Act01] cell %s missing component %s"),
				   *Actor->GetActorNameOrLabel(),
				   *CellDoorMeshComponentName.ToString());
		}
	}
}

USceneComponent *AAct01RuleIntroDirector::FindCellDoorComponent(AActor *CellActor) const
{
	if (!CellActor)
	{
		return nullptr;
	}

	TArray<USceneComponent *> SceneComps;
	CellActor->GetComponents<USceneComponent>(SceneComps);
	for (USceneComponent *C : SceneComps)
	{
		if (C && C->GetFName() == CellDoorMeshComponentName)
		{
			return C;
		}
	}
	return nullptr;
}

AActor *AAct01RuleIntroDirector::ResolveActorByClassAndLabel(TSubclassOf<AActor> InClass, FName Label) const
{
	if (!InClass)
	{
		return nullptr;
	}

	TArray<AActor *> Found;
	UGameplayStatics::GetAllActorsOfClass(this, InClass, Found);
	if (!Label.IsNone())
	{
		const FString LabelStr = Label.ToString();
		for (AActor *A : Found)
		{
			if (A && A->GetActorNameOrLabel().Equals(LabelStr, ESearchCase::IgnoreCase))
			{
				return A;
			}
		}
	}
	return Found.Num() > 0 ? Found[0] : nullptr;
}

void AAct01RuleIntroDirector::ResolveTargetActors(bool bLogResolution)
{
	if (!IsValid(NavTargetCached))
	{
		NavTargetCached = NavTargetActor.LoadSynchronous();
	}
	if (!IsValid(NavTargetCached))
	{
		NavTargetCached = ResolveActorByClassAndLabel(NavTargetClass, NavTargetActorLabel);
	}

	if (!IsValid(NavLookTargetCached))
	{
		NavLookTargetCached = NavLookTargetActor.LoadSynchronous();
	}
	if (!IsValid(NavLookTargetCached))
	{
		NavLookTargetCached = ResolveActorByClassAndLabel(NavLookTargetClass, NavLookTargetActorLabel);
	}

	if (!IsValid(MediaPlateCached))
	{
		MediaPlateCached = MediaPlateActor.LoadSynchronous();
	}
	if (!IsValid(MediaPlateCached))
	{
		MediaPlateCached = Cast<AMediaPlate>(
			UGameplayStatics::GetActorOfClass(this, AMediaPlate::StaticClass()));
	}

	if (bLogResolution)
	{
		UE_LOG(LogAct01, Log, TEXT("[Act01] resolve: NavTarget=%s NavLook=%s MediaPlate=%s"),
			   *GetNameSafe(NavTargetCached),
			   *GetNameSafe(NavLookTargetCached),
			   *GetNameSafe(MediaPlateCached));
	}
}

void AAct01RuleIntroDirector::RegisterDebugConsoleCommands()
{
	if (StartCommand || CancelCommand)
	{
		return;
	}
	IConsoleManager &CM = IConsoleManager::Get();
	StartCommand = CM.RegisterConsoleCommand(
		TEXT("act01.start"),
		TEXT("Trigger ACT01 rule intro scene."),
		FConsoleCommandDelegate::CreateUObject(this, &AAct01RuleIntroDirector::BeginAct01Console),
		ECVF_Default);
	CancelCommand = CM.RegisterConsoleCommand(
		TEXT("act01.cancel"),
		TEXT("Cancel running ACT01 scene."),
		FConsoleCommandDelegate::CreateUObject(this, &AAct01RuleIntroDirector::CancelAct01),
		ECVF_Default);
}

void AAct01RuleIntroDirector::BeginAct01Console()
{
	BeginAct01();
}

void AAct01RuleIntroDirector::UnregisterDebugConsoleCommands()
{
	IConsoleManager &CM = IConsoleManager::Get();
	if (StartCommand)
	{
		CM.UnregisterConsoleObject(StartCommand);
		StartCommand = nullptr;
	}
	if (CancelCommand)
	{
		CM.UnregisterConsoleObject(CancelCommand);
		CancelCommand = nullptr;
	}
}

void AAct01RuleIntroDirector::ResetToInitialPositions()
{
	int32 Reset = 0;
	for (auto &Pair : InitialNPCTransforms)
	{
		AActor *Actor = Pair.Key.Get();
		if (!IsValid(Actor))
		{
			continue;
		}
		Actor->SetActorTransform(Pair.Value, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
		if (APawn *P = Cast<APawn>(Actor))
		{
			if (AAIController *AIC = Cast<AAIController>(P->GetController()))
			{
				AIC->StopMovement();
			}
		}
		++Reset;
	}
	ResetDoorsToClosed();
	DebugMessage(FString::Printf(TEXT("[Act01] reset %d NPC(s) and %d door(s) to initial state"),
								 Reset, CachedDoors.Num()),
				 FLinearColor::White);
}

void AAct01RuleIntroDirector::ResetDoorsToClosed()
{
	for (const FCachedDoor &Door : CachedDoors)
	{
		if (USceneComponent *C = Door.Component.Get())
		{
			C->SetRelativeRotation(Door.ClosedRelativeRotation);
		}
	}
}

void AAct01RuleIntroDirector::StartOpeningDoors()
{
	DoorAnimationElapsed = 0.0f;
	SceneState = EAct01State::OpeningDoors;
	DebugMessage(TEXT("[Act01] opening cell doors"), FLinearColor::Green);
}

void AAct01RuleIntroDirector::TickOpenDoors(float DeltaSeconds)
{
	DoorAnimationElapsed += DeltaSeconds;
	const float Alpha = FMath::Clamp(DoorAnimationElapsed / FMath::Max(0.01f, DoorAnimationSeconds), 0.f, 1.f);
	for (const FCachedDoor &Door : CachedDoors)
	{
		if (USceneComponent *C = Door.Component.Get())
		{
			const FRotator Target = Door.ClosedRelativeRotation + DoorOpenRelativeRotation;
			const FRotator Lerped = FMath::Lerp(Door.ClosedRelativeRotation, Target, Alpha);
			C->SetRelativeRotation(Lerped);
		}
	}
	if (Alpha >= 1.0f)
	{
		StartNPCMovement();
	}
}

void AAct01RuleIntroDirector::StartNPCMovement()
{
	MovementElapsed = 0.0f;
	MovementSettleElapsed = 0.0f;
	bArrivalSettling = false;
	SceneState = EAct01State::NPCsMovingToTV;

	if (!ScatterQueryAsset)
	{
		FailAct01(TEXT("ScatterQueryAsset not set"));
		return;
	}

	TArray<AActor *> NPCArray;
	NPCArray.Reserve(InitialNPCTransforms.Num());
	for (auto &Pair : InitialNPCTransforms)
	{
		if (AActor *Actor = Pair.Key.Get())
		{
			NPCArray.Add(Actor);
		}
	}

	UAILiveProjectScatterMover::ScatterNPCsAroundTarget(
		ScatterQueryAsset,
		NavTargetCached,
		NPCArray,
		NavLookTargetCached,
		this);

	DebugMessage(FString::Printf(TEXT("[Act01] dispatched %d NPC(s) via EQS scatter around NavTarget"), NPCArray.Num()),
				 FLinearColor::Green);
}

void AAct01RuleIntroDirector::TickNPCMovement(float DeltaSeconds)
{
	MovementElapsed += DeltaSeconds;
	if (MovementElapsed > MovementTimeoutSeconds)
	{
		FailAct01(TEXT("NPC movement timeout"));
		return;
	}

	if (MovementElapsed < ScatterDispatchDelaySeconds)
	{
		return;
	}

	if (AreAllNPCsIdle())
	{
		if (!bArrivalSettling)
		{
			bArrivalSettling = true;
			MovementSettleElapsed = 0.0f;
		}
		else
		{
			MovementSettleElapsed += DeltaSeconds;
			if (MovementSettleElapsed >= ArrivalSettleSeconds)
			{
				StartVideo();
			}
		}
	}
	else
	{
		bArrivalSettling = false;
		MovementSettleElapsed = 0.0f;
	}
}

bool AAct01RuleIntroDirector::AreAllNPCsIdle() const
{
	int32 ValidCount = 0;
	int32 IdleCount = 0;
	for (const auto &Pair : InitialNPCTransforms)
	{
		AActor *Actor = Pair.Key.Get();
		if (!IsValid(Actor))
		{
			continue;
		}
		++ValidCount;

		const APawn *P = Cast<APawn>(Actor);
		if (!P)
		{
			++IdleCount;
			continue;
		}
		const AAIController *AIC = Cast<AAIController>(P->GetController());
		if (!AIC)
		{
			++IdleCount;
			continue;
		}
		if (AIC->GetMoveStatus() == EPathFollowingStatus::Idle)
		{
			++IdleCount;
		}
	}
	return ValidCount > 0 && IdleCount == ValidCount;
}

void AAct01RuleIntroDirector::StartVideo()
{
	SceneState = EAct01State::PlayingVideo;
	VideoElapsed = 0.0f;

	if (!MediaPlateCached || !MediaPlateCached->MediaPlateComponent)
	{
		FailAct01(TEXT("MediaPlate component missing"));
		return;
	}
	UMediaPlateComponent *PlateComponent = MediaPlateCached->MediaPlateComponent;
	UMediaPlayer *Player = PlateComponent->GetMediaPlayer();
	if (!Player)
	{
		FailAct01(TEXT("MediaPlayer null"));
		return;
	}

	UMediaPlaylist *Playlist = PlateComponent->GetMediaPlaylist();
	UMediaSource *MediaSource = Playlist ? Playlist->Get(0) : nullptr;
	if (!MediaSource)
	{
		FailAct01(TEXT("MediaPlate has no media source"));
		return;
	}

	MediaPlateCached->SetActorHiddenInGame(false);
	if (MediaPlateCached->StaticMeshComponent)
	{
		MediaPlateCached->StaticMeshComponent->SetHiddenInGame(false);
		MediaPlateCached->StaticMeshComponent->SetVisibility(true, true);
	}
	Player->OnEndReached.AddUniqueDynamic(this, &AAct01RuleIntroDirector::HandleVideoEnded);
	bVideoDelegateBound = true;

	PlateComponent->Close();
	PlateComponent->SetLoop(false);
	PlateComponent->bPlayOnOpen = false;
	PlateComponent->Open();
	DebugMessage(FString::Printf(TEXT("[Act01] video open requested: %s"), *MediaSource->GetUrl()),
				 FLinearColor::Green);
}

void AAct01RuleIntroDirector::HandleVideoEnded()
{
	if (SceneState != EAct01State::PlayingVideo)
	{
		return;
	}
	DebugMessage(TEXT("[Act01] video OnEndReached fired"), FLinearColor::Green);
	CompleteAct01();
}

void AAct01RuleIntroDirector::TickVideoFallback(float DeltaSeconds)
{
	VideoElapsed += DeltaSeconds;
	if (MediaPlateCached && MediaPlateCached->MediaPlateComponent)
	{
		if (UMediaPlayer *Player = MediaPlateCached->MediaPlateComponent->GetMediaPlayer())
		{
			if (Player->IsReady() && !Player->IsPlaying())
			{
				MediaPlateCached->MediaPlateComponent->Play();
				if (Player->Play())
				{
					DebugMessage(FString::Printf(TEXT("[Act01] video started: %s"), *Player->GetUrl()),
								 FLinearColor::Green);
				}
				else if (FMath::IsNearlyZero(FMath::Fmod(VideoElapsed, 1.0f), 0.05f))
				{
					UE_LOG(LogAct01, Warning, TEXT("[Act01] MediaPlayer ready but Play() failed url=%s"),
						   *Player->GetUrl());
				}
			}
			else if (!Player->IsReady() && FMath::IsNearlyZero(FMath::Fmod(VideoElapsed, 1.0f), 0.05f))
			{
				UE_LOG(LogAct01, Log, TEXT("[Act01] waiting for video open elapsed=%.1fs url=%s"),
					   VideoElapsed, *Player->GetUrl());
			}
			if (!Player->IsReady() && VideoElapsed > 15.0f)
			{
				FailAct01(FString::Printf(TEXT("MediaPlayer did not become ready url=%s"),
										   *Player->GetUrl()));
				return;
			}
		}
	}
	if (VideoElapsed > VideoFallbackTimeoutSeconds)
	{
		UE_LOG(LogAct01, Warning, TEXT("[Act01] video fallback timeout fired (%.1fs)"), VideoElapsed);
		CompleteAct01();
	}
}

void AAct01RuleIntroDirector::UnbindVideoDelegate()
{
	if (!bVideoDelegateBound)
	{
		return;
	}
	if (MediaPlateCached && MediaPlateCached->MediaPlateComponent)
	{
		if (UMediaPlayer *Player = MediaPlateCached->MediaPlateComponent->GetMediaPlayer())
		{
			Player->OnEndReached.RemoveDynamic(this, &AAct01RuleIntroDirector::HandleVideoEnded);
		}
	}
	bVideoDelegateBound = false;
}

void AAct01RuleIntroDirector::CompleteAct01()
{
	UnbindVideoDelegate();
	OnAct01Completed.Broadcast();
	DebugMessage(TEXT("[Act01] complete"), FLinearColor::Green);
	SceneState = EAct01State::Idle;
}

void AAct01RuleIntroDirector::FailAct01(const FString &Reason)
{
	UnbindVideoDelegate();
	OnAct01Failed.Broadcast(Reason);
	DebugMessage(FString::Printf(TEXT("[Act01] failed: %s"), *Reason), FLinearColor::Red);
	SceneState = EAct01State::Idle;
}

void AAct01RuleIntroDirector::DebugMessage(const FString &Message, const FLinearColor &Color) const
{
	UE_LOG(LogAct01, Log, TEXT("%s"), *Message);
	if (bDebugPrintScreen)
	{
		UKismetSystemLibrary::PrintString(this, Message, true, false, Color, 4.0f);
	}
}
