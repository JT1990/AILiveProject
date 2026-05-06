#pragma once

// =============================================================================
// 中文教学：AILiveParserClient.h —— Reasoner→Parser 双 LLM 串联中的「Parser」客户端
//
// AILive 双 LLM 流程（PRD T7）：
//   ┌─────────────┐     RawText (4 sections)        ┌──────────────┐
//   │  Reasoner   │ ──────────────────────────────→ │   Parser     │
//   │ (NPC 自由   │     <SCRATCHPAD>...</...>       │ (固定 prompt │
//   │  写四段)    │     <INTENDED>...</...>         │  把四段抠出  │
//   │             │     <BID>...</...>              │  并校验为   │
//   │             │     <NOTE_TO_SELF>...</...>     │  JSON object）│
//   └─────────────┘                                 └──────────────┘
//                                                          │
//                                                          ▼
//                                       FParseResult { Scratchpad, IntendedJson, BidJson, NoteText }
//
//   Reasoner 用每个 NPC 各自 provider 的模型；Parser 用统一的 DeepSeek（决定
//   于 .cpp 里的 kParserProvider 常量），保证 Parser 行为可复现。
//
// 三个公开 API：
//   - ParseFourChannels(Req)            ：完整流程（预检 + LLM + 输出校验）
//   - PrevalidateRawTaggedSections(Raw) ：仅做 raw text 标签存在性 + JSON 结构性预检
//   - ValidateParserOutputJson(Json)    ：仅做 Parser LLM 输出 JSON 的字段类型校验
//
// 三段失败标识 FailedStage：
//   "raw_prevalidate" / "llm_call" / "output_validate"
// =============================================================================

#include "CoreMinimal.h"

namespace AILiveParser
{
	struct FParseRequest
	{
		FString RawText;     // Reasoner 输出的完整原始文本（含四段 XML 风格标签）
		FString AgentId;     // 调用方 NPC 标识，用于日志归因
	};

	// Parser 处理结果。bOk=true 时 Scratchpad/IntendedJson/BidJson/NoteText 均填好；
	// bOk=false 时 ErrorReason + FailedStage 提供失败信息。
	struct FParseResult
	{
		bool    bOk = false;

		FString Scratchpad;        // 思维链文本（用 <SCRATCHPAD> 标签包裹的部分）
		FString IntendedJson;      // 行为意图 JSON（含 text / intended_action / addressed_to_hint）
		FString BidJson;           // 发言竞价 JSON（含 urgency / proposed_target / rationale）
		FString NoteText;          // 自我备忘文本

		FString ErrorReason;       // 失败原因（人话）
		FString FailedStage;       // "raw_prevalidate" / "llm_call" / "output_validate"
		FString ParserRawJson;     // Parser LLM 输出的原始 JSON（debug 用）
	};

	// 完整流程：raw 预检 → LLM 调用 → 输出 JSON 校验。
	AILIVEPROJECT_API FParseResult ParseFourChannels(const FParseRequest& Req);

	// Stage 1：仅检查 raw text 是否含四段标签 + intended/bid 段是否合法 JSON。
	AILIVEPROJECT_API FParseResult PrevalidateRawTaggedSections(const FString& RawText);

	// Stage 3：仅检查 Parser LLM 返回的 JSON 字段类型是否符合 schema。
	AILIVEPROJECT_API FParseResult ValidateParserOutputJson(const FString& ParserJson);
}
