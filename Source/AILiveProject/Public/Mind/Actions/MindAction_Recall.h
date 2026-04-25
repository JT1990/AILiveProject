#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_Recall.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_Recall : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_Recall();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
