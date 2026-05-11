#include "Rounds/Round01RuleIntroDirector.h"

#include "AIController.h"
#include "AILiveProjectBrainSessionSubsystem.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogRound01, Log, All);

ARound01RuleIntroDirector::ARound01RuleIntroDirector()
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

void ARound01RuleIntroDirector::BeginPlay()
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

	UE_LOG(LogRound01, Log, TEXT("[Round01] BeginPlay cached %d NPC(s), %d cell door(s)"),
		   InitialNPCTransforms.Num(), CachedDoors.Num());
}

void ARound01RuleIntroDirector::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterDebugConsoleCommands();
	UnbindVideoDelegate();
	Super::EndPlay(EndPlayReason);
}

void ARound01RuleIntroDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	HeartbeatElapsed += DeltaSeconds;
	if (HeartbeatElapsed >= 5.0f)
	{
		HeartbeatElapsed = 0.0f;
		APlayerController *HBPC = UGameplayStatics::GetPlayerController(this, 0);
		UE_LOG(LogRound01, VeryVerbose, TEXT("[Round01] tick alive state=%d PC=%s"),
			   static_cast<int32>(SceneState), *GetNameSafe(HBPC));
	}

	if (bEnableKeyTrigger && SceneState == ERound01State::Idle)
	{
		APlayerController *PC = UGameplayStatics::GetPlayerController(this, 0);
		if (PC && StartKey.IsValid() && PC->WasInputKeyJustPressed(StartKey))
		{
			UE_LOG(LogRound01, Log, TEXT("[Round01] StartKey pressed via PC"));
			BeginRound01();
			return;
		}
	}

	switch (SceneState)
	{
	case ERound01State::OpeningDoors:
		TickOpenDoors(DeltaSeconds);
		break;
	case ERound01State::NPCsMovingToTV:
		TickNPCMovement(DeltaSeconds);
		break;
	case ERound01State::PlayingVideo:
		TickVideoFallback(DeltaSeconds);
		break;
	default:
		break;
	}
}

bool ARound01RuleIntroDirector::BeginRound01()
{
	if (SceneState != ERound01State::Idle)
	{
		DebugMessage(TEXT("[Round01] already running"), FLinearColor::Yellow);
		return false;
	}

	ResolveTargetActors(/*bLogResolution=*/false);

	if (!NavTargetCached || !NavLookTargetCached || !MediaPlateCached)
	{
		FailRound01(FString::Printf(
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
			FailRound01(TEXT("no NPC instances found via NPCMoverClass"));
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

void ARound01RuleIntroDirector::CancelRound01()
{
	if (SceneState == ERound01State::Idle)
	{
		return;
	}
	UnbindVideoDelegate();
	SceneState = ERound01State::Idle;
	DebugMessage(TEXT("[Round01] cancelled"), FLinearColor::Yellow);
}

void ARound01RuleIntroDirector::CacheInitialNPCTransforms()
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

void ARound01RuleIntroDirector::CacheCellDoorComponents()
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
			UE_LOG(LogRound01, Warning, TEXT("[Round01] cell %s missing component %s"),
				   *Actor->GetActorNameOrLabel(),
				   *CellDoorMeshComponentName.ToString());
		}
	}
}

USceneComponent *ARound01RuleIntroDirector::FindCellDoorComponent(AActor *CellActor) const
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

AActor *ARound01RuleIntroDirector::ResolveActorByClassAndLabel(TSubclassOf<AActor> InClass, FName Label) const
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

void ARound01RuleIntroDirector::ResolveTargetActors(bool bLogResolution)
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
		UE_LOG(LogRound01, Log, TEXT("[Round01] resolve: NavTarget=%s NavLook=%s MediaPlate=%s"),
			   *GetNameSafe(NavTargetCached),
			   *GetNameSafe(NavLookTargetCached),
			   *GetNameSafe(MediaPlateCached));
	}
}

void ARound01RuleIntroDirector::RegisterDebugConsoleCommands()
{
	if (StartCommand || CancelCommand)
	{
		return;
	}
	IConsoleManager &CM = IConsoleManager::Get();
	StartCommand = CM.RegisterConsoleCommand(
		TEXT("round01.start"),
		TEXT("Trigger ROUND01 rule intro scene."),
		FConsoleCommandDelegate::CreateUObject(this, &ARound01RuleIntroDirector::BeginRound01Console),
		ECVF_Default);
	CancelCommand = CM.RegisterConsoleCommand(
		TEXT("round01.cancel"),
		TEXT("Cancel running ROUND01 scene."),
		FConsoleCommandDelegate::CreateUObject(this, &ARound01RuleIntroDirector::CancelRound01),
		ECVF_Default);
}

void ARound01RuleIntroDirector::BeginRound01Console()
{
	BeginRound01();
}

void ARound01RuleIntroDirector::UnregisterDebugConsoleCommands()
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

void ARound01RuleIntroDirector::ResetToInitialPositions()
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
	DebugMessage(FString::Printf(TEXT("[Round01] reset %d NPC(s) and %d door(s) to initial state"),
								 Reset, CachedDoors.Num()),
				 FLinearColor::White);
}

void ARound01RuleIntroDirector::ResetDoorsToClosed()
{
	for (const FCachedDoor &Door : CachedDoors)
	{
		if (USceneComponent *C = Door.Component.Get())
		{
			C->SetRelativeRotation(Door.ClosedRelativeRotation);
		}
	}
}

void ARound01RuleIntroDirector::StartOpeningDoors()
{
	DoorAnimationElapsed = 0.0f;
	SceneState = ERound01State::OpeningDoors;
	DebugMessage(TEXT("[Round01] opening cell doors"), FLinearColor::Green);
}

void ARound01RuleIntroDirector::TickOpenDoors(float DeltaSeconds)
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

void ARound01RuleIntroDirector::StartNPCMovement()
{
	MovementElapsed = 0.0f;
	MovementSettleElapsed = 0.0f;
	bArrivalSettling = false;
	SceneState = ERound01State::NPCsMovingToTV;

	if (!ScatterQueryAsset)
	{
		FailRound01(TEXT("ScatterQueryAsset not set"));
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

	DebugMessage(FString::Printf(TEXT("[Round01] dispatched %d NPC(s) via EQS scatter around NavTarget"), NPCArray.Num()),
				 FLinearColor::Green);
}

void ARound01RuleIntroDirector::TickNPCMovement(float DeltaSeconds)
{
	MovementElapsed += DeltaSeconds;
	if (MovementElapsed > MovementTimeoutSeconds)
	{
		FailRound01(TEXT("NPC movement timeout"));
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

bool ARound01RuleIntroDirector::AreAllNPCsIdle() const
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

void ARound01RuleIntroDirector::StartVideo()
{
	SceneState = ERound01State::PlayingVideo;
	VideoElapsed = 0.0f;

	if (!MediaPlateCached || !MediaPlateCached->MediaPlateComponent)
	{
		FailRound01(TEXT("MediaPlate component missing"));
		return;
	}
	UMediaPlateComponent *PlateComponent = MediaPlateCached->MediaPlateComponent;
	UMediaPlayer *Player = PlateComponent->GetMediaPlayer();
	if (!Player)
	{
		FailRound01(TEXT("MediaPlayer null"));
		return;
	}

	UMediaPlaylist *Playlist = PlateComponent->GetMediaPlaylist();
	UMediaSource *MediaSource = Playlist ? Playlist->Get(0) : nullptr;
	if (!MediaSource)
	{
		FailRound01(TEXT("MediaPlate has no media source"));
		return;
	}

	MediaPlateCached->SetActorHiddenInGame(false);
	if (MediaPlateCached->StaticMeshComponent)
	{
		MediaPlateCached->StaticMeshComponent->SetHiddenInGame(false);
		MediaPlateCached->StaticMeshComponent->SetVisibility(true, true);
	}
	Player->OnEndReached.AddUniqueDynamic(this, &ARound01RuleIntroDirector::HandleVideoEnded);
	bVideoDelegateBound = true;

	PlateComponent->Close();
	PlateComponent->SetLoop(false);
	PlateComponent->bPlayOnOpen = false;
	PlateComponent->Open();
	DebugMessage(FString::Printf(TEXT("[Round01] video open requested: %s"), *MediaSource->GetUrl()),
				 FLinearColor::Green);
}

void ARound01RuleIntroDirector::HandleVideoEnded()
{
	if (SceneState != ERound01State::PlayingVideo)
	{
		return;
	}
	DebugMessage(TEXT("[Round01] video OnEndReached fired"), FLinearColor::Green);
	CompleteRound01();
}

void ARound01RuleIntroDirector::TickVideoFallback(float DeltaSeconds)
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
					DebugMessage(FString::Printf(TEXT("[Round01] video started: %s"), *Player->GetUrl()),
								 FLinearColor::Green);
				}
				else if (FMath::IsNearlyZero(FMath::Fmod(VideoElapsed, 1.0f), 0.05f))
				{
					UE_LOG(LogRound01, Warning, TEXT("[Round01] MediaPlayer ready but Play() failed url=%s"),
						   *Player->GetUrl());
				}
			}
			else if (!Player->IsReady() && FMath::IsNearlyZero(FMath::Fmod(VideoElapsed, 1.0f), 0.05f))
			{
				UE_LOG(LogRound01, Log, TEXT("[Round01] waiting for video open elapsed=%.1fs url=%s"),
					   VideoElapsed, *Player->GetUrl());
			}
			if (!Player->IsReady() && VideoElapsed > 15.0f)
			{
				FailRound01(FString::Printf(TEXT("MediaPlayer did not become ready url=%s"),
											*Player->GetUrl()));
				return;
			}
		}
	}
	if (VideoElapsed > VideoFallbackTimeoutSeconds)
	{
		UE_LOG(LogRound01, Warning, TEXT("[Round01] video fallback timeout fired (%.1fs)"), VideoElapsed);
		CompleteRound01();
	}
}

void ARound01RuleIntroDirector::UnbindVideoDelegate()
{
	if (!bVideoDelegateBound)
	{
		return;
	}
	if (MediaPlateCached && MediaPlateCached->MediaPlateComponent)
	{
		if (UMediaPlayer *Player = MediaPlateCached->MediaPlateComponent->GetMediaPlayer())
		{
			Player->OnEndReached.RemoveDynamic(this, &ARound01RuleIntroDirector::HandleVideoEnded);
		}
	}
	bVideoDelegateBound = false;
}

void ARound01RuleIntroDirector::CompleteRound01()
{
	UnbindVideoDelegate();

	// 开场动画收尾后让 Brain 启动 Round-001 的 3 拍 LLM 自由发言（day_discuss）。
	// 失败仅 log，不阻塞场景推进；UE 端开场动画的播放成功不应被 Brain 后台
	// 状态打断。
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UAILiveProjectBrainSessionSubsystem* Brain =
				GI->GetSubsystem<UAILiveProjectBrainSessionSubsystem>())
		{
			const bool bSent = Brain->RequestStartLLMPhase(/*RoundNo=*/1, TEXT("day_discuss"), /*NTicks=*/3);
			UE_LOG(LogRound01, Log,
				TEXT("[Round01] requested LLM phase start: success=%d"),
				bSent ? 1 : 0);
		}
	}

	OnRound01Completed.Broadcast();
	DebugMessage(TEXT("[Round01] complete"), FLinearColor::Green);
	SceneState = ERound01State::Idle;
}

void ARound01RuleIntroDirector::FailRound01(const FString &Reason)
{
	UnbindVideoDelegate();
	OnRound01Failed.Broadcast(Reason);
	DebugMessage(FString::Printf(TEXT("[Round01] failed: %s"), *Reason), FLinearColor::Red);
	SceneState = ERound01State::Idle;
}

void ARound01RuleIntroDirector::DebugMessage(const FString &Message, const FLinearColor &Color) const
{
	UE_LOG(LogRound01, Log, TEXT("%s"), *Message);
	if (bDebugPrintScreen)
	{
		UKismetSystemLibrary::PrintString(this, Message, true, false, Color, 4.0f);
	}
}
