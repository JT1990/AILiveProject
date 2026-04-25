#include "Mind/GameMaster/Actions/MindAction_Challenge.h"

UMindAction_Challenge::UMindAction_Challenge()
{
	ActionName = TEXT("challenge");
	Description = TEXT("质疑上家的声明，输者抽牌并面对左轮");
	ParamSchemaJson = TEXT(R"({})");
}

void UMindAction_Challenge::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T13 实现：Speak（拍桌台词）→ Done(true)；GM.Apply 比对 actual_indices 与 claim
	Done.ExecuteIfBound(true, TEXT("challenge not implemented (T13)"));
}
