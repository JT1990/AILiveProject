#include "AILiveProjectScatterMover.h"

#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogAILiveScatter, Log, All);

namespace AILiveScatterMoverImpl
{
	// 必须匹配 BP 函数 MoveAndLookAtLocation 的参数布局：
	// 输入 MoveLocation (FVector)、LookTarget (AActor*)，输出 bSucceeded (bool)。
	struct FMoveAndLookAtLocationParams
	{
		FVector MoveLocation = FVector::ZeroVector;
		AActor* LookTarget = nullptr;
		bool bSucceeded = false;
	};
}
using namespace AILiveScatterMoverImpl;

void UAILiveProjectScatterMover::ScatterNPCsAroundTarget(
	UEnvQuery* QueryAsset,
	AActor* CenterActor,
	const TArray<AActor*>& NPCs,
	AActor* LookTarget,
	UObject* /*WorldContextObject*/)
{
	if (!QueryAsset)
	{
		UE_LOG(LogAILiveScatter, Warning, TEXT("[Scatter] QueryAsset null, abort"));
		return;
	}
	if (!CenterActor)
	{
		UE_LOG(LogAILiveScatter, Warning, TEXT("[Scatter] CenterActor null, abort"));
		return;
	}
	if (NPCs.Num() == 0)
	{
		UE_LOG(LogAILiveScatter, Warning, TEXT("[Scatter] NPCs array empty, abort"));
		return;
	}

	// 过滤掉玩家控制的 Pawn —— L_prison 的玩家也是 SandboxCharacter_Mover_C 直接实例，
	// 会被 Level BP 的 GetAllActorsOfClass(MoverNPCClass) 顺手抓到。
	TArray<TWeakObjectPtr<AActor>> WeakNPCs;
	WeakNPCs.Reserve(NPCs.Num());
	int32 SkippedPlayer = 0;
	for (AActor* NPC : NPCs)
	{
		if (APawn* Pawn = Cast<APawn>(NPC))
		{
			if (Pawn->IsPlayerControlled())
			{
				++SkippedPlayer;
				continue;
			}
		}
		WeakNPCs.Add(NPC);
	}
	TWeakObjectPtr<AActor> WeakLookTarget(LookTarget);

	UE_LOG(LogAILiveScatter, Display,
		TEXT("[Scatter] firing EQS '%s' with Querier=%s for %d NPCs (skipped %d player-controlled)"),
		*QueryAsset->GetName(), *CenterActor->GetName(),
		WeakNPCs.Num(), SkippedPlayer);

	FEnvQueryRequest Request(QueryAsset, CenterActor);
	Request.Execute(EEnvQueryRunMode::AllMatching,
		FQueryFinishedSignature::CreateLambda(
			[WeakNPCs, WeakLookTarget](TSharedPtr<FEnvQueryResult> Result)
			{
				if (!Result.IsValid() || !Result->IsSuccessful())
				{
					UE_LOG(LogAILiveScatter, Warning,
						TEXT("[Scatter] EQS callback: invalid/failed result"));
					return;
				}

				TArray<FVector> Locations;
				Result->GetAllAsLocations(Locations);
				if (Locations.Num() == 0)
				{
					UE_LOG(LogAILiveScatter, Warning,
						TEXT("[Scatter] EQS returned 0 items, no NPCs dispatched"));
					return;
				}

				UE_LOG(LogAILiveScatter, Display,
					TEXT("[Scatter] EQS returned %d candidate points:"),
					Locations.Num());
				for (int32 li = 0; li < Locations.Num(); ++li)
				{
					const FVector& L = Locations[li];
					UE_LOG(LogAILiveScatter, Display,
						TEXT("[Scatter]   point[%d] = (%.1f, %.1f, %.1f)"),
						li, L.X, L.Y, L.Z);
				}

				AActor* LookTargetActor = WeakLookTarget.Get();
				const int32 NumNPCs = WeakNPCs.Num();
				const int32 NumPoints = Locations.Num();

				// 贪心最近匹配：每轮在所有未分配 NPC × 未占用点里挑全局最短距离的
				// 一对，分配掉。复杂度 O(N^2 * min(N,P))，N=10、P~37 完全可接受。
				// 比 stride 采样合理：NPC 直接走最近的散点而不是被强制配到远处。
				TArray<int32> AssignedIdx;
				AssignedIdx.Init(-1, NumNPCs);
				TArray<bool> NPCDone;
				NPCDone.Init(false, NumNPCs);
				TArray<bool> PointClaimed;
				PointClaimed.Init(false, NumPoints);
				int32 Remaining = FMath::Min(NumNPCs, NumPoints);

				// 提前缓存 NPC 位置，回避 lambda 内重复调用 GetActorLocation。
				TArray<FVector> NPCStarts;
				NPCStarts.Init(FVector::ZeroVector, NumNPCs);
				for (int32 i = 0; i < NumNPCs; ++i)
				{
					if (AActor* NPC = WeakNPCs[i].Get())
					{
						NPCStarts[i] = NPC->GetActorLocation();
					}
					else
					{
						NPCDone[i] = true;
					}
				}

				while (Remaining > 0)
				{
					int32 BestNPC = -1, BestPoint = -1;
					float BestDistSq = TNumericLimits<float>::Max();
					for (int32 i = 0; i < NumNPCs; ++i)
					{
						if (NPCDone[i]) { continue; }
						for (int32 j = 0; j < NumPoints; ++j)
						{
							if (PointClaimed[j]) { continue; }
							const float D = FVector::DistSquared(NPCStarts[i], Locations[j]);
							if (D < BestDistSq)
							{
								BestDistSq = D;
								BestNPC = i;
								BestPoint = j;
							}
						}
					}
					if (BestNPC < 0) { break; }
					AssignedIdx[BestNPC] = BestPoint;
					NPCDone[BestNPC] = true;
					PointClaimed[BestPoint] = true;
					--Remaining;
				}

				int32 Dispatched = 0;
				for (int32 i = 0; i < NumNPCs; ++i)
				{
					AActor* NPC = WeakNPCs[i].Get();
					if (!NPC) { continue; }

					int32 LocIdx = AssignedIdx[i];
					if (LocIdx < 0)
					{
						// NPC 多于点数（罕见）：降级为复用最后一个点。
						LocIdx = NumPoints - 1;
					}
					const FVector Target = Locations[LocIdx];
					const FVector NPCStart = NPCStarts[i];
					const float DistCm = FVector::Dist(NPCStart, Target);

					UFunction* Fn = NPC->FindFunction(
						FName(TEXT("MoveAndLookAtLocation")));
					if (!Fn)
					{
						UE_LOG(LogAILiveScatter, Warning,
							TEXT("[Scatter] %s has no BP function MoveAndLookAtLocation, skip"),
							*NPC->GetName());
						continue;
					}

					UE_LOG(LogAILiveScatter, Display,
						TEXT("[Scatter] NPC[%d] %s: from (%.1f, %.1f, %.1f) -> point[%d] (%.1f, %.1f, %.1f), dist=%.1f"),
						i, *NPC->GetName(),
						NPCStart.X, NPCStart.Y, NPCStart.Z,
						LocIdx, Target.X, Target.Y, Target.Z,
						DistCm);

					FMoveAndLookAtLocationParams Params;
					Params.MoveLocation = Target;
					Params.LookTarget = LookTargetActor;
					NPC->ProcessEvent(Fn, &Params);
					++Dispatched;
				}

				if (WeakNPCs.Num() > Locations.Num())
				{
					UE_LOG(LogAILiveScatter, Warning,
						TEXT("[Scatter] %d NPCs but only %d EQS points; "
							"surplus NPCs reuse the last point"),
						WeakNPCs.Num(), Locations.Num());
				}

				UE_LOG(LogAILiveScatter, Display,
					TEXT("[Scatter] dispatched %d NPCs to %d EQS points"),
					Dispatched, Locations.Num());
			}));
}
