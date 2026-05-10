#include "AILiveProjectRosterSubsystem.h"

#include "AILiveAgent.h"
#include "AILiveProjectLog.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "Internationalization/Regex.h"

void UAILiveProjectRosterSubsystem::RegisterPawn(const FString& ActorId, APawn* Pawn)
{
	if (ActorId.IsEmpty() || !Pawn) { return; }
	const TWeakObjectPtr<APawn> Weak = Pawn;
	ActorIdToPawn.Add(ActorId, Weak);
	PawnToActorId.Add(Weak, ActorId);
	BPClassFNameToActorId.Add(StripBlueprintSuffix(Pawn->GetClass()->GetFName()), ActorId);
}

APawn* UAILiveProjectRosterSubsystem::FindPawnByActorId(const FString& ActorId) const
{
	if (const TWeakObjectPtr<APawn>* Found = ActorIdToPawn.Find(ActorId))
	{
		return Found->Get();
	}
	return nullptr;
}

bool UAILiveProjectRosterSubsystem::TryGetActorIdForPawn(APawn* Pawn, FString& OutActorId) const
{
	if (!Pawn) { return false; }
	if (const FString* Found = PawnToActorId.Find(Pawn))
	{
		OutActorId = *Found;
		return true;
	}
	return false;
}

bool UAILiveProjectRosterSubsystem::TryGetActorIdForBPClassFName(FName BPClassFName, FString& OutActorId) const
{
	if (const FString* Found = BPClassFNameToActorId.Find(StripBlueprintSuffix(BPClassFName)))
	{
		OutActorId = *Found;
		return true;
	}
	return false;
}

void UAILiveProjectRosterSubsystem::GetAllRegistered(
	TArray<TPair<FString, TWeakObjectPtr<APawn>>>& Out) const
{
	Out.Reset();
	Out.Reserve(ActorIdToPawn.Num());
	for (const TPair<FString, TWeakObjectPtr<APawn>>& Pair : ActorIdToPawn)
	{
		Out.Add(Pair);
	}
}

void UAILiveProjectRosterSubsystem::ClearAll()
{
	ActorIdToPawn.Reset();
	PawnToActorId.Reset();
	BPClassFNameToActorId.Reset();
}

FName UAILiveProjectRosterSubsystem::StripBlueprintSuffix(FName In)
{
	FString S = In.ToString();
	if (S.EndsWith(TEXT("_C"))) { S.LeftChopInline(2); }
	return FName(*S);
}

bool UAILiveProjectRosterSubsystem::TryParseNpcIndexFromBPClassName(
	const FString& BPClassName, int32& OutIndex)
{
	// Match BP_NPC_MH_Character_<N> with optional trailing _C suffix.
	const FRegexPattern Pattern(TEXT("BP_NPC_MH_Character_(\\d+)(?:_C)?$"));
	FRegexMatcher Matcher(Pattern, BPClassName);
	if (Matcher.FindNext())
	{
		const FString IndexStr = Matcher.GetCaptureGroup(1);
		OutIndex = FCString::Atoi(*IndexStr);
		return OutIndex > 0;
	}
	// Fallback: trailing digits anywhere in the name.
	const FRegexPattern FallbackPattern(TEXT("(\\d+)$"));
	FRegexMatcher Fb(FallbackPattern, BPClassName);
	if (Fb.FindNext())
	{
		OutIndex = FCString::Atoi(*Fb.GetCaptureGroup(1));
		return OutIndex > 0;
	}
	return false;
}

int32 UAILiveProjectRosterSubsystem::EnumerateAndRegisterAgentsInWorld(UWorld* World)
{
	ClearAll();
	if (!World) { return 0; }
	int32 Count = 0;
	for (TActorIterator<APawn> It(World); It; ++It)
	{
		APawn* Pawn = *It;
		if (!Pawn || !Pawn->GetClass()->ImplementsInterface(UAILiveAgent::StaticClass())) { continue; }
		const FString BpName = Pawn->GetClass()->GetName();
		int32 Index = 0;
		if (!TryParseNpcIndexFromBPClassName(BpName, Index))
		{
			UE_LOG(LogAILiveBrain, Error,
				TEXT("Cannot infer actor_id from BP class '%s'; skipping pawn '%s'"),
				*BpName, *Pawn->GetName());
			continue;
		}
		const FString ActorId = FString::Printf(TEXT("NPC%02d"), Index);
		RegisterPawn(ActorId, Pawn);
		++Count;
	}
	UE_LOG(LogAILiveBrain, Log, TEXT("Roster enumerated count=%d"), Count);
	return Count;
}
