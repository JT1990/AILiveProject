#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "LLM/AILiveAgentRoster.h"
#include "LLM/OpenAIChatClient.h"
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

enum class EWillingness : uint8
{
	None             = 0,
	Weak             = 2,
	Moderate         = 3,
	Strong           = 4,
	ExtremelyStrong  = 5,
};

struct FAct02NPCRuntime
{
	FNPCAgentConfig Config;
	TWeakObjectPtr<AActor> Pawn;
	FString UnspokenContent;
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
	void GatherSeedAndPickWinner();
	void GatherReactionAndPickWinner();

	void StartSpeak(int32 NPCIndex, const FString& Text);
	void TickSpeakWatchdog(float Dt);
	void HandleSpeechFinished(bool bSuccess);

	void AdvanceReactionRound();
	void CompleteAct02();
	void FailAct02(const FString& Reason);

	void DebugMessage(const FString& Msg, const FLinearColor& Color = FLinearColor::White) const;

	struct FInflight
	{
		int32 NPCIndex = INDEX_NONE;
		FString SystemPrompt;
		FString UserPrompt;
		ELLMProvider Provider = ELLMProvider::DeepSeek;
		FString Model;
		FString RequestId;  // 配对 system.llm_inflight ↔ speech.public，principles §5.4
		TFuture<OpenAIChat::FResult> Future;
	};

	struct FParsedAnswer
	{
		EWillingness Willingness = EWillingness::None;
		bool bWantToSpeak = true;
		FString Content;
		bool bParseError = false;
	};

	FParsedAnswer ParseAnswer(const FString& Json, bool bExpectWantToSpeak) const;
	EWillingness WillingnessFromString(const FString& S) const;
	const TCHAR* WillingnessLabel(EWillingness W) const;

	FString BuildSeedSystemPrompt(const FNPCAgentConfig& Cfg, const FString& GameRule) const;
	FString BuildSeedUserPrompt(const FNPCAgentConfig& Cfg) const;
	// Reaction phase prompt 已迁到 AILivePromptAssembler（T6 落地）；原
	// BuildReactionSystemPrompt / BuildReactionUserPrompt 函数已删除。

	void DispatchLLMs(bool bSeed);

	// EventStore payload builders（T5：Director 直接 wrap LLM 文本为 speech.public，
	// 这是 T7 引入 bid + intended 协议前的过渡形态——所有 speech.public 的 payload
	// 含 "legacy_pre_bid": true，T7 后 SQL 用 IS NULL 过滤）。
	static FString BuildSpeechPublicPayloadJson(int32 NPCIndex,
	                                             const FParsedAnswer& Ans,
	                                             const OpenAIChat::FResult& R,
	                                             const FString& RequestId);
	static FString BuildLLMInflightPayloadJson(int32 NPCIndex,
	                                            const FString& RequestId,
	                                            const FString& SystemPromptHash,
	                                            const FString& UserPromptHash,
	                                            const FString& StartedAtIso8601);
	static FString BuildWinnerDecisionPayloadJson(int32 WinnerNPCIndex,
	                                               EWillingness Willingness,
	                                               int32 RoundNo,
	                                               const TCHAR* WillingnessLabelStr);

	UPROPERTY(Transient)
	TObjectPtr<AActor> NavTargetCached;

	UPROPERTY(Transient)
	TObjectPtr<AActor> NavLookTargetCached;

	TMap<TWeakObjectPtr<AActor>, FTransform> InitialNPCTransforms;

	TArray<FAct02NPCRuntime> NPCs;
	TArray<FInflight> CurrentInflight;
	TMap<int32, FParsedAnswer> CurrentAnswers;

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
