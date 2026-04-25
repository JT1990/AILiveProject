#include "Mind/Actions/MindAction_Think.h"

UMindAction_Think::UMindAction_Think()
{
	ActionName = TEXT("think");
	Description = TEXT("默想，写入私有记忆，不发声不动作");
	ParamSchemaJson = TEXT(R"({"thought":"string"})");
}

void UMindAction_Think::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T19 实现：Memory.Write 带 tag={"type":"thought","channel":"private"}
	Done.ExecuteIfBound(true, TEXT("think not implemented (T19)"));
}
