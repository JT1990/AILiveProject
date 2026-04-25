#include "Mind/Actions/MindAction_Recall.h"

UMindAction_Recall::UMindAction_Recall()
{
	ActionName = TEXT("recall");
	Description = TEXT("检索 top-K 相关记忆并注入下一次 prompt（受 RecallChainDepth ≤ 2 限制）");
	ParamSchemaJson = TEXT(R"({"query":"string","top_k":"int?"})");
}

void UMindAction_Recall::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T19 实现：MindMemoryClient->Recall + 写 Owner->StashedRecall + 同决策内重调 LLM
	Done.ExecuteIfBound(true, TEXT("recall not implemented (T19)"));
}
