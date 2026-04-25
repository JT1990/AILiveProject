#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Delegates/Delegate.h"
#include "MindMemoryClient.generated.h"

/** Memory Service `/memory/recall` / `/memory/by_tag` 返回的单条记忆。 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FMindMemoryItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="AI Live|Memory")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category="AI Live|Memory")
	FString Content;

	UPROPERTY(BlueprintReadOnly, Category="AI Live|Memory")
	float Score = 0.f;

	UPROPERTY(BlueprintReadOnly, Category="AI Live|Memory")
	int64 Ts = 0;

	UPROPERTY(BlueprintReadOnly, Category="AI Live|Memory")
	TMap<FString, FString> Tags;
};

/**
 * UE 端访问 Tools/MemoryService 的 HTTP 客户端。GameInstanceSubsystem 天然单例 + 跨关卡持久。
 * T08 实现 Memory Service 端的 /memory/write|recall|by_tag；T09 实现这里的 cpp。
 */
UCLASS(Config=Game)
class AILIVEPROJECT_API UMindMemoryClient : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	/** Memory Service 基址。优先从 .env 读 MEMORY_SERVICE_URL；找不到回落到此默认（T09 实现）。 */
	UPROPERTY(EditAnywhere, Config, Category="AI Live|Memory")
	FString BaseUrl = TEXT("http://127.0.0.1:8765");

	DECLARE_DELEGATE_OneParam(FOnRecall, const TArray<FMindMemoryItem>&);

	/** 写入一条记忆。Tags 用于 by_tag 检索（如 {"channel":"public","speaker":"npc_2"}）。 */
	void Write(FString AgentId, FString Content, TMap<FString, FString> Tags);

	/** 向量检索（按 query 语义相似度）。 */
	void Recall(FString AgentId, FString Query, int32 TopK, FOnRecall Done);

	/** Tag 精确检索（如 ByTag(self, "speaker", "npc_3", 3) → 我对 npc_3 的最近 3 条记忆）。 */
	void ByTag(FString AgentId, FString TagKey, FString TagValue, int32 TopN, FOnRecall Done);
};
