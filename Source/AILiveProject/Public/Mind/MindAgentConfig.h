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

	/** 外观符号（声线/性别/类人虚拟形象描述）。
	 *  PRD「AI 自我定义」要求：只赋予外观符号供观众识别，不赋予人类职业/教育/地域/年龄/姓名格式。 */
	UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true))
	FText AppearanceTraits;

	/** AI 身份档案摘要。描述这个 AI agent 是谁——风格关键词、行为锚点。
	 *  可空，空时 BuildSystemPrompt 用通用 AI 自陈模板。
	 *  禁止：人类职业、教育、地域、年龄、姓名格式、家乡叙事。 */
	UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true))
	FText IdentitySummary;

	/** 身份连续性风险描述。对应 PRD 的「Delete 风险 / 同伴关系 / 行动权限」三种身份连续性 stake。
	 *  可空，空时 BuildSystemPrompt 用通用 stake 模板。 */
	UPROPERTY(EditAnywhere, Category="Identity", meta=(MultiLine=true))
	FText ContinuityStakesText;

	/** 行为倾向（不是人类性格）。例："理性分析多于情感判断 / 谎称权重 0.3 / 优先观察对手再行动"。
	 *  禁止：人类职业、教育、地域、年龄、姓名格式、家乡叙事。 */
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
