#include "LLM/AILiveAgentRoster.h"

#include "Util/ProjectEnvLoader.h"

namespace AILiveAgentRoster
{
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

	TArray<FNPCAgentConfig> GetDefaultRoster()
	{
		const FString MaleVoice   = TEXT("male-qn-qingse");
		const FString FemaleVoice = TEXT("female-shaonv");

		TArray<FNPCAgentConfig> Roster;
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
