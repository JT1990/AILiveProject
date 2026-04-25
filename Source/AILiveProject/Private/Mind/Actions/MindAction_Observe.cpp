#include "Mind/Actions/MindAction_Observe.h"

UMindAction_Observe::UMindAction_Observe()
{
	ActionName = TEXT("observe");
	Description = TEXT("强刷一次感知，把最新视野/听觉合并到下次决策上下文");
	ParamSchemaJson = TEXT(R"({})");
}

void UMindAction_Observe::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T19 实现：PerceptionComponent->ForceRebuildPerceptionPriorityList
	Done.ExecuteIfBound(true, TEXT("observe not implemented (T19)"));
}
