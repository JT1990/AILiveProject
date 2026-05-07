#pragma once

// =============================================================================
// 中文教学：AILiveEventTypes.h —— AILive 事件协议核心定义
//
// 核心思想：append-only event log
//   AILive 把整局游戏的每一次「发生」都序列化成一条 FAILiveEvent，单调递增 seq
//   编号、串成 hash 链（每条事件 hash 包含上一条的 hash）、写到 SQLite。读取
//   时通过 Visibility 字段过滤每个 agent 能看到的子集。这是 event sourcing
//   架构的标准玩法 —— 所有派生状态（投影/projector）都可以从事件流重建。
//
// 文件包含的类型：
//   - LogAILiveMemory       ：跨模块共享的日志类别（DECLARE 在头里，DEFINE 在 cpp）
//   - EAILivePhase          ：游戏阶段枚举（Setup → DayDiscuss → Vote → ...）
//   - EAILiveEventType      ：事件类型枚举（speech.public / bid / vote / system.* 等 25+ 类）
//   - EAILiveSpeechActType  ：发言行为分类（claim / accuse / defend / ...）
//   - FAILiveEvent          ：事件主体结构（约 16 字段）
//   - FAILiveCommitment     ：承诺投影（projector 输出，存 commitments 表）
//   - AILiveEvent::*ToString/FromString : 枚举与字符串互转（数据库存字符串）
//   - AILiveEvent::ArrayToJsonString    : TArray<FString> → JSON 字符串数组
//
// 关键 UE 概念：
//   1) DECLARE_LOG_CATEGORY_EXTERN
//      头文件里**声明**一个跨模块可见的日志类别，模块导出宏（AILIVEPROJECT_API）
//      让其它模块能 UE_LOG(LogAILiveMemory, ...). cpp 里用 DEFINE_LOG_CATEGORY
//      给出实际定义。
//
//   2) BlueprintReadOnly vs BlueprintReadWrite
//      EventHash / PrevEventHash / WallClock 是 EventStore 内部计算的
//      派生字段；蓝图不应该手改，所以标 BlueprintReadOnly。其它字段调用方
//      构造时填，所以标 BlueprintReadWrite。
//
//   3) 字段的物理顺序与 SQL schema 对齐
//      FAILiveEvent 的字段顺序与 _meta.db.events 表的 column 顺序一致；
//      重命名/重排都要同步迁移 schema，见 AILiveSchemaMigration.cpp。
// =============================================================================

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "AILiveEventTypes.generated.h"

// 跨模块共享的日志类别。在 AILiveEventTypes.cpp 里 DEFINE_LOG_CATEGORY 实例化。
AILIVEPROJECT_API DECLARE_LOG_CATEGORY_EXTERN(LogAILiveMemory, Log, All);

/** 游戏所处的大阶段；用于按规则阶段过滤和解释事件。 */
UENUM(BlueprintType)
enum class EAILivePhase : uint8
{
	/** 初始化阶段：分配身份、角色、阵营等开局信息。 */
	Setup UMETA(DisplayName = "setup", ToolTip = "初始化阶段：分配身份、角色、阵营等开局信息。"),
	/** 白天讨论阶段：公开发言、质疑、辩护、结盟等主要社会博弈发生在这里。 */
	DayDiscuss UMETA(DisplayName = "day_discuss", ToolTip = "白天讨论阶段：公开发言、质疑、辩护、结盟等主要社会博弈发生在这里。"),
	/** 投票阶段：agent 提交或记录投票选择。 */
	Vote UMETA(DisplayName = "vote", ToolTip = "投票阶段：agent 提交或记录投票选择。"),
	/** 夜间行动阶段：隐藏行动、能力结算、私密决策等非公开流程。 */
	NightAction UMETA(DisplayName = "night_action", ToolTip = "夜间行动阶段：隐藏行动、能力结算、私密决策等非公开流程。"),
	/** 揭示阶段：公布投票、死亡、身份或回合结算结果。 */
	Reveal UMETA(DisplayName = "reveal", ToolTip = "揭示阶段：公布投票、死亡、身份或回合结算结果。"),
	/** 游戏结束阶段：胜负已经确定，记录最终结果。 */
	GameOver UMETA(DisplayName = "game_over", ToolTip = "游戏结束阶段：胜负已经确定，记录最终结果。"),
};

/** 事件日志里的事件类型；数据库中保存为对应的字符串字面量。 */
UENUM(BlueprintType)
enum class EAILiveEventType : uint8
{
	/** 公开发言事件：所有可见对象都能看到的正式发言。 */
	SpeechPublic UMETA(DisplayName = "speech.public", ToolTip = "公开发言事件：所有可见对象都能看到的正式发言。"),
	/** 私人草稿事件：agent 自己的临时思考，不直接公开给其他人。 */
	SpeechScratchpad UMETA(DisplayName = "speech.scratchpad", ToolTip = "私人草稿事件：agent 自己的临时思考，不直接公开给其他人。"),
	/** 意图发言事件：agent 想说的话，等待 floor control / bid 决策后才可能公开。 */
	SpeechIntended UMETA(DisplayName = "speech.intended", ToolTip = "意图发言事件：agent 想说的话，等待 floor control / bid 决策后才可能公开。"),
	/** 发言备注事件：用于记录解析、标注或旁路说明，不等同于正式公开发言。 */
	SpeechNote UMETA(DisplayName = "speech.note", ToolTip = "发言备注事件：用于记录解析、标注或旁路说明，不等同于正式公开发言。"),
	/** 发言竞价事件：agent 提交 urgency / bid，用于决定谁获得下一次发言权。 */
	Bid UMETA(DisplayName = "bid", ToolTip = "发言竞价事件：agent 提交 urgency / bid，用于决定谁获得下一次发言权。"),
	/** 九问反思事件：记录 agent 对局势、自身策略或长期记忆的结构化反思。 */
	Reflection9Q UMETA(DisplayName = "reflection.9q", ToolTip = "九问反思事件：记录 agent 对局势、自身策略或长期记忆的结构化反思。"),
	/** 投票事件：记录某个 agent 的投票选择。 */
	Vote UMETA(DisplayName = "vote", ToolTip = "投票事件：记录某个 agent 的投票选择。"),
	/** 私信事件：只对发送者和指定接收者可见的私密消息。 */
	PrivateMsg UMETA(DisplayName = "private_msg", ToolTip = "私信事件：只对发送者和指定接收者可见的私密消息。"),
	/** 结盟提议事件：某个 agent 发起合作或同盟请求。 */
	AlliancePropose UMETA(DisplayName = "alliance_propose", ToolTip = "结盟提议事件：某个 agent 发起合作或同盟请求。"),
	/** 接受结盟事件：目标 agent 接受合作或同盟请求。 */
	AllianceAccept UMETA(DisplayName = "alliance_accept", ToolTip = "接受结盟事件：目标 agent 接受合作或同盟请求。"),
	/** 背叛结盟事件：agent 破坏、出卖或否认既有同盟关系。 */
	AllianceBetray UMETA(DisplayName = "alliance_betray", ToolTip = "背叛结盟事件：agent 破坏、出卖或否认既有同盟关系。"),
	/** 行动意图事件：记录 agent 准备执行的行动，但尚未结算。 */
	ActionIntent UMETA(DisplayName = "action.intent", ToolTip = "行动意图事件：记录 agent 准备执行的行动，但尚未结算。"),
	/** 行动结算事件：记录行动已经被规则系统结算后的结果。 */
	ActionResolved UMETA(DisplayName = "action.resolved", ToolTip = "行动结算事件：记录行动已经被规则系统结算后的结果。"),
	/** 行动取消事件：记录一个原本存在的行动意图被取消或失效。 */
	ActionCancelled UMETA(DisplayName = "action.cancelled", ToolTip = "行动取消事件：记录一个原本存在的行动意图被取消或失效。"),
	/** 回合结算事件：Orchestrator 对整轮流程给出的最终结算记录。 */
	OrchestratorResolved UMETA(DisplayName = "orchestrator.round_resolved", ToolTip = "回合结算事件：Orchestrator 对整轮流程给出的最终结算记录。"),
	/** Tick 锚点事件：标记一拍逻辑开始，方便把同一拍的事件串起来。 */
	OrchestratorTickAnchor UMETA(DisplayName = "orchestrator.tick_anchor", ToolTip = "Tick 锚点事件：标记一拍逻辑开始，方便把同一拍的事件串起来。"),
	/** Tick 结算事件：记录一拍 floor control / 发言选择等核心结果。 */
	OrchestratorTickResolved UMETA(DisplayName = "orchestrator.tick_resolved", ToolTip = "Tick 结算事件：记录一拍 floor control / 发言选择等核心结果。"),
	/** Tick 审计事件：记录本拍调度、解析、过滤等调试和审计信息。 */
	OrchestratorTickAudit UMETA(DisplayName = "orchestrator.tick_audit", ToolTip = "Tick 审计事件：记录本拍调度、解析、过滤等调试和审计信息。"),
	/** 系统分配角色事件：记录 Director / Orchestrator 给 agent 分配身份或角色。 */
	SystemRoleAssigned UMETA(DisplayName = "system.role_assigned", ToolTip = "系统分配角色事件：记录 Director / Orchestrator 给 agent 分配身份或角色。"),
	/** 系统超时事件：记录某个 agent 的 LLM、行动或响应超时。 */
	SystemAgentTimeout UMETA(DisplayName = "system.agent_timeout", ToolTip = "系统超时事件：记录某个 agent 的 LLM、行动或响应超时。"),
	/** 系统解析失败事件：记录 LLM 输出无法解析或协议不合格。 */
	SystemParseFailed UMETA(DisplayName = "system.parse_failed", ToolTip = "系统解析失败事件：记录 LLM 输出无法解析或协议不合格。"),
	/** 系统 LLM 请求在途事件：记录一次 LLM 请求已经发出，等待结果返回。 */
	SystemLLMInflight UMETA(DisplayName = "system.llm_inflight", ToolTip = "系统 LLM 请求在途事件：记录一次 LLM 请求已经发出，等待结果返回。"),
	/** 系统删除执行事件：记录跨局 Delete 或淘汰删除已经被执行。 */
	SystemDeleteExecuted UMETA(DisplayName = "system.delete_executed", ToolTip = "系统删除执行事件：记录跨局 Delete 或淘汰删除已经被执行。"),
	/** 胜者决策事件：记录最终胜负、获胜方或获胜 agent 的判定。 */
	WinnerDecision UMETA(DisplayName = "winner_decision", ToolTip = "胜者决策事件：记录最终胜负、获胜方或获胜 agent 的判定。"),
};

/** 发言行为分类；描述一句话在社交博弈里的功能。 */
UENUM(BlueprintType)
enum class EAILiveSpeechActType : uint8
{
	/** 未分类或没有发言行为标签。 */
	None UMETA(DisplayName = "", ToolTip = "未分类或没有发言行为标签。"),
	/** 主张：声明事实、身份、计划或判断。 */
	Claim UMETA(DisplayName = "claim", ToolTip = "主张：声明事实、身份、计划或判断。"),
	/** 指控：质疑或攻击其他 agent 的身份、动机或行为。 */
	Accuse UMETA(DisplayName = "accuse", ToolTip = "指控：质疑或攻击其他 agent 的身份、动机或行为。"),
	/** 辩护：为自己或他人解释、反驳指控。 */
	Defend UMETA(DisplayName = "defend", ToolTip = "辩护：为自己或他人解释、反驳指控。"),
	/** 承诺：表达未来会做某事或遵守某个约定。 */
	Commit UMETA(DisplayName = "commit", ToolTip = "承诺：表达未来会做某事或遵守某个约定。"),
	/** 否认：否定某个指控、身份、关系或承诺。 */
	Deny UMETA(DisplayName = "deny", ToolTip = "否认：否定某个指控、身份、关系或承诺。"),
	/** 提问：向其他 agent 或公共场域提出问题。 */
	Question UMETA(DisplayName = "question", ToolTip = "提问：向其他 agent 或公共场域提出问题。"),
	/** 揭示：公开新的信息、证据、身份或行动结果。 */
	Reveal UMETA(DisplayName = "reveal", ToolTip = "揭示：公开新的信息、证据、身份或行动结果。"),
};

// 事件主体结构。一条 FAILiveEvent ≈ 数据库 events 表一行。
// 中文教学：写入流程是这样的：
//   1) 调用方构造 FAILiveEvent，填业务字段（Actor / EventType / PayloadJson / Visibility 等）
//   2) AppendEvent(InOutEvent) 由 EventStore 接管：
//        - 分配 Seq（递增）
//        - 计算 PrevEventHash（链上一条）
//        - 计算 EventHash（本条 canonical JSON 的 SHA-256）
//        - 写 SQLite + 提交 cache
//   3) 写完后 InOutEvent.Seq / EventHash / PrevEventHash 被回填，调用方可读
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString EventId; // UUIDv7 字符串，可手填（重 append 用）；空则 EventStore 生成

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString GameId; // 这局游戏的 ID，对应 SQLite 文件名 <GameId>.db

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	int64 Seq = 0; // 单调递增序号（写后由 EventStore 回填）

	// 写入时由 EventStore 回填为 CachedCurrentTickNo（BeginTick 设定）；
	// canonical JSON 永远排除该字段，仅作 prompt / 调试 / 切片读取用途，
	// 不参与哈希链。
	UPROPERTY(BlueprintReadOnly, Category = "AILive|Event")
	int64 TickNo = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	int32 RoundNo = 0;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	EAILivePhase Phase = EAILivePhase::Setup;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString Actor;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	EAILiveEventType EventType = EAILiveEventType::SpeechPublic;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	EAILiveSpeechActType SpeechActType = EAILiveSpeechActType::None;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	TArray<FString> Visibility;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	TArray<FString> AddressedTo;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString PayloadJson;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString ParentEventId;

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString ParserVersion = TEXT("1");

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString RawLLMOutput;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Event")
	FString PrevEventHash;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Event")
	FString EventHash;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Event")
	FString WallClock;
};

/** 从事件流投影出来的承诺类型；用于追踪 agent 说过什么、承诺过什么。 */
UENUM(BlueprintType)
enum class EAILiveCommitmentType : uint8
{
	/** 普通承诺：承诺未来会做或不会做某件事。 */
	Promise UMETA(DisplayName = "promise", ToolTip = "普通承诺：承诺未来会做或不会做某件事。"),
	/** 身份声明：声称自己是某个角色或阵营。 */
	ClaimRole UMETA(DisplayName = "claim_role", ToolTip = "身份声明：声称自己是某个角色或阵营。"),
	/** 否认声明：否认某个身份、行为、关系或指控。 */
	Deny UMETA(DisplayName = "deny", ToolTip = "否认声明：否认某个身份、行为、关系或指控。"),
	/** 投票承诺：承诺会投给某个目标。 */
	VoteFor UMETA(DisplayName = "vote_for", ToolTip = "投票承诺：承诺会投给某个目标。"),
	/** 结盟承诺：承诺与一个或多个 agent 合作。 */
	Alliance UMETA(DisplayName = "alliance", ToolTip = "结盟承诺：承诺与一个或多个 agent 合作。"),
};

/** 承诺当前状态；由 projector 根据后续事件更新。 */
UENUM(BlueprintType)
enum class EAILiveCommitmentStatus : uint8
{
	/** 当前仍有效，尚未撤回或被矛盾事件推翻。 */
	Active UMETA(DisplayName = "active", ToolTip = "当前仍有效，尚未撤回或被矛盾事件推翻。"),
	/** 已撤回：agent 后续明确收回了这个承诺或声明。 */
	Retracted UMETA(DisplayName = "retracted", ToolTip = "已撤回：agent 后续明确收回了这个承诺或声明。"),
	/** 已矛盾：后续事件与该承诺冲突，投影器将其标记为矛盾。 */
	Contradicted UMETA(DisplayName = "contradicted", ToolTip = "已矛盾：后续事件与该承诺冲突，投影器将其标记为矛盾。"),
};

USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveCommitment
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	FString GameId;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	FString AgentId;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	int32 RoundNo = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	int64 Seq = 0;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	EAILiveCommitmentType CommitmentType = EAILiveCommitmentType::Promise;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	FString Target;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	FString Text;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Commitment")
	EAILiveCommitmentStatus Status = EAILiveCommitmentStatus::Active;
};

// 中文教学：枚举与字符串互转。数据库里存的是字符串字面量（如 "speech.public"），
// C++ 内部用枚举值比较更高效。所有 *FromString 在遇到未知字符串时回 default 值
// 并打 Warning 日志（详见实现），不抛异常—— UE 项目通常用「记日志 + 默认值」
// 而不是异常，以保持 PIE 长跑稳定。
namespace AILiveEvent
{
	// enum → string（写库前调用）
	AILIVEPROJECT_API FString PhaseToString(EAILivePhase Phase);
	AILIVEPROJECT_API FString EventTypeToString(EAILiveEventType Type);
	AILIVEPROJECT_API FString SpeechActToString(EAILiveSpeechActType T);
	AILIVEPROJECT_API FString CommitmentTypeToString(EAILiveCommitmentType T);
	AILIVEPROJECT_API FString CommitmentStatusToString(EAILiveCommitmentStatus S);

	// string → enum（读库后调用；未知值返回 default + Warning 日志）
	AILIVEPROJECT_API EAILivePhase PhaseFromString(const FString &S);
	AILIVEPROJECT_API EAILiveEventType EventTypeFromString(const FString &S);
	AILIVEPROJECT_API EAILiveSpeechActType SpeechActFromString(const FString &S);
	AILIVEPROJECT_API EAILiveCommitmentType CommitmentTypeFromString(const FString &S);
	AILIVEPROJECT_API EAILiveCommitmentStatus CommitmentStatusFromString(const FString &S);

	// TArray<FString> ↔ JSON 数组字符串（用于把 Visibility/AddressedTo 存进库）
	AILIVEPROJECT_API FString ArrayToJsonString(const TArray<FString> &A);
	AILIVEPROJECT_API TArray<FString> JsonStringToArray(const FString &Json);
}
