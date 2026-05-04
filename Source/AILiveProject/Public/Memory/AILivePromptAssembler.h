#pragma once

#include "CoreMinimal.h"

class UAILiveEventStoreSubsystem;
struct FNPCAgentConfig;

/**
 * T6 — PromptAssembler 把 memory_principles.md §4.3 的 prompt 拼装优先级落到代码。
 *
 * 设计：
 * - 纯 namespace 函数，无状态；不调 LLM；所有数据走 EventStore 读 API。
 * - 调用方（Act02RuleReceiveDirector reaction 分支）通过 GetEventStore(this)
 *   拿 Store 指针后传入；Cfg 是 NPCs[i].Config。
 * - 段顺序按 §4.3 必保留 1-9：
 *     System: [INVARIANT REMINDERS] / [CURRENT TICK] / [OUTPUT SCHEMA]
 *     User:   [YOUR OWN COMPLETE STATEMENT HISTORY]
 *           + [YOUR RECENT INTENDED-BUT-NOT-SAID]
 *           + [YOUR NOTES TO FUTURE SELF]
 *           + [YOUR COMMITMENTS]
 *           + [RECENT NEAR-WINDOW PUBLIC EVENTS]
 *           + [PRIVATE / NON-PUBLIC EVENTS ADDRESSED TO YOU]
 *           + [PREFETCHED EVIDENCE FROM REFERENCED ROUND {N}]  // 仅 ChallengeText 非空时
 *
 * 接管范围：Reaction phase 的 system + user prompt；Seed phase（开局首拍无历史）
 * 仍走 BuildSeedSystemPrompt/UserPrompt。
 *
 * Output schema 保留 ACT02 现有 legacy JSON `{ want_to_speak, willingness, content }`
 * ——T7 主循环重写时才切换到四通道认知输出。
 */
namespace AILivePromptAssembler
{
	struct FAssembleOptions
	{
		// PromptAssembler 始终给某个 agent 自己拼装 prompt；Viewer 同时是 self
		// actor 过滤目标（own history / pending intended / notes）和 visibility
		// filter 视角（near window / private chat / challenge prefetch）。
		// Caller 用 FString::Printf(TEXT("NPC%02d"), Cfg.NPCIndex) 构造。
		FString Viewer;

		int32 CurrentRound = 0;

		// principles §4.3 line 223 要求 K 必须固定。本 build K=5（覆盖 1-2 个
		// day_discuss 反应链；与 bidding §5.2bis 推荐 N=3 语义不同——后者是抢
		// 麦判霸麦阈值）。
		int32 NearWindowK = 5;

		// 由 caller 预先 LoadGameRule 后传入，避免 PromptAssembler 触碰文件系统。
		FString GameRule;

		// 可选；非空时触发 §4.4 challenge prefetch：抽取"第 N 轮"引用 →
		// QuoteRecentRounds(round, 1, Viewer) 把该轮全部可见事件注入 prompt。
		FString ChallengeText;
	};

	// 任务卡 T6 line 25 签名。System 当前实现不读 Store 但保留参数对齐 + 未来
	// 扩展（如插入 [CURRENT TICK] 时查 EventStore 拿当前 phase）无需破签名。
	AILIVEPROJECT_API FString AssembleSystemPrompt(
		const UAILiveEventStoreSubsystem* Store,
		const FNPCAgentConfig& Cfg,
		const FAssembleOptions& Opt);

	AILIVEPROJECT_API FString AssembleUserPrompt(
		const UAILiveEventStoreSubsystem* Store,
		const FNPCAgentConfig& Cfg,
		const FAssembleOptions& Opt);
}
