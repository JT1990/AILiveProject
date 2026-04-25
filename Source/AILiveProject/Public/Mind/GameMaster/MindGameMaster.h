#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Templates/SubclassOf.h"
#include "Mind/MindActionEnvelope.h"
#include "Mind/MindAgentView.h"
#include "MindGameMaster.generated.h"

class UMindAction;
class UMindComponent;

/**
 * GameMaster 抽象基类。每个游戏（LiarsBar / MinorityRule）派生子类实现阶段机。
 *
 * P0-A 强约束：Validate（只读）/ Apply（OnActionDone 后才改 state）拆分。
 * T03 阶段：仅落骨架（virtual 函数空体），子类 Place Actor 验收。
 * T11 实施：StartGame / TransitionToPhase / AwakeAgent / RegisterActionsForAgent 真实现。
 * T12 / T21：子类按 phase 实现 BuildViewFor / Validate / Apply / OnAgentActionFinished。
 */
UCLASS(Abstract)
class AILIVEPROJECT_API AMindGameMaster : public AActor
{
	GENERATED_BODY()
public:
	/** 参与本场游戏的 NPC actor 引用。在关卡里手填或 StartGame 前注入。 */
	UPROPERTY(EditAnywhere, Category="AI Live|GM")
	TArray<TObjectPtr<AActor>> Participants;

	/** 当前阶段 FName。子类用 TransitionToPhase 切换。 */
	UPROPERTY(BlueprintReadOnly, Category="AI Live|GM")
	FName CurrentPhase = NAME_None;

	UFUNCTION(BlueprintCallable, Category="AI Live|GM")
	virtual void StartGame();

	UFUNCTION(BlueprintCallable, Category="AI Live|GM")
	virtual void EndGame();

	/** 给某个 agent 构造当前局面快照（prompt user 段输入）。子类按游戏规则填字段。 */
	virtual FMindAgentView BuildViewFor(AActor* Agent);

	/** 只读校验（P0-A）。OutError 在返回 false 时必填。子类按 phase 严格判定。 */
	virtual bool Validate(AActor* Agent, const FMindActionEnvelope& Env, FString& OutError) const;

	/** Action.Execute 成功 Done 后由 MindComponent::HandleActionDone 调用（P0-A）。 */
	virtual void Apply(AActor* Agent, const FMindActionEnvelope& Env);

	/** Apply 之后调用，子类决定是否切换阶段。 */
	virtual void OnAgentActionFinished(AActor* Agent, const FMindActionEnvelope& Env, bool bExecuteOk);

	/** Phase 切换 native multicast——T11 升级为 DYNAMIC 让 BP/UMG 可订阅。 */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPhaseChanged, FName /*Old*/, FName /*New*/);
	FOnPhaseChanged OnPhaseChanged;

protected:
	void TransitionToPhase(FName NewPhase);
	void AwakeAgent(AActor* Agent, FString Reason);
	void RegisterActionsForAgent(AActor* Agent, const TArray<TSubclassOf<UMindAction>>& Actions);
};
