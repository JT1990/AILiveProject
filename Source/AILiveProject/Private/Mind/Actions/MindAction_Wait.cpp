#include "Mind/Actions/MindAction_Wait.h"

UMindAction_Wait::UMindAction_Wait()
{
	ActionName = TEXT("wait");
	Description = TEXT("空转 N 秒不做事");
	ParamSchemaJson = TEXT(R"({"seconds":"float"})");
}

void UMindAction_Wait::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T19 实现：FTimerHandle 计时，到点 Done(true)
	Done.ExecuteIfBound(true, TEXT("wait not implemented (T19)"));
}
