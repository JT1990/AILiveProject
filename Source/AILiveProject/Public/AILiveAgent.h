#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "AILiveAgent.generated.h"

UINTERFACE(BlueprintType, Blueprintable)
class AILIVEPROJECT_API UAILiveAgent : public UInterface
{
	GENERATED_BODY()
};

/**
 * Marker interface for AI Live agents. Implementing actors become first-class
 * citizens in perception, memory and social-graph systems. The interface is
 * intentionally empty for now; future milestones will add team / identity hooks.
 */
class AILIVEPROJECT_API IAILiveAgent
{
	GENERATED_BODY()
};
