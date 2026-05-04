#pragma once

#include "CoreMinimal.h"
#include "AILiveBidTypes.generated.h"

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveBid
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	FString Actor;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	int64 Seq = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	int64 IntendedSeq = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	float Urgency = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	float BidOffset = 0.f;

	// T7 加：principles §5.4 反霸麦衰减 + 被 @ 加权 + 沉默加权汇总，用于 tick_audit.all_bids[].runtime_adj。
	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	float RuntimeAdj = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	float FinalScore = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	FString ProposedTarget;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	FString Rationale;
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveTickResolution
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	int32 TickNo = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	FString WinnerActor;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	int64 WinnerIntendedSeq = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	int64 DerivedPublicSeq = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	TArray<FAILiveBid> AllBids;
};
