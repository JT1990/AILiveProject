#pragma once

// =============================================================================
// 中文教学：AILiveBidTypes.h —— 「抢麦」竞价协议数据结构
//
// AILive 的「Bid（抢麦）」机制：
//   每一拍（tick），所有还想发言的 NPC 给出一个 urgency 分（0~10）+ rationale。
//   Director 按 final_score = urgency + bid_offset + runtime_adj 排序，挑分数最
//   高者获 floor（说话权），其它人发言被「截留」成 speech.intended（仅自己可见）。
//
//   这是「会议主持人发言权调度」的算法化 —— 让 LLM agent 之间可以并行思考，
//   但只有一人公开发言；并提供「未发言意图」的可观察记录用于后续 prompt。
//
// 两个结构：
//   - FAILiveBid             ：单个 NPC 在一拍中的 bid 提交
//   - FAILiveTickResolution  ：Director 整理出的本拍最终结果（赢家 + 全部 bids）
//
// 关键概念：
//   - 全部字段 BlueprintReadOnly：bid 在 EventStore 投影后是「事实快照」，
//     不应该让蓝图改写它影响后续判定
//   - principles §5.4 反霸麦：runtime_adj 包含「连续发言衰减」+「被 @ 加权」
//     + 「沉默累积加权」，是动态调整项
//   - 术语对齐：本文件「抢麦」= 项目代码 / Act02RuleReceiveDirector 注释中
//     的「floor」/「floor control」（发言权调度）—— 同一概念三种叫法
// =============================================================================

#include "CoreMinimal.h"
#include "AILiveBidTypes.generated.h"

// 单个 NPC 在一拍 (tick) 中提交的 bid（抢麦请求）。
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveBid
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	FString Actor;                       // 发起 bid 的 agent_id

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	int64 Seq = 0;                       // 本 bid 事件本身的 seq（Bid 也是一种 event）

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	int64 IntendedSeq = 0;               // 关联的 speech.intended 事件 seq（赢则被 promote 成 public）

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	float Urgency = 0.f;                 // NPC 自己声称的紧迫度 (0~10)，由 Parser 校验

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	float BidOffset = 0.f;               // 角色静态偏移（来自 FAILiveAgentBattleConfig.BidOffset）

	// T7 加：principles §5.4 反霸麦衰减 + 被 @ 加权 + 沉默加权汇总，用于 tick_audit.all_bids[].runtime_adj。
	// 中文教学：「霸麦衰减」= 同一 actor 连续 win floor 时分数下降；「被@加权」
	// = 上一拍被指名时本拍 bid 加分；「沉默加权」= 长时间不发言累积浮动加分。
	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	float RuntimeAdj = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	float FinalScore = 0.f;              // 排序用最终分数 = Urgency + BidOffset + RuntimeAdj

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	FString ProposedTarget;              // 想说话的对象（agent_id），可空

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	FString Rationale;                   // 自我说服词（仅日志/audit 用）
};

// Director 整理出的本 tick 最终结果。AllBids 用于 audit 事件，便于复盘。
USTRUCT(BlueprintType)
struct AILIVEPROJECT_API FAILiveTickResolution
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	int32 TickNo = 0;                    // 第几拍

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	FString WinnerActor;                 // 抢中 floor 的 agent；空表示本拍无人发言

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	int64 WinnerIntendedSeq = 0;         // 该 actor 当拍 speech.intended 的 seq

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	int64 DerivedPublicSeq = 0;          // promote 出来的 speech.public 事件 seq

	UPROPERTY(BlueprintReadOnly, Category = "AILive|Bid")
	TArray<FAILiveBid> AllBids;          // 本拍所有 bid（含落选者），用于 audit
};
