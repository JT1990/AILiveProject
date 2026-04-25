#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Templates/SubclassOf.h"
#include "Mind/MindMockResponseTable.h"
#include "MindAgentConfig.generated.h"

class UMindLLMProvider;

/**
 * 单个 NPC 的 Mind 决策层配置。设计成 UPrimaryDataAsset，便于在 Content Browser 集中管理。
 *
 * AgentIdStable 是跨关卡 / 跨局稳定的 agent ID（P0-C 强约束）。
 * LLM 相关字段（ModelId / ApiBaseEnvName / ApiKeyEnvName）在 Initialize 时拷给 Provider 子类
 * 同名字段；若 Provider 子类自身已设值则以 Provider 实例字段为准（T06 实现细节）。
 */
UCLASS(BlueprintType)
class AILIVEPROJECT_API UMindAgentConfig : public UPrimaryDataAsset
{
	GENERATED_BODY()
public:
	/** 用户手填稳定 ID（如 "npc_1"），跨关卡 / PIE 重启不变；不能用 GetName()（_C_0 后缀坑）。 */
	UPROPERTY(EditAnywhere, Category="Identity")
	FString AgentIdStable;

	UPROPERTY(EditAnywhere, Category="Identity")
	FText DisplayName;

	UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true))
	FText Persona;

	UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true))
	FText Goals;

	/** LLM Provider 子类（DeepSeek / GLM / Mock）。EditInlineNew + Abstract 父类，子类必须 non-Abstract 才能在下拉中显示。 */
	UPROPERTY(EditAnywhere, Category="LLM")
	TSubclassOf<UMindLLMProvider> ProviderClass;

	UPROPERTY(EditAnywhere, Category="LLM")
	FString ModelId;

	/** 例 "DEEPSEEK_API_BASE"——通过 UMinimaxACELibrary::GetEnvValueFromProjectEnv 解析。 */
	UPROPERTY(EditAnywhere, Category="LLM")
	FString ApiBaseEnvName;

	UPROPERTY(EditAnywhere, Category="LLM")
	FString ApiKeyEnvName;

	UPROPERTY(EditAnywhere, Category="Speech")
	FString MinimaxVoiceId;

	UPROPERTY(EditAnywhere, Category="Speech")
	FName A2FProviderName = FName(TEXT("LocalA2F-James"));

	UPROPERTY(EditAnywhere, Category="Decision")
	float DecisionCooldownSeconds = 2.0f;

	UPROPERTY(EditAnywhere, Category="Memory")
	int32 MemoryRecallTopK = 5;

	/** 仅 Mock provider 用（T05.5）。完整类型 #include 而非前向声明——前向声明在 reflection 系统中不能编辑器绑定。 */
	UPROPERTY(EditAnywhere, Category="LLM")
	TObjectPtr<UMindMockResponseTable> MockResponseTable;
};
