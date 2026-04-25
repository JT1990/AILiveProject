#include "Mind/MindSpeechHelpers.h"

AActor* UMindSpeechHelpers::ResolveSpeechActor(AActor* MindOwner)
{
	// T02 仅返 owner 兜底；T06 实现真正解析（VisualOverride child actor 路径）
	return MindOwner;
}
