// =============================================================================
// 中文教学：AILiveEventTypes.cpp —— 枚举↔字符串映射 + JSON 数组工具实现
//
// 文件结构：
//   1) DEFINE_LOG_CATEGORY(LogAILiveMemory) —— 把头里的 EXTERN 声明落实
//   2) *ToString：每个枚举一个 switch；default 不写（UE 编译器会就缺枚举值警告）
//      每个函数最后都有兜底 return —— 防止 default 漏掉新枚举值时编译失败
//   3) *FromString：if/else 链；未知字符串 → 默认值 + Warning 日志
//   4) ArrayToJsonString / JsonStringToArray：UE JSON API 包装
//
// 设计取舍：
//   - 不用 TMap<EnumValue, FString> 查表：枚举值少，switch 编译器能优化为跳转表，
//     可读性更好；少几行代码也少几次 cache miss
//   - 未知字符串不抛异常：UE 项目惯例「记日志 + 默认值」；让 PIE 长跑场景下
//     遇到旧 schema 数据也能继续运行（写一条 Warning 而非 crash）
// =============================================================================

#include "Memory/AILiveEventTypes.h"

#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// 头里 DECLARE_LOG_CATEGORY_EXTERN 是「外部声明」；这里 DEFINE 才生成实例。
// 所有用到 LogAILiveMemory 的 .cpp 都靠链接器找到这一份定义。
DEFINE_LOG_CATEGORY(LogAILiveMemory);

namespace AILiveEvent
{
	FString PhaseToString(EAILivePhase Phase)
	{
		switch (Phase)
		{
		case EAILivePhase::Setup:       return TEXT("setup");
		case EAILivePhase::DayDiscuss:  return TEXT("day_discuss");
		case EAILivePhase::Vote:        return TEXT("vote");
		case EAILivePhase::NightAction: return TEXT("night_action");
		case EAILivePhase::Reveal:      return TEXT("reveal");
		case EAILivePhase::GameOver:    return TEXT("game_over");
		}
		return TEXT("setup");
	}

	FString EventTypeToString(EAILiveEventType Type)
	{
		switch (Type)
		{
		case EAILiveEventType::SpeechPublic:             return TEXT("speech.public");
		case EAILiveEventType::SpeechScratchpad:         return TEXT("speech.scratchpad");
		case EAILiveEventType::SpeechIntended:           return TEXT("speech.intended");
		case EAILiveEventType::SpeechNote:               return TEXT("speech.note");
		case EAILiveEventType::Bid:                      return TEXT("bid");
		case EAILiveEventType::Reflection9Q:             return TEXT("reflection.9q");
		case EAILiveEventType::Vote:                     return TEXT("vote");
		case EAILiveEventType::PrivateMsg:               return TEXT("private_msg");
		case EAILiveEventType::AlliancePropose:          return TEXT("alliance_propose");
		case EAILiveEventType::AllianceAccept:           return TEXT("alliance_accept");
		case EAILiveEventType::AllianceBetray:           return TEXT("alliance_betray");
		case EAILiveEventType::ActionIntent:             return TEXT("action.intent");
		case EAILiveEventType::ActionResolved:           return TEXT("action.resolved");
		case EAILiveEventType::ActionCancelled:          return TEXT("action.cancelled");
		case EAILiveEventType::OrchestratorResolved:     return TEXT("orchestrator.round_resolved");
		case EAILiveEventType::OrchestratorTickAnchor:   return TEXT("orchestrator.tick_anchor");
		case EAILiveEventType::OrchestratorTickResolved: return TEXT("orchestrator.tick_resolved");
		case EAILiveEventType::OrchestratorTickAudit:    return TEXT("orchestrator.tick_audit");
		case EAILiveEventType::SystemRoleAssigned:       return TEXT("system.role_assigned");
		case EAILiveEventType::SystemAgentTimeout:       return TEXT("system.agent_timeout");
		case EAILiveEventType::SystemParseFailed:        return TEXT("system.parse_failed");
		case EAILiveEventType::SystemLLMInflight:        return TEXT("system.llm_inflight");
		case EAILiveEventType::SystemDeleteExecuted:     return TEXT("system.delete_executed");
		case EAILiveEventType::WinnerDecision:           return TEXT("winner_decision");
		}
		return TEXT("speech.public");
	}

	FString SpeechActToString(EAILiveSpeechActType T)
	{
		switch (T)
		{
		case EAILiveSpeechActType::None:     return FString();
		case EAILiveSpeechActType::Claim:    return TEXT("claim");
		case EAILiveSpeechActType::Accuse:   return TEXT("accuse");
		case EAILiveSpeechActType::Defend:   return TEXT("defend");
		case EAILiveSpeechActType::Commit:   return TEXT("commit");
		case EAILiveSpeechActType::Deny:     return TEXT("deny");
		case EAILiveSpeechActType::Question: return TEXT("question");
		case EAILiveSpeechActType::Reveal:   return TEXT("reveal");
		}
		return FString();
	}

	FString CommitmentTypeToString(EAILiveCommitmentType T)
	{
		switch (T)
		{
		case EAILiveCommitmentType::Promise:   return TEXT("promise");
		case EAILiveCommitmentType::ClaimRole: return TEXT("claim_role");
		case EAILiveCommitmentType::Deny:      return TEXT("deny");
		case EAILiveCommitmentType::VoteFor:   return TEXT("vote_for");
		case EAILiveCommitmentType::Alliance:  return TEXT("alliance");
		}
		return TEXT("promise");
	}

	FString CommitmentStatusToString(EAILiveCommitmentStatus S)
	{
		switch (S)
		{
		case EAILiveCommitmentStatus::Active:       return TEXT("active");
		case EAILiveCommitmentStatus::Retracted:    return TEXT("retracted");
		case EAILiveCommitmentStatus::Contradicted: return TEXT("contradicted");
		}
		return TEXT("active");
	}

	EAILivePhase PhaseFromString(const FString& S)
	{
		if (S == TEXT("setup"))        return EAILivePhase::Setup;
		if (S == TEXT("day_discuss"))  return EAILivePhase::DayDiscuss;
		if (S == TEXT("vote"))         return EAILivePhase::Vote;
		if (S == TEXT("night_action")) return EAILivePhase::NightAction;
		if (S == TEXT("reveal"))       return EAILivePhase::Reveal;
		if (S == TEXT("game_over"))    return EAILivePhase::GameOver;
		UE_LOG(LogAILiveMemory, Warning, TEXT("PhaseFromString: unknown '%s', defaulting to setup"), *S);
		return EAILivePhase::Setup;
	}

	EAILiveEventType EventTypeFromString(const FString& S)
	{
		if (S == TEXT("speech.public"))             return EAILiveEventType::SpeechPublic;
		if (S == TEXT("speech.scratchpad"))         return EAILiveEventType::SpeechScratchpad;
		if (S == TEXT("speech.intended"))           return EAILiveEventType::SpeechIntended;
		if (S == TEXT("speech.note"))               return EAILiveEventType::SpeechNote;
		if (S == TEXT("bid"))                       return EAILiveEventType::Bid;
		if (S == TEXT("reflection.9q"))             return EAILiveEventType::Reflection9Q;
		if (S == TEXT("vote"))                      return EAILiveEventType::Vote;
		if (S == TEXT("private_msg"))               return EAILiveEventType::PrivateMsg;
		if (S == TEXT("alliance_propose"))          return EAILiveEventType::AlliancePropose;
		if (S == TEXT("alliance_accept"))           return EAILiveEventType::AllianceAccept;
		if (S == TEXT("alliance_betray"))           return EAILiveEventType::AllianceBetray;
		if (S == TEXT("action.intent"))             return EAILiveEventType::ActionIntent;
		if (S == TEXT("action.resolved"))           return EAILiveEventType::ActionResolved;
		if (S == TEXT("action.cancelled"))          return EAILiveEventType::ActionCancelled;
		if (S == TEXT("orchestrator.round_resolved")) return EAILiveEventType::OrchestratorResolved;
		if (S == TEXT("orchestrator.tick_anchor"))  return EAILiveEventType::OrchestratorTickAnchor;
		if (S == TEXT("orchestrator.tick_resolved")) return EAILiveEventType::OrchestratorTickResolved;
		if (S == TEXT("orchestrator.tick_audit"))   return EAILiveEventType::OrchestratorTickAudit;
		if (S == TEXT("system.role_assigned"))      return EAILiveEventType::SystemRoleAssigned;
		if (S == TEXT("system.agent_timeout"))      return EAILiveEventType::SystemAgentTimeout;
		if (S == TEXT("system.parse_failed"))       return EAILiveEventType::SystemParseFailed;
		if (S == TEXT("system.llm_inflight"))       return EAILiveEventType::SystemLLMInflight;
		if (S == TEXT("system.delete_executed"))    return EAILiveEventType::SystemDeleteExecuted;
		if (S == TEXT("winner_decision"))           return EAILiveEventType::WinnerDecision;
		UE_LOG(LogAILiveMemory, Warning, TEXT("EventTypeFromString: unknown '%s', defaulting to speech.public"), *S);
		return EAILiveEventType::SpeechPublic;
	}

	EAILiveSpeechActType SpeechActFromString(const FString& S)
	{
		if (S.IsEmpty())            return EAILiveSpeechActType::None;
		if (S == TEXT("claim"))     return EAILiveSpeechActType::Claim;
		if (S == TEXT("accuse"))    return EAILiveSpeechActType::Accuse;
		if (S == TEXT("defend"))    return EAILiveSpeechActType::Defend;
		if (S == TEXT("commit"))    return EAILiveSpeechActType::Commit;
		if (S == TEXT("deny"))      return EAILiveSpeechActType::Deny;
		if (S == TEXT("question"))  return EAILiveSpeechActType::Question;
		if (S == TEXT("reveal"))    return EAILiveSpeechActType::Reveal;
		UE_LOG(LogAILiveMemory, Warning, TEXT("SpeechActFromString: unknown '%s', defaulting to None"), *S);
		return EAILiveSpeechActType::None;
	}

	EAILiveCommitmentType CommitmentTypeFromString(const FString& S)
	{
		if (S == TEXT("promise"))    return EAILiveCommitmentType::Promise;
		if (S == TEXT("claim_role")) return EAILiveCommitmentType::ClaimRole;
		if (S == TEXT("deny"))       return EAILiveCommitmentType::Deny;
		if (S == TEXT("vote_for"))   return EAILiveCommitmentType::VoteFor;
		if (S == TEXT("alliance"))   return EAILiveCommitmentType::Alliance;
		UE_LOG(LogAILiveMemory, Warning, TEXT("CommitmentTypeFromString: unknown '%s', defaulting to promise"), *S);
		return EAILiveCommitmentType::Promise;
	}

	EAILiveCommitmentStatus CommitmentStatusFromString(const FString& S)
	{
		if (S == TEXT("active"))       return EAILiveCommitmentStatus::Active;
		if (S == TEXT("retracted"))    return EAILiveCommitmentStatus::Retracted;
		if (S == TEXT("contradicted")) return EAILiveCommitmentStatus::Contradicted;
		UE_LOG(LogAILiveMemory, Warning, TEXT("CommitmentStatusFromString: unknown '%s', defaulting to active"), *S);
		return EAILiveCommitmentStatus::Active;
	}

	// 把 TArray<FString> 序列化成 JSON 数组字符串。
	// 中文教学：UE 没有直接「数组 → JSON」便利函数，要先包装成 TArray<TSharedPtr<FJsonValue>>。
	// 输出形如：["NPC01","NPC02","public"]
	FString ArrayToJsonString(const TArray<FString>& A)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(A.Num());
		for (const FString& S : A)
		{
			Items.Add(MakeShared<FJsonValueString>(S));   // 每个字符串包成 JsonValue
		}
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Items, Writer);
		return Out;
	}

	TArray<FString> JsonStringToArray(const FString& Json)
	{
		TArray<FString> Out;
		if (Json.IsEmpty())
		{
			return Out;
		}
		TArray<TSharedPtr<FJsonValue>> Items;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Items))
		{
			UE_LOG(LogAILiveMemory, Warning, TEXT("JsonStringToArray: failed to parse '%s'"), *Json);
			return Out;
		}
		Out.Reserve(Items.Num());
		for (const TSharedPtr<FJsonValue>& V : Items)
		{
			if (V.IsValid() && V->Type == EJson::String)
			{
				Out.Add(V->AsString());
			}
		}
		return Out;
	}
}
