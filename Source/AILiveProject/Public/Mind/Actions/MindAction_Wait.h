#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_Wait.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_Wait : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_Wait();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
