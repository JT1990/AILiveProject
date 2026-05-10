#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "AILiveProjectRosterSubsystem.generated.h"

class APawn;

/**
 * Maintains the bi-directional map between brain-side actor_id (NPC01..NPC10)
 * and the corresponding APawn instances spawned in the level.
 *
 * Population happens once per PIE session in BrainSessionSubsystem after
 * GameMode::BeginPlay completes (Pawn spawns settled). actor_id is inferred
 * from the BP class name pattern `BP_NPC_MH_Character_<N>(_C)?` and formatted
 * as `NPC%02d`.
 *
 * WorldStateCollector calls TryGetActorIdForBPClassFName because
 * UAILiveProjectPerceptionLogger returns stripped BP class FNames (not Pawn
 * pointers) on its perception entries.
 */
UCLASS()
class AILIVEPROJECT_API UAILiveProjectRosterSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	void RegisterPawn(const FString& ActorId, APawn* Pawn);

	APawn* FindPawnByActorId(const FString& ActorId) const;

	bool TryGetActorIdForPawn(APawn* Pawn, FString& OutActorId) const;
	bool TryGetActorIdForBPClassFName(FName BPClassFName, FString& OutActorId) const;

	void GetAllRegistered(TArray<TPair<FString, TWeakObjectPtr<APawn>>>& Out) const;
	int32 NumRegistered() const { return ActorIdToPawn.Num(); }

	void ClearAll();

	/**
	 * Walk the world, find every Pawn implementing IAILiveAgent, infer
	 * `NPC%02d` from BP class name, and register. Returns the count
	 * registered. Logs and skips Pawns whose names don't match.
	 */
	int32 EnumerateAndRegisterAgentsInWorld(UWorld* World);

	/** Strips trailing `_C` (UE BP class suffix) from an FName. */
	static FName StripBlueprintSuffix(FName In);

	/** Public for unit-test visibility. */
	static bool TryParseNpcIndexFromBPClassName(const FString& BPClassName, int32& OutIndex);

private:
	TMap<FString, TWeakObjectPtr<APawn>> ActorIdToPawn;
	TMap<TWeakObjectPtr<APawn>, FString> PawnToActorId;
	TMap<FName, FString>                 BPClassFNameToActorId;
};
