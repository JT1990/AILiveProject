#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_ProposeAlliance.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_ProposeAlliance : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_ProposeAlliance();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
