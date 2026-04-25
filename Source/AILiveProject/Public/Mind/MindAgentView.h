#pragma once

#include "CoreMinimal.h"
#include "MindAgentView.generated.h"

/**
 * GameMaster 给某个 agent 提供的"当前局面快照"，供 UMindComponent 拼 prompt 用。
 * 实际拼接逻辑（含 BuildSceneAwarenessSection 注入）在 T06 / T18 / T12 / T21 实现。
 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FMindAgentView
{
	GENERATED_BODY()

	/** 接收方 agent 的稳定 ID（来自 UMindAgentConfig.AgentIdStable）。 */
	UPROPERTY(BlueprintReadWrite, Category="AI Live|Mind")
	FString OwnAgentId;

	/** GameMaster 当前阶段（如 "PlayerTurn" / "Negotiate"）。 */
	UPROPERTY(BlueprintReadWrite, Category="AI Live|Mind")
	FString GameStage;

	/** GM 序列化的公开信息（所有 agent 都能看见的桌面状态）。 */
	UPROPERTY(BlueprintReadWrite, Category="AI Live|Mind", meta=(MultiLine=true))
	FString PublicStateText;

	/** GM 序列化的本 agent 私有信息（手牌 / 自己的剩余 chamber 等）。 */
	UPROPERTY(BlueprintReadWrite, Category="AI Live|Mind", meta=(MultiLine=true))
	FString PrivateStateText;

	/** 最近事件列表（GM 决定是否给本 agent 推送）。 */
	UPROPERTY(BlueprintReadWrite, Category="AI Live|Mind")
	TArray<FString> RecentEvents;

	/** 拼成 prompt user 段的文本——T06 实现。 */
	FString ToPromptText() const { return FString(); }
};
