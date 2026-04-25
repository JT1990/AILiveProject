#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_AskQuestion.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_AskQuestion : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_AskQuestion();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
