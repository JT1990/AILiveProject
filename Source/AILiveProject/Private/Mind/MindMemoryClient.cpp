#include "Mind/MindMemoryClient.h"

void UMindMemoryClient::Write(FString /*AgentId*/, FString /*Content*/, TMap<FString, FString> /*Tags*/)
{
	// T09 实现：HTTP POST /memory/write
}

void UMindMemoryClient::Recall(FString /*AgentId*/, FString /*Query*/, int32 /*TopK*/, FOnRecall /*Done*/)
{
	// T09 实现：HTTP POST /memory/recall
}

void UMindMemoryClient::ByTag(FString /*AgentId*/, FString /*TagKey*/, FString /*TagValue*/, int32 /*TopN*/, FOnRecall /*Done*/)
{
	// T09 实现：HTTP POST /memory/by_tag
}
