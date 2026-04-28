#include "SightMemoryComponent.h"

#include "AIController.h"
#include "AILiveAgent.h"
#include "Engine/World.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense_Sight.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY(LogSightMemory);

USightMemoryComponent::USightMemoryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USightMemoryComponent::BeginPlay()
{
	Super::BeginPlay();
	AttemptBindPerceptionDelegate();
}

void USightMemoryComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BindRetryHandle);
	}

	if (bDelegateBound)
	{
		if (AAIController* AIC = Cast<AAIController>(GetOwner()))
		{
			if (UAIPerceptionComponent* PC = AIC->GetPerceptionComponent())
			{
				PC->OnTargetPerceptionUpdated.RemoveDynamic(this, &USightMemoryComponent::OnPerceptionUpdated);
			}
		}
		bDelegateBound = false;
	}

	Super::EndPlay(EndPlayReason);
}

void USightMemoryComponent::AttemptBindPerceptionDelegate()
{
	AAIController* AIC = Cast<AAIController>(GetOwner());
	if (!AIC)
	{
		UE_LOG(LogSightMemory, Warning, TEXT("Owner is not AAIController, abort bind for %s"),
			*GetNameSafe(GetOwner()));
		return;
	}

	UAIPerceptionComponent* PC = AIC->GetPerceptionComponent();
	if (!PC)
	{
		if (BindRetryCount < MaxBindRetry)
		{
			++BindRetryCount;
			if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().SetTimer(
					BindRetryHandle, this,
					&USightMemoryComponent::AttemptBindPerceptionDelegate,
					BindRetryInterval, false);
			}
			return;
		}
		UE_LOG(LogSightMemory, Warning,
			TEXT("PerceptionComponent missing on %s after %d retries, giving up"),
			*AIC->GetName(), MaxBindRetry);
		return;
	}

	PC->OnTargetPerceptionUpdated.AddDynamic(this, &USightMemoryComponent::OnPerceptionUpdated);
	bDelegateBound = true;
	UE_LOG(LogSightMemory, Log, TEXT("Bound OnTargetPerceptionUpdated for %s"), *AIC->GetName());
}

void USightMemoryComponent::OnPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus)
{
	if (!Actor)
	{
		return;
	}
	if (Stimulus.Type != UAISense::GetSenseID<UAISense_Sight>())
	{
		return;
	}

	AAIController* AIC = Cast<AAIController>(GetOwner());
	if (AIC && Actor == AIC->GetPawn())
	{
		return;
	}
	if (!Actor->GetClass()->ImplementsInterface(UAILiveAgent::StaticClass()))
	{
		return;
	}

	const FString PerceiverName = AIC ? AIC->GetName() : TEXT("Unknown");
	const TWeakObjectPtr<AActor> WeakActor(Actor);

	if (Stimulus.WasSuccessfullySensed())
	{
		const bool bWasVisible = CurrentlyVisibleActors.Contains(WeakActor);
		CurrentlyVisibleActors.Add(WeakActor);
		LastSeenLocations.Add(WeakActor, Stimulus.StimulusLocation);

		if (!bWasVisible)
		{
			OnSightEnter.Broadcast(Actor, Stimulus.StimulusLocation);
			UE_LOG(LogSightMemory, Log, TEXT("[SightMemory:%s] ENTER %s at %s"),
				*PerceiverName, *Actor->GetName(), *Stimulus.StimulusLocation.ToString());
			if (bDebugPrintScreen)
			{
				const FString Msg = FString::Printf(TEXT("[SightMemory:%s] ENTER %s at %s"),
					*PerceiverName, *Actor->GetName(), *Stimulus.StimulusLocation.ToString());
				UKismetSystemLibrary::PrintString(this, Msg, true, false, FLinearColor::Green, 4.f);
			}
		}
	}
	else
	{
		const bool bWasVisible = CurrentlyVisibleActors.Contains(WeakActor);
		if (bWasVisible)
		{
			FVector LastLoc = FVector::ZeroVector;
			if (const FVector* Found = LastSeenLocations.Find(WeakActor))
			{
				LastLoc = *Found;
			}
			OnSightExit.Broadcast(Actor, LastLoc);
			UE_LOG(LogSightMemory, Log, TEXT("[SightMemory:%s] EXIT %s, last seen at %s"),
				*PerceiverName, *Actor->GetName(), *LastLoc.ToString());
			if (bDebugPrintScreen)
			{
				const FString Msg = FString::Printf(TEXT("[SightMemory:%s] EXIT %s last %s"),
					*PerceiverName, *Actor->GetName(), *LastLoc.ToString());
				UKismetSystemLibrary::PrintString(this, Msg, true, false, FLinearColor::Yellow, 4.f);
			}
		}
		CurrentlyVisibleActors.Remove(WeakActor);
	}
}

bool USightMemoryComponent::GetLastSeenLocation(AActor* Target, FVector& OutLocation) const
{
	if (!Target)
	{
		return false;
	}
	const TWeakObjectPtr<AActor> WeakTarget(Target);
	if (const FVector* Found = LastSeenLocations.Find(WeakTarget))
	{
		OutLocation = *Found;
		return true;
	}
	return false;
}

bool USightMemoryComponent::IsCurrentlyVisible(AActor* Target) const
{
	if (!Target)
	{
		return false;
	}
	return CurrentlyVisibleActors.Contains(TWeakObjectPtr<AActor>(Target));
}

void USightMemoryComponent::GetCurrentlyVisibleActors(TArray<AActor*>& OutActors) const
{
	OutActors.Reset();
	for (const TWeakObjectPtr<AActor>& Weak : CurrentlyVisibleActors)
	{
		if (AActor* A = Weak.Get())
		{
			OutActors.Add(A);
		}
	}
}

void USightMemoryComponent::GetAllLastSeenLocations(TMap<AActor*, FVector>& OutLocations) const
{
	OutLocations.Reset();
	for (const TPair<TWeakObjectPtr<AActor>, FVector>& Pair : LastSeenLocations)
	{
		if (AActor* A = Pair.Key.Get())
		{
			OutLocations.Add(A, Pair.Value);
		}
	}
}
