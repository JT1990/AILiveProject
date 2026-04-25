#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_Observe.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_Observe : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_Observe();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
