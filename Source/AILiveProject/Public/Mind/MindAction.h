#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Delegates/Delegate.h"
#include "MindAction.generated.h"

class UMindComponent;

/**
 * Mind 决策层"动作"基类。子类构造函数里设 ActionName / Description / ParamSchemaJson，
 * 这三个字段会被 UMindComponent::BuildSystemPrompt 拼到 LLM 的 system 段，让 LLM 知道有哪些动作可选。
 *
 * Execute 是 fire-and-forget——异步触发 TTS / 动画后立即调 Done(true) 让 GM 推进 Apply 阶段。
 * 真正的 GM state 修改由 GameMaster.Apply 完成（P0-A 强约束：Validate / Apply 拆分）。
 */
UCLASS(Abstract, Blueprintable)
class AILIVEPROJECT_API UMindAction : public UObject
{
	GENERATED_BODY()
public:
	DECLARE_DELEGATE_TwoParams(FOnActionDone, bool /*bOk*/, FString /*Summary*/);

	/** LLM 使用的动作名（如 "speak" / "play_cards" / "vote"）。 */
	UPROPERTY(EditDefaultsOnly, Category="AI Live|Mind")
	FString ActionName;

	/** 动作参数 schema，写到 LLM prompt 里让 LLM 知道怎么填 ParamsJson。 */
	UPROPERTY(EditDefaultsOnly, Category="AI Live|Mind", meta=(MultiLine=true))
	FString ParamSchemaJson;

	/** 给 LLM 看的动作描述（中文，让 LLM 选动作时有上下文）。 */
	UPROPERTY(EditDefaultsOnly, Category="AI Live|Mind", meta=(MultiLine=true))
	FString Description;

	/** 解析 ParamsJson + 触发外显动作（TTS / Montage）。完成后调 Done。子类必须实现。 */
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done)
		PURE_VIRTUAL(UMindAction::Execute, );
};
