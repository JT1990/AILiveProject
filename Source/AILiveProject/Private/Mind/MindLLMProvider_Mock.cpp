#include "Mind/MindLLMProvider_Mock.h"
#include "Mind/MindLog.h"
#include "Containers/Ticker.h"

void UMindLLMProvider_Mock::RequestCompletion(
	const FString& /*SystemPrompt*/,
	const FString& UserPrompt,
	const TArray<TSubclassOf<UMindAction>>& /*AvailableActions*/,
	FOnLLMResult Done)
{
	if (!Table)
	{
		UE_LOG(LogMind, Warning, TEXT("UMindLLMProvider_Mock: no Table bound"));
		Done.ExecuteIfBound(false, TEXT("{\"error\":\"mock table not bound\"}"));
		return;
	}

	// 1) substring match → 收集命中行 + 累计权重
	TArray<const FMockResponseRow*> Hits;
	float TotalWeight = 0.f;
	for (const FMockResponseRow& R : Table->Rows)
	{
		if (!R.MatchPattern.IsEmpty() && UserPrompt.Contains(R.MatchPattern))
		{
			Hits.Add(&R);
			TotalWeight += FMath::Max(0.f, R.Weight);
		}
	}

	// 2) 加权随机或 fallback
	FString Out;
	if (Hits.Num() == 0 || TotalWeight <= 0.f)
	{
		Out = Table->FallbackResponseJson;
		UE_LOG(LogMind, Verbose, TEXT("UMindLLMProvider_Mock: fallback (no match)"));
	}
	else
	{
		const float Pick = FMath::FRandRange(0.f, TotalWeight);
		Out = Hits.Last()->ResponseJson; // 末位兜底，避免浮点误差让 Pick == TotalWeight 落空
		float Acc = 0.f;
		for (const FMockResponseRow* R : Hits)
		{
			Acc += FMath::Max(0.f, R->Weight);
			if (Pick <= Acc)
			{
				Out = R->ResponseJson;
				break;
			}
		}
	}

	// 3) FTSTicker 模拟延迟（统一路径，不依赖 UWorld；commandlet / PIE 通用）
	const float Delay = FMath::Max(0.f, MockDelaySeconds);
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Done, Out](float) -> bool
		{
			Done.ExecuteIfBound(true, Out);
			return false; // 单次触发，不再回调
		}),
		Delay);
}
