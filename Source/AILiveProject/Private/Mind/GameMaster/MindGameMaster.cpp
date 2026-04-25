#include "Mind/GameMaster/MindGameMaster.h"

void AMindGameMaster::StartGame()
{
	// T11 实现：抓 Participants 上的 MindComponent → Initialize（注入自身 GM 引用）→ OnGameStart hook
}

void AMindGameMaster::EndGame()
{
	// T11 实现
}

FMindAgentView AMindGameMaster::BuildViewFor(AActor* /*Agent*/)
{
	// T12 (LiarsBar) / T21 (MinorityRule) 子类按游戏规则填字段
	return FMindAgentView{};
}

bool AMindGameMaster::Validate(AActor* /*Agent*/, const FMindActionEnvelope& /*Env*/, FString& /*OutError*/) const
{
	// 子类按 phase 严格判定（T12 / T21）
	return false;
}

void AMindGameMaster::Apply(AActor* /*Agent*/, const FMindActionEnvelope& /*Env*/)
{
	// 子类按 phase 修改 state（T12 / T21）
}

void AMindGameMaster::OnAgentActionFinished(AActor* /*Agent*/, const FMindActionEnvelope& /*Env*/, bool /*bExecuteOk*/)
{
	// 子类决定 Phase 切换（T12 / T21）
}

void AMindGameMaster::TransitionToPhase(FName /*NewPhase*/)
{
	// T11 实现：日志 + 广播 OnPhaseChanged + OnEnterPhase hook
}

void AMindGameMaster::AwakeAgent(AActor* /*Agent*/, FString /*Reason*/)
{
	// T11 实现：找 MindComponent，调 RequestDecision
}

void AMindGameMaster::RegisterActionsForAgent(AActor* /*Agent*/, const TArray<TSubclassOf<UMindAction>>& /*Actions*/)
{
	// T11 实现：找 MindComponent，调 SetGameActions
}
