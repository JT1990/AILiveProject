#include "Mind/GameMaster/Actions/MindAction_PlayCards.h"

UMindAction_PlayCards::UMindAction_PlayCards()
{
	ActionName = TEXT("play_cards");
	Description = TEXT("打出 1-3 张牌（背面），公开声明 rank+count");
	ParamSchemaJson = TEXT(R"({"claim_rank":"string","claim_count":"int","actual_indices":"int[]"})");
}

void UMindAction_PlayCards::Execute(UMindComponent* /*Owner*/, const FString& /*ParamsJson*/, FOnActionDone Done)
{
	// T13 实现：解析 claim → Speak → Done(true)；GM.Apply 移 Hand 写 CurrentClaim
	Done.ExecuteIfBound(true, TEXT("play_cards not implemented (T13)"));
}
