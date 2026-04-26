#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UObject/WeakObjectPtr.h"
#include "Templates/SubclassOf.h"
#include "Mind/MindActionEnvelope.h"
#include "MindComponent.generated.h"

class UMindAgentConfig;
class UMindLLMProvider;
class UMindAction;
class AMindGameMaster;  // 前向声明：避免 include "Mind/GameMaster/MindGameMaster.h" 引头文件循环

UENUM(BlueprintType)
enum class EMindState : uint8
{
	Idle      UMETA(DisplayName="Idle"),
	Building  UMETA(DisplayName="Building Prompt"),
	Calling   UMETA(DisplayName="Calling LLM"),
	Acting    UMETA(DisplayName="Executing Action"),
	Cooldown  UMETA(DisplayName="Cooldown"),
};

/**
 * Mind 决策层中央调度组件。挂在 NPC Pawn 上，负责：
 *   1. 接受 GM 阶段唤醒 / Perception 触发 → RequestDecision
 *   2. 构造 prompt 调 Provider → 收 LLM 输出
 *   3. 派发到 ActionRegistry 找对应 UMindAction → Execute
 *   4. 完成后通知 GM Apply + 写记忆 + 切回 Idle
 *
 * 字段一次列全（T02 钉死）；后置卡（T05 / T05.5 / T06 / T07 / T09 / T11 / T18 / T19）只填 .cpp。
 */
UCLASS(ClassGroup=(Mind), meta=(BlueprintSpawnableComponent))
class AILIVEPROJECT_API UMindComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UMindComponent();

	// ============================================================
	// 配置 / 状态
	// ============================================================

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|Mind")
	TObjectPtr<UMindAgentConfig> Config;

	UPROPERTY(BlueprintReadOnly, Category="AI Live|Mind")
	EMindState State = EMindState::Idle;

	/** T18 用：M0 默认 false，T07 接 Mind 后由 NPC.Initialize 显式打开。 */
	UPROPERTY(BlueprintReadOnly, Category="AI Live|Mind")
	bool bPerceptionCanTriggerDecision = false;

	// ============================================================
	// 引用（弱指针 / 实例化对象）
	// ============================================================

	/** GM 弱引用——前向声明 + TWeakObjectPtr 避免 include 循环；T11 cast 实例。 */
	TWeakObjectPtr<AMindGameMaster> GameMaster;

	UPROPERTY()
	TObjectPtr<UMindLLMProvider> Provider;

	/** 当前可用动作集——通用 Action（Speak 等）+ GM 阶段动态注册的游戏专属 Action。 */
	UPROPERTY()
	TMap<FString, TObjectPtr<UMindAction>> ActionRegistry;

	// ============================================================
	// 决策状态（T06 / T11 消费）
	// ============================================================

	UPROPERTY()
	FMindActionEnvelope LastEnvelope;

	UPROPERTY()
	float LastDecisionAt = 0.f;

	// ============================================================
	// Recall 防死循环（P0-F 强约束）
	// ============================================================

	UPROPERTY()
	int32 RecallChainDepth = 0;

	static constexpr int32 RecallChainMax = 2;

	// ============================================================
	// Perception 节流（T18 消费）——cpp 内部状态，不带 UPROPERTY
	// （TWeakObjectPtr 不被 UPROPERTY 反射支持作为 map key）
	// ============================================================

	TMap<TWeakObjectPtr<AActor>, float> LastPerceptionAt;

	// ============================================================
	// 通用 Action 状态（T19 消费）
	// ============================================================

	UPROPERTY(BlueprintReadOnly, Category="AI Live|Mind")
	FString CurrentIntent;

	UPROPERTY()
	FString StashedRecall;

	// ============================================================
	// 接口签名（cpp 全空 / 默认值；后置卡只改 .cpp）
	// ============================================================

	UFUNCTION(BlueprintCallable, Category="AI Live|Mind")
	void Initialize(UMindAgentConfig* InConfig, AActor* InGameMaster);

	UFUNCTION(BlueprintCallable, Category="AI Live|Mind")
	void RequestDecision(FString TriggerReason);

	UFUNCTION(BlueprintCallable, Category="AI Live|Mind")
	void SetGameActions(const TArray<TSubclassOf<UMindAction>>& Actions);

	void RegisterAction(TSubclassOf<UMindAction> ActionClass);

	UFUNCTION(BlueprintCallable, Category="AI Live|Mind")
	void DispatchAction(const FMindActionEnvelope& Env);

	void HandleActionDone(bool bOk, FString Summary);

	// ============================================================
	// Helper accessors
	// ============================================================

	/** 优先返回 Config->AgentIdStable；为空时回退 DisplayName + 警告（T07 实现完整 fallback）。 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind")
	FString GetAgentId() const;

	/**
	 * GM 弱指针 accessor。**仅 C++**——`AMindGameMaster*` 返回类型在 T02 阶段无完整类型
	 * （T03 才建 GM 头文件），UFUNCTION 反射不允许前向声明的类，所以不暴露给 BP。
	 * T22 / T11 在 .cpp 中 `Owner->GetGameMaster()` 直接使用即可。
	 */
	AMindGameMaster* GetGameMaster() const;

	// ============================================================
	// Perception API（T18 消费；签名先就位，cpp 在 T18 实现）
	// 注：OnPerceptionUpdated 委托回调签名涉及 FAIStimulus，在 T18 引
	// "Perception/AIPerceptionTypes.h" 后再补——T02 不强求钉死
	// ============================================================

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind")
	TArray<FString> GetCurrentlyVisibleAgentIds() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind")
	TArray<FString> GetCurrentlyAudibleAgentIds() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="AI Live|Mind")
	bool CanSenseActor(AActor* Other, FName SenseTag) const;

	UFUNCTION(BlueprintCallable, Category="AI Live|Mind")
	void SetPerceptionCanTriggerDecision(bool bEnabled);

private:
	/** 拼 system prompt（人格 + 目标 + injection 防御 + ActionRegistry schema）。T06 实装。 */
	FString BuildSystemPrompt() const;

	/** Provider 异步回调。绑定为 `FOnLLMResult::CreateUObject(this, ...)`，所以不能 static。
	 *  签名按值传 FString —— 与 DECLARE_DELEGATE_TwoParams(... FString ...) 完全匹配，避免 const&/value 不一致。 */
	void OnLLMResponse(bool bOk, FString JsonText);
};
