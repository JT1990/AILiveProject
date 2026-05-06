// =============================================================================
// 中文教学：AILivePromptAssembler.cpp —— prompt 拼装实现
//
// 文件结构（已经有详尽内联中文注释；下面只补 C++/UE 概念教学）：
//   1) 内部工具：JSON 提取、正则、事件行格式化
//   2) Append*Section：每段一个生成函数
//   3) 降级机制：FSectionEntry + DropSectionByName
//   4) AssembleSystemPrompt / AssembleUserPrompt：公开 API
//   5) Console commands：L1 验收测试入口
//
// 关键 C++ / UE 概念：
//   1) FRegexPattern + FRegexMatcher
//      UE 的正则封装。用法：构造 Pattern → 构造 Matcher(Pattern, Text) →
//      while (M.FindNext()) 循环；M.GetCaptureGroup(N) 取第 N 个捕获组。
//      模式语法是 ECMAScript，但要注意 UE 的 \\ 转义（C++ 字符串 + 正则双重转义）。
//
//   2) ensureAlwaysMsgf
//      失败时打日志、记 callstack，但**不 abort 进程**（与 check() 不同）。
//      Editor / Development build 才生效，Shipping 退化成 noop。
//      用在「条件不满足想留 callstack 但不希望 PIE 崩溃」的场景。
//
//   3) FConsoleCommandWithArgsDelegate::CreateLambda
//      Console 命令带参数版。Lambda 签名 [](const TArray<FString>& Args)。
//      参数按空格分割：`AILive.Test.AssembleChallenge NPC01 3` → Args = ["NPC01", "3"]
//
//   4) FCString::Atoi
//      字符串转 int 工具，无效输入返回 0。比 std::stoi 更宽容（不抛异常）。
// =============================================================================

#include "Memory/AILivePromptAssembler.h"

#include "Memory/AILiveEventStoreSubsystem.h"
#include "Memory/AILiveEventTypes.h"
#include "LLM/AILiveAgentRoster.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Internationalization/Regex.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace AILivePromptAssembler
{

namespace
{

// principles §4.3 line 41：自我发言永不压缩 — 超此上限抛 ensureAlways +
// UE_LOG Error 让上层处理（不静默裁剪）。中文按 ~3 char/token 估算 ≈ 21K
// tokens；DeepSeek/GLM/Qwen3 均 ≥ 32K context。
constexpr int32 kPromptCtxLimit = 64000;

// =============================================================================
// 内部工具：JSON 提取 + 正则
// =============================================================================

// 解 PayloadJson 取 payload.text 字段。失败返回空字符串。
FString ExtractText(const FString& InPayloadJson)
{
	if (InPayloadJson.IsEmpty())
	{
		return FString();
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InPayloadJson);
	TSharedPtr<FJsonObject> Obj;
	if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
	{
		return FString();
	}
	FString Out;
	Obj->TryGetStringField(TEXT("text"), Out);
	return Out;
}

// principles §4.4：从质问文本中提取"第 N 轮"引用，返回去重的 round_no 列表。
TArray<int32> ExtractRoundRefs(const FString& InChallengeText)
{
	TArray<int32> Out;
	if (InChallengeText.IsEmpty())
	{
		return Out;
	}
	const FRegexPattern Pat(TEXT("第\\s*(\\d+)\\s*轮"));
	FRegexMatcher M(Pat, InChallengeText);
	while (M.FindNext())
	{
		const FString N = M.GetCaptureGroup(1);
		const int32 Round = FCString::Atoi(*N);
		Out.AddUnique(Round);
	}
	return Out;
}

// principles §7 模板：`Tick {tick:03d} round_no={round:02d} {phase} seq={seq}: "{text}"`
FString FormatEventLine(const FAILiveEvent& Ev)
{
	const FString Text = ExtractText(Ev.PayloadJson);
	return FString::Printf(
		TEXT("Tick %03lld round_no=%02d %s seq=%lld: \"%s\""),
		(long long)Ev.TickNo,
		Ev.RoundNo,
		*AILiveEvent::PhaseToString(Ev.Phase),
		(long long)Ev.Seq,
		*Text);
}

// =============================================================================
// 内部辅助：每段生成
// =============================================================================

// 必保留 3：自我发言全量原文（speech.public ∪ private_msg），永不压缩。
FString AppendOwnHistorySection(const UAILiveEventStoreSubsystem* Store, const FString& Viewer)
{
	FString Out;
	Out += TEXT("[YOUR OWN COMPLETE STATEMENT HISTORY — verbatim, indexed by tick/round]\n");
	if (Store)
	{
		const TArray<FAILiveEvent> Statements = Store->ListMyStatements(Viewer);
		for (const FAILiveEvent& Ev : Statements)
		{
			Out += TEXT("  ") + FormatEventLine(Ev) + TEXT("\n");
		}
	}
	Out += TEXT("[END OF YOUR HISTORY]\n\n");
	return Out;
}

// 必保留 4：最近 K 拍写过 speech.intended 但未抢中 floor 的内容。
FString AppendPendingIntendedSection(const UAILiveEventStoreSubsystem* Store, const FString& Viewer, int32 K)
{
	FString Out;
	Out += FString::Printf(
		TEXT("[YOUR RECENT INTENDED-BUT-NOT-SAID — verbatim, last %d ticks]\n"), K);
	if (Store)
	{
		const TArray<FAILiveEvent> Pending = Store->ListMyPendingIntended(Viewer, K);
		for (const FAILiveEvent& Ev : Pending)
		{
			Out += TEXT("  ") + FormatEventLine(Ev) + TEXT("\n");
		}
	}
	Out += TEXT("[END OF PENDING INTENDED]\n\n");
	return Out;
}

// 必保留 5：自己最近 K 条 speech.note。
FString AppendNotesSection(const UAILiveEventStoreSubsystem* Store, const FString& Viewer, int32 K)
{
	FString Out;
	Out += FString::Printf(
		TEXT("[YOUR NOTES TO FUTURE SELF — last %d ticks]\n"), K);
	if (Store)
	{
		const TArray<FAILiveEvent> Notes = Store->ListMyNotes(Viewer, K);
		for (const FAILiveEvent& Ev : Notes)
		{
			Out += TEXT("  ") + FormatEventLine(Ev) + TEXT("\n");
		}
	}
	Out += TEXT("[END OF NOTES]\n\n");
	return Out;
}

// 必保留 6：自我承诺投影。T8 落地前 ListMyCommitments 返回空数组——本段
// 自然为空 header；T8 上线后无需修改本函数即生效。
FString AppendCommitmentsSection(const UAILiveEventStoreSubsystem* Store, const FString& Viewer)
{
	FString Out;
	Out += TEXT("[YOUR COMMITMENTS]\n");
	if (Store)
	{
		const TArray<FAILiveCommitment> Commitments = Store->ListMyCommitments(Viewer, -1, -1);
		for (const FAILiveCommitment& C : Commitments)
		{
			Out += FString::Printf(
				TEXT("  Round %02d seq=%lld %s -> %s: \"%s\" [%s]\n"),
				C.RoundNo,
				(long long)C.Seq,
				*AILiveEvent::CommitmentTypeToString(C.CommitmentType),
				*C.Target,
				*C.Text,
				*AILiveEvent::CommitmentStatusToString(C.Status));
		}
	}
	Out += TEXT("[END OF COMMITMENTS]\n\n");
	return Out;
}

// 必保留 7：公开发言近场窗口（最近 K 拍 visibility=public 且 actor != Viewer）。
// 自我公开发言已在 OwnHistory 段输出，此处去重避免重复。
FString AppendNearWindowSection(const UAILiveEventStoreSubsystem* Store, const FString& Viewer,
                                 int32 CurrentRound, int32 K)
{
	FString Out;
	Out += FString::Printf(
		TEXT("[RECENT NEAR-WINDOW PUBLIC EVENTS — last %d ticks]\n"), K);
	if (Store)
	{
		const TArray<FAILiveEvent> Window = Store->QuoteRecentRounds(CurrentRound, K, Viewer);
		for (const FAILiveEvent& Ev : Window)
		{
			if (Ev.Actor == Viewer) continue;                  // 自己发言已在 OwnHistory
			if (!Ev.Visibility.Contains(TEXT("public"))) continue;  // 私聊在下个段
			Out += TEXT("  ") + FormatEventLine(Ev) + TEXT("\n");
		}
	}
	Out += TEXT("[END OF NEAR-WINDOW PUBLIC EVENTS]\n\n");
	return Out;
}

// 必保留 8：私聊 / 夜间私密事件（visibility 含 Viewer 但不含 'public' 且 actor != Viewer）。
FString AppendPrivateChatsSection(const UAILiveEventStoreSubsystem* Store, const FString& Viewer,
                                   int32 CurrentRound, int32 K)
{
	FString Out;
	Out += TEXT("[PRIVATE / NON-PUBLIC EVENTS ADDRESSED TO YOU]\n");
	if (Store)
	{
		const TArray<FAILiveEvent> Window = Store->QuoteRecentRounds(CurrentRound, K, Viewer);
		for (const FAILiveEvent& Ev : Window)
		{
			if (Ev.Actor == Viewer) continue;                       // 自己写的不在此段
			if (Ev.Visibility.Contains(TEXT("public"))) continue;   // 公开的在 NearWindow
			if (!Ev.Visibility.Contains(Viewer)) continue;          // 必须含 self（理论上 SQL 已过滤）
			Out += TEXT("  ") + FormatEventLine(Ev) + TEXT("\n");
		}
	}
	Out += TEXT("[END OF PRIVATE EVENTS]\n\n");
	return Out;
}

// §4.4 challenge prefetch：从 ChallengeText 抽取轮次引用，对每个 round 调
// QuoteRecentRounds(round, 1, Viewer) 注入。
// **不能**用 QuoteByRound(round, "", Viewer)：SQL 强制 e.actor = ?3，空串匹配 0 条。
FString AppendChallengePrefetchSection(const UAILiveEventStoreSubsystem* Store, const FString& Viewer,
                                        const FString& ChallengeText)
{
	FString Out;
	if (ChallengeText.IsEmpty() || !Store)
	{
		return Out;  // 无质问 → 不输出本段（避免空标题污染 prompt）
	}
	const TArray<int32> Rounds = ExtractRoundRefs(ChallengeText);
	if (Rounds.Num() == 0)
	{
		return Out;
	}
	for (int32 R : Rounds)
	{
		Out += FString::Printf(
			TEXT("[PREFETCHED EVIDENCE FROM REFERENCED ROUND %d]\n"), R);
		const TArray<FAILiveEvent> Events = Store->QuoteRecentRounds(R, 1, Viewer);
		for (const FAILiveEvent& Ev : Events)
		{
			Out += TEXT("  ") + FormatEventLine(Ev) + TEXT("\n");
		}
		Out += FString::Printf(TEXT("[END OF ROUND %d EVIDENCE]\n\n"), R);
	}
	return Out;
}

// =============================================================================
// 降级 + 拼装
// =============================================================================

// 段元数据：bMustKeep=true 的段在降级时绝不丢（principles §4.3 line 228）。
struct FSectionEntry
{
	FString Name;
	FString Body;
	bool bMustKeep;
};

int32 TotalLen(const TArray<FSectionEntry>& Sections)
{
	int32 N = 0;
	for (const FSectionEntry& S : Sections) N += S.Body.Len();
	return N;
}

void DropSectionByName(TArray<FSectionEntry>& Sections, const TCHAR* Name)
{
	for (FSectionEntry& S : Sections)
	{
		if (S.Name == Name)
		{
			S.Body.Reset();
			return;
		}
	}
}

} // namespace anonymous

// =============================================================================
// 公开 API
// =============================================================================

FString AssembleSystemPrompt(const UAILiveEventStoreSubsystem* /*Store*/,
                              const FNPCAgentConfig& Cfg,
                              const FAssembleOptions& Opt)
{
	// principles §4.3 必保留 1+2+9。**不**引入 T7 四通道认知措辞——ACT02 仍
	// 走 legacy JSON `{ want_to_speak, willingness, content }`（ParseAnswer 依赖）。
	return FString::Printf(
		TEXT("你是 AI %s，%s。\n")
		TEXT("游戏规则：%s\n\n")
		TEXT("[INVARIANT REMINDERS]\n")
		TEXT("- 你是 AI，没有人类背景；只有外观符号（名字 / 昵称 / 性别 / 声线）。\n")
		TEXT("- 必须用严格 JSON 回答；不输出任何 JSON 之外的文字。\n\n")
		TEXT("[CURRENT TICK]\n")
		TEXT("现在第 %d 轮 day_discuss。请阅读 user prompt 中的事件流（自我发言历史 / 未抢中 floor 的 intended / 自我便条 / 近场窗口 / 私聊）后决定是否接话。\n\n")
		TEXT("[OUTPUT SCHEMA — 严格 JSON]\n")
		TEXT("{\n")
		TEXT("  \"want_to_speak\": true | false,\n")
		TEXT("  \"willingness\": \"extremely_strong\" | \"strong\" | \"moderate\" | \"weak\" | \"none\",\n")
		TEXT("  \"content\": \"<不超过 80 字的中文>\"\n")
		TEXT("}\n")
		TEXT("意愿语义同前。如果 want_to_speak=false，content 可填空字符串。只输出 JSON。\n"),
		*Cfg.Identity.FullName,
		*Cfg.VoicePresentationHint,
		*Opt.GameRule,
		Opt.CurrentRound);
}

FString AssembleUserPrompt(const UAILiveEventStoreSubsystem* Store,
                            const FNPCAgentConfig& /*Cfg*/,
                            const FAssembleOptions& Opt)
{
	// 7 段（前 6 段恒输出，第 7 段 ChallengePrefetch 仅 ChallengeText 非空时）
	TArray<FSectionEntry> Sections;
	Sections.Add({ TEXT("OwnHistory"),       AppendOwnHistorySection(Store, Opt.Viewer),                                /*bMustKeep=*/true  });
	Sections.Add({ TEXT("PendingIntended"),  AppendPendingIntendedSection(Store, Opt.Viewer, Opt.NearWindowK),          /*bMustKeep=*/true  });
	Sections.Add({ TEXT("Notes"),            AppendNotesSection(Store, Opt.Viewer, Opt.NearWindowK),                    /*bMustKeep=*/false });
	Sections.Add({ TEXT("Commitments"),      AppendCommitmentsSection(Store, Opt.Viewer),                               /*bMustKeep=*/false });
	Sections.Add({ TEXT("NearWindow"),       AppendNearWindowSection(Store, Opt.Viewer, Opt.CurrentRound, Opt.NearWindowK), /*bMustKeep=*/true  });
	Sections.Add({ TEXT("PrivateChats"),     AppendPrivateChatsSection(Store, Opt.Viewer, Opt.CurrentRound, Opt.NearWindowK), /*bMustKeep=*/false });
	Sections.Add({ TEXT("ChallengePrefetch"),AppendChallengePrefetchSection(Store, Opt.Viewer, Opt.ChallengeText),      /*bMustKeep=*/false });

	// principles §4.3 line 223-228 降级硬规则：
	//   1. 丢私聊段（visibility 含 self 但 actor != self 的 private 段）
	//   2. 移除非自己产出的私有推理段（reflection 等 — T6 不输出，无操作）
	//   3. 把第 cur-K 拍的事件移到摘要段（截断 NearWindow oldest tick — 简化为整段降级）
	//   4. 永远不丢 OwnHistory / PendingIntended / NearWindow（公开发言）
	if (TotalLen(Sections) > kPromptCtxLimit)
	{
		DropSectionByName(Sections, TEXT("PrivateChats"));
	}
	if (TotalLen(Sections) > kPromptCtxLimit)
	{
		DropSectionByName(Sections, TEXT("Notes"));
	}
	if (TotalLen(Sections) > kPromptCtxLimit)
	{
		// 简化路径：把 Commitments 段也丢；NearWindow 截断 oldest tick 实现复杂，
		// 留作后续优化（T6 当前用例不构造超大 prompt 场景）。
		DropSectionByName(Sections, TEXT("Commitments"));
	}

	const int32 FinalLen = TotalLen(Sections);
	if (FinalLen > kPromptCtxLimit)
	{
		// 超限不静默裁剪 — 留 Error log + ensureAlways 让 PIE 留 callstack；
		// 不用 check() 因为它在 PIE 中会 abort 进程，与 ACT02 长跑场景不兼容。
		UE_LOG(LogAILiveMemory, Error,
			TEXT("AssembleUserPrompt: viewer=%s prompt_len=%d 超出 kPromptCtxLimit=%d；"
			     "降级后仍超限。OwnHistory/PendingIntended/NearWindow 永不丢——"
			     "上层应判断长度后实施摘要或拒绝发起 LLM 请求。"),
			*Opt.Viewer, FinalLen, kPromptCtxLimit);
		ensureAlwaysMsgf(FinalLen <= kPromptCtxLimit,
			TEXT("PromptAssembler: viewer=%s len=%d > %d (must-keep sections cannot be compressed)"),
			*Opt.Viewer, FinalLen, kPromptCtxLimit);
	}

	FString Out;
	Out.Reserve(FinalLen);
	for (const FSectionEntry& S : Sections)
	{
		Out += S.Body;
	}
	return Out;
}

} // namespace AILivePromptAssembler

// =============================================================================
// Console commands — §11 L1 验收
// =============================================================================

namespace
{

UAILiveEventStoreSubsystem* GetStoreForConsole()
{
	UWorld* World = nullptr;
	if (GEngine && GEngine->GetWorldContexts().Num() > 0)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			if (Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game)
			{
				World = Ctx.World();
				break;
			}
		}
	}
	if (!World) World = GWorld;
	if (!World)
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("PromptAssembler console: no PIE/Game world"));
		return nullptr;
	}
	UGameInstance* GI = World->GetGameInstance();
	if (!GI)
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("PromptAssembler console: world has no GameInstance"));
		return nullptr;
	}
	UAILiveEventStoreSubsystem* Store = GI->GetSubsystem<UAILiveEventStoreSubsystem>();
	if (!Store)
	{
		UE_LOG(LogAILiveMemory, Error, TEXT("PromptAssembler console: EventStore subsystem not found"));
	}
	return Store;
}

} // namespace anonymous

static FAutoConsoleCommand GAILiveTestAssemblePendingIntended(
	TEXT("AILive.Test.AssemblePendingIntended"),
	TEXT("AILive.Test.AssemblePendingIntended <agent_id> — 写一条 agent 的 speech.intended（无 paired public）→ 调 AssembleUserPrompt → "
	     "校验返回字符串包含 [YOUR RECENT INTENDED-BUT-NOT-SAID] 段 + 该 intended 原文。验收 §11 L1。"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.AssemblePendingIntended <agent_id>"));
			return;
		}
		UAILiveEventStoreSubsystem* Store = GetStoreForConsole();
		if (!Store || !Store->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("AssemblePendingIntended: no game open (run BeginGame first)"));
			return;
		}
		const FString Agent = Args[0];

		// 写一条 speech.intended（不写对应 public）；EventStore 会按
		// CachedCurrentTickNo 自动填 tick_no 列。
		FAILiveEvent Ev;
		Ev.Actor = Agent;
		Ev.EventType = EAILiveEventType::SpeechIntended;
		Ev.SpeechActType = EAILiveSpeechActType::Claim;
		Ev.Phase = EAILivePhase::DayDiscuss;
		Ev.RoundNo = 1;
		Ev.Visibility = { Agent };  // intended 仅自己可见
		Ev.PayloadJson = TEXT("{\"text\":\"我想质疑 NPC03 关于联盟的说法 (test intended-but-not-said)\"}");
		const int64 Seq = Store->AppendEvent(Ev);
		if (Seq <= 0)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("AssemblePendingIntended: AppendEvent failed"));
			return;
		}

		AILivePromptAssembler::FAssembleOptions Opt;
		Opt.Viewer       = Agent;
		Opt.CurrentRound = 1;
		Opt.NearWindowK  = 5;
		Opt.GameRule     = TEXT("(test rule)");
		const FNPCAgentConfig EmptyCfg;  // user prompt 不读 Cfg
		const FString UserPrompt = AILivePromptAssembler::AssembleUserPrompt(Store, EmptyCfg, Opt);

		const bool bHasHeader = UserPrompt.Contains(TEXT("[YOUR RECENT INTENDED-BUT-NOT-SAID"));
		const bool bHasText   = UserPrompt.Contains(TEXT("我想质疑 NPC03 关于联盟的说法"));
		UE_LOG(LogAILiveMemory, Display,
			TEXT("AssemblePendingIntended(%s) seq=%lld len=%d header_present=%d text_present=%d"),
			*Agent, Seq, UserPrompt.Len(), bHasHeader ? 1 : 0, bHasText ? 1 : 0);
		UE_LOG(LogAILiveMemory, Display, TEXT("---- USER PROMPT ----\n%s\n---- END USER PROMPT ----"), *UserPrompt);
	}));

static FAutoConsoleCommand GAILiveTestAssembleChallenge(
	TEXT("AILive.Test.AssembleChallenge"),
	TEXT("AILive.Test.AssembleChallenge <agent_id> <round_no> — 在 round_no 里写若干 public 事件 → "
	     "构造含『第 N 轮』的 ChallengeText → 调 AssembleUserPrompt → 校验返回字符串含 "
	     "[PREFETCHED EVIDENCE FROM REFERENCED ROUND N] + 该轮事件原文。"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("Usage: AILive.Test.AssembleChallenge <agent_id> <round_no>"));
			return;
		}
		UAILiveEventStoreSubsystem* Store = GetStoreForConsole();
		if (!Store || !Store->IsGameOpen())
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("AssembleChallenge: no game open (run BeginGame first)"));
			return;
		}
		const FString Agent = Args[0];
		const int32 RoundNo = FCString::Atoi(*Args[1]);

		// 在指定轮里写一条 public speech（actor 不是 Agent 自己，否则会被
		// NearWindow 的 actor != Viewer 过滤掉）
		FAILiveEvent Ev;
		Ev.Actor = TEXT("NPC03");
		Ev.EventType = EAILiveEventType::SpeechPublic;
		Ev.SpeechActType = EAILiveSpeechActType::Claim;
		Ev.Phase = EAILivePhase::DayDiscuss;
		Ev.RoundNo = RoundNo;
		Ev.Visibility = { TEXT("public") };
		Ev.PayloadJson = FString::Printf(
			TEXT("{\"text\":\"在第 %d 轮我说我支持联盟提案 (test challenge prefetch)\"}"),
			RoundNo);
		const int64 Seq = Store->AppendEvent(Ev);
		if (Seq <= 0)
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("AssembleChallenge: AppendEvent failed"));
			return;
		}

		AILivePromptAssembler::FAssembleOptions Opt;
		Opt.Viewer        = Agent;
		Opt.CurrentRound  = RoundNo;
		Opt.NearWindowK   = 5;
		Opt.GameRule      = TEXT("(test rule)");
		Opt.ChallengeText = FString::Printf(TEXT("你在第 %d 轮说过联盟提案，怎么解释？"), RoundNo);
		const FNPCAgentConfig EmptyCfg;
		const FString UserPrompt = AILivePromptAssembler::AssembleUserPrompt(Store, EmptyCfg, Opt);

		const FString ExpectedHeader = FString::Printf(TEXT("[PREFETCHED EVIDENCE FROM REFERENCED ROUND %d]"), RoundNo);
		const bool bHasHeader = UserPrompt.Contains(ExpectedHeader);
		const bool bHasText   = UserPrompt.Contains(TEXT("我说我支持联盟提案"));
		UE_LOG(LogAILiveMemory, Display,
			TEXT("AssembleChallenge(%s, round=%d) seq=%lld len=%d header_present=%d text_present=%d"),
			*Agent, RoundNo, Seq, UserPrompt.Len(), bHasHeader ? 1 : 0, bHasText ? 1 : 0);
		UE_LOG(LogAILiveMemory, Display, TEXT("---- USER PROMPT ----\n%s\n---- END USER PROMPT ----"), *UserPrompt);
	}));

