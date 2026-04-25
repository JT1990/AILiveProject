#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_AcceptAlliance.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_AcceptAlliance : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_AcceptAlliance();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
