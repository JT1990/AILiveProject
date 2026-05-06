// =============================================================================
// 中文教学：AILiveAgentRoster.cpp —— 名册实现
//
// C++ 知识点：
//   1) Lambda 捕获 [&]
//      `auto Add = [&](...) { ... };`
//      `[&]` 表示「按引用捕获外部所有变量」，所以 Add 内部能直接 push 到 Roster。
//      也可以 `[&Roster]` 只捕获指定变量，更精细，但本场景写 `[&]` 即可。
//
//   2) const TCHAR* vs const FString&
//      字符串字面量 `TEXT("xxx")` 类型是 `const TCHAR*`，传字面量更省一次构造。
//      Voice 那个用 const FString& 是因为外面已经构造了 MaleVoice 变量。
//
//   3) FString::Printf(TEXT("NPC%02d"), Idx)
//      UE 版 sprintf：%02d 表示宽度 2、不足前补 0。所以 Idx=3 → "NPC03"。
//
//   4) Endpoint URL 末尾补全 /chat/completions
//      .env 里允许只写 base URL（如 https://api.deepseek.com）；这里检测尾部
//      斜杠合并 /chat/completions 路径。OpenAI 兼容协议的标准路径都是这个。
// =============================================================================

#include "LLM/AILiveAgentRoster.h"

#include "Util/ProjectEnvLoader.h"

namespace AILiveAgentRoster
{
	// 枚举 → 小写字符串。习惯：未识别值返回 "unknown"，绝不 crash。
	FString ProviderToString(ELLMProvider Provider)
	{
		switch (Provider)
		{
		case ELLMProvider::DeepSeek: return TEXT("deepseek");
		case ELLMProvider::GLM:      return TEXT("glm");
		case ELLMProvider::Qwen3:    return TEXT("qwen3");
		default:                     return TEXT("unknown");
		}
	}

	// 返回 10 个 NPC 的硬编码默认配置。
	// 中文教学：硬编码在代码里 vs 配置在 DataAsset 里 —— 项目目前选硬编码，
	// 便于代码内联调试；后续可以重构成 DataAsset 让策划在编辑器里配。
	TArray<FNPCAgentConfig> GetDefaultRoster()
	{
		const FString MaleVoice   = TEXT("male-qn-qingse");      // MiniMax TTS 男声 ID
		const FString FemaleVoice = TEXT("female-shaonv");        // MiniMax TTS 女声 ID

		TArray<FNPCAgentConfig> Roster;
		// Lambda 工厂：每次 Add 一个 NPC 配置。`[&]` 引用捕获 Roster
		auto Add = [&](int32 Idx, const TCHAR* FullName, ELLMProvider Provider,
		               const FString& Voice, const TCHAR* GenderHint)
		{
			FNPCAgentConfig C;

			C.Core.AgentId = FString::Printf(TEXT("NPC%02d"), Idx);
			C.Core.PersonaVersion = 1;
			C.Core.ModelProvider = ProviderToString(Provider);
			// Core.ModelName / CreatedAt / DeletedAt 留空：T2/T5 写库时填
			C.Core.Status = EAILiveAgentStatus::Active;

			C.Identity.FullName = FullName;
			// Identity.Nickname / Appearance 留空：DataAsset 后续填
			C.Identity.VoicePresentation = EAILiveVoicePresentation::Synthetic;
			C.Identity.Voice = Voice;

			// Battle.* 全部留默认（Faction/Role/PrivateGoal 等空字符串；BidOffset=0；SeqStart=0；bAlive=true）
			//   T2 setup phase 由 Director 决

			C.NPCIndex = Idx;
			C.NPCActorLabel = FName(*FString::Printf(TEXT("BP_NPC_MH_Character_%d"), Idx));
			C.VoicePresentationHint = GenderHint;
			C.Provider = Provider;

			Roster.Add(C);
		};

		// DeepSeek×4 → NPC 1, 2, 5, 6
		// GLM×3      → NPC 3, 7, 9
		// Qwen×3     → NPC 4, 8, 10
		Add(1,  TEXT("NPC-01"), ELLMProvider::DeepSeek, MaleVoice,   TEXT("male"));
		Add(2,  TEXT("NPC-02"), ELLMProvider::DeepSeek, FemaleVoice, TEXT("female"));
		Add(3,  TEXT("NPC-03"), ELLMProvider::GLM,      MaleVoice,   TEXT("male"));
		Add(4,  TEXT("NPC-04"), ELLMProvider::Qwen3,    MaleVoice,   TEXT("male"));
		Add(5,  TEXT("NPC-05"), ELLMProvider::DeepSeek, MaleVoice,   TEXT("male"));
		Add(6,  TEXT("NPC-06"), ELLMProvider::DeepSeek, FemaleVoice, TEXT("female"));
		Add(7,  TEXT("NPC-07"), ELLMProvider::GLM,      FemaleVoice, TEXT("female"));
		Add(8,  TEXT("NPC-08"), ELLMProvider::Qwen3,    FemaleVoice, TEXT("female"));
		Add(9,  TEXT("NPC-09"), ELLMProvider::GLM,      MaleVoice,   TEXT("male"));
		Add(10, TEXT("NPC-10"), ELLMProvider::Qwen3,    MaleVoice,   TEXT("male"));
		return Roster;
	}

	// 把 provider 枚举映射到具体 ApiKey/URL/Model。所有值从 .env 读。
	// 中文教学：DeepSeek/GLM 的 .env base URL 允许省略 /chat/completions 后缀，
	// 这里自动补全（让 .env 更短）。Qwen3 的 .env 通常已经写完整 URL（dashscope
	// 不是 OpenAI 兼容的标准路径），所以 Qwen3 分支不做后缀补全。
	FProviderEndpoint ResolveProviderEndpoint(ELLMProvider Provider)
	{
		FProviderEndpoint Out;
		switch (Provider)
		{
		case ELLMProvider::DeepSeek:
			Out.ApiKey = ProjectEnvLoader::Get(TEXT("DEEPSEEK_API_KEY"));
			Out.Endpoint = ProjectEnvLoader::Get(TEXT("DEEPSEEK_API_BASE"));
			if (!Out.Endpoint.IsEmpty() && !Out.Endpoint.Contains(TEXT("/chat/completions")))
			{
				if (Out.Endpoint.EndsWith(TEXT("/")))
				{
					Out.Endpoint += TEXT("chat/completions");
				}
				else
				{
					Out.Endpoint += TEXT("/chat/completions");
				}
			}
			Out.Model = ProjectEnvLoader::Get(TEXT("DEEPSEEK_MODEL_NAME"));
			break;
		case ELLMProvider::GLM:
			Out.ApiKey = ProjectEnvLoader::Get(TEXT("GLM_API_KEY"));
			Out.Endpoint = ProjectEnvLoader::Get(TEXT("GLM_API_BASE"));
			if (!Out.Endpoint.IsEmpty() && !Out.Endpoint.Contains(TEXT("/chat/completions")))
			{
				if (Out.Endpoint.EndsWith(TEXT("/")))
				{
					Out.Endpoint += TEXT("chat/completions");
				}
				else
				{
					Out.Endpoint += TEXT("/chat/completions");
				}
			}
			Out.Model = ProjectEnvLoader::Get(TEXT("GLM_MODEL_NAME"));
			break;
		case ELLMProvider::Qwen3:
			Out.ApiKey = ProjectEnvLoader::Get(TEXT("QWEN3_API_KEY"));
			Out.Endpoint = ProjectEnvLoader::Get(TEXT("QWEN3_API_BASE"));
			Out.Model = ProjectEnvLoader::Get(TEXT("QWEN3_MODEL_NAME"));
			break;
		default:
			break;
		}
		return Out;
	}
}
