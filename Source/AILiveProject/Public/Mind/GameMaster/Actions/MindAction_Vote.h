#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_Vote.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_Vote : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_Vote();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
