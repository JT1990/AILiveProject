#pragma once

#include "CoreMinimal.h"
#include "AILiveProtocolTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "Templates/PimplPtr.h"
#include "AILiveProjectBrainSessionSubsystem.generated.h"

class FAILiveProjectBrainHttpClient;
class FAILiveProjectBrainWsClient;

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
 *   1. Health  — verify protocol_version == "0.3.0" or fatal log + abort.
 *   2. WebSocket connect to BrainService.
 *   3. Create  — session.create over WebSocket, store game_id in memory only.
 *   4. Roster  — enumerate IAILiveAgent Pawns + roster.register.
 *   5. Runtime — start world-state sampling; action/speech arrive by push.
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
	FAILiveProjectBrainWsClient* GetWsClient() const { return WsClient.Get(); }

	bool SendWorldState(const FAIL_WorldStatePushRequest& Req);
	bool SendActionResult(const FAIL_ActionResultRequest& Req);
	bool SendSpeechResult(const FAIL_SpeechResultRequest& Req);
	bool SendIngressReject(const FAIL_IngressRejectRequest& Req);
	bool AckBrainEvent(int64 Seq);

	/**
	 * UE 端开场动画（Round-001 Tick-0001/0002）完成后调用。
	 * Brain 接到 `round.start_llm_phase` 帧后启动 N 拍 LLM 循环，
	 * 期间通过现有 `event.speech_public` / `event.action_intent` 帧推送给 UE。
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Brain")
	bool RequestStartLLMPhase(int32 RoundNo, const FString& Phase, int32 NTicks);

	/**
	 * Trigger from GM_Sandbox BP BeginPlay (preferred) or rely on the
	 * automatic OnWorldBeginPlay hook installed in Initialize. Idempotent:
	 * returns early if already in flight.
	 */
	UFUNCTION(BlueprintCallable, Category = "AILive|Brain")
	void StartHandshake();

private:
	void Phase1_HealthCheck();
	void Phase2_ConnectWebSocket();
	void Phase3_CreateSession();
	void Phase4_RegisterRoster();
	void Phase5_StartRuntime();
	void ScheduleReconnect();
	void InstallWebSocketHandlers();

	void OnWorldBeginPlayHook(UWorld* World);

	TPimplPtr<FAILiveProjectBrainHttpClient> Client;
	TPimplPtr<FAILiveProjectBrainWsClient> WsClient;
	FString GameId;
	FString ProtocolVersion;
	bool bReady = false;
	bool bHandshakeInFlight = false;
	FDelegateHandle WorldInitDelegateHandle;
	FTimerHandle ReconnectTimerHandle;
};
