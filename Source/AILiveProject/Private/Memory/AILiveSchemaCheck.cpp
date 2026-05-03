#include "Memory/AILiveAgentTypes.h"
#include "Memory/AILiveEventTypes.h"
#include "LLM/AILiveAgentRoster.h"

#include "Containers/Set.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"

namespace
{
	enum class EExpectedKind : uint8
	{
		StrProperty,
		IntProperty,
		EnumVoicePresentation,
	};

	struct FFieldMapping
	{
		const TCHAR* YamlPath;
		UScriptStruct* (*StructGetter)();
		const TCHAR* StructFieldName;
		EExpectedKind Kind;
	};

	static UScriptStruct* GetCoreStruct()     { return FAILiveAgentCore::StaticStruct(); }
	static UScriptStruct* GetIdentityStruct() { return FAILiveAgentIdentity::StaticStruct(); }

	// schema.yaml 当前 9 个 __storage: asset 字段的显式映射表。
	// schema.yaml 增删 asset 字段时同步更新本表 + 对应 USTRUCT。
	static const FFieldMapping kFieldMappings[] = {
		{ TEXT("agent.agent_id"),              &GetCoreStruct,     TEXT("AgentId"),           EExpectedKind::StrProperty },
		{ TEXT("agent.persona_version"),       &GetCoreStruct,     TEXT("PersonaVersion"),    EExpectedKind::IntProperty },
		{ TEXT("agent.model_provider"),        &GetCoreStruct,     TEXT("ModelProvider"),     EExpectedKind::StrProperty },
		{ TEXT("agent.model_name"),            &GetCoreStruct,     TEXT("ModelName"),         EExpectedKind::StrProperty },
		{ TEXT("identity.full_name"),          &GetIdentityStruct, TEXT("FullName"),          EExpectedKind::StrProperty },
		{ TEXT("identity.nickname"),           &GetIdentityStruct, TEXT("Nickname"),          EExpectedKind::StrProperty },
		{ TEXT("identity.voice_presentation"), &GetIdentityStruct, TEXT("VoicePresentation"), EExpectedKind::EnumVoicePresentation },
		{ TEXT("identity.voice"),              &GetIdentityStruct, TEXT("Voice"),             EExpectedKind::StrProperty },
		{ TEXT("identity.appearance"),         &GetIdentityStruct, TEXT("Appearance"),        EExpectedKind::StrProperty },
	};

	bool IsIdentChar(TCHAR C)
	{
		return FChar::IsAlpha(C) || FChar::IsDigit(C) || C == TEXT('_');
	}

	TSet<FString> CollectExpectedAssetFields()
	{
		TSet<FString> Out;
		const FString YamlPath = FPaths::ProjectDir() / TEXT("Docs/schema.yaml");
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *YamlPath))
		{
			UE_LOG(LogAILiveMemory, Error, TEXT("[SchemaCheck] cannot load %s"), *YamlPath);
			return Out;
		}

		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);

		FString CurSection;
		for (const FString& Raw : Lines)
		{
			if (Raw.IsEmpty())
			{
				continue;
			}

			// 顶层段名：列 0 起 [a-z_][a-z0-9_]*: ...
			if (!FChar::IsWhitespace(Raw[0]))
			{
				int32 ColonIdx = INDEX_NONE;
				if (Raw.FindChar(TEXT(':'), ColonIdx) && ColonIdx > 0)
				{
					bool bValid = true;
					for (int32 i = 0; i < ColonIdx; ++i)
					{
						if (!IsIdentChar(Raw[i]))
						{
							bValid = false;
							break;
						}
					}
					if (bValid)
					{
						CurSection = Raw.Left(ColonIdx);
					}
				}
				continue;
			}

			// 字段行：恰好 2 个前导空格（排除 4+ 空格的嵌套 type/enum 描述）
			if (Raw.Len() < 3 || Raw[0] != TEXT(' ') || Raw[1] != TEXT(' ') || Raw[2] == TEXT(' '))
			{
				continue;
			}

			if (CurSection != TEXT("agent") && CurSection != TEXT("identity"))
			{
				continue;
			}

			if (!Raw.Contains(TEXT("__storage: asset")))
			{
				continue;
			}

			const FString Body = Raw.Mid(2);
			int32 ColonIdx = INDEX_NONE;
			if (!Body.FindChar(TEXT(':'), ColonIdx) || ColonIdx <= 0)
			{
				continue;
			}
			const FString FieldName = Body.Left(ColonIdx);
			bool bValidIdent = !FieldName.IsEmpty();
			for (TCHAR C : FieldName)
			{
				if (!IsIdentChar(C))
				{
					bValidIdent = false;
					break;
				}
			}
			if (!bValidIdent)
			{
				continue;
			}

			Out.Add(CurSection + TEXT(".") + FieldName);
		}
		return Out;
	}

	bool MatchExpectedKind(FProperty* P, EExpectedKind Kind)
	{
		if (!P)
		{
			return false;
		}
		switch (Kind)
		{
		case EExpectedKind::StrProperty:
			return P->IsA<FStrProperty>();
		case EExpectedKind::IntProperty:
			return P->IsA<FIntProperty>();
		case EExpectedKind::EnumVoicePresentation:
			if (FEnumProperty* EP = CastField<FEnumProperty>(P))
			{
				return EP->GetEnum() == StaticEnum<EAILiveVoicePresentation>();
			}
			return false;
		}
		return false;
	}

	const TCHAR* ExpectedKindName(EExpectedKind Kind)
	{
		switch (Kind)
		{
		case EExpectedKind::StrProperty:           return TEXT("FStrProperty");
		case EExpectedKind::IntProperty:           return TEXT("FIntProperty");
		case EExpectedKind::EnumVoicePresentation: return TEXT("FEnumProperty<EAILiveVoicePresentation>");
		}
		return TEXT("?");
	}

	void RunSchemaCheck()
	{
		UE_LOG(LogAILiveMemory, Display, TEXT("[SchemaCheck] starting"));

		const TSet<FString> Expected = CollectExpectedAssetFields();
		UE_LOG(LogAILiveMemory, Display, TEXT("[SchemaCheck] schema.yaml asset fields collected: %d"), Expected.Num());

		TSet<FString> MappingKeys;
		for (const FFieldMapping& M : kFieldMappings)
		{
			MappingKeys.Add(M.YamlPath);
		}

		int32 FailCount = 0;
		int32 OkCount = 0;

		// (a) expected ⊆ mapping —— schema 新增字段忘加映射
		for (const FString& E : Expected)
		{
			if (!MappingKeys.Contains(E))
			{
				UE_LOG(LogAILiveMemory, Error, TEXT("[SchemaCheck] schema asset field '%s' missing in mapping table"), *E);
				++FailCount;
			}
		}

		// (b) mapping ⊆ expected —— stale mapping（schema 删/改字段后旧映射残留）
		for (const FFieldMapping& M : kFieldMappings)
		{
			const FString Key(M.YamlPath);
			if (!Expected.Contains(Key))
			{
				UE_LOG(LogAILiveMemory, Error,
					TEXT("[SchemaCheck] mapping entry '%s' not in current schema asset set (stale mapping)"),
					*Key);
				++FailCount;
			}
		}

		// (c) mapping → reflection
		for (const FFieldMapping& M : kFieldMappings)
		{
			UScriptStruct* SS = M.StructGetter();
			FProperty* P = FindFProperty<FProperty>(SS, M.StructFieldName);
			if (!P)
			{
				UE_LOG(LogAILiveMemory, Error, TEXT("[SchemaCheck] USTRUCT field '%s::%s' missing"),
					*SS->GetName(), M.StructFieldName);
				++FailCount;
				continue;
			}
			if (!MatchExpectedKind(P, M.Kind))
			{
				UE_LOG(LogAILiveMemory, Error,
					TEXT("[SchemaCheck] USTRUCT field '%s::%s' type mismatch (expected %s)"),
					*SS->GetName(), M.StructFieldName, ExpectedKindName(M.Kind));
				++FailCount;
				continue;
			}
			++OkCount;
		}

		UE_LOG(LogAILiveMemory, Display,
			TEXT("[SchemaCheck] OK count=%d FAILED count=%d"), OkCount, FailCount);
	}

	void RunRosterDualTrackCheck()
	{
		UE_LOG(LogAILiveMemory, Display, TEXT("[DualTrack] starting"));
		const TArray<FNPCAgentConfig> Roster = AILiveAgentRoster::GetDefaultRoster();
		int32 FailCount = 0;
		for (const FNPCAgentConfig& C : Roster)
		{
			const FString Expected = AILiveAgentRoster::ProviderToString(C.Provider);
			if (C.Core.ModelProvider != Expected)
			{
				UE_LOG(LogAILiveMemory, Error,
					TEXT("[DualTrack] NPC%d drift: Core.ModelProvider='%s' expected '%s'"),
					C.NPCIndex, *C.Core.ModelProvider, *Expected);
				++FailCount;
			}
		}
		UE_LOG(LogAILiveMemory, Display,
			TEXT("[DualTrack] OK count=%d FAILED count=%d"),
			Roster.Num() - FailCount, FailCount);
	}
}

static FAutoConsoleCommand CCmdSchemaMapping(
	TEXT("AILive.CheckSchemaMapping"),
	TEXT("Validate Memory/AILiveAgentTypes USTRUCT vs schema.yaml asset fields."),
	FConsoleCommandDelegate::CreateStatic(&RunSchemaCheck));

static FAutoConsoleCommand CCmdRosterDualTrack(
	TEXT("AILive.CheckRosterDualTrack"),
	TEXT("Verify FNPCAgentConfig.Core.ModelProvider == ProviderToString(Provider) for default roster."),
	FConsoleCommandDelegate::CreateStatic(&RunRosterDualTrackCheck));
