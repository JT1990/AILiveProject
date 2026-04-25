#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_MoveTo.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_MoveTo : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_MoveTo();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
