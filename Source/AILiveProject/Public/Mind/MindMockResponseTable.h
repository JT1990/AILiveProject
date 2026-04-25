#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MindMockResponseTable.generated.h"

/**
 * Mock LLM Provider 用的单条预设响应。
 * 匹配 trigger reason / phase 的 substring，命中则返回对应 ResponseJson。
 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FMockResponseRow
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category="AI Live|Mind")
	FString MatchPattern;

	UPROPERTY(EditAnywhere, Category="AI Live|Mind", meta=(MultiLine=true))
	FString ResponseJson;

	UPROPERTY(EditAnywhere, Category="AI Live|Mind")
	float Weight = 1.0f;
};

/**
 * Mock LLM Provider 的预设响应表，开发期用于加速迭代（T05.5）。
 * T02 仅落骨架——保证 UMindAgentConfig.MockResponseTable 字段在 Details 面板可绑。
 */
UCLASS(BlueprintType)
class AILIVEPROJECT_API UMindMockResponseTable : public UPrimaryDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category="AI Live|Mind")
	TArray<FMockResponseRow> Rows;

	UPROPERTY(EditAnywhere, Category="AI Live|Mind", meta=(MultiLine=true))
	FString FallbackResponseJson;
};
