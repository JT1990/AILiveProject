#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_Challenge.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_Challenge : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_Challenge();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
