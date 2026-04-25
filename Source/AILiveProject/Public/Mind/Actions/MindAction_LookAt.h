#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_LookAt.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_LookAt : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_LookAt();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
