#include "Mind/MindSpeechHelpers.h"
#include "Mind/MindLog.h"
#include "Components/ChildActorComponent.h"
#include "GameFramework/Actor.h"

AActor* UMindSpeechHelpers::ResolveSpeechActor(AActor* MindOwner)
{
	if (!IsValid(MindOwner))
	{
		UE_LOG(LogMindAction, Warning, TEXT("ResolveSpeechActor: MindOwner is null"));
		return nullptr;
	}

	TArray<UChildActorComponent*> Children;
	MindOwner->GetComponents<UChildActorComponent>(Children);
	for (UChildActorComponent* Child : Children)
	{
		if (!IsValid(Child)) continue;
		if (!Child->ComponentTags.Contains(FName(TEXT("VisualOverride")))) continue;
		if (AActor* Spawned = Child->GetChildActor())
		{
			return Spawned;
		}
	}

	UE_LOG(LogMindAction, Verbose,
		TEXT("ResolveSpeechActor: no spawned VisualOverride child on %s, fallback to owner"),
		*MindOwner->GetName());
	return MindOwner;
}
