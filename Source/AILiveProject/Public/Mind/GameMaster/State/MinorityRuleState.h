#pragma once

#include "CoreMinimal.h"
#include "MinorityRuleState.generated.h"

/**
 * 少数决单玩家状态。T03 阶段：字段就位，业务逻辑在 T21 / T22 实现。
 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FMinorityRulePlayer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	FString AgentId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	bool bAlive = true;

	/** 铭牌钻石数量（象征值，T21 阶段固定为 1）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	int32 Diamonds = 1;

	/** 当前投票："yes" / "no" / ""（未投）。Tally 后清空。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	FString CurrentVote;

	/** 自我认定的盟友列表（agent_id），双向匹配由 GM 在 AcceptAlliance 时维护。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	TArray<FString> AlliancePerception;
};

/**
 * 少数决单局会话状态容器。
 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FMinorityRuleSessionState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	TArray<FMinorityRulePlayer> Players;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	FString CurrentQuestion;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	FString CurrentQuestionerId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	int32 RoundNumber = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	TArray<FString> EliminatedThisRound;

	/** 历史公开事件（每个 NPC 都能 recall 到）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|MinorityRule")
	TArray<FString> PublicHistoryLog;
};
