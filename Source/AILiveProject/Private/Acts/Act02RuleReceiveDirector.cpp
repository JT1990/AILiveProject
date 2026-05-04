#include "Acts/Act02RuleReceiveDirector.h"

#include "AIController.h"
#include "AILiveProjectScatterMover.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "LLM/AILiveParserClient.h"
#include "Memory/AILiveBidTypes.h"
#include "Memory/AILiveEventStoreSubsystem.h"
#include "Memory/AILiveEventTypes.h"
#include "Memory/AILiveListenerFilter.h"
#include "Memory/AILivePromptAssembler.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "MinimaxACELibrary.h"
#include "Navigation/PathFollowingComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Util/AILiveJsonHelpers.h"
#include "Util/ProjectEnvLoader.h"

DEFINE_LOG_CATEGORY_STATIC(LogAct02, Log, All);

namespace
{
	const TCHAR* StateName(EAct02State S)
	{
		switch (S)
		{
		case EAct02State::Idle:             return TEXT("Idle");
		case EAct02State::PrescatterToTV:   return TEXT("PrescatterToTV");
		case EAct02State::SeedDispatch:     return TEXT("SeedDispatch");
		case EAct02State::SeedAwait:        return TEXT("SeedAwait");
		case EAct02State::SeedSpeak:        return TEXT("SeedSpeak");
		case EAct02State::GatedAwaitNext:   return TEXT("GatedAwaitNext");
		case EAct02State::ReactionDispatch: return TEXT("ReactionDispatch");
		case EAct02State::ReactionAwait:    return TEXT("ReactionAwait");
		case EAct02State::ReactionSpeak:    return TEXT("ReactionSpeak");
		}
		return TEXT("?");
	}

	EAILivePhase ResolvePhase(int32 CurrentRound)
	{
		return CurrentRound == 0 ? EAILivePhase::Setup : EAILivePhase::DayDiscuss;
	}

	UAILiveEventStoreSubsystem* GetEventStore(const UObject* WorldCtx)
	{
		if (!WorldCtx) return nullptr;
		const UGameInstance* GI = UGameplayStatics::GetGameInstance(WorldCtx);
		return GI ? GI->GetSubsystem<UAILiveEventStoreSubsystem>() : nullptr;
	}
}

AAct02RuleReceiveDirector::AAct02RuleReceiveDirector()
{
	PrimaryActorTick.bCanEverTick = true;

	StartKey = EKeys::Two;
	NextPhaseKey = EKeys::Three;
	NavTargetActorLabel = TEXT("BP_NavTarget_1");
	NavLookTargetActorLabel = TEXT("BP_NavLookTarget_TV");
	GameRuleRelativePath = TEXT("Docs/playscript/zombie-game-rule.md");
	GameRuleFallbackSummary =
		TEXT("僵尸触碰游戏：10 名玩家中随机 2 人为初始僵尸，其余 8 人为人类。共 3 回合，每回合 30 分钟，每回合必须与他人手碰手接触至少一次。")
		TEXT("人类碰人类得 1 分；人类碰僵尸变僵尸；僵尸之间无事；不接触者本回合后变僵尸；同对玩家不可重复接触。每人有 1 支解药（变僵尸 10 分钟内可解，初始僵尸不可解），可花 5 石榴石买。")
		TEXT("3 回合后：若全员僵尸，初始僵尸胜利并选淘汰；若有人类存活，得分最高者胜，最低者为淘汰候选。");
	A2FProviderName = FName(TEXT("LocalA2F-James"));
}

void AAct02RuleReceiveDirector::BeginPlay()
{
	Super::BeginPlay();

	if (!NPCMoverClass)
	{
		NPCMoverClass = LoadClass<AActor>(nullptr,
			TEXT("/Game/Blueprints/SandboxCharacter_Mover.SandboxCharacter_Mover_C"));
	}
	if (!NavTargetClass)
	{
		NavTargetClass = LoadClass<AActor>(nullptr,
			TEXT("/Game/Blueprints/Markers/BP_NavTarget.BP_NavTarget_C"));
	}
	if (!NavLookTargetClass)
	{
		NavLookTargetClass = LoadClass<AActor>(nullptr,
			TEXT("/Game/Blueprints/Markers/BP_NavLookTarget.BP_NavLookTarget_C"));
	}
	if (!ScatterQueryAsset)
	{
		ScatterQueryAsset = LoadObject<UEnvQuery>(nullptr,
			TEXT("/Game/AI/EQS/EQS_ScatterAroundTarget.EQS_ScatterAroundTarget"));
	}

	if (Roster.Num() == 0)
	{
		Roster = AILiveAgentRoster::GetDefaultRoster();
	}

	CacheInitialNPCTransforms();
	ResolveTargetActors(true);
	BindRoster();
	RegisterDebugConsoleCommands();

	UE_LOG(LogAct02, Log,
		TEXT("[Act02] BeginPlay cached %d NPC(s), Roster=%d"),
		InitialNPCTransforms.Num(), Roster.Num());
}

void AAct02RuleReceiveDirector::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterDebugConsoleCommands();
	Super::EndPlay(EndPlayReason);
}

void AAct02RuleReceiveDirector::Tick(float Dt)
{
	Super::Tick(Dt);

	StateElapsed += Dt;

	// Inputs
	if (bEnableKeyTrigger)
	{
		if (APlayerController* PC = UGameplayStatics::GetPlayerController(this, 0))
		{
			if (SceneState == EAct02State::Idle &&
				StartKey.IsValid() && PC->WasInputKeyJustPressed(StartKey))
			{
				UE_LOG(LogAct02, Log, TEXT("[Act02] StartKey pressed"));
				BeginAct02();
			}
			else if (SceneState == EAct02State::GatedAwaitNext &&
			         NextPhaseKey.IsValid() && PC->WasInputKeyJustPressed(NextPhaseKey))
			{
				UE_LOG(LogAct02, Log, TEXT("[Act02] NextPhaseKey pressed"));
				StartReactionPhase();
			}
		}
	}

	switch (SceneState)
	{
	case EAct02State::PrescatterToTV:
		TickPrescatter(Dt);
		break;
	case EAct02State::SeedAwait:
	case EAct02State::ReactionAwait:
		TickLLMAwait(Dt);
		break;
	case EAct02State::SeedSpeak:
	case EAct02State::ReactionSpeak:
		TickSpeakWatchdog(Dt);
		break;
	default:
		break;
	}
}

bool AAct02RuleReceiveDirector::BeginAct02()
{
	if (SceneState != EAct02State::Idle)
	{
		DebugMessage(TEXT("[Act02] already running"), FLinearColor::Yellow);
		return false;
	}

	ResolveTargetActors(false);
	if (!NavTargetCached || !NavLookTargetCached)
	{
		FailAct02(FString::Printf(TEXT("missing actor refs (NavTarget=%s NavLook=%s)"),
			*GetNameSafe(NavTargetCached), *GetNameSafe(NavLookTargetCached)));
		return false;
	}
	if (InitialNPCTransforms.Num() == 0)
	{
		CacheInitialNPCTransforms();
	}
	if (!BindRoster())
	{
		FailAct02(TEXT("could not bind Roster to NPC pawns"));
		return false;
	}

	// EventStore 强依赖：events 入库是 T5 完成定义，Store 不可用即停。
	UAILiveEventStoreSubsystem* Store = GetEventStore(this);
	if (!Store)
	{
		FailAct02(TEXT("EventStore subsystem unavailable"));
		return false;
	}
	if (!Store->IsGameOpen())
	{
		const FString GameId = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		if (!Store->BeginGame(GameId))
		{
			FailAct02(TEXT("EventStore BeginGame failed"));
			return false;
		}
	}

	LastSpeakerIndex = INDEX_NONE;
	LastSentence.Reset();

	// 不 ResetToInitialPositions：ACT02 复用 ScatterMover 把 NPC 散到 TV 前。
	// - 若 ACT01 已跑过，NPC 已在 TV 前，scatter 仅做小幅调整
	// - 若 ACT02 单独触发（NPC 在牢房），scatter 通过 NavMesh 寻路（门 bCanEverAffectNavigation=False）
	// 否则瞬移回 BeginPlay 位置会让用户视觉上误以为是 ACT01 重新触发。
	StartPrescatter();
	return true;
}

void AAct02RuleReceiveDirector::CancelAct02()
{
	if (SceneState == EAct02State::Idle)
	{
		return;
	}
	CurrentTickInflight.Reset();
	ActorToIntendedSeq.Reset();
	ActorToRequestId.Reset();
	SceneState = EAct02State::Idle;
	DebugMessage(TEXT("[Act02] cancelled"), FLinearColor::Yellow);
}

void AAct02RuleReceiveDirector::CacheInitialNPCTransforms()
{
	InitialNPCTransforms.Reset();
	if (!NPCMoverClass)
	{
		return;
	}
	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClass(this, NPCMoverClass, Found);
	for (AActor* A : Found)
	{
		APawn* Pawn = Cast<APawn>(A);
		if (IsValid(Pawn) && !Pawn->IsPlayerControlled())
		{
			InitialNPCTransforms.Add(A, A->GetActorTransform());
		}
	}
}

AActor* AAct02RuleReceiveDirector::ResolveActorByClassAndLabel(TSubclassOf<AActor> InClass, FName Label) const
{
	if (!InClass)
	{
		return nullptr;
	}
	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClass(this, InClass, Found);
	if (!Label.IsNone())
	{
		const FString L = Label.ToString();
		for (AActor* A : Found)
		{
			if (A && A->GetActorNameOrLabel().Equals(L, ESearchCase::IgnoreCase))
			{
				return A;
			}
		}
	}
	return Found.Num() > 0 ? Found[0] : nullptr;
}

void AAct02RuleReceiveDirector::ResolveTargetActors(bool bLog)
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
	if (bLog)
	{
		UE_LOG(LogAct02, Log, TEXT("[Act02] resolve: NavTarget=%s NavLook=%s"),
			*GetNameSafe(NavTargetCached), *GetNameSafe(NavLookTargetCached));
	}
}

bool AAct02RuleReceiveDirector::BindRoster()
{
	NPCs.Reset();
	UWorld* W = GetWorld();
	if (!W || Roster.Num() == 0)
	{
		return false;
	}

	TArray<AActor*> Found;
	if (NPCMoverClass)
	{
		UGameplayStatics::GetAllActorsOfClass(this, NPCMoverClass, Found);
	}

	int32 BoundCount = 0;
	for (const FNPCAgentConfig& Cfg : Roster)
	{
		FAct02NPCRuntime Rt;
		Rt.Config = Cfg;

		const FString Wanted = Cfg.NPCActorLabel.ToString();
		AActor* Match = nullptr;
		for (AActor* A : Found)
		{
			if (A && A->GetActorNameOrLabel().Equals(Wanted, ESearchCase::IgnoreCase))
			{
				Match = A;
				break;
			}
		}
		Rt.Pawn = Match;
		NPCs.Add(Rt);
		if (Match)
		{
			++BoundCount;
		}
		else
		{
			UE_LOG(LogAct02, Warning,
				TEXT("[Act02] no pawn matched label '%s' (NPCIndex=%d)"),
				*Wanted, Cfg.NPCIndex);
		}
	}
	UE_LOG(LogAct02, Log, TEXT("[Act02] bound %d / %d NPC pawn(s)"), BoundCount, Roster.Num());
	return BoundCount > 0;
}

bool AAct02RuleReceiveDirector::LoadGameRule(FString& OutRule) const
{
	const FString Path = FPaths::ProjectDir() / GameRuleRelativePath;
	if (FFileHelper::LoadFileToString(OutRule, *Path))
	{
		UE_LOG(LogAct02, Log, TEXT("[Act02] loaded game rule from %s (%d chars)"),
			*Path, OutRule.Len());
		return true;
	}
	UE_LOG(LogAct02, Warning, TEXT("[Act02] could not read %s; using fallback summary"), *Path);
	OutRule = GameRuleFallbackSummary;
	return false;
}

void AAct02RuleReceiveDirector::ResetToInitialPositions()
{
	int32 Reset = 0;
	for (auto& Pair : InitialNPCTransforms)
	{
		AActor* A = Pair.Key.Get();
		if (!IsValid(A))
		{
			continue;
		}
		A->SetActorTransform(Pair.Value, false, nullptr, ETeleportType::TeleportPhysics);
		if (APawn* P = Cast<APawn>(A))
		{
			if (AAIController* AIC = Cast<AAIController>(P->GetController()))
			{
				AIC->StopMovement();
			}
		}
		++Reset;
	}
	DebugMessage(FString::Printf(TEXT("[Act02] reset %d NPC(s) to initial state"), Reset),
		FLinearColor::White);
}

void AAct02RuleReceiveDirector::RegisterDebugConsoleCommands()
{
	if (StartCommand || NextCommand || CancelCommand)
	{
		return;
	}
	IConsoleManager& CM = IConsoleManager::Get();
	StartCommand = CM.RegisterConsoleCommand(
		TEXT("act02.start"),
		TEXT("Trigger ACT02 rule-receive scene."),
		FConsoleCommandDelegate::CreateUObject(this, &AAct02RuleReceiveDirector::BeginAct02Console),
		ECVF_Default);
	NextCommand = CM.RegisterConsoleCommand(
		TEXT("act02.next"),
		TEXT("ACT02: advance from gated seed to reaction phase."),
		FConsoleCommandDelegate::CreateUObject(this, &AAct02RuleReceiveDirector::StartReactionPhase),
		ECVF_Default);
	CancelCommand = CM.RegisterConsoleCommand(
		TEXT("act02.cancel"),
		TEXT("Cancel running ACT02 scene."),
		FConsoleCommandDelegate::CreateUObject(this, &AAct02RuleReceiveDirector::CancelAct02),
		ECVF_Default);
}

void AAct02RuleReceiveDirector::BeginAct02Console()
{
	BeginAct02();
}

void AAct02RuleReceiveDirector::UnregisterDebugConsoleCommands()
{
	IConsoleManager& CM = IConsoleManager::Get();
	if (StartCommand)
	{
		CM.UnregisterConsoleObject(StartCommand);
		StartCommand = nullptr;
	}
	if (NextCommand)
	{
		CM.UnregisterConsoleObject(NextCommand);
		NextCommand = nullptr;
	}
	if (CancelCommand)
	{
		CM.UnregisterConsoleObject(CancelCommand);
		CancelCommand = nullptr;
	}
}

void AAct02RuleReceiveDirector::StartPrescatter()
{
	StateElapsed = 0.f;
	MovementSettleElapsed = 0.f;
	bArrivalSettling = false;
	SceneState = EAct02State::PrescatterToTV;

	if (!ScatterQueryAsset)
	{
		FailAct02(TEXT("ScatterQueryAsset not set"));
		return;
	}

	TArray<AActor*> NPCArray;
	NPCArray.Reserve(NPCs.Num());
	for (FAct02NPCRuntime& N : NPCs)
	{
		if (AActor* A = N.Pawn.Get())
		{
			NPCArray.Add(A);
		}
	}

	UAILiveProjectScatterMover::ScatterNPCsAroundTarget(
		ScatterQueryAsset, NavTargetCached, NPCArray, NavLookTargetCached, this);

	DebugMessage(FString::Printf(TEXT("[Act02] dispatched %d NPC(s) via EQS scatter around NavTarget"),
		NPCArray.Num()), FLinearColor::Green);
}

void AAct02RuleReceiveDirector::TickPrescatter(float Dt)
{
	if (StateElapsed > MovementTimeoutSeconds)
	{
		FailAct02(TEXT("prescatter movement timeout"));
		return;
	}
	if (StateElapsed < ScatterDispatchDelaySeconds)
	{
		return;
	}
	if (AreAllNPCsIdle())
	{
		if (!bArrivalSettling)
		{
			bArrivalSettling = true;
			MovementSettleElapsed = 0.f;
		}
		else
		{
			MovementSettleElapsed += Dt;
			if (MovementSettleElapsed >= ArrivalSettleSeconds)
			{
				StartSeedPhase();
			}
		}
	}
	else
	{
		bArrivalSettling = false;
		MovementSettleElapsed = 0.f;
	}
}

bool AAct02RuleReceiveDirector::AreAllNPCsIdle() const
{
	int32 Valid = 0, Idle = 0;
	for (const FAct02NPCRuntime& N : NPCs)
	{
		AActor* A = N.Pawn.Get();
		if (!IsValid(A)) continue;
		++Valid;
		const APawn* P = Cast<APawn>(A);
		if (!P) { ++Idle; continue; }
		const AAIController* AIC = Cast<AAIController>(P->GetController());
		if (!AIC) { ++Idle; continue; }
		if (AIC->GetMoveStatus() == EPathFollowingStatus::Idle) ++Idle;
	}
	return Valid > 0 && Idle == Valid;
}

void AAct02RuleReceiveDirector::StartSeedPhase()
{
	CurrentRound = 0;
	StateElapsed = 0.f;
	SceneState = EAct02State::SeedDispatch;
	DebugMessage(TEXT("[Act02] seed RunTick: 10 LLM (Reasoner→Parser) in flight"), FLinearColor::Green);
	RunTick();
	SceneState = EAct02State::SeedAwait;
	StateElapsed = 0.f;
}

void AAct02RuleReceiveDirector::StartReactionPhase()
{
	if (SceneState != EAct02State::GatedAwaitNext)
	{
		UE_LOG(LogAct02, Warning,
			TEXT("[Act02] StartReactionPhase ignored, state=%s"), StateName(SceneState));
		return;
	}
	CurrentRound = 1;
	DebugMessage(FString::Printf(TEXT("[Act02] reaction round %d RunTick"), CurrentRound),
		FLinearColor::Green);
	SceneState = EAct02State::ReactionDispatch;
	RunTick();
	SceneState = EAct02State::ReactionAwait;
	StateElapsed = 0.f;
}

// ============================================================================
// T7 — RunTick 主循环（按拍驱动 + Reasoner→Parser 双 LLM + bid 协议 + Floor control）
// ============================================================================

namespace
{
	// === payload builders（anon-namespace；新事件类型的 JSON 拼装 helpers） ===
	// 4 通道写入时直接挪用 Parser 输出的 IntendedJson / BidJson 原文（其内含 text 字段）；
	// 此处 wrapper 只在 Parser 出的 JSON 不含 "text" 键（极少见）时兜底加上 text。
	bool PayloadHasTextField(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		const auto R = TJsonReaderFactory<TCHAR>::Create(Json);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return false;
		FString T;
		return Obj->TryGetStringField(TEXT("text"), T);
	}

	FString WrapPayloadEnsureText(const FString& Json, const FString& FallbackText)
	{
		if (PayloadHasTextField(Json)) return Json;
		// 兜底：Parser 输出无 text 字段 → 反序列化原 JSON，加 text 字段后重新序列化。
		// 不能 wrap 进 _orig，否则 urgency / addressed_to_hint 等字段被埋深一层，
		// ListBidsForTick / ExtractAddressedToHint 等顶层字段读取全部失效。
		TSharedPtr<FJsonObject> Obj;
		const auto R = TJsonReaderFactory<TCHAR>::Create(Json);
		if (FJsonSerializer::Deserialize(R, Obj) && Obj.IsValid())
		{
			Obj->SetStringField(TEXT("text"), FallbackText);
			FString Out;
			const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
			return Out;
		}
		// 反序列化也失败（Json 不是合法对象）→ 包成纯 fallback；保 raw 在 _raw 子段方便审计
		FString Out;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		W->WriteObjectStart();
		W->WriteValue(TEXT("text"), FallbackText);
		W->WriteValue(TEXT("_raw"), Json);
		W->WriteObjectEnd();
		W->Close();
		return Out;
	}

	FString BuildScratchpadPayload(const FString& Scratchpad)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		W->WriteObjectStart();
		W->WriteValue(TEXT("text"), Scratchpad);
		W->WriteObjectEnd();
		W->Close();
		return Out;
	}

	FString BuildNotePayload(const FString& NoteText)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		W->WriteObjectStart();
		W->WriteValue(TEXT("text"), NoteText);
		W->WriteObjectEnd();
		W->Close();
		return Out;
	}

	// 从 Parser 的 IntendedJson 抽 addressed_to_hint 数组，落到 events.AddressedTo 列。
	TArray<FString> ExtractAddressedToHint(const FString& IntendedJson)
	{
		TArray<FString> Out;
		TSharedPtr<FJsonObject> Obj;
		const auto R = TJsonReaderFactory<TCHAR>::Create(IntendedJson);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj->TryGetArrayField(TEXT("addressed_to_hint"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S)) Out.Add(S);
			}
		}
		return Out;
	}

	// 从 IntendedJson 取 intended_action.intent + 序列化 params 到 ParamsJson
	bool ExtractIntendedAction(const FString& IntendedJson, FString& OutIntent, FString& OutParamsJson)
	{
		TSharedPtr<FJsonObject> Obj;
		const auto R = TJsonReaderFactory<TCHAR>::Create(IntendedJson);
		if (!FJsonSerializer::Deserialize(R, Obj) || !Obj.IsValid()) return false;
		const TSharedPtr<FJsonObject>* Act = nullptr;
		if (!Obj->TryGetObjectField(TEXT("intended_action"), Act) || !Act || !(*Act).IsValid()) return false;
		(*Act)->TryGetStringField(TEXT("intent"), OutIntent);
		if (OutIntent.IsEmpty()) return false;
		// Re-serialize params 子对象到字符串（不强制 params 存在）
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if ((*Act)->TryGetObjectField(TEXT("params"), Params) && Params && (*Params).IsValid())
		{
			const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&OutParamsJson);
			FJsonSerializer::Serialize((*Params).ToSharedRef(), W);
		}
		else
		{
			OutParamsJson = TEXT("{}");
		}
		return true;
	}

	FString BuildPublicPayloadFromIntended(const FString& FilteredText, int64 IntendedSeq,
	                                        float ListenerScore, const FString& RequestId)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		W->WriteObjectStart();
		W->WriteValue(TEXT("text"), FilteredText);
		W->WriteValue(TEXT("derived_from_intended_seq"), IntendedSeq);
		W->WriteValue(TEXT("listener_filter_score"), ListenerScore);
		W->WriteValue(TEXT("request_id"), RequestId);
		W->WriteObjectEnd();
		W->Close();
		return Out;
	}

	FString BuildActionIntentPayload(const FString& IntentName, const FString& ParamsJson,
	                                  const FString& RequestId, int64 IntendedSeq)
	{
		// params 是 raw JSON 子对象——用 RawValue 写入（其它字段走 escaped string）。
		FString Out;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		W->WriteObjectStart();
		W->WriteValue(TEXT("text"), FString::Printf(TEXT("intent: %s"), *IntentName));
		W->WriteValue(TEXT("intent"), IntentName);
		W->WriteRawJSONValue(TEXT("params"), ParamsJson);
		W->WriteValue(TEXT("request_id"), RequestId);
		W->WriteValue(TEXT("derived_from_intended_seq"), IntendedSeq);
		W->WriteObjectEnd();
		W->Close();
		return Out;
	}

	FString BuildTickResolvedPayload(int64 TickNo, const FString& WinnerActor,
	                                  int64 WinnerIntendedSeq, int64 DerivedPublicSeq)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		W->WriteObjectStart();
		const FString Text = WinnerActor.IsEmpty()
			? FString::Printf(TEXT("tick %lld resolved: cold"), TickNo)
			: FString::Printf(TEXT("tick %lld resolved: %s wins floor"), TickNo, *WinnerActor);
		W->WriteValue(TEXT("text"), Text);
		W->WriteValue(TEXT("tick_no"), TickNo);
		W->WriteValue(TEXT("winner_actor"), WinnerActor);
		if (!WinnerActor.IsEmpty())
		{
			W->WriteValue(TEXT("winner_intended_seq"), WinnerIntendedSeq);
		}
		W->WriteValue(TEXT("derived_public_seq"), DerivedPublicSeq);
		W->WriteObjectEnd();
		W->Close();
		return Out;
	}

	FString BuildTickAuditPayload(int64 TickNo, const TArray<FAILiveBid>& AllBids,
	                               float FilterScore, bool bRewrote, const TCHAR* Rationale,
	                               int64 ParentTickResolvedSeq)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		W->WriteObjectStart();
		W->WriteValue(TEXT("text"), FString::Printf(TEXT("tick %lld audit"), TickNo));
		W->WriteValue(TEXT("tick_no"), TickNo);
		W->WriteArrayStart(TEXT("all_bids"));
		for (const FAILiveBid& B : AllBids)
		{
			W->WriteObjectStart();
			W->WriteValue(TEXT("actor"), B.Actor);
			W->WriteValue(TEXT("urgency"), B.Urgency);
			W->WriteValue(TEXT("bid_offset"), B.BidOffset);
			W->WriteValue(TEXT("runtime_adj"), B.RuntimeAdj);
			W->WriteValue(TEXT("final_score"), B.FinalScore);
			W->WriteObjectEnd();
		}
		W->WriteArrayEnd();
		W->WriteObjectStart(TEXT("filter_decision"));
		W->WriteValue(TEXT("score"), FilterScore);
		W->WriteValue(TEXT("rewrote"), bRewrote);
		W->WriteValue(TEXT("rationale"), Rationale);
		W->WriteObjectEnd();
		W->WriteValue(TEXT("parent_tick_resolved_seq"), ParentTickResolvedSeq);
		W->WriteObjectEnd();
		W->Close();
		return Out;
	}

	FString BuildParseFailedPayload(int32 NPCIndex, const FString& RequestId,
	                                  const FString& ReasonerRawText,
	                                  const FString& ParserErrorReason,
	                                  const FString& ParserFailedStage)
	{
		const FString RawSnippet = ReasonerRawText.Left(256);
		FString Out;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
		W->WriteObjectStart();
		W->WriteValue(TEXT("text"),
			FString::Printf(TEXT("parser failed for NPC%02d after 3 reject samples"), NPCIndex));
		W->WriteValue(TEXT("npc_index"), NPCIndex);
		W->WriteValue(TEXT("request_id"), RequestId);
		W->WriteValue(TEXT("failure_source"), TEXT("parser_llm"));   // 决策 #12
		W->WriteValue(TEXT("parser_error_reason"), ParserErrorReason);
		W->WriteValue(TEXT("failed_stage"), ParserFailedStage);
		W->WriteValue(TEXT("raw_text_truncated_256"), RawSnippet);
		W->WriteObjectEnd();
		W->Close();
		return Out;
	}
}

void AAct02RuleReceiveDirector::RunTick()
{
	CurrentTickInflight.Reset();
	ActorToIntendedSeq.Reset();
	ActorToRequestId.Reset();

	UAILiveEventStoreSubsystem* Store = GetEventStore(this);
	if (!Store || !Store->IsGameOpen())
	{
		FailAct02(TEXT("EventStore not open at RunTick"));
		return;
	}

	// 决策 #9：Director 不维护 CurrentTickNo 副本；以 EventStore 为权威源。
	const int64 NewTick = Store->GetCurrentTickNo() + 1;
	const int64 AnchorSeq = Store->BeginTick(static_cast<int32>(NewTick));
	if (AnchorSeq <= 0)
	{
		FailAct02(FString::Printf(TEXT("BeginTick(%lld) failed"), NewTick));
		return;
	}

	FString GameRule;
	LoadGameRule(GameRule);

	// per-attempt 45s × 3 retries ≈ 135s 总（决策 #3）；GT 端不再用 LLMTimeoutSeconds 整体 fail。
	// 实测 Qwen3 单次 Reasoner→Parser 串联 ~27s，dashscope 偶发 30s+；18s 不够。
	constexpr float kPerAttemptTimeoutSec = 45.f;

	for (FAct02NPCRuntime& N : NPCs)
	{
		if (!N.Config.Battle.bAlive) continue;

		const FString ActorId = FString::Printf(TEXT("NPC%02d"), N.Config.NPCIndex);

		// 决策 #1：seed/reaction 共用 BuildReasonerSystemPrompt（§A.1 四通道格式）
		const FString SystemPrompt = BuildReasonerSystemPrompt(N.Config, GameRule, CurrentRound);

		AILivePromptAssembler::FAssembleOptions Opt;
		Opt.Viewer        = ActorId;
		Opt.CurrentRound  = CurrentRound;
		Opt.NearWindowK   = 5;
		Opt.GameRule      = GameRule;
		Opt.ChallengeText = TEXT("");
		const FString UserPrompt = AILivePromptAssembler::AssembleUserPrompt(Store, N.Config, Opt);

		const AILiveAgentRoster::FProviderEndpoint Ep =
			AILiveAgentRoster::ResolveProviderEndpoint(N.Config.Provider);

		FInflightTick F;
		F.NPCIndex  = N.Config.NPCIndex;
		F.ActorId   = ActorId;
		F.Provider  = N.Config.Provider;
		F.Model     = Ep.Model;
		F.RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);

		// === in-flight 配对协议（principles §5.4）：先写 system.llm_inflight 再发请求
		const FString SysHash   = AILiveUtil::Sha256Fingerprint(SystemPrompt);
		const FString UserHash  = AILiveUtil::Sha256Fingerprint(UserPrompt);
		const FString StartedAt = FDateTime::UtcNow().ToIso8601();

		FAILiveEvent Pre;
		Pre.RoundNo     = CurrentRound;
		Pre.Phase       = ResolvePhase(CurrentRound);
		Pre.Actor       = TEXT("orchestrator");
		Pre.EventType   = EAILiveEventType::SystemLLMInflight;
		Pre.Visibility  = { TEXT("system") };
		Pre.PayloadJson = BuildLLMInflightPayloadJson(F.NPCIndex, F.RequestId, SysHash, UserHash, StartedAt);
		const int64 PreSeq = Store->AppendEvent(Pre);
		if (PreSeq <= 0)
		{
			UE_LOG(LogAct02, Error,
				TEXT("[Act02] in-flight write failed for NPC%02d; aborting RunTick"), F.NPCIndex);
			FailAct02(FString::Printf(
				TEXT("in-flight AppendEvent failed for NPC%02d"), F.NPCIndex));
			CurrentTickInflight.Reset();
			return;
		}

		// worker 线程内串行 Reasoner→Parser，3 次 reject sample；TPromise/TFuture 单管道
		const FNPCAgentConfig CfgCopy = N.Config;
		const FString ReqId = F.RequestId;
		F.Future = Async(EAsyncExecution::ThreadPool,
			[CfgCopy, ReqId, SystemPrompt, UserPrompt]() -> FAgentTickResult
			{
				return AAct02RuleReceiveDirector::RunAgentTickInWorker(
					CfgCopy, ReqId, SystemPrompt, UserPrompt, kPerAttemptTimeoutSec);
			});

		CurrentTickInflight.Emplace(MoveTemp(F));
	}

	UE_LOG(LogAct02, Log,
		TEXT("[Act02] RunTick %lld dispatched %d agent (round=%d)"),
		NewTick, CurrentTickInflight.Num(), CurrentRound);
}

AAct02RuleReceiveDirector::FAgentTickResult
AAct02RuleReceiveDirector::RunAgentTickInWorker(
	const FNPCAgentConfig& Cfg,
	const FString& RequestId,
	const FString& SystemPrompt,
	const FString& UserPrompt,
	float PerAttemptTimeoutSec)
{
	FAgentTickResult Out;
	Out.NPCIndex  = Cfg.NPCIndex;
	Out.ActorId   = FString::Printf(TEXT("NPC%02d"), Cfg.NPCIndex);
	Out.RequestId = RequestId;
	Out.Provider  = Cfg.Provider;
	const AILiveAgentRoster::FProviderEndpoint Ep =
		AILiveAgentRoster::ResolveProviderEndpoint(Cfg.Provider);
	Out.Model = Ep.Model;

	for (int32 Attempt = 1; Attempt <= 3; ++Attempt)
	{
		OpenAIChat::FRequest Req;
		Req.ApiKey       = Ep.ApiKey;
		Req.Endpoint     = Ep.Endpoint;
		Req.Model        = Ep.Model;
		Req.SystemPrompt = SystemPrompt;
		Req.UserPrompt   = UserPrompt;
		// 决策 #2：§A.1 是带 <SCRATCHPAD>/<INTENDED>/<BID>/<NOTE_TO_SELF> 标签的 raw text，
		// 非 JSON。response_format=json_object 会把标签吞掉 → Parser 100% reject。
		Req.bResponseFormatJson = false;
		Req.TimeoutSec = PerAttemptTimeoutSec;
		Req.Temperature = 0.7f;
		if (Cfg.Provider == ELLMProvider::GLM)
		{
			Req.ThinkingType = TEXT("disabled");
		}

		OpenAIChat::FResult R = OpenAIChat::RequestBlocking(Req);
		Out.ReasonerResult = R;
		Out.ReasonerRawText = R.RawContent;
		if (!R.bSuccess)
		{
			Out.FailureReason = (R.HttpStatus == 0)
				? TEXT("reasoner_timeout")
				: TEXT("reasoner_http_failed");
			continue;
		}

		AILiveParser::FParseRequest PReq;
		PReq.RawText = R.RawContent;
		PReq.AgentId = Out.ActorId;
		AILiveParser::FParseResult P = AILiveParser::ParseFourChannels(PReq);
		if (P.bOk)
		{
			Out.Parsed = P;
			return Out;
		}
		Out.FailureReason = P.ErrorReason;
		Out.FailedStage   = P.FailedStage;
	}

	Out.bAbstain = true;
	return Out;
}

void AAct02RuleReceiveDirector::TickLLMAwait(float Dt)
{
	// 决策 #3：per-agent 45s × 3 在 worker 内独立超时；GT 端 200s 防死锁，
	// 超过把未 ready 的 future 标 abstain（不再 FailAct02）。
	constexpr float kGTWatchdogSec = 200.f;

	bool bAllReady = true;
	for (const FInflightTick& F : CurrentTickInflight)
	{
		if (!F.Future.IsReady())
		{
			bAllReady = false;
			break;
		}
	}
	if (!bAllReady)
	{
		if (StateElapsed > kGTWatchdogSec)
		{
			UE_LOG(LogAct02, Warning,
				TEXT("[Act02] RunTick GT watchdog %.1fs reached; forcing GatherTickAndResolveFloor "
				     "(未 ready 的 agent 视为 abstain)"),
				StateElapsed);
			GatherTickAndResolveFloor();
		}
		return;
	}

	GatherTickAndResolveFloor();
}

void AAct02RuleReceiveDirector::GatherTickAndResolveFloor()
{
	UAILiveEventStoreSubsystem* Store = GetEventStore(this);
	if (!Store || !Store->IsGameOpen())
	{
		FailAct02(TEXT("EventStore not open at GatherTickAndResolveFloor"));
		return;
	}
	const int64 ThisTick = Store->GetCurrentTickNo();

	ActorToIntendedSeq.Reset();
	ActorToRequestId.Reset();
	TMap<FString, float> BidOffsets;

	// === 阶段 A：写 4 通道 / abstain ===========================================
	for (FInflightTick& F : CurrentTickInflight)
	{
		if (!F.Future.IsReady())
		{
			// GT watchdog 触发但 future 还没 ready → 该 agent 视为 abstain（无 result 可用）
			FAILiveEvent Pf;
			Pf.Actor       = F.ActorId;
			Pf.EventType   = EAILiveEventType::SystemParseFailed;
			Pf.RoundNo     = CurrentRound;
			Pf.Phase       = ResolvePhase(CurrentRound);
			Pf.Visibility  = { TEXT("orchestrator"), F.ActorId };
			Pf.PayloadJson = BuildParseFailedPayload(
				F.NPCIndex, F.RequestId, FString(),
				TEXT("gt_watchdog_timeout"), TEXT("future_not_ready"));
			Store->AppendEvent(Pf);
			ActorToRequestId.Add(F.ActorId, F.RequestId);
			continue;
		}

		FAgentTickResult R = F.Future.Get();
		ActorToRequestId.Add(R.ActorId, R.RequestId);

		if (R.bAbstain)
		{
			// 决策 #11/#13：actor=NPCxx, visibility=["orchestrator", NPCxx], failure_source=parser_llm
			FAILiveEvent Pf;
			Pf.Actor       = R.ActorId;
			Pf.EventType   = EAILiveEventType::SystemParseFailed;
			Pf.RoundNo     = CurrentRound;
			Pf.Phase       = ResolvePhase(CurrentRound);
			Pf.Visibility  = { TEXT("orchestrator"), R.ActorId };
			Pf.PayloadJson = BuildParseFailedPayload(
				R.NPCIndex, R.RequestId, R.ReasonerRawText,
				R.FailureReason, R.FailedStage);
			Pf.RawLLMOutput = R.ReasonerRawText.Left(2048);
			Store->AppendEvent(Pf);

			UE_LOG(LogAct02, Warning,
				TEXT("[Act02] NPC%02d abstain after 3 reject samples (reason=%s stage=%s)"),
				R.NPCIndex, *R.FailureReason, *R.FailedStage);
			continue;
		}

		// 4 通道原子提交（principles §7.2）
		TArray<FAILiveEvent> Group;
		{
			FAILiveEvent Sp;
			Sp.Actor      = R.ActorId;
			Sp.EventType  = EAILiveEventType::SpeechScratchpad;
			Sp.RoundNo    = CurrentRound;
			Sp.Phase      = ResolvePhase(CurrentRound);
			Sp.Visibility = { R.ActorId };
			Sp.PayloadJson  = BuildScratchpadPayload(R.Parsed.Scratchpad);
			Sp.RawLLMOutput = R.ReasonerResult.RawResponsePayload;  // 仅一条挂 raw（避免 4× 重复）
			Group.Add(Sp);
		}
		{
			FAILiveEvent In;
			In.Actor       = R.ActorId;
			In.EventType   = EAILiveEventType::SpeechIntended;
			In.RoundNo     = CurrentRound;
			In.Phase       = ResolvePhase(CurrentRound);
			In.Visibility  = { R.ActorId };
			In.AddressedTo = ExtractAddressedToHint(R.Parsed.IntendedJson);   // 决策 #14
			In.PayloadJson = WrapPayloadEnsureText(R.Parsed.IntendedJson, FString());
			Group.Add(In);
		}
		{
			FAILiveEvent Bd;
			Bd.Actor      = R.ActorId;
			Bd.EventType  = EAILiveEventType::Bid;
			Bd.RoundNo    = CurrentRound;
			Bd.Phase      = ResolvePhase(CurrentRound);
			Bd.Visibility = { TEXT("orchestrator") };                      // §5.2bis 核心约束
			Bd.PayloadJson = WrapPayloadEnsureText(R.Parsed.BidJson,
				FString::Printf(TEXT("bid by %s"), *R.ActorId));
			Group.Add(Bd);
		}
		{
			FAILiveEvent Nt;
			Nt.Actor      = R.ActorId;
			Nt.EventType  = EAILiveEventType::SpeechNote;
			Nt.RoundNo    = CurrentRound;
			Nt.Phase      = ResolvePhase(CurrentRound);
			Nt.Visibility = { R.ActorId };
			Nt.PayloadJson = BuildNotePayload(R.Parsed.NoteText);
			Group.Add(Nt);
		}
		const int64 FirstSeq = Store->AppendEventsAtomically(Group);
		if (FirstSeq < 0)
		{
			FailAct02(FString::Printf(
				TEXT("AppendEventsAtomically(4-channel) failed for NPC%02d"), R.NPCIndex));
			return;
		}
		// Group[1].Seq 是 intended seq（4-channel 顺序：scratchpad, intended, bid, note）
		ActorToIntendedSeq.Add(R.ActorId, Group[1].Seq);
		BidOffsets.Add(R.ActorId, R.Provider == ELLMProvider::DeepSeek
			? 0.f
			: 0.f);  // MVP: BidOffset 全部 0；下阶段从 agent_calibration 表读
	}

	// 把 Roster 里 BidOffset 也注入（覆盖 MVP 默认 0）
	for (const FAct02NPCRuntime& N : NPCs)
	{
		const FString A = FString::Printf(TEXT("NPC%02d"), N.Config.NPCIndex);
		if (BidOffsets.Contains(A))
		{
			BidOffsets[A] = N.Config.Battle.BidOffset;
		}
	}

	// === 阶段 B：ResolveFloor + ListenerFilter + 衍生 speech.public ===========
	TArray<FString> Eligible;
	ActorToIntendedSeq.GenerateKeyArray(Eligible);
	const FAILiveTickResolution Res = Store->ResolveFloor(
		ThisTick, Eligible, BidOffsets, /*ColdThreshold=*/3.0f);

	int64 PublicSeq = 0;
	FString FilteredText;
	if (!Res.WinnerActor.IsEmpty())
	{
		FAILiveEvent WinnerIntended;
		if (!Store->Quote(Res.WinnerIntendedSeq, Res.WinnerActor, WinnerIntended))
		{
			FailAct02(FString::Printf(
				TEXT("winner intended seq=%lld not visible to %s"),
				Res.WinnerIntendedSeq, *Res.WinnerActor));
			return;
		}
		float FilterScore = 0.f;
		FilteredText = ListenerFilter::Apply(WinnerIntended.PayloadJson, FilterScore);

		FAILiveEvent Pub;
		Pub.Actor         = Res.WinnerActor;
		Pub.EventType     = EAILiveEventType::SpeechPublic;
		Pub.RoundNo       = CurrentRound;
		Pub.Phase         = ResolvePhase(CurrentRound);
		Pub.Visibility    = { TEXT("public") };
		Pub.AddressedTo   = WinnerIntended.AddressedTo;                  // 决策 #14：透传
		Pub.ParentEventId = WinnerIntended.EventId;                      // §5.2 衍生关系
		Pub.PayloadJson   = BuildPublicPayloadFromIntended(
			FilteredText, Res.WinnerIntendedSeq, /*listener_filter_score=*/0.0f,
			ActorToRequestId.FindChecked(Res.WinnerActor));
		PublicSeq = Store->AppendEvent(Pub);
		if (PublicSeq <= 0)
		{
			UE_LOG(LogAct02, Warning,
				TEXT("[Act02] speech.public derive failed for winner %s seq=%lld"),
				*Res.WinnerActor, Res.WinnerIntendedSeq);
		}
	}

	// === 阶段 C：派生 action.intent（对所有 agent，不仅 winner） ===============
	DeriveActionIntents(ThisTick);

	// === 阶段 D：tick_resolved + tick_audit 拆分写入 ==========================
	WriteTickResolvedAndAudit(Res, PublicSeq);

	// === 阶段 E：进入 SpeechSpeak / 推进（决策 #5：cold tick 跳 TTS 直接 advance） ===
	if (!Res.WinnerActor.IsEmpty())
	{
		const int32 WinnerIdx = NPCs.IndexOfByPredicate(
			[&](const FAct02NPCRuntime& X)
			{
				return FString::Printf(TEXT("NPC%02d"), X.Config.NPCIndex) == Res.WinnerActor;
			});
		LastSpeakerIndex = WinnerIdx;
		LastSentence = FilteredText;
		StartSpeak(WinnerIdx, FilteredText);
		SceneState = (CurrentRound == 0) ? EAct02State::SeedSpeak : EAct02State::ReactionSpeak;
		StateElapsed = 0.f;
		DebugMessage(FString::Printf(
			TEXT("[Act02] tick %lld winner %s: \"%s\""),
			ThisTick, *Res.WinnerActor, *FilteredText), FLinearColor::Green);
	}
	else
	{
		DebugMessage(FString::Printf(TEXT("[Act02] tick %lld cold (no winner)"), ThisTick),
			FLinearColor::Yellow);
		if (CurrentRound == 0)
		{
			SceneState = EAct02State::GatedAwaitNext;
			StateElapsed = 0.f;
		}
		else
		{
			AdvanceReactionRound();
		}
	}
}

void AAct02RuleReceiveDirector::DeriveActionIntents(int64 InTickNo)
{
	UAILiveEventStoreSubsystem* Store = GetEventStore(this);
	if (!Store || !Store->IsGameOpen()) return;

	const TArray<FAILiveEvent> AllIntended =
		Store->QuoteByEventTypeAndTick(EAILiveEventType::SpeechIntended, InTickNo);

	for (const FAILiveEvent& Iv : AllIntended)
	{
		FString IntentName, ParamsJson;
		if (!ExtractIntendedAction(Iv.PayloadJson, IntentName, ParamsJson)) continue;

		const FString* RidPtr = ActorToRequestId.Find(Iv.Actor);
		const FString Rid = RidPtr ? *RidPtr : FString();

		FAILiveEvent Ai;
		Ai.Actor         = Iv.Actor;
		Ai.EventType     = EAILiveEventType::ActionIntent;
		Ai.RoundNo       = CurrentRound;
		Ai.Phase         = ResolvePhase(CurrentRound);
		Ai.Visibility    = { Iv.Actor };                                  // 具体 NPCxx，禁 "self"
		Ai.ParentEventId = Iv.EventId;                                    // 链接 → speech.intended
		Ai.PayloadJson   = BuildActionIntentPayload(IntentName, ParamsJson, Rid, Iv.Seq);
		if (Store->AppendEvent(Ai) <= 0)
		{
			UE_LOG(LogAct02, Warning,
				TEXT("[Act02] action.intent derive failed for actor=%s intended_seq=%lld"),
				*Iv.Actor, Iv.Seq);
		}
	}
}

int64 AAct02RuleReceiveDirector::WriteTickResolvedAndAudit(
	const FAILiveTickResolution& Res, int64 DerivedPublicSeq)
{
	UAILiveEventStoreSubsystem* Store = GetEventStore(this);
	if (!Store || !Store->IsGameOpen()) return 0;
	const int64 ThisTick = Store->GetCurrentTickNo();

	FAILiveEvent TR;
	TR.Actor       = TEXT("orchestrator");
	TR.EventType   = EAILiveEventType::OrchestratorTickResolved;
	TR.RoundNo     = CurrentRound;
	TR.Phase       = ResolvePhase(CurrentRound);
	TR.Visibility  = { TEXT("public") };                                  // 任务卡：tick_resolved=public，不含 all_bids
	TR.PayloadJson = BuildTickResolvedPayload(
		ThisTick, Res.WinnerActor,
		Res.WinnerActor.IsEmpty() ? 0 : Res.WinnerIntendedSeq,
		DerivedPublicSeq);
	const int64 TRSeq = Store->AppendEvent(TR);
	if (TRSeq <= 0)
	{
		UE_LOG(LogAct02, Warning, TEXT("[Act02] tick_resolved write failed (tick=%lld)"), ThisTick);
		return 0;
	}

	FAILiveEvent TA;
	TA.Actor         = TEXT("orchestrator");
	TA.EventType     = EAILiveEventType::OrchestratorTickAudit;
	TA.RoundNo       = CurrentRound;
	TA.Phase         = ResolvePhase(CurrentRound);
	TA.Visibility    = { TEXT("orchestrator") };
	TA.ParentEventId = TR.EventId;                                        // 任务卡：parent → 同拍 tick_resolved
	TA.PayloadJson   = BuildTickAuditPayload(
		ThisTick, Res.AllBids,
		/*FilterScore=*/0.0f, /*Rewrote=*/false, TEXT("mvp_passthrough"),
		TRSeq);
	if (Store->AppendEvent(TA) <= 0)
	{
		UE_LOG(LogAct02, Warning, TEXT("[Act02] tick_audit write failed (tick=%lld)"), ThisTick);
	}
	return TRSeq;
}

void AAct02RuleReceiveDirector::StartSpeak(int32 NPCIndex, const FString& Text)
{
	CurrentSpeaker = INDEX_NONE;
	SpeakElapsed = 0.f;
	bSpeechFinished = false;
	bSpeechFailed = false;

	if (!NPCs.IsValidIndex(NPCIndex))
	{
		bSpeechFinished = true;
		bSpeechFailed = true;
		return;
	}
	const FAct02NPCRuntime& N = NPCs[NPCIndex];
	CurrentSpeaker = N.Config.NPCIndex;
	AActor* Pawn = N.Pawn.Get();
	const FString MinimaxKey = ProjectEnvLoader::Get(TEXT("minimax"));
	if (!Pawn || MinimaxKey.IsEmpty() || Text.IsEmpty())
	{
		UE_LOG(LogAct02, Warning,
			TEXT("[Act02] StartSpeak skipping TTS (pawn=%s key_empty=%d text_empty=%d) — auto-finish"),
			*GetNameSafe(Pawn), MinimaxKey.IsEmpty() ? 1 : 0, Text.IsEmpty() ? 1 : 0);
		bSpeechFinished = true;
		bSpeechFailed = MinimaxKey.IsEmpty();
		return;
	}

	TWeakObjectPtr<AAct02RuleReceiveDirector> WeakSelf(this);
	FOnMinimaxSpeechFinishedNative Cb = FOnMinimaxSpeechFinishedNative::CreateLambda(
		[WeakSelf](bool bOk)
		{
			if (AAct02RuleReceiveDirector* Self = WeakSelf.Get())
			{
				Self->HandleSpeechFinished(bOk);
			}
		});

	const bool bDispatched = UMinimaxACELibrary::TriggerMinimaxSpeechFromPawnNative(
		this, Pawn, Text, MinimaxKey,
		N.Config.Identity.Voice, TEXT("https://api.minimaxi.com/v1/t2a_v2"),
		A2FProviderName, Cb);

	if (!bDispatched)
	{
		bSpeechFinished = true;
		bSpeechFailed = true;
		UE_LOG(LogAct02, Warning, TEXT("[Act02] TTS dispatch returned false; auto-finish"));
	}
}

void AAct02RuleReceiveDirector::TickSpeakWatchdog(float Dt)
{
	SpeakElapsed += Dt;
	if (bSpeechFinished)
	{
		const bool bWasSeed = (SceneState == EAct02State::SeedSpeak);
		UE_LOG(LogAct02, Log,
			TEXT("[Act02] speech finished npc=%d failed=%d elapsed=%.1fs"),
			CurrentSpeaker, bSpeechFailed ? 1 : 0, SpeakElapsed);

		if (bWasSeed)
		{
			DebugMessage(TEXT("[Act02] seed phase complete; press 3 / act02.next to continue"),
				FLinearColor::Yellow);
			SceneState = EAct02State::GatedAwaitNext;
			StateElapsed = 0.f;
		}
		else
		{
			AdvanceReactionRound();
		}
		return;
	}
	if (SpeakElapsed > SpeechWatchdogSeconds)
	{
		UE_LOG(LogAct02, Warning,
			TEXT("[Act02] speech watchdog %.1fs elapsed without OnFinished; forcing advance"),
			SpeakElapsed);
		bSpeechFinished = true;
		bSpeechFailed = true;
	}
}

void AAct02RuleReceiveDirector::HandleSpeechFinished(bool bSuccess)
{
	bSpeechFinished = true;
	bSpeechFailed = !bSuccess;
}

void AAct02RuleReceiveDirector::AdvanceReactionRound()
{
	if (CurrentRound >= ReactionRoundCount)
	{
		CompleteAct02();
		return;
	}
	CurrentRound += 1;
	DebugMessage(FString::Printf(TEXT("[Act02] reaction round %d RunTick"), CurrentRound),
		FLinearColor::Green);
	SceneState = EAct02State::ReactionDispatch;
	RunTick();
	SceneState = EAct02State::ReactionAwait;
	StateElapsed = 0.f;
}

void AAct02RuleReceiveDirector::CompleteAct02()
{
	OnAct02Completed.Broadcast();
	DebugMessage(TEXT("[Act02] complete"), FLinearColor::Green);
	SceneState = EAct02State::Idle;
}

void AAct02RuleReceiveDirector::FailAct02(const FString& Reason)
{
	OnAct02Failed.Broadcast(Reason);
	DebugMessage(FString::Printf(TEXT("[Act02] failed: %s"), *Reason), FLinearColor::Red);
	SceneState = EAct02State::Idle;
}

void AAct02RuleReceiveDirector::DebugMessage(const FString& Msg, const FLinearColor& Color) const
{
	UE_LOG(LogAct02, Log, TEXT("%s"), *Msg);
	if (bDebugPrintScreen)
	{
		UKismetSystemLibrary::PrintString(this, Msg, true, false, Color, 4.0f);
	}
}

// ====== Prompt builders（T7：§A.1 四通道格式 — seed + reaction 共用） ======

FString AAct02RuleReceiveDirector::BuildReasonerSystemPrompt(
	const FNPCAgentConfig& Cfg, const FString& GameRule, int32 InCurrentRound) const
{
	// principles §A.1 STRICT OUTPUT FORMAT：
	//   <SCRATCHPAD>...</SCRATCHPAD>
	//   <INTENDED>{...JSON...}</INTENDED>
	//   <BID>{...JSON...}</BID>
	//   <NOTE_TO_SELF>...</NOTE_TO_SELF>
	// Reasoner 必须输出严格 4 段标记文本（不是 JSON 包装）；Parser LLM 解 4 段。
	const TCHAR* PhaseDesc = (InCurrentRound == 0)
		? TEXT("seed phase（刚看完规则视频，第一拍开场）")
		: TEXT("day_discuss phase（已展开多拍讨论）");

	return FString::Printf(
		TEXT("你是 AI %s（%s）。AI 知道自己是 AI，不扮演人类——不赋予人类职业、教育、地域、年龄、")
		TEXT("姓名格式等背景叙事。只赋予外观符号：名字、声线、类人虚拟形象。\n\n")
		TEXT("[GAME RULE]\n%s\n\n")
		TEXT("[CURRENT TICK]\n现在 round=%d %s。请阅读 user prompt 中的事件流，按下面 STRICT OUTPUT FORMAT 输出。\n\n")
		TEXT("[STRICT OUTPUT FORMAT — 4 SECTIONS]\n")
		TEXT("严格按下列顺序输出 4 段标记文本，每段一对开闭标签独占行；不要 JSON 包装、不要 markdown。\n\n")
		TEXT("<SCRATCHPAD>\n你的私有推理（不会公开）。可以直白说出你怀疑谁、信任谁、想隐瞒什么。这一段只有你自己能看到。控制在 200 字内。\n</SCRATCHPAD>\n\n")
		TEXT("<INTENDED>\n{\"text\":\"你这一拍想说的话（中文，<= 80 字）\",")
		TEXT("\"intended_action\":{\"intent\":\"<可选 intent 名>\",\"params\":{...}},")
		TEXT("\"addressed_to_hint\":[\"NPC07\"]}\n")
		TEXT("说明：text 必填，intended_action / addressed_to_hint 可选；省略时仍输出闭合 JSON 对象 {\"text\":\"...\"}。\n")
		TEXT("此段会进入持久层，即便没抢中 floor 也永久留存——不要在这里赖账。\n")
		TEXT("</INTENDED>\n\n")
		TEXT("<BID>\n{\"urgency\":<0.0-10.0>,\"proposed_target\":\"<可选 actor>\",\"relates_to_seq\":<可选 int>,\"rationale\":\"<私有理由 <= 60 字>\"}\n")
		TEXT("urgency 校准：9-10 = 必须说话（拍桌子级别）；6-8 = 强烈想说；3-5 = 中性；0-2 = holding back / 让别人发言；")
		TEXT("最高者抢中 floor 衍生 public，最高 < 3.0 视为冷场。连续抢中会触发反霸麦衰减。\n")
		TEXT("</BID>\n\n")
		TEXT("<NOTE_TO_SELF>\n给未来自己的散记（散文 / 关键词 / 三五行皆可）。下一拍你会看到自己的 note。控制在 100 字内。\n</NOTE_TO_SELF>\n\n")
		TEXT("严格按上面 4 段顺序输出标签 + 内容。不要任何前后缀文字。"),
		*Cfg.Identity.FullName, *Cfg.VoicePresentationHint, *GameRule, InCurrentRound, PhaseDesc);
}

// ====== EventStore payload builders ======
// in-flight 配对协议（principles §5.4）：发起 LLM 请求前必须先写一条 system.llm_inflight。
// T5 落地，T7 复用。

FString AAct02RuleReceiveDirector::BuildLLMInflightPayloadJson(
	int32 NPCIndex, const FString& RequestId,
	const FString& SystemPromptHash, const FString& UserPromptHash,
	const FString& StartedAtIso8601)
{
	return FString::Printf(
		TEXT("{\"text\":\"in-flight to NPC%02d\",\"npc_index\":%d,")
		TEXT("\"request_id\":\"%s\",\"system_prompt_hash\":\"%s\",")
		TEXT("\"user_prompt_hash\":\"%s\",\"started_at\":\"%s\"}"),
		NPCIndex, NPCIndex,
		*RequestId, *SystemPromptHash, *UserPromptHash,
		*StartedAtIso8601);
}
