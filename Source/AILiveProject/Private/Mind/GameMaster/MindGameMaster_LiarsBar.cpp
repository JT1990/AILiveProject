#include "Mind/GameMaster/MindGameMaster_LiarsBar.h"

void AMindGameMaster_LiarsBar::StartGame()
{
	// T12 实现：初始化 State.Players + Phase = Deal
	Super::StartGame();
}

FMindAgentView AMindGameMaster_LiarsBar::BuildViewFor(AActor* /*Agent*/)
{
	// T12 实现：private = 自己的 Hand + RouletteChamber；public = CurrentClaim + 桌面状态
	return FMindAgentView{};
}

bool AMindGameMaster_LiarsBar::Validate(AActor* /*Agent*/, const FMindActionEnvelope& /*Env*/, FString& /*OutError*/) const
{
	// T12 实现：按 Phase 严格校验（Claim 阶段只接 play_cards / pass_turn 等）
	return false;
}

void AMindGameMaster_LiarsBar::Apply(AActor* /*Agent*/, const FMindActionEnvelope& /*Env*/)
{
	// T12 实现：写 CurrentClaim / 移除 Hand 中已出牌
}

void AMindGameMaster_LiarsBar::OnAgentActionFinished(AActor* /*Agent*/, const FMindActionEnvelope& /*Env*/, bool /*bExecuteOk*/)
{
	// T12 实现：决定切到下一玩家 / Challenge 阶段 / Roulette 阶段
}
