#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_Think.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_Think : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_Think();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
