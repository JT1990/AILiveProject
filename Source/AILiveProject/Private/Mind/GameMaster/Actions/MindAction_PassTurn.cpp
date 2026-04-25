#include "Mind/GameMaster/Actions/MindAction_PassTurn.h"

UMindAction_PassTurn::UMindAction_PassTurn()
{
	ActionName = TEXT("pass_turn");
	Description = TEXT("跳过本回合不出牌（仅特定阶段合法）");
	ParamSchemaJson = TEXT(R"({})");
}

void UMindAction_PassTurn::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T13 实现：Done(true)；GM.Apply 切下一玩家
	Done.ExecuteIfBound(true, TEXT("pass_turn not implemented (T13)"));
}
