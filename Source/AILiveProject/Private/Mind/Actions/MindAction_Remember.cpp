#include "Mind/Actions/MindAction_Remember.h"

UMindAction_Remember::UMindAction_Remember()
{
	ActionName = TEXT("remember");
	Description = TEXT("显式写入一条记忆");
	ParamSchemaJson = TEXT(R"({"content":"string","tags":"object?"})");
}

void UMindAction_Remember::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T19 实现：MindMemoryClient->Write
	Done.ExecuteIfBound(true, TEXT("remember not implemented (T19)"));
}
