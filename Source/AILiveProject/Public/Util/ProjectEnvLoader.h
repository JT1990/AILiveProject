#pragma once

#include "CoreMinimal.h"

namespace ProjectEnvLoader
{
	AILIVEPROJECT_API FString Get(const FString& Key);
	AILIVEPROJECT_API void Reload();
}
