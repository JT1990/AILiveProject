#pragma once

// =============================================================================
// 中文教学：AILiveAgentTypes.h —— AI Agent 的「档案」「身份」「战斗配置」三件套
//
// 这是什么：
//   把一个 NPC 的所有可序列化属性拆成三个 USTRUCT：
//     - FAILiveAgentCore       ：核心档案（agent_id / persona_version / model / status / 时间戳）
//     - FAILiveAgentIdentity   ：身份呈现（外观符号；按 PRD 规则不含人类背景）
//     - FAILiveAgentBattleConfig：战斗配置（faction / role / 私下目标 / bid 偏移）
//
//   FNPCAgentConfig（在 LLM/AILiveAgentRoster.h）持有这三个结构 + 运行时映射字段。
//   分三层是为了让 EventStore / Roster / Director 各取所需而不污染对方关注点。
//
// 关键概念：
//   - USTRUCT(BlueprintType)：纯数据载体，可在蓝图 Make/Break。
//     注意 USTRUCT **不是** UObject，没有 GC 管理；按值拷贝即可。
//   - UENUM(BlueprintType) + UMETA(DisplayName = "...")：枚举暴露给蓝图，
//     DisplayName 决定下拉菜单上显示的字符串（与 .ToString() 字面值不一定相同）
//   - 默认成员初始化 (= 1, = false)：C++11 起的 in-class member initializer。
//     不写默认值的 FString 自动是空串，TArray 自动是空数组
// =============================================================================

#include "CoreMinimal.h"
#include "AILiveAgentTypes.generated.h"

// Agent 生命周期状态（对应数据库 _meta.db.agent_registry.status 字段）
UENUM(BlueprintType)
enum class EAILiveAgentStatus : uint8
{
	Active   UMETA(DisplayName = "active"),    // 正常活跃中
	Deleted  UMETA(DisplayName = "deleted"),   // 跨局已删（PRD T9 跨局 Delete 桥接）
	Archived UMETA(DisplayName = "archived"),  // 归档（保留历史但不参与新局）
};

// 表征声线与形象呈现，非人类二元生理性别。对齐 PRD：AI 是 AI，不扮演人类。
// 中文教学：注意 PRD 强约束——所有 NPC prompt 严禁注入「人类背景」(职业/学历/家乡)，
// 只能给「外观符号」(声线、虚拟形象)。这个枚举是这条规则在数据层的体现。
UENUM(BlueprintType)
enum class EAILiveVoicePresentation : uint8
{
	Masculine    UMETA(DisplayName = "masculine"),
	Feminine     UMETA(DisplayName = "feminine"),
	Androgynous  UMETA(DisplayName = "androgynous"),
	Synthetic    UMETA(DisplayName = "synthetic"),
	Custom       UMETA(DisplayName = "custom"),
};

// 核心档案（每个 agent 一份，写入 _meta.db.agent_registry 表）
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveAgentCore
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	FString AgentId;                                      // 全局唯一 ID，例如 "NPC01"

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	int32 PersonaVersion = 1;                             // persona 版本号；改 prompt 要 bump

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	FString ModelProvider;                                // 厂商，"deepseek"/"glm"/"qwen3"

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	FString ModelName;                                    // 具体模型名，"deepseek-chat" 等

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	EAILiveAgentStatus Status = EAILiveAgentStatus::Active;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	FString CreatedAt;                                    // ISO8601 时间戳，BeginGame 时填

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Core")
	FString DeletedAt;                                    // 跨局 Delete 时填；空表示未删
};

// 身份呈现（外观符号；按 PRD 规则不含人类背景叙事）
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveAgentIdentity
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
	FString FullName;                                     // 全名，如 "NPC-01"

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
	FString Nickname;                                     // 昵称，可空

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
	EAILiveVoicePresentation VoicePresentation = EAILiveVoicePresentation::Synthetic;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
	FString Voice;                                        // MiniMax TTS voice ID

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Identity")
	FString Appearance;                                   // 视觉描述（仅 prompt / UI 用）
};

// 战斗配置（局内行为参数；setup phase 由 Director 决定）
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveAgentBattleConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	FString Faction;                                      // 阵营，如 "good" / "evil"

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	FString Role;                                         // 角色，如 "villager" / "wolf"

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	TArray<FString> AllianceMembers;                      // 盟友 agent_id 列表

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	FString PrivateGoal;                                  // 私下目标（仅自己可见；Director 注入 prompt）

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	float BidOffset = 0.f;                                // bid 静态偏移（角色自带的发言欲倾向）

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	int64 SeqStart = 0;                                   // 该 agent 在 EventStore 中的起始 seq

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILive|Agent|Battle")
	bool bAlive = true;                                   // 是否还在局中
};
