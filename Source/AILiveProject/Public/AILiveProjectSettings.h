#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AILiveProjectSettings.generated.h"

/**
 * Project-level configuration for the AILive Brain HTTP client.
 *
 * Read at PIE start by the BrainSessionSubsystem and HttpClient. Persisted in
 * Config/DefaultGame.ini under [/Script/AILiveProject.AILiveProjectSettings].
 * Never write to DefaultEngine.ini (project red line).
 *
 * game_id is intentionally absent: MVP does not support cross-PIE session
 * recovery. The only way to obtain a game_id is POST /v1/games at PIE start;
 * GameInstanceSubsystem holds it in memory for the session lifetime.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "AI Live Project"))
class AILIVEPROJECT_API UAILiveProjectSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Brain HTTP server base URL (with port), no trailing slash. */
	UPROPERTY(Config, EditAnywhere, Category = "Brain Service")
	FString BrainBaseUrl = TEXT("http://127.0.0.1:8000");

	/** Brain Authorization Bearer token; must match brain BRAIN_API_TOKEN env. */
	UPROPERTY(Config, EditAnywhere, Category = "Brain Service", meta = (PasswordField = true))
	FString BrainApiToken = TEXT("dev_token");

	UPROPERTY(Config, EditAnywhere, Category = "Brain Service|Polling", meta = (ClampMin = "50"))
	int32 PollingIntervalActionMs = 200;

	UPROPERTY(Config, EditAnywhere, Category = "Brain Service|Polling", meta = (ClampMin = "50"))
	int32 PollingIntervalSpeechMs = 200;

	UPROPERTY(Config, EditAnywhere, Category = "Brain Service|Polling", meta = (ClampMin = "100"))
	int32 PollingIntervalWorldStateMs = 500;

	UPROPERTY(Config, EditAnywhere, Category = "Brain Service|HTTP", meta = (ClampMin = "500"))
	int32 HttpTimeoutMs = 5000;

	UPROPERTY(Config, EditAnywhere, Category = "Brain Service|HTTP", meta = (ClampMin = "0"))
	int32 MaxRetries = 5;

	UPROPERTY(Config, EditAnywhere, Category = "Brain Service|HTTP", meta = (ClampMin = "0.0"))
	float RetryBackoffBaseSeconds = 1.0f;

	/**
	 * Minimax TTS API key. Leave empty to fall back to
	 * UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv() which reads the
	 * `minimax=...` line from <ProjectDir>/.env (test-only).
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Brain Service|TTS", meta = (PasswordField = true))
	FString MinimaxApiKey;
};
