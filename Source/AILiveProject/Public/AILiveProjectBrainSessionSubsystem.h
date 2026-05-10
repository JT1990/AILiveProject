#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/PimplPtr.h"
#include "AILiveProjectBrainSessionSubsystem.generated.h"

class FAILiveProjectBrainHttpClient;

/**
 * Owns the BrainService HTTP client, the in-memory game_id, and orchestrates
 * the per-PIE handshake.
 *
 * Sole entry point: GM_Sandbox::BeginPlay calls StartHandshake(). Initialize
 * does NOT connect to brain — at Initialize time Pawns are not yet spawned,
 * so roster enumeration would be empty and brain would 4xx the
 * roster_register call (schema requires minItems: 1).
 *
 * Sequence (each step waits for the previous future to resolve on GameThread):
 *   1. Health  — verify protocol_version == "0.1.1" or fatal log + abort.
 *   2. Create  — POST /v1/games, store game_id in memory only.
 *   3. Roster  — enumerate IAILiveAgent Pawns + POST /roster.
 *   4. Polling — start WorldStateCollector / ActionDispatcher /
 *                SpeakDispatcher timers (those subsystems are created on
 *                demand by the World; this subsystem just signals readiness).
 */
UCLASS()
class AILIVEPROJECT_API UAILiveProjectBrainSessionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool IsReady() const { return bReady; }
	const FString& GetGameId() const { return GameId; }
	FAILiveProjectBrainHttpClient* GetClient() const { return Client.Get(); }

	/**
	 * Trigger from GM_Sandbox BP BeginPlay (preferred) or rely on the
	 * automatic OnWorldBeginPlay hook installed in Initialize. Idempotent:
	 * returns early if already in flight.
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Brain")
	void StartHandshake();

private:
	void Phase1_HealthCheck();
	void Phase2_CreateSession();
	void Phase3_RegisterRoster();
	void Phase4_StartPolling();

	void OnWorldBeginPlayHook(UWorld* World);

	TPimplPtr<FAILiveProjectBrainHttpClient> Client;
	FString GameId;
	FString ProtocolVersion;
	bool bReady = false;
	bool bHandshakeInFlight = false;
	FDelegateHandle WorldInitDelegateHandle;
};
