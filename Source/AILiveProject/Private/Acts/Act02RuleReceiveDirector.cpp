#include "Acts/Act02RuleReceiveDirector.h"

#include "AIController.h"
#include "AILiveProjectScatterMover.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MinimaxACELibrary.h"
#include "Navigation/PathFollowingComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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

	// Session timestamp for log dir
	SessionTimestamp = FDateTime::Now().ToString(TEXT("%Y-%m-%d_%H%M%S"));
	LastSpeakerIndex = INDEX_NONE;
	LastSentence.Reset();
	for (FAct02NPCRuntime& N : NPCs)
	{
		N.UnspokenContent.Reset();
	}

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
	CurrentInflight.Reset();
	CurrentAnswers.Reset();
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
	DebugMessage(TEXT("[Act02] seed dispatch: 10 LLM in flight"), FLinearColor::Green);
	DispatchLLMs(/*bSeed=*/true);
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
	DebugMessage(FString::Printf(TEXT("[Act02] reaction round %d dispatch"), CurrentRound),
		FLinearColor::Green);
	SceneState = EAct02State::ReactionDispatch;
	DispatchLLMs(/*bSeed=*/false);
	SceneState = EAct02State::ReactionAwait;
	StateElapsed = 0.f;
}

void AAct02RuleReceiveDirector::DispatchLLMs(bool bSeed)
{
	CurrentInflight.Reset();
	CurrentAnswers.Reset();

	FString GameRule;
	LoadGameRule(GameRule);

	FString SpeakerName;
	if (LastSpeakerIndex != INDEX_NONE && NPCs.IsValidIndex(LastSpeakerIndex))
	{
		SpeakerName = NPCs[LastSpeakerIndex].Config.Identity.FullName;
	}

	for (FAct02NPCRuntime& N : NPCs)
	{
		// Reaction phase: skip the previous speaker
		if (!bSeed && LastSpeakerIndex != INDEX_NONE)
		{
			const int32 ArrayIdx = NPCs.IndexOfByPredicate(
				[&](const FAct02NPCRuntime& X) { return X.Config.NPCIndex == N.Config.NPCIndex; });
			if (ArrayIdx == LastSpeakerIndex) continue;
		}

		FInflight Item;
		Item.NPCIndex = N.Config.NPCIndex;
		Item.Provider = N.Config.Provider;

		if (bSeed)
		{
			Item.SystemPrompt = BuildSeedSystemPrompt(N.Config, GameRule);
			Item.UserPrompt = BuildSeedUserPrompt(N.Config);
		}
		else
		{
			Item.SystemPrompt = BuildReactionSystemPrompt(
				N.Config, SpeakerName, LastSentence, N.UnspokenContent);
			Item.UserPrompt = BuildReactionUserPrompt(N.Config);
		}

		const AILiveAgentRoster::FProviderEndpoint Ep =
			AILiveAgentRoster::ResolveProviderEndpoint(N.Config.Provider);
		Item.Model = Ep.Model;

		OpenAIChat::FRequest Req;
		Req.ApiKey = Ep.ApiKey;
		Req.Endpoint = Ep.Endpoint;
		Req.Model = Ep.Model;
		Req.SystemPrompt = Item.SystemPrompt;
		Req.UserPrompt = Item.UserPrompt;
		Req.bResponseFormatJson = true;
		Req.TimeoutSec = LLMTimeoutSeconds - 5.f;
		Req.Temperature = 0.7f;
		if (N.Config.Provider == ELLMProvider::GLM)
		{
			Req.ThinkingType = TEXT("disabled");
		}

		Item.Future = Async(EAsyncExecution::ThreadPool,
			[Req = MoveTemp(Req)]() -> OpenAIChat::FResult
			{
				return OpenAIChat::RequestBlocking(Req);
			});

		CurrentInflight.Emplace(MoveTemp(Item));
	}

	UE_LOG(LogAct02, Log,
		TEXT("[Act02] dispatched %d LLM (seed=%d round=%d)"),
		CurrentInflight.Num(), bSeed ? 1 : 0, CurrentRound);
}

void AAct02RuleReceiveDirector::TickLLMAwait(float Dt)
{
	if (StateElapsed > LLMTimeoutSeconds)
	{
		FailAct02(FString::Printf(
			TEXT("LLM timeout after %.1fs (state=%s)"),
			StateElapsed, StateName(SceneState)));
		return;
	}
	bool bAllReady = true;
	for (const FInflight& F : CurrentInflight)
	{
		if (!F.Future.IsReady())
		{
			bAllReady = false;
			break;
		}
	}
	if (!bAllReady)
	{
		return;
	}

	// All ready — gather results
	if (SceneState == EAct02State::SeedAwait)
	{
		GatherSeedAndPickWinner();
	}
	else if (SceneState == EAct02State::ReactionAwait)
	{
		GatherReactionAndPickWinner();
	}
}

EWillingness AAct02RuleReceiveDirector::WillingnessFromString(const FString& S) const
{
	const FString L = S.ToLower();
	if (L == TEXT("extremely_strong")) return EWillingness::ExtremelyStrong;
	if (L == TEXT("strong"))           return EWillingness::Strong;
	if (L == TEXT("moderate"))         return EWillingness::Moderate;
	if (L == TEXT("weak"))             return EWillingness::Weak;
	if (L == TEXT("none"))             return EWillingness::None;
	return EWillingness::None;
}

const TCHAR* AAct02RuleReceiveDirector::WillingnessLabel(EWillingness W) const
{
	switch (W)
	{
	case EWillingness::ExtremelyStrong: return TEXT("extremely_strong");
	case EWillingness::Strong:          return TEXT("strong");
	case EWillingness::Moderate:        return TEXT("moderate");
	case EWillingness::Weak:            return TEXT("weak");
	case EWillingness::None:            return TEXT("none");
	}
	return TEXT("none");
}

AAct02RuleReceiveDirector::FParsedAnswer
AAct02RuleReceiveDirector::ParseAnswer(const FString& JsonStr, bool bExpectWantToSpeak) const
{
	FParsedAnswer Out;
	if (JsonStr.IsEmpty())
	{
		Out.bParseError = true;
		return Out;
	}
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonStr);
	TSharedPtr<FJsonObject> Obj;
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		Out.bParseError = true;
		return Out;
	}
	FString Will;
	Obj->TryGetStringField(TEXT("willingness"), Will);
	Out.Willingness = WillingnessFromString(Will);
	Obj->TryGetStringField(TEXT("content"), Out.Content);
	Out.Content.TrimStartAndEndInline();
	if (bExpectWantToSpeak)
	{
		Out.bWantToSpeak = true;
		Obj->TryGetBoolField(TEXT("want_to_speak"), Out.bWantToSpeak);
		if (!Out.bWantToSpeak)
		{
			Out.Willingness = EWillingness::None;
		}
	}
	else
	{
		Out.bWantToSpeak = (Out.Willingness != EWillingness::None);
	}
	return Out;
}

void AAct02RuleReceiveDirector::GatherSeedAndPickWinner()
{
	int32 BestIdx = INDEX_NONE;
	uint8 BestWill = 0;
	int32 BestNPCIndex = TNumericLimits<int32>::Max();

	for (const FInflight& F : CurrentInflight)
	{
		OpenAIChat::FResult R = F.Future.Get();
		FParsedAnswer Ans = ParseAnswer(R.ParsedJson, /*bExpectWantToSpeak=*/false);

		const int32 Idx = NPCs.IndexOfByPredicate(
			[&](const FAct02NPCRuntime& X) { return X.Config.NPCIndex == F.NPCIndex; });
		if (Idx != INDEX_NONE)
		{
			CurrentAnswers.Add(F.NPCIndex, Ans);
			NPCs[Idx].UnspokenContent = Ans.Content;
		}

		WriteLLMLog(F.NPCIndex, F.Provider, F.Model, R.HttpStatus, R.LatencyMs,
			R.PromptTokens, R.CompletionTokens, Ans,
			F.SystemPrompt, F.UserPrompt, R.RawContent, R.ParsedJson,
			R.ErrorMessage, R.FinishReason, R.ReasoningContent, R.RawResponsePayload,
			R.bRetriedWithoutResponseFormat);

		UE_LOG(LogAct02, Log,
			TEXT("[Act02] LLM %s npc=%d http=%d latency=%.0fms willingness=%s parse_error=%d"),
			*AILiveAgentRoster::ProviderToString(F.Provider), F.NPCIndex, R.HttpStatus,
			R.LatencyMs, WillingnessLabel(Ans.Willingness), Ans.bParseError ? 1 : 0);

		const uint8 W = static_cast<uint8>(Ans.Willingness);
		if (W > BestWill ||
			(W == BestWill && F.NPCIndex < BestNPCIndex))
		{
			BestWill = W;
			BestNPCIndex = F.NPCIndex;
			BestIdx = Idx;
		}
	}

	if (BestIdx == INDEX_NONE || BestWill == 0)
	{
		FailAct02(TEXT("seed: no NPC produced a usable willingness"));
		return;
	}

	LastSpeakerIndex = BestIdx;
	const FParsedAnswer& Winner = CurrentAnswers[NPCs[BestIdx].Config.NPCIndex];
	LastSentence = Winner.Content;
	NPCs[BestIdx].UnspokenContent.Reset(); // 已说出口

	WriteWinnerLog(BestIdx, TEXT("seed"), 0);

	DebugMessage(FString::Printf(
		TEXT("[Act02] seed winner: NPC%02d willingness=%s content=\"%s\""),
		NPCs[BestIdx].Config.NPCIndex,
		WillingnessLabel(Winner.Willingness),
		*Winner.Content), FLinearColor::Green);

	StartSpeak(BestIdx, Winner.Content);
	SceneState = EAct02State::SeedSpeak;
	StateElapsed = 0.f;
}

void AAct02RuleReceiveDirector::GatherReactionAndPickWinner()
{
	int32 BestIdx = INDEX_NONE;
	uint8 BestWill = 0;
	int32 BestNPCIndex = TNumericLimits<int32>::Max();
	int32 WantSpeakers = 0;

	for (const FInflight& F : CurrentInflight)
	{
		OpenAIChat::FResult R = F.Future.Get();
		FParsedAnswer Ans = ParseAnswer(R.ParsedJson, /*bExpectWantToSpeak=*/true);

		const int32 Idx = NPCs.IndexOfByPredicate(
			[&](const FAct02NPCRuntime& X) { return X.Config.NPCIndex == F.NPCIndex; });
		if (Idx != INDEX_NONE)
		{
			CurrentAnswers.Add(F.NPCIndex, Ans);
			if (Ans.bWantToSpeak && !Ans.Content.IsEmpty())
			{
				NPCs[Idx].UnspokenContent = Ans.Content;
			}
		}

		WriteLLMLog(F.NPCIndex, F.Provider, F.Model, R.HttpStatus, R.LatencyMs,
			R.PromptTokens, R.CompletionTokens, Ans,
			F.SystemPrompt, F.UserPrompt, R.RawContent, R.ParsedJson,
			R.ErrorMessage, R.FinishReason, R.ReasoningContent, R.RawResponsePayload,
			R.bRetriedWithoutResponseFormat);

		UE_LOG(LogAct02, Log,
			TEXT("[Act02] LLM %s npc=%d http=%d latency=%.0fms wts=%d willingness=%s parse_error=%d"),
			*AILiveAgentRoster::ProviderToString(F.Provider), F.NPCIndex, R.HttpStatus,
			R.LatencyMs, Ans.bWantToSpeak ? 1 : 0,
			WillingnessLabel(Ans.Willingness), Ans.bParseError ? 1 : 0);

		if (!Ans.bWantToSpeak) continue;
		++WantSpeakers;
		const uint8 W = static_cast<uint8>(Ans.Willingness);
		if (W > BestWill ||
			(W == BestWill && F.NPCIndex < BestNPCIndex))
		{
			BestWill = W;
			BestNPCIndex = F.NPCIndex;
			BestIdx = Idx;
		}
	}

	if (BestIdx == INDEX_NONE || WantSpeakers == 0 || BestWill == 0)
	{
		DebugMessage(FString::Printf(
			TEXT("[Act02] reaction round %d ended (no one wants to speak)"), CurrentRound),
			FLinearColor::Yellow);
		CompleteAct02();
		return;
	}

	LastSpeakerIndex = BestIdx;
	const FParsedAnswer& Winner = CurrentAnswers[NPCs[BestIdx].Config.NPCIndex];
	LastSentence = Winner.Content;
	NPCs[BestIdx].UnspokenContent.Reset();

	WriteWinnerLog(BestIdx, TEXT("reaction"), CurrentRound);

	DebugMessage(FString::Printf(
		TEXT("[Act02] reaction round %d winner: NPC%02d willingness=%s content=\"%s\""),
		CurrentRound, NPCs[BestIdx].Config.NPCIndex,
		WillingnessLabel(Winner.Willingness),
		*Winner.Content), FLinearColor::Green);

	StartSpeak(BestIdx, Winner.Content);
	SceneState = EAct02State::ReactionSpeak;
	StateElapsed = 0.f;
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
	DebugMessage(FString::Printf(TEXT("[Act02] reaction round %d dispatch"), CurrentRound),
		FLinearColor::Green);
	SceneState = EAct02State::ReactionDispatch;
	DispatchLLMs(/*bSeed=*/false);
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

// ====== Prompt builders ======

FString AAct02RuleReceiveDirector::BuildSeedSystemPrompt(
	const FNPCAgentConfig& Cfg, const FString& GameRule) const
{
	return FString::Printf(
		TEXT("你是 AI，以 AI 身份出场。AI 知道自己是 AI，不扮演人类——不赋予人类职业、教育、地域、年龄、姓名格式等背景叙事。\n")
		TEXT("只赋予外观符号：名字（%s）、性别（%s）、声线、类人虚拟形象。这是 AI 的外壳，不是人类身份。\n\n")
		TEXT("你刚刚通过电视机看完游戏规则的视频介绍。规则全文：\n<<<\n%s\n>>>\n\n")
		TEXT("刚看完规则的瞬间，你想说一句话（也可以选择不说）。\n")
		TEXT("请输出严格 JSON：\n")
		TEXT("{\n  \"willingness\": \"extremely_strong\" | \"strong\" | \"moderate\" | \"weak\" | \"none\",\n")
		TEXT("  \"content\": \"<不超过 80 字的中文。如果 willingness=none 也可填空字符串>\"\n}\n")
		TEXT("意愿语义对照：\n- extremely_strong: 强烈想说，必须说\n- strong: 想说\n- moderate: 一般\n- weak: 不太想说但可以\n- none: 不想说\n\n")
		TEXT("只输出 JSON，不要任何前后缀文本。"),
		*Cfg.Identity.FullName, *Cfg.VoicePresentationHint, *GameRule);
}

FString AAct02RuleReceiveDirector::BuildSeedUserPrompt(const FNPCAgentConfig& Cfg) const
{
	return FString::Printf(
		TEXT("你是 %s。请按 system 指示输出 JSON。"),
		*Cfg.Identity.FullName);
}

FString AAct02RuleReceiveDirector::BuildReactionSystemPrompt(
	const FNPCAgentConfig& Cfg,
	const FString& SpeakerName,
	const FString& InLastSentence,
	const FString& MyUnspoken) const
{
	const FString MyUns = MyUnspoken.IsEmpty() ? TEXT("（你上一轮选择了不说）") : MyUnspoken;
	return FString::Printf(
		TEXT("你是 AI %s，%s。游戏规则：僵尸触碰游戏，三回合，每回合需与他人手碰手；人碰人得 1 分，人碰僵尸变僵尸，初始僵尸不可解。\n\n")
		TEXT("刚刚 %s 说：「%s」\n\n")
		TEXT("你上一轮想说但没说出口的内容：「%s」\n\n")
		TEXT("请决定是否接话、接话内容。严格 JSON：\n")
		TEXT("{\n  \"want_to_speak\": true | false,\n  \"willingness\": \"extremely_strong\" | \"strong\" | \"moderate\" | \"weak\" | \"none\",\n  \"content\": \"<不超过 80 字的中文>\"\n}\n")
		TEXT("意愿语义同前。如果 want_to_speak=false，content 可填空字符串。\n")
		TEXT("只输出 JSON。"),
		*Cfg.Identity.FullName, *Cfg.VoicePresentationHint,
		*SpeakerName, *InLastSentence, *MyUns);
}

FString AAct02RuleReceiveDirector::BuildReactionUserPrompt(const FNPCAgentConfig& Cfg) const
{
	return FString::Printf(
		TEXT("你是 %s。请按 system 指示输出 JSON。"),
		*Cfg.Identity.FullName);
}

// ====== Logging ======

FString AAct02RuleReceiveDirector::GetSessionDir() const
{
	return FPaths::ProjectSavedDir() / TEXT("Logs") / TEXT("Act02") / SessionTimestamp;
}

FString AAct02RuleReceiveDirector::MakeSubDir(const FString& Sub) const
{
	const FString Dir = GetSessionDir() / Sub;
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	if (!PF.DirectoryExists(*Dir))
	{
		PF.CreateDirectoryTree(*Dir);
	}
	return Dir;
}

void AAct02RuleReceiveDirector::WriteLLMLog(int32 NPCIndex, ELLMProvider Provider, const FString& Model,
	int32 HttpStatus, float LatencyMs, int32 PromptTokens, int32 CompletionTokens,
	const FParsedAnswer& Answer,
	const FString& SystemPrompt, const FString& UserPrompt,
	const FString& RawContent, const FString& ParsedJson,
	const FString& ErrorMessage, const FString& FinishReason,
	const FString& ReasoningContent, const FString& RawResponsePayload,
	bool bRetriedWithoutResponseFormat) const
{
	const bool bSeed = (CurrentRound == 0);
	const FString Sub = bSeed
		? TEXT("seed")
		: FString::Printf(TEXT("reaction_round_%02d"), CurrentRound);
	const FString Dir = const_cast<AAct02RuleReceiveDirector*>(this)->MakeSubDir(Sub);
	const FString FName = FString::Printf(TEXT("npc_%02d_%s.md"), NPCIndex,
		*AILiveAgentRoster::ProviderToString(Provider));
	const FString Path = Dir / FName;

	FString Md;
	Md += TEXT("---\n");
	Md += FString::Printf(TEXT("session: %s\n"), *SessionTimestamp);
	Md += FString::Printf(TEXT("phase: %s\n"), bSeed ? TEXT("seed") : TEXT("reaction"));
	Md += FString::Printf(TEXT("round: %d\n"), CurrentRound);
	Md += FString::Printf(TEXT("npc_index: %d\n"), NPCIndex);
	Md += FString::Printf(TEXT("provider: %s\n"), *AILiveAgentRoster::ProviderToString(Provider));
	Md += FString::Printf(TEXT("model: %s\n"), *Model);
	Md += FString::Printf(TEXT("http_status: %d\n"), HttpStatus);
	Md += FString::Printf(TEXT("latency_ms: %.0f\n"), LatencyMs);
	Md += FString::Printf(TEXT("prompt_tokens: %d\n"), PromptTokens);
	Md += FString::Printf(TEXT("completion_tokens: %d\n"), CompletionTokens);
	Md += FString::Printf(TEXT("finish_reason: %s\n"), FinishReason.IsEmpty() ? TEXT("none") : *FinishReason);
	Md += FString::Printf(TEXT("retried_without_response_format: %s\n"),
		bRetriedWithoutResponseFormat ? TEXT("true") : TEXT("false"));
	Md += FString::Printf(TEXT("parsed_willingness: %s\n"), WillingnessLabel(Answer.Willingness));
	Md += FString::Printf(TEXT("parsed_want_to_speak: %s\n"),
		Answer.bWantToSpeak ? TEXT("true") : TEXT("false"));
	Md += FString::Printf(TEXT("parse_error: %s\n"),
		Answer.bParseError ? TEXT("true") : TEXT("false"));
	Md += TEXT("---\n\n");

	Md += TEXT("## System\n\n");
	Md += SystemPrompt;
	Md += TEXT("\n\n## User\n\n");
	Md += UserPrompt;
	Md += TEXT("\n\n## Error message\n\n");
	Md += ErrorMessage.IsEmpty() ? TEXT("(empty)") : ErrorMessage;
	Md += TEXT("\n\n## Raw response\n\n");
	Md += RawContent;
	Md += TEXT("\n\n## Reasoning content\n\n");
	Md += ReasoningContent;
	Md += TEXT("\n\n## Parsed JSON\n\n```json\n");
	Md += ParsedJson;
	Md += TEXT("\n```\n\n## Full response payload\n\n```json\n");
	Md += RawResponsePayload;
	Md += TEXT("\n```\n");

	FFileHelper::SaveStringToFile(Md, *Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void AAct02RuleReceiveDirector::WriteWinnerLog(int32 WinnerIdx, const FString& Phase, int32 RoundIdx) const
{
	if (!NPCs.IsValidIndex(WinnerIdx)) return;

	const FString Sub = (Phase == TEXT("seed"))
		? TEXT("seed")
		: FString::Printf(TEXT("reaction_round_%02d"), RoundIdx);
	const FString Dir = const_cast<AAct02RuleReceiveDirector*>(this)->MakeSubDir(Sub);
	const FString Path = Dir / TEXT("_winner.md");

	const FAct02NPCRuntime& W = NPCs[WinnerIdx];
	const FParsedAnswer* WAns = CurrentAnswers.Find(W.Config.NPCIndex);

	FString Md;
	Md += TEXT("---\n");
	Md += FString::Printf(TEXT("session: %s\n"), *SessionTimestamp);
	Md += FString::Printf(TEXT("phase: %s\n"), *Phase);
	Md += FString::Printf(TEXT("round: %d\n"), RoundIdx);
	Md += FString::Printf(TEXT("winner_npc_index: %d\n"), W.Config.NPCIndex);
	Md += FString::Printf(TEXT("winner_provider: %s\n"),
		*AILiveAgentRoster::ProviderToString(W.Config.Provider));
	if (WAns)
	{
		Md += FString::Printf(TEXT("winner_willingness: %s\n"), WillingnessLabel(WAns->Willingness));
	}
	Md += TEXT("---\n\n");
	Md += TEXT("## Winner content\n\n");
	if (WAns)
	{
		Md += WAns->Content;
	}
	Md += TEXT("\n\n## Other NPCs unspoken (after this round)\n\n");
	for (const FAct02NPCRuntime& N : NPCs)
	{
		if (N.Config.NPCIndex == W.Config.NPCIndex) continue;
		Md += FString::Printf(TEXT("- NPC%02d (%s): %s\n"),
			N.Config.NPCIndex,
			*AILiveAgentRoster::ProviderToString(N.Config.Provider),
			N.UnspokenContent.IsEmpty() ? TEXT("(空)") : *N.UnspokenContent);
	}

	FFileHelper::SaveStringToFile(Md, *Path,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
