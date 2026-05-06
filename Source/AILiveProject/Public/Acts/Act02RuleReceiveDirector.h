#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "LLM/AILiveAgentRoster.h"
#include "LLM/AILiveParserClient.h"
#include "LLM/OpenAIChatClient.h"
#include "Memory/AILiveBidTypes.h"
#include "Act02RuleReceiveDirector.generated.h"

class USceneComponent;
class UEnvQuery;
struct IConsoleCommand;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAct02CompletedDelegate);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAct02FailedDelegate, FString, Reason);

UENUM(BlueprintType)
enum class EAct02State : uint8
{
	Idle,
	PrescatterToTV,
	SeedDispatch,
	SeedAwait,
	SeedSpeak,
	GatedAwaitNext,
	ReactionDispatch,
	ReactionAwait,
	ReactionSpeak,
};

struct FAct02NPCRuntime
{
	FNPCAgentConfig Config;
	TWeakObjectPtr<AActor> Pawn;
};

UCLASS(BlueprintType, Blueprintable)
class AILIVEPROJECT_API AAct02RuleReceiveDirector : public AActor
{
	GENERATED_BODY()

public:
	AAct02RuleReceiveDirector();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Input")
	bool bEnableKeyTrigger = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Input")
	FKey StartKey;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Input")
	FKey NextPhaseKey;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Actors")
	TSubclassOf<AActor> NPCMoverClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Actors")
	TSoftObjectPtr<AActor> NavTargetActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Actors")
	TSoftObjectPtr<AActor> NavLookTargetActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Actors")
	TSubclassOf<AActor> NavTargetClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Actors")
	TSubclassOf<AActor> NavLookTargetClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Actors")
	FName NavTargetActorLabel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Actors")
	FName NavLookTargetActorLabel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Movement")
	TObjectPtr<UEnvQuery> ScatterQueryAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Movement")
	float MovementTimeoutSeconds = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Movement")
	float ScatterDispatchDelaySeconds = 2.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Movement")
	float ArrivalSettleSeconds = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Roster")
	TArray<FNPCAgentConfig> Roster;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Prompt")
	FString GameRuleRelativePath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Prompt")
	FString GameRuleFallbackSummary;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|LLM")
	float LLMTimeoutSeconds = 65.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|TTS")
	FName A2FProviderName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|TTS")
	float SpeechWatchdogSeconds = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Reaction")
	int32 ReactionRoundCount = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act02|Debug")
	bool bDebugPrintScreen = true;

	UPROPERTY(BlueprintAssignable, Category = "AILiveProject|Act02")
	FAct02CompletedDelegate OnAct02Completed;

	UPROPERTY(BlueprintAssignable, Category = "AILiveProject|Act02")
	FAct02FailedDelegate OnAct02Failed;

	UFUNCTION(BlueprintCallable, Category = "AILiveProject|Act02")
	bool BeginAct02();

	UFUNCTION(BlueprintCallable, Category = "AILiveProject|Act02")
	void StartReactionPhase();

	UFUNCTION(BlueprintCallable, Category = "AILiveProject|Act02")
	void CancelAct02();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "AILiveProject|Act02")
	EAct02State GetSceneState() const { return SceneState; }

	// 监听 ACT01 完成事件，自动触发 BeginAct02（开发调试：按 1 即可一路跑完 act01 → act02 → 反应轮）
	UFUNCTION()
	void HandleAct01Completed();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void CacheInitialNPCTransforms();
	AActor* ResolveActorByClassAndLabel(TSubclassOf<AActor> InClass, FName Label) const;
	void ResolveTargetActors(bool bLog);
	void ResetToInitialPositions();
	bool BindRoster();
	bool LoadGameRule(FString& OutRule) const;

	void RegisterDebugConsoleCommands();
	void UnregisterDebugConsoleCommands();
	void BeginAct02Console();

	void StartPrescatter();
	void TickPrescatter(float Dt);
	bool AreAllNPCsIdle() const;

	void StartSeedPhase();
	void TickLLMAwait(float Dt);

	// === T7 主循环路径（替代 DispatchLLMs / GatherSeed* / GatherReaction*） ====

	// 单 agent 一拍 Reasoner→Parser 双 LLM 串联结果（worker SetValue 一次 → GT 只读 Get）
	struct FAgentTickResult
	{
		int32 NPCIndex = INDEX_NONE;
		FString ActorId;            // "NPC%02d"
		FString RequestId;
		ELLMProvider Provider = ELLMProvider::DeepSeek;
		FString Model;
		bool    bAbstain = false;   // 3 次 reject sample 失败 → abstain
		FString FailureReason;      // P.ErrorReason | "reasoner_http_failed" | "reasoner_timeout"
		FString FailedStage;        // P.FailedStage（PrevalidateRawTaggedSections / ParseFourChannels / ...）
		FString ReasonerRawText;    // 最后一次完整 raw text
		AILiveParser::FParseResult Parsed;
		OpenAIChat::FResult ReasonerResult;
	};

	struct FInflightTick
	{
		int32 NPCIndex = INDEX_NONE;
		FString ActorId;
		FString RequestId;
		ELLMProvider Provider;
		FString Model;
		TFuture<FAgentTickResult> Future;
	};

	void RunTick();
	static FAgentTickResult RunAgentTickInWorker(
		const FNPCAgentConfig& Cfg,
		const FString& RequestId,
		const FString& SystemPrompt,
		const FString& UserPrompt,
		float PerAttemptTimeoutSec);
	void GatherTickAndResolveFloor();
	void DeriveActionIntents(int64 InTickNo);
	int64 WriteTickResolvedAndAudit(const FAILiveTickResolution& Res, int64 DerivedPublicSeq);

	// §A.1 四通道格式 system prompt（seed + reaction 共用，替代 BuildSeedSystemPrompt）
	FString BuildReasonerSystemPrompt(const FNPCAgentConfig& Cfg,
	                                  const FString& GameRule,
	                                  int32 InCurrentRound) const;

	void StartSpeak(int32 NPCIndex, const FString& Text);
	void TickSpeakWatchdog(float Dt);
	void HandleSpeechFinished(bool bSuccess);

	void AdvanceReactionRound();
	void CompleteAct02();
	void FailAct02(const FString& Reason);

	void DebugMessage(const FString& Msg, const FLinearColor& Color = FLinearColor::White) const;

	// in-flight 配对协议 payload builder 保留（principles §5.4）
	static FString BuildLLMInflightPayloadJson(int32 NPCIndex,
	                                            const FString& RequestId,
	                                            const FString& SystemPromptHash,
	                                            const FString& UserPromptHash,
	                                            const FString& StartedAtIso8601);

	UPROPERTY(Transient)
	TObjectPtr<AActor> NavTargetCached;

	UPROPERTY(Transient)
	TObjectPtr<AActor> NavLookTargetCached;

	TMap<TWeakObjectPtr<AActor>, FTransform> InitialNPCTransforms;

	TArray<FAct02NPCRuntime> NPCs;
	TArray<FInflightTick> CurrentTickInflight;
	// 当拍状态缓存：actor → 该 actor 同拍 intended 事件 seq（GatherTickAndResolveFloor 阶段 A 填，
	// 阶段 B/C 用于 ResolveFloor / action.intent / speech.public 衍生）。
	TMap<FString, int64> ActorToIntendedSeq;
	TMap<FString, FString> ActorToRequestId;

	int32 LastSpeakerIndex = INDEX_NONE;
	FString LastSentence;

	int32 CurrentRound = 0; // 0 = seed, 1..N = reaction
	int32 CurrentSpeaker = INDEX_NONE;

	float StateElapsed = 0.f;
	float MovementSettleElapsed = 0.f;
	bool bArrivalSettling = false;
	float SpeakElapsed = 0.f;

	bool bSpeechFinished = false;
	bool bSpeechFailed = false;

	EAct02State SceneState = EAct02State::Idle;

	IConsoleCommand* StartCommand = nullptr;
	IConsoleCommand* NextCommand = nullptr;
	IConsoleCommand* CancelCommand = nullptr;
};
