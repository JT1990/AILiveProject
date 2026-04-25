#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Templates/SubclassOf.h"
#include "Delegates/Delegate.h"
#include "MindLLMProvider.generated.h"

class UMindAction;

/**
 * LLM Provider 抽象基类。子类（DeepSeek / GLM / Mock）实现具体 HTTP / 匹配逻辑。
 *
 * EditInlineNew 让 Config 的 Details 面板能下拉选子类；Abstract 让自身不能被实例化（必须选具体子类）。
 */
UCLASS(Abstract, EditInlineNew)
class AILIVEPROJECT_API UMindLLMProvider : public UObject
{
	GENERATED_BODY()
public:
	DECLARE_DELEGATE_TwoParams(FOnLLMResult, bool /*bOk*/, FString /*JsonText*/);

	/** 异步请求 LLM。AvailableActions 用于让 Provider 把动作 schema 拼进 prompt（T06 实现细节）。 */
	virtual void RequestCompletion(
		const FString& SystemPrompt,
		const FString& UserPrompt,
		const TArray<TSubclassOf<UMindAction>>& AvailableActions,
		FOnLLMResult Done) PURE_VIRTUAL(UMindLLMProvider::RequestCompletion, );
};
