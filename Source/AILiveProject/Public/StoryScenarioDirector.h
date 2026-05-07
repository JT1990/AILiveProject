#pragma once

// =============================================================================
// 中文教学：StoryScenarioDirector.h —— 「两个 NPC 走到一起对话」剧情 actor
//
// 这是 Act 系列之外的另一个 Director，做更轻量的 2-NPC 对话场景：
//   1) NPC2 走到 NPC1 附近的会面点
//   2) 到达后停下、轮流播放预设台词（DialogueLines）
//   3) 全部说完后 NPC2 走回原位
//
// 与 Act01/02 的对比：
//   - Act01：剧情触发器（开门 + 走到电视 + 播视频）
//   - Act02：LLM 驱动的复杂对话主循环
//   - StoryScenarioDirector：固定台词的简单 2-NPC 演出（早期实验用，没接 LLM）
//
// 状态机 EStoryScenarioState：
//   Idle → MovingToMeeting → Dialogue → Returning → Idle
//
// 关键 UE 概念：
//
//   1) ATargetPoint
//      场景里的「锚点 actor」，提供 transform 但没渲染。常用于剧情节点 /
//      AI 移动目标。本类运行时 SpawnActor 几个 TargetPoint 当移动目标。
//
//   2) UPROPERTY(Transient)
//      标记字段「不参与序列化」（不存关卡 / 不存 SaveGame）。运行时缓存的
//      指针适合用 Transient ——存档没意义，PIE 重启自然重建。
//
//   3) FTimerHandle 用法集中演示
//      MovementPollTimer / SettleTimer / DialogueTimer 三个 handle 演示了
//      多个并行 timer 的管理：每个用途一个 handle，结束/取消时一个个 ClearTimer。
//      组件 EndPlay 必须清理 handle 防止 dangling 回调。
// =============================================================================

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
	/** StoryScenarioDirector 的内部状态机；只在 C++ 内部使用，不暴露给蓝图。 */
	enum class EStoryScenarioState : uint8
	{
		/** 空闲状态：场景未运行，允许开始新的双 NPC 演出。 */
		Idle,
		/** 前往会面点：NPC2 正在移动到 NPC1 附近的临时目标点。 */
		MovingToMeeting,
		/** 对话中：NPC 已到位，正在按 DialogueLines 轮流播放台词。 */
		Dialogue,
		/** 返回原位：对话结束后 NPC2 正在回到初始位置。 */
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
