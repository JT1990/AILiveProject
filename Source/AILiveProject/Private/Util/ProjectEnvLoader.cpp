#include "Util/ProjectEnvLoader.h"

#include "HAL/CriticalSection.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogProjectEnv, Log, All);

namespace
{
	FCriticalSection EnvCS;
	TMap<FString, FString> CachedEnv;
	bool bLoaded = false;

	void LoadIfNeeded()
	{
		FScopeLock Lock(&EnvCS);
		if (bLoaded)
		{
			return;
		}
		const FString Path = FPaths::ProjectDir() / TEXT(".env");
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *Path))
		{
			UE_LOG(LogProjectEnv, Warning, TEXT("Could not read .env at %s"), *Path);
			bLoaded = true;
			return;
		}

		TArray<FString> Lines;
		Contents.ParseIntoArrayLines(Lines);
		for (FString& Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
			{
				continue;
			}
			FString Key, Value;
			if (Line.Split(TEXT("="), &Key, &Value))
			{
				Key.TrimStartAndEndInline();
				Value.TrimStartAndEndInline();
				Value.TrimQuotesInline();
				CachedEnv.Add(Key, Value);
			}
		}
		UE_LOG(LogProjectEnv, Log, TEXT(".env loaded with %d entries"), CachedEnv.Num());
		bLoaded = true;
	}
}

namespace ProjectEnvLoader
{
	FString Get(const FString& Key)
	{
		LoadIfNeeded();
		FScopeLock Lock(&EnvCS);
		// case-insensitive lookup
		for (const TPair<FString, FString>& Pair : CachedEnv)
		{
			if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase))
			{
				return Pair.Value;
			}
		}
		return FString();
	}

	void Reload()
	{
		FScopeLock Lock(&EnvCS);
		CachedEnv.Reset();
		bLoaded = false;
	}
}
