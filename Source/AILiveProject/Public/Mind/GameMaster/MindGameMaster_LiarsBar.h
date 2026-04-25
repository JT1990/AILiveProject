#pragma once

#include "CoreMinimal.h"
#include "Mind/GameMaster/MindGameMaster.h"
#include "Mind/GameMaster/State/LiarsBarState.h"
#include "MindGameMaster_LiarsBar.generated.h"

/**
 * 骗子酒馆 GM。T03 阶段：仅落骨架（State 字段 + 5 个 override 空体），可 Place Actor。
 * T12 实施：Phase 机（Deal / Claim / Challenge / Roulette / RoundEnd / GameOver）+ Validate / Apply。
 */
UCLASS()
class AILIVEPROJECT_API AMindGameMaster_LiarsBar : public AMindGameMaster
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|LiarsBar")
	FLiarsBarTableState State;

	/** T12 指向 DA_GameConfig_LiarsBar。前向声明 UDataAsset 避免本卡引依赖。 */
	UPROPERTY(EditAnywhere, Category="AI Live|GM|LiarsBar")
	TObjectPtr<class UDataAsset> GameConfig;

	virtual void StartGame() override;
	virtual FMindAgentView BuildViewFor(AActor* Agent) override;
	virtual bool Validate(AActor* Agent, const FMindActionEnvelope& Env, FString& OutError) const override;
	virtual void Apply(AActor* Agent, const FMindActionEnvelope& Env) override;
	virtual void OnAgentActionFinished(AActor* Agent, const FMindActionEnvelope& Env, bool bExecuteOk) override;
};
