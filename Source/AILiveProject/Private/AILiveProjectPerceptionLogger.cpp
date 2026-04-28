#include "AILiveProjectPerceptionLogger.h"

#include "AIController.h"
#include "AILiveAgent.h"
#include "GameFramework/Pawn.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense.h"
#include "Perception/AISense_Sight.h"

DEFINE_LOG_CATEGORY_STATIC(LogAILivePerception, Log, All);

namespace
{
	FName StripBlueprintSuffix(const FName ClassFName)
	{
		FString S = ClassFName.ToString();
		if (S.EndsWith(TEXT("_C")))
		{
			S.LeftChopInline(2, EAllowShrinking::No);
		}
		return FName(*S);
	}

	bool IsAgentActor(const AActor* A)
	{
		return A && A->GetClass()->ImplementsInterface(UAILiveAgent::StaticClass());
	}

	float NormalizeYaw180(float Yaw)
	{
		while (Yaw > 180.f) { Yaw -= 360.f; }
		while (Yaw < -180.f) { Yaw += 360.f; }
		return Yaw;
	}
}

TArray<FPerceivedAgentInfo> UAILiveProjectPerceptionLogger::GatherSightPerception(
	AAIController* Perceiver,
	UObject* /*WorldContextObject*/)
{
	TArray<FPerceivedAgentInfo> Result;
	if (!Perceiver)
	{
		UE_LOG(LogAILivePerception, Warning, TEXT("Diag: Perceiver=null"));
		return Result;
	}

	APawn* PerceiverPawn = Perceiver->GetPawn();
	if (!PerceiverPawn)
	{
		UE_LOG(LogAILivePerception, Warning, TEXT("Diag: Controller=%s Pawn=null"),
			*Perceiver->GetClass()->GetName());
		return Result;
	}

	UAIPerceptionComponent* PC = Perceiver->FindComponentByClass<UAIPerceptionComponent>();
	if (!PC)
	{
		UE_LOG(LogAILivePerception, Warning,
			TEXT("Diag: Controller=%s Pawn=%s PC=null (controller has NO AIPerceptionComponent)"),
			*Perceiver->GetClass()->GetName(),
			*PerceiverPawn->GetName());
		return Result;
	}

	TArray<AActor*> Currently;
	PC->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), Currently);

	const FAISenseID SightID = UAISense::GetSenseID<UAISense_Sight>();
	const FVector PerceiverLoc = PerceiverPawn->GetActorLocation();
	const float PerceiverYawWorld = PerceiverPawn->GetActorRotation().Yaw;

	Result.Reserve(Currently.Num());
	for (AActor* A : Currently)
	{
		if (!A || A == PerceiverPawn) { continue; }
		if (!IsAgentActor(A)) { continue; }

		FActorPerceptionBlueprintInfo Info;
		PC->GetActorsPerception(A, Info);

		FAIStimulus SightStim;
		bool bFound = false;
		for (const FAIStimulus& S : Info.LastSensedStimuli)
		{
			if (S.Type == SightID)
			{
				SightStim = S;
				bFound = true;
				break;
			}
		}

		if (!bFound || !SightStim.WasSuccessfullySensed()) { continue; }

		FPerceivedAgentInfo Entry;
		Entry.Identity = StripBlueprintSuffix(A->GetClass()->GetFName());
		const FVector AtoP = A->GetActorLocation() - PerceiverLoc;
		Entry.DistanceCm = AtoP.Size();
		Entry.DirectionFromPerceiver = AtoP.Rotation();
		Entry.RelativeYawDeg = NormalizeYaw180(Entry.DirectionFromPerceiver.Yaw - PerceiverYawWorld);
		Entry.bCurrentlySensed = true;
		Entry.StimulusAge = SightStim.GetAge();
		Entry.LastStimulusLocation = SightStim.StimulusLocation;
		Result.Add(Entry);
	}
	return Result;
}

int32 UAILiveProjectPerceptionLogger::LogPerceptionToOutput(
	AAIController* Perceiver,
	FString Tag,
	UObject* WorldContextObject)
{
	const TArray<FPerceivedAgentInfo> List = GatherSightPerception(Perceiver, WorldContextObject);

	const FString Header = FString::Printf(TEXT("[%s] perception count=%d"), *Tag, List.Num());
	UE_LOG(LogAILivePerception, Display, TEXT("%s"), *Header);
	UKismetSystemLibrary::PrintString(WorldContextObject, Header,
		true, true, FLinearColor(0.f, 1.f, 1.f), 6.f);

	for (const FPerceivedAgentInfo& I : List)
	{
		const FString Line = FString::Printf(
			TEXT("[%s] -> %s dist=%.1f relYaw=%.1f age=%.2f"),
			*Tag,
			*I.Identity.ToString(),
			I.DistanceCm,
			I.RelativeYawDeg,
			I.StimulusAge);
		UE_LOG(LogAILivePerception, Display, TEXT("%s"), *Line);
		UKismetSystemLibrary::PrintString(WorldContextObject, Line,
			true, true, FLinearColor(0.f, 1.f, 0.5f), 6.f);
	}
	return List.Num();
}
