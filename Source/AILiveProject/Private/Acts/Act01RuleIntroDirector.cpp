// =============================================================================
// 中文教学：Act01RuleIntroDirector.cpp —— 第一幕剧情导演实现
//
// 文件大概分四段：
//   1) BeginPlay/Tick/EndPlay 生命周期 + 状态机推进
//   2) 开门 (StartOpeningDoors / TickOpenDoors / ResetDoorsToClosed)
//   3) NPC 移动 (StartNPCMovement / TickNPCMovement) + 等待全部 idle
//   4) 视频播放 (StartVideo / HandleVideoEnded / fallback timeout)
//   5) Console 命令注册（debug 用）
//
// 关键 UE 概念：
//
//   1) AIController + UNavMoverComponent
//      移动指令是发给 AIController 的（因为 NPC 是 AI 控制的 Pawn）。
//      项目用 GASP Mover 2.0 的 NavMoverComponent 寻路；指令通过
//      ScatterMover 组件下发。
//
//   2) FRotator + 相对/世界旋转
//      门的开关用相对旋转（绕铰链轴转一定角度）。FCachedDoor 缓存关闭时的
//      旋转，开门时插值到 ClosedRelativeRotation + DoorOpenRelativeRotation。
//
//   3) TWeakObjectPtr<USceneComponent>
//      关门用的 SceneComponent 缓存指针——弱引用避免野指针，每次访问要
//      `IsValid()` 检查。万一关卡中 actor 被销毁，weak ptr 会自动失效。
//
//   4) UEnvQuery
//      EQS（Environment Query System）的查询模板。给定空间约束（"找最近的非
//      占用 nav 网格点"）让 AI 系统挑目标位置。NPC 散开走到电视前用它。
//
//   5) UFUNCTION() 标记的 HandleVideoEnded
//      不带参数的 UFUNCTION 标记让函数能被 UE 反射系统找到，从而能 bind 给
//      dynamic delegate（如 MediaPlate 的 OnEnded 事件）。raw 函数 binding 不需要它。
// =============================================================================

#include "Acts/Act01RuleIntroDirector.h"

#include "AIController.h"
#include "AILiveProjectScatterMover.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
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
#include "Memory/AILiveEventStoreSubsystem.h"
#include "Memory/AILiveEventTypes.h"
#include "Navigation/PathFollowingComponent.h"
#include "Util/AILiveJsonHelpers.h"
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

	// EventStore 强依赖：T5 完成定义要求 ACT01 setup 事件入库。Store 不可用即停。
	UGameInstance* GI = UGameplayStatics::GetGameInstance(this);
	UAILiveEventStoreSubsystem* Store = GI ? GI->GetSubsystem<UAILiveEventStoreSubsystem>() : nullptr;
	if (!Store)
	{
		FailAct01(TEXT("EventStore subsystem unavailable"));
		return false;
	}
	if (!Store->IsGameOpen())
	{
		const FString GameId = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		if (!Store->BeginGame(GameId))
		{
			FailAct01(TEXT("EventStore BeginGame failed"));
			return false;
		}
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
		APawn *Pawn = Cast<APawn>(Actor);
		if (IsValid(Pawn) && !Pawn->IsPlayerControlled())
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

	// T5 setup-phase 入库节点 #1：开门动画完成、即将 Scatter
	if (!AppendOrchestratorRoundResolved(TEXT("cell doors opened")))
	{
		FailAct01(TEXT("setup event 'cell doors opened' write failed"));
		return;
	}

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

	// T5 setup-phase 入库节点 #2：播放请求已发出（不依赖 Player->Play() 返回值，
	// 保证 ACT01 一定写一条 video started 事件——TickVideoFallback 中 Player->Play()
	// 在 Player 已 IsPlaying 等状态下不一定进入）。
	if (!AppendOrchestratorRoundResolved(TEXT("rule intro video started")))
	{
		FailAct01(TEXT("setup event 'video started' write failed"));
		return;
	}

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

	// T5 setup-phase 入库节点 #3：视频结束。失败仅 Warning（游戏即将结束，没必要 fail）。
	if (!AppendOrchestratorRoundResolved(TEXT("rule intro video ended")))
	{
		UE_LOG(LogAct01, Warning, TEXT("[Act01] setup event 'video ended' write failed (continuing)"));
	}

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
		// T5 setup-phase 入库节点 #3 (fallback path)：视频超时也算结束
		if (!AppendOrchestratorRoundResolved(TEXT("rule intro video ended")))
		{
			UE_LOG(LogAct01, Warning, TEXT("[Act01] setup event 'video ended' write failed (continuing)"));
		}
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

bool AAct01RuleIntroDirector::AppendOrchestratorRoundResolved(const FString& Text)
{
	UGameInstance* GI = UGameplayStatics::GetGameInstance(this);
	UAILiveEventStoreSubsystem* Store = GI ? GI->GetSubsystem<UAILiveEventStoreSubsystem>() : nullptr;
	if (!Store || !Store->IsGameOpen())
	{
		return false;
	}
	FAILiveEvent Ev;
	Ev.RoundNo    = 0;
	Ev.Phase      = EAILivePhase::Setup;
	Ev.Actor      = TEXT("orchestrator");
	Ev.EventType  = EAILiveEventType::OrchestratorResolved;
	Ev.Visibility = { TEXT("public") };
	Ev.PayloadJson = FString::Printf(
		TEXT("{\"text\":%s,\"phase\":\"setup\"}"),
		*AILiveUtil::EscapeJsonString(Text));
	return Store->AppendEvent(Ev) > 0;
}
