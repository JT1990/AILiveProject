#pragma once

#include "CoreMinimal.h"
#include "Mind/GameMaster/MindGameMaster.h"
#include "Mind/GameMaster/State/MinorityRuleState.h"
#include "MindGameMaster_MinorityRule.generated.h"

/**
 * 少数决 GM。T03 阶段：仅落骨架（State 字段 + 5 个 override 空体），可 Place Actor。
 * T21 实施：Phase 机（Setup / AskQuestion / Negotiate / Vote / Tally / Eliminate / RoundEnd / GameOver）。
 */
UCLASS()
class AILIVEPROJECT_API AMindGameMaster_MinorityRule : public AMindGameMaster
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	FMinorityRuleSessionState State;

	/** T21 指向 DA_GameConfig_MinorityRule。 */
	UPROPERTY(EditAnywhere, Category="AI Live|GM|MinorityRule")
	TObjectPtr<class UDataAsset> GameConfig;

	virtual void StartGame() override;
	virtual FMindAgentView BuildViewFor(AActor* Agent) override;
	virtual bool Validate(AActor* Agent, const FMindActionEnvelope& Env, FString& OutError) const override;
	virtual void Apply(AActor* Agent, const FMindActionEnvelope& Env) override;
	virtual void OnAgentActionFinished(AActor* Agent, const FMindActionEnvelope& Env, bool bExecuteOk) override;
};
