#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_Remember.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_Remember : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_Remember();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
