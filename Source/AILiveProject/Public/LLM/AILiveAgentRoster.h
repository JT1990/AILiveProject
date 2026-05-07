#pragma once

// =============================================================================
// 中文教学：AILiveAgentRoster.h —— NPC 名册 + LLM provider 解析
//
// 这是什么：
//   - 定义 ELLMProvider 枚举（DeepSeek / GLM / Qwen3）
//   - 定义 FNPCAgentConfig 结构（一个 NPC 的完整配置：核心档案 + 身份 + 战斗 +
//     运行时映射 actor 名等）
//   - 提供 GetDefaultRoster() 返回 10 个 NPC 的硬编码默认配置
//   - 提供 ResolveProviderEndpoint() 把 provider 枚举映射到具体 ApiKey/URL/Model
//
// 关键 UE 概念：
//   1) UENUM(BlueprintType)
//      把 C++ enum 暴露给蓝图。`uint8` 底层类型是 BlueprintType 的硬性要求。
//
//   2) USTRUCT(BlueprintType)
//      把 C++ 结构体暴露给蓝图。蓝图里可以以「Make / Break」节点直接构造和拆分。
//
//   3) UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "...")
//      `EditAnywhere`     ：编辑器属性面板任何地方都可改（蓝图默认值 + 实例上）
//      `BlueprintReadWrite`：蓝图既能读也能写
//      `Category = "X|Y"` ：蓝图节点 / 编辑面板里的归类（用 | 分层）
//
//   4) UMETA(DisplayName = "...")
//      仅作用于 enum 单值，让编辑器下拉里显示成更友好的名字。
//
//   5) GENERATED_BODY()
//      由 UnrealHeaderTool（UHT，UE 反射代码生成器）展开成大量样板代码。
//      每个 UCLASS / USTRUCT 都必须放一行，放在第一行成员之前。
// =============================================================================

#include "CoreMinimal.h"
#include "Memory/AILiveAgentTypes.h"        // FAILiveAgentCore / FAILiveAgentIdentity / FAILiveAgentBattleConfig
#include "AILiveAgentRoster.generated.h"    // ⚠️ 必须最后一个 include；UHT 生成的反射代码

// LLM 厂商三选一。底层 uint8 是 BlueprintType 必需。
/** NPC 使用的 LLM 厂商；Director 会据此解析 API key、endpoint 和模型名。 */
UENUM(BlueprintType)
enum class ELLMProvider : uint8
{
	/** DeepSeek 模型厂商；用于把 NPC 的推理请求路由到 DeepSeek endpoint。 */
	DeepSeek UMETA(DisplayName = "DeepSeek", ToolTip = "DeepSeek 模型厂商；用于把 NPC 的推理请求路由到 DeepSeek endpoint。"),
	/** GLM / 智谱模型厂商；用于把 NPC 的推理请求路由到 GLM endpoint。 */
	GLM      UMETA(DisplayName = "GLM", ToolTip = "GLM / 智谱模型厂商；用于把 NPC 的推理请求路由到 GLM endpoint。"),
	/** Qwen3 / 通义千问模型厂商；用于把 NPC 的推理请求路由到 Qwen endpoint。 */
	Qwen3    UMETA(DisplayName = "Qwen3", ToolTip = "Qwen3 / 通义千问模型厂商；用于把 NPC 的推理请求路由到 Qwen endpoint。"),
};

// 一个 NPC 的完整配置。BlueprintType 让蓝图能 Make / Break。
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FNPCAgentConfig
{
	GENERATED_BODY()

	// 核心档案（agent_id / persona_version / model_provider 等），见 AILiveAgentTypes.h
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
	FAILiveAgentCore Core;

	// 身份（FullName / Nickname / Voice 等）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
	FAILiveAgentIdentity Identity;

	// 战斗配置（Faction / Role / Goal / BidOffset 等），由 Director 在 setup phase 决定
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster")
	FAILiveAgentBattleConfig Battle;

	// 1..10，与场景里 BP_NPC_MH_Character_<N> 实例对应
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
	int32 NPCIndex = 1;

	// 关卡里 actor 的 label（用于按名查找）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
	FName NPCActorLabel;

	// 声线性别提示（"male" / "female"），仅作元信息使用
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
	FString VoicePresentationHint;

	// 用哪个 LLM provider 推理这个 NPC
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Roster|Runtime")
	ELLMProvider Provider = ELLMProvider::DeepSeek;
};

// 命名空间组织三个工具函数 + 一个 POD 结构（不进反射）
namespace AILiveAgentRoster
{
	// 返回硬编码的 10 个 NPC 默认配置：4 DeepSeek + 3 GLM + 3 Qwen3
	AILIVEPROJECT_API TArray<FNPCAgentConfig> GetDefaultRoster();

	// 枚举 → 小写字符串（用于日志、库字段、provider id）
	AILIVEPROJECT_API FString ProviderToString(ELLMProvider Provider);

	// LLM endpoint 解析结果。纯 C++ struct，不进反射。
	struct FProviderEndpoint
	{
		FString ApiKey;     // 来自 .env 的密钥
		FString Endpoint;   // 完整 URL，自动补全 /chat/completions 后缀
		FString Model;      // 模型名
	};

	// 把 provider 枚举映射到具体 ApiKey/URL/Model（从 .env 读）
	AILIVEPROJECT_API FProviderEndpoint ResolveProviderEndpoint(ELLMProvider Provider);
}
