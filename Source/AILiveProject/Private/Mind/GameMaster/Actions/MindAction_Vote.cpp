#include "Mind/GameMaster/Actions/MindAction_Vote.h"

UMindAction_Vote::UMindAction_Vote()
{
	ActionName = TEXT("vote");
	Description = TEXT("走到投票箱前投出秘密一票");
	ParamSchemaJson = TEXT(R"({"choice":"yes|no","reasoning":"string?"})");
}

void UMindAction_Vote::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T22 实现：EQS_FindNearestVoteBox(choice) → ApproachAndUse → Speak 掩护 → Done(true)
	Done.ExecuteIfBound(true, TEXT("vote not implemented (T22)"));
}
