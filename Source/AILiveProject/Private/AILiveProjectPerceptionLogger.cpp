// =============================================================================
// 中文教学：AILiveProjectPerceptionLogger.cpp —— 感知收集 + BP 工具实现
//
// 文件做这几件事：
//   1) GatherSightPerception：从 AIController 的 PerceptionComponent 抓出当前
//      sight stimulus 列表，每个匹配 IAILiveAgent 的 actor 算出距离/方位/相对偏航
//   2) GatherHearingPerception：同上但是 hearing
//   3) Log* 系列：把上面收集结果打到 Output Log（debug 用）
//   4) ClaimFirstSlotInActor：在指定 SmartObject actor 上找首个空闲 slot 申领
//
// 关键 UE 概念：
//   1) Implements<UAILiveAgent>()
//      检查 actor 是否实现了 IAILiveAgent 接口（注意是 U 前缀的 marker class）。
//      所有 NPC actor 都该实现 IAILiveAgent，这样过滤逻辑就能区分敌我。
//
//   2) FRotator::FromDirection / FVector::Size
//      方向 → 偏航角 / 距离的标准 UE 数学。常用工具。
//
//   3) AIPerceptionComponent::GetCurrentlyPerceivedActors
//      只取「当前还能感知到」的 actor（excludes 已离开视野超过 max age 的）。
//      想拿历史记录用 GetPerceivedActors（不带 Currently 前缀）。
// =============================================================================

#include "AILiveProjectPerceptionLogger.h"

#include "AIController.h"
#include "AILiveAgent.h"
#include "Components/ChildActorComponent.h"
#include "GameFramework/Pawn.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISense.h"
#include "Perception/AISense_Hearing.h"
#include "Perception/AISense_Sight.h"
#include "GameplayTagContainer.h"
#include "SmartObjectBlueprintFunctionLibrary.h"
#include "SmartObjectComponent.h"
#include "SmartObjectRequestTypes.h"
#include "SmartObjectTypes.h"

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

TArray<FHeardSoundInfo> UAILiveProjectPerceptionLogger::GatherHearingPerception(
	AAIController* Perceiver,
	UObject* /*WorldContextObject*/)
{
	TArray<FHeardSoundInfo> Result;
	if (!Perceiver)
	{
		UE_LOG(LogAILivePerception, Warning, TEXT("Diag: Hearing Perceiver=null"));
		return Result;
	}

	APawn* PerceiverPawn = Perceiver->GetPawn();
	if (!PerceiverPawn)
	{
		UE_LOG(LogAILivePerception, Warning, TEXT("Diag: Hearing Controller=%s Pawn=null"),
			*Perceiver->GetClass()->GetName());
		return Result;
	}

	UAIPerceptionComponent* PC = Perceiver->FindComponentByClass<UAIPerceptionComponent>();
	if (!PC)
	{
		UE_LOG(LogAILivePerception, Warning,
			TEXT("Diag: Hearing Controller=%s Pawn=%s PC=null"),
			*Perceiver->GetClass()->GetName(),
			*PerceiverPawn->GetName());
		return Result;
	}

	TArray<AActor*> Currently;
	PC->GetCurrentlyPerceivedActors(UAISense_Hearing::StaticClass(), Currently);

	const FAISenseID HearingID = UAISense::GetSenseID<UAISense_Hearing>();
	const FVector PerceiverLoc = PerceiverPawn->GetActorLocation();
	const float PerceiverYawWorld = PerceiverPawn->GetActorRotation().Yaw;

	Result.Reserve(Currently.Num());
	for (AActor* A : Currently)
	{
		if (!A || A == PerceiverPawn) { continue; }
		if (!IsAgentActor(A)) { continue; }

		FActorPerceptionBlueprintInfo Info;
		PC->GetActorsPerception(A, Info);

		FAIStimulus HearingStim;
		bool bFound = false;
		for (const FAIStimulus& S : Info.LastSensedStimuli)
		{
			if (S.Type == HearingID)
			{
				HearingStim = S;
				bFound = true;
				break;
			}
		}

		if (!bFound || !HearingStim.WasSuccessfullySensed()) { continue; }

		FHeardSoundInfo Entry;
		Entry.SourceIdentity = StripBlueprintSuffix(A->GetClass()->GetFName());
		const FVector StimLoc = HearingStim.StimulusLocation;
		const FVector PtoStim = StimLoc - PerceiverLoc;
		Entry.DistanceCm = PtoStim.Size();
		Entry.DirectionFromPerceiver = PtoStim.Rotation();
		Entry.RelativeYawDeg = NormalizeYaw180(Entry.DirectionFromPerceiver.Yaw - PerceiverYawWorld);
		Entry.StimulusLocation = StimLoc;
		Entry.Loudness = HearingStim.Strength;
		Entry.StimulusAge = HearingStim.GetAge();
		Result.Add(Entry);
	}
	return Result;
}

AActor* UAILiveProjectPerceptionLogger::GetChildActorOf(UChildActorComponent* Component)
{
	return Component ? Component->GetChildActor() : nullptr;
}

int32 UAILiveProjectPerceptionLogger::LogHearingPerceptionToOutput(
	AAIController* Perceiver,
	FString Tag,
	UObject* WorldContextObject)
{
	const TArray<FHeardSoundInfo> List = GatherHearingPerception(Perceiver, WorldContextObject);

	const FString Header = FString::Printf(TEXT("[%s] hearing count=%d"), *Tag, List.Num());
	UE_LOG(LogAILivePerception, Display, TEXT("%s"), *Header);
	UKismetSystemLibrary::PrintString(WorldContextObject, Header,
		true, true, FLinearColor(1.f, 0.7f, 0.f), 6.f);

	for (const FHeardSoundInfo& I : List)
	{
		const FString Line = FString::Printf(
			TEXT("[%s] heard %s dist=%.1f relYaw=%.1f loud=%.2f age=%.2f"),
			*Tag,
			*I.SourceIdentity.ToString(),
			I.DistanceCm,
			I.RelativeYawDeg,
			I.Loudness,
			I.StimulusAge);
		UE_LOG(LogAILivePerception, Display, TEXT("%s"), *Line);
		UKismetSystemLibrary::PrintString(WorldContextObject, Line,
			true, true, FLinearColor(1.f, 0.5f, 0.f), 6.f);
	}
	return List.Num();
}

FSmartObjectClaimHandle UAILiveProjectPerceptionLogger::ClaimFirstSlotInActor(
	AActor* SmartObjectActor,
	AActor* UserActor,
	UObject* WorldContextObject)
{
	if (!SmartObjectActor)
	{
		UE_LOG(LogAILivePerception, Warning, TEXT("[Sit] ClaimFirstSlotInActor: SmartObjectActor is null"));
		return FSmartObjectClaimHandle();
	}

	TArray<FSmartObjectRequestResult> Results;
	FSmartObjectRequestFilter Filter;
	// SO_BenchDefinition.UserTagFilter = ANY_EXACT(SmartObject.ObjectType.NPC, .Player)
	// 默认 Filter.UserTags 为空 → ANY_EXACT 评估为 false → 所有 slot 被过滤。
	// 主动给 NPC 这个 user tag 让 SO 接受请求。
	const FGameplayTag NPCTag = FGameplayTag::RequestGameplayTag(
		FName(TEXT("SmartObject.ObjectType.NPC")), /*bErrorIfNotFound=*/false);
	Filter.UserTags.AddTag(NPCTag);
	UE_LOG(LogAILivePerception, Display,
		TEXT("[Sit] Filter prepared: NPCTag.IsValid=%d UserTags=%s bShouldEvaluateConditions=%d bShouldIncludeClaimedSlots=%d"),
		NPCTag.IsValid() ? 1 : 0,
		*Filter.UserTags.ToString(),
		Filter.bShouldEvaluateConditions ? 1 : 0,
		Filter.bShouldIncludeClaimedSlots ? 1 : 0);

	// 也直接遍历 SO actor 的所有 SmartObjectComponent 看 RegisteredHandle 是否就位。
	TArray<UActorComponent*> SOComps;
	SmartObjectActor->GetComponents(USmartObjectComponent::StaticClass(), SOComps);
	for (UActorComponent* C : SOComps)
	{
		if (USmartObjectComponent* SOC = Cast<USmartObjectComponent>(C))
		{
			UE_LOG(LogAILivePerception, Display,
				TEXT("[Sit]   SOComp on %s: RegisteredHandle.IsValid=%d Definition=%s"),
				*SmartObjectActor->GetName(),
				SOC->GetRegisteredHandle().IsValid() ? 1 : 0,
				*GetNameSafe(SOC->GetDefinition()));
		}
	}

	const bool bAny = USmartObjectBlueprintFunctionLibrary::FindSmartObjectsInActor(
		Filter, SmartObjectActor, Results, UserActor);
	UE_LOG(LogAILivePerception, Display,
		TEXT("[Sit] FindSmartObjectsInActor returned bAny=%d Results.Num=%d"),
		bAny ? 1 : 0, Results.Num());
	if (!bAny || Results.Num() == 0)
	{
		UE_LOG(LogAILivePerception, Warning,
			TEXT("[Sit] ClaimFirstSlotInActor: no available slot on %s"),
			*SmartObjectActor->GetName());
		return FSmartObjectClaimHandle();
	}

	const FSmartObjectClaimHandle Handle =
		USmartObjectBlueprintFunctionLibrary::MarkSmartObjectSlotAsClaimed(
			WorldContextObject, Results[0].SlotHandle, UserActor,
			ESmartObjectClaimPriority::Normal);

	UE_LOG(LogAILivePerception, Display,
		TEXT("[Sit] ClaimFirstSlotInActor: claimed slot on %s, valid=%d"),
		*SmartObjectActor->GetName(), Handle.IsValid() ? 1 : 0);

	return Handle;
}
