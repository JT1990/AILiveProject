#include "Mind/Actions/MindAction_MoveTo.h"

UMindAction_MoveTo::UMindAction_MoveTo()
{
	ActionName = TEXT("move_to");
	Description = TEXT("走到另一个 agent 或命名地点");
	ParamSchemaJson = TEXT(R"({"target_actor_id":"string?","named_location":"string?"})");
}

void UMindAction_MoveTo::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T19 实现：EQS_FindFacingPoint → AIController::MoveTo
	Done.ExecuteIfBound(true, TEXT("move_to not implemented (T19)"));
}
