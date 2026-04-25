#pragma once

#include "CoreMinimal.h"
#include "MindActionEnvelope.generated.h"

/**
 * LLM 输出的动作信封，由 UMindComponent 解析并派发给 UMindAction 子类。
 * Reasoning 写入记忆（其他 NPC 可能 recall 到）；InnerMonologue 仅 Output Log，不写记忆。
 */
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FMindActionEnvelope
{
	GENERATED_BODY()

	/** LLM 选定的动作名（与 UMindAction::ActionName 对齐，如 "speak" / "play_cards"）。 */
	UPROPERTY(BlueprintReadWrite, Category="AI Live|Mind")
	FString ActionName;

	/** 动作参数 JSON（schema 由对应 UMindAction::ParamSchemaJson 暴露给 LLM）。 */
	UPROPERTY(BlueprintReadWrite, Category="AI Live|Mind", meta=(MultiLine=true))
	FString ParamsJson;

	/** 公开理由：写入记忆。 */
	UPROPERTY(BlueprintReadWrite, Category="AI Live|Mind", meta=(MultiLine=true))
	FString Reasoning;

	/** 内心独白：仅 Output Log，不写记忆（避免污染其他 NPC 视角）。 */
	UPROPERTY(BlueprintReadWrite, Category="AI Live|Mind", meta=(MultiLine=true))
	FString InnerMonologue;
};
