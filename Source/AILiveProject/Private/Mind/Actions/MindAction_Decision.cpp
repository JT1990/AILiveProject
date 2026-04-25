#include "Mind/Actions/MindAction_Decision.h"

UMindAction_Decision::UMindAction_Decision()
{
	ActionName = TEXT("decision");
	Description = TEXT("做出高层意图，下一轮 prompt 自动带 Current Intent");
	ParamSchemaJson = TEXT(R"({"intent":"string","target":"string?","justification":"string?"})");
}

void UMindAction_Decision::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T19 实现：写 Owner->CurrentIntent + Memory.Write 带 tag={"type":"intent"}
	Done.ExecuteIfBound(true, TEXT("decision not implemented (T19)"));
}
