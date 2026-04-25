#pragma once

#include "CoreMinimal.h"
#include "Mind/MindAction.h"
#include "MindAction_Speak.generated.h"

UCLASS()
class AILIVEPROJECT_API UMindAction_Speak : public UMindAction
{
	GENERATED_BODY()
public:
	UMindAction_Speak();
	virtual void Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done) override;
};
