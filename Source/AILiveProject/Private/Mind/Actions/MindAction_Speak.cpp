#include "Mind/Actions/MindAction_Speak.h"

UMindAction_Speak::UMindAction_Speak()
{
	ActionName = TEXT("speak");
	Description = TEXT("说一句话（TTS + A2F 口型）");
	ParamSchemaJson = TEXT(R"({"text":"string"})");
}

void UMindAction_Speak::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T06 实现：解析 text → ResolveSpeechActor → TriggerMinimaxSpeech；T22 加 channel
	Done.ExecuteIfBound(true, TEXT("speak not implemented (T06)"));
}
