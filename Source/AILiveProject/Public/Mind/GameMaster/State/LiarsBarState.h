#pragma once

#include "CoreMinimal.h"
#include "LiarsBarState.generated.h"

/**
 * 骗子酒馆单玩家状态。T03 阶段：字段就位，业务逻辑在 T12 / T13 实现。
 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FLiarsBarPlayerState
{
	GENERATED_BODY()

	/** 玩家稳定 ID（来自 UMindAgentConfig.AgentIdStable）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|LiarsBar")
	FString AgentId;

	/** 手牌索引数组（具体编码 T13 定义）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|LiarsBar")
	TArray<int32> Hand;

	/** 轮盘剩余空格数；致命概率 = 1 / RouletteChamber。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|LiarsBar")
	int32 RouletteChamber = 6;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|LiarsBar")
	bool bAlive = true;
};

/**
 * 骗子酒馆牌桌状态容器。
 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FLiarsBarTableState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|LiarsBar")
	TArray<FLiarsBarPlayerState> Players;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|LiarsBar")
	FString CurrentTurnAgentId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|LiarsBar")
	FString CurrentClaimRank;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|LiarsBar")
	int32 CurrentClaimCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|LiarsBar")
	TArray<int32> DiscardPile;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="AI Live|GM|LiarsBar")
	int32 RoundNumber = 0;
};
