#include "Mind/Actions/MindAction_LookAt.h"

UMindAction_LookAt::UMindAction_LookAt()
{
	ActionName = TEXT("look_at");
	Description = TEXT("平滑转向另一个 agent（须在视线内）");
	ParamSchemaJson = TEXT(R"({"target_actor_id":"string"})");
}

void UMindAction_LookAt::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T19 实现：CanSenseActor(Sight) 检查 + RInterpTo 平滑转向
	Done.ExecuteIfBound(true, TEXT("look_at not implemented (T19)"));
}
