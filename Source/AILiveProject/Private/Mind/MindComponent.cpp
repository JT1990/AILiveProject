#include "Mind/MindComponent.h"

UMindComponent::UMindComponent()
{
	// MindComponent 不加 tick——事件驱动（GM 阶段唤醒 / Perception 触发）
	PrimaryComponentTick.bCanEverTick = false;
}

void UMindComponent::Initialize(UMindAgentConfig* /*InConfig*/, AActor* /*InGameMaster*/)
{
	// T06 实现：缓存 Config + GameMaster；NewObject Provider；注册 Speak 等通用 Action
}

void UMindComponent::RequestDecision(FString /*TriggerReason*/)
{
	// T06 实现：节流 + Building/Calling 状态机 + Provider->RequestCompletion
}

void UMindComponent::SetGameActions(const TArray<TSubclassOf<UMindAction>>& /*Actions*/)
{
	// T11 实现：GM 阶段切换时清空 + 重注册游戏专属动作
}

void UMindComponent::RegisterAction(TSubclassOf<UMindAction> /*ActionClass*/)
{
	// T06 实现：NewObject<UMindAction> + 用 ActionName 注册到 ActionRegistry
}

void UMindComponent::DispatchAction(const FMindActionEnvelope& /*Env*/)
{
	// T06 实现：派发到 ActionRegistry；T11 重写为 Validate → Execute → Apply 拆分
}

void UMindComponent::HandleActionDone(bool /*bOk*/, const FString& /*Summary*/)
{
	// T11 实现：GM.Apply + Memory.Write + State=Idle + GM.OnAgentActionFinished
}

FString UMindComponent::GetAgentId() const
{
	// T07 实现完整 fallback；T02 仅返空字符串占位
	return FString();
}

AMindGameMaster* UMindComponent::GetGameMaster() const
{
	return GameMaster.Get();
}

TArray<FString> UMindComponent::GetCurrentlyVisibleAgentIds() const
{
	// T18 实现
	return TArray<FString>();
}

TArray<FString> UMindComponent::GetCurrentlyAudibleAgentIds() const
{
	// T18 实现
	return TArray<FString>();
}

bool UMindComponent::CanSenseActor(AActor* /*Other*/, FName /*SenseTag*/) const
{
	// T18 实现
	return false;
}

void UMindComponent::SetPerceptionCanTriggerDecision(bool bEnabled)
{
	bPerceptionCanTriggerDecision = bEnabled;
}
