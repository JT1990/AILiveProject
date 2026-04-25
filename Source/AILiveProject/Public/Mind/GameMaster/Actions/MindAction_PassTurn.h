#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_PassTurn.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_PassTurn : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_PassTurn();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
