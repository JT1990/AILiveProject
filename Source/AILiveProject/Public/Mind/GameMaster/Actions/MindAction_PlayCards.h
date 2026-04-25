#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_PlayCards.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_PlayCards : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_PlayCards();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
