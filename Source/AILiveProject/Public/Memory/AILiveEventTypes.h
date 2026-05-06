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

UENUM(BlueprintType)
enum class EAILivePhase : uint8
{
	Setup        UMETA(DisplayName = "setup"),
	DayDiscuss   UMETA(DisplayName = "day_discuss"),
	Vote         UMETA(DisplayName = "vote"),
	NightAction  UMETA(DisplayName = "night_action"),
	Reveal       UMETA(DisplayName = "reveal"),
	GameOver     UMETA(DisplayName = "game_over"),
};

UENUM(BlueprintType)
enum class EAILiveEventType : uint8
{
	SpeechPublic           UMETA(DisplayName = "speech.public"),
	SpeechScratchpad       UMETA(DisplayName = "speech.scratchpad"),
	SpeechIntended         UMETA(DisplayName = "speech.intended"),
	SpeechNote             UMETA(DisplayName = "speech.note"),
	Bid                    UMETA(DisplayName = "bid"),
	Reflection9Q           UMETA(DisplayName = "reflection.9q"),
	Vote                   UMETA(DisplayName = "vote"),
	PrivateMsg             UMETA(DisplayName = "private_msg"),
	AlliancePropose        UMETA(DisplayName = "alliance_propose"),
	AllianceAccept         UMETA(DisplayName = "alliance_accept"),
	AllianceBetray         UMETA(DisplayName = "alliance_betray"),
	ActionIntent           UMETA(DisplayName = "action.intent"),
	ActionResolved         UMETA(DisplayName = "action.resolved"),
	ActionCancelled        UMETA(DisplayName = "action.cancelled"),
	OrchestratorResolved   UMETA(DisplayName = "orchestrator.round_resolved"),
	OrchestratorTickAnchor UMETA(DisplayName = "orchestrator.tick_anchor"),
	OrchestratorTickResolved UMETA(DisplayName = "orchestrator.tick_resolved"),
	OrchestratorTickAudit  UMETA(DisplayName = "orchestrator.tick_audit"),
	SystemRoleAssigned     UMETA(DisplayName = "system.role_assigned"),
	SystemAgentTimeout     UMETA(DisplayName = "system.agent_timeout"),
	SystemParseFailed      UMETA(DisplayName = "system.parse_failed"),
	SystemLLMInflight      UMETA(DisplayName = "system.llm_inflight"),
	SystemDeleteExecuted   UMETA(DisplayName = "system.delete_executed"),
	WinnerDecision         UMETA(DisplayName = "winner_decision"),
};

UENUM(BlueprintType)
enum class EAILiveSpeechActType : uint8
{
	None     UMETA(DisplayName = ""),
	Claim    UMETA(DisplayName = "claim"),
	Accuse   UMETA(DisplayName = "accuse"),
	Defend   UMETA(DisplayName = "defend"),
	Commit   UMETA(DisplayName = "commit"),
	Deny     UMETA(DisplayName = "deny"),
	Question UMETA(DisplayName = "question"),
	Reveal   UMETA(DisplayName = "reveal"),
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
	FString EventId;            // UUIDv7 字符串，可手填（重 append 用）；空则 EventStore 生成

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	FString GameId;             // 这局游戏的 ID，对应 SQLite 文件名 <GameId>.db

	UPROPERTY(BlueprintReadWrite, Category = "AILive|Event")
	int64 Seq = 0;              // 单调递增序号（写后由 EventStore 回填）

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

UENUM(BlueprintType)
enum class EAILiveCommitmentType : uint8
{
	Promise   UMETA(DisplayName = "promise"),
	ClaimRole UMETA(DisplayName = "claim_role"),
	Deny      UMETA(DisplayName = "deny"),
	VoteFor   UMETA(DisplayName = "vote_for"),
	Alliance  UMETA(DisplayName = "alliance"),
};

UENUM(BlueprintType)
enum class EAILiveCommitmentStatus : uint8
{
	Active        UMETA(DisplayName = "active"),
	Retracted     UMETA(DisplayName = "retracted"),
	Contradicted  UMETA(DisplayName = "contradicted"),
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
	AILIVEPROJECT_API EAILivePhase            PhaseFromString(const FString& S);
	AILIVEPROJECT_API EAILiveEventType        EventTypeFromString(const FString& S);
	AILIVEPROJECT_API EAILiveSpeechActType    SpeechActFromString(const FString& S);
	AILIVEPROJECT_API EAILiveCommitmentType   CommitmentTypeFromString(const FString& S);
	AILIVEPROJECT_API EAILiveCommitmentStatus CommitmentStatusFromString(const FString& S);

	// TArray<FString> ↔ JSON 数组字符串（用于把 Visibility/AddressedTo 存进库）
	AILIVEPROJECT_API FString          ArrayToJsonString(const TArray<FString>& A);
	AILIVEPROJECT_API TArray<FString>  JsonStringToArray(const FString& Json);
}
