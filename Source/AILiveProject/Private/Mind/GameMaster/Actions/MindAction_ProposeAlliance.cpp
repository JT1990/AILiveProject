#include "Mind/GameMaster/Actions/MindAction_ProposeAlliance.h"

UMindAction_ProposeAlliance::UMindAction_ProposeAlliance()
{
	ActionName = TEXT("propose_alliance");
	Description = TEXT("私下向另一个 agent 提出结盟邀请");
	ParamSchemaJson = TEXT(R"({"target_agent_id":"string","terms":"string?"})");
}

void UMindAction_ProposeAlliance::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T22 实现：Speak(channel=private:target) → Done(true)；GM.Apply 入"待接受"队列
	Done.ExecuteIfBound(true, TEXT("propose_alliance not implemented (T22)"));
}
