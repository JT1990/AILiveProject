#include "Mind/GameMaster/MindGameMaster_MinorityRule.h"

void AMindGameMaster_MinorityRule::StartGame()
{
	// T21 实现：初始化 State.Players（8 人，alive=true，diamonds=1）+ Phase = Setup
	Super::StartGame();
}

FMindAgentView AMindGameMaster_MinorityRule::BuildViewFor(AActor* /*Agent*/)
{
	// T21 实现：private = 自己的当前投票意向 + 自我认定盟友；public = CurrentQuestion + alive 列表 + 历史
	return FMindAgentView{};
}

bool AMindGameMaster_MinorityRule::Validate(AActor* /*Agent*/, const FMindActionEnvelope& /*Env*/, FString& /*OutError*/) const
{
	// T21 实现：按 Phase 严格校验（AskQuestion / Negotiate / Vote 阶段允许的动作不同）
	return false;
}

void AMindGameMaster_MinorityRule::Apply(AActor* /*Agent*/, const FMindActionEnvelope& /*Env*/)
{
	// T21 + T22 实现：写 CurrentQuestion / CurrentVote / 维护 Alliance 双向关系
}

void AMindGameMaster_MinorityRule::OnAgentActionFinished(AActor* /*Agent*/, const FMindActionEnvelope& /*Env*/, bool /*bExecuteOk*/)
{
	// T21 实现：决定切到 Negotiate / Vote / Tally
}
