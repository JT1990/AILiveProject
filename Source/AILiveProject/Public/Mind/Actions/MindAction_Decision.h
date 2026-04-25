#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_Decision.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_Decision : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_Decision();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
