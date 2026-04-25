#include "Mind/GameMaster/Actions/MindAction_AskQuestion.h"

UMindAction_AskQuestion::UMindAction_AskQuestion()
{
	ActionName = TEXT("ask_question");
	Description = TEXT("向桌面提一个二元（yes/no）问题，启动一轮投票");
	ParamSchemaJson = TEXT(R"({"question_text":"string"})");
}

void UMindAction_AskQuestion::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T22 实现：Speak 念出问题 → Done(true)；GM.Apply 写 CurrentQuestion + 广播 OnGameEvent
	Done.ExecuteIfBound(true, TEXT("ask_question not implemented (T22)"));
}
