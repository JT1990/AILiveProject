#include "Mind/GameMaster/Actions/MindAction_AcceptAlliance.h"

UMindAction_AcceptAlliance::UMindAction_AcceptAlliance()
{
	ActionName = TEXT("accept_alliance");
	Description = TEXT("接受另一个 agent 提出的结盟邀请");
	ParamSchemaJson = TEXT(R"({"from_agent_id":"string"})");
}

void UMindAction_AcceptAlliance::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T22 实现：Speak(channel=private:from) → Done(true)；GM.Apply 双向标盟友 + 写双方记忆
	Done.ExecuteIfBound(true, TEXT("accept_alliance not implemented (T22)"));
}
