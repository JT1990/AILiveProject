#include "Mind/Actions/MindAction_Speak.h"

#include "Mind/MindComponent.h"
#include "Mind/MindAgentConfig.h"
#include "Mind/MindSpeechHelpers.h"
#include "Mind/MindLog.h"
#include "MinimaxACELibrary.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

UMindAction_Speak::UMindAction_Speak()
{
	ActionName = TEXT("speak");
	Description = TEXT("说一句话（TTS + A2F 口型）");
	ParamSchemaJson = TEXT(R"({"text":"string"})");
}

void UMindAction_Speak::Execute(UMindComponent* Owner, const FString& ParamsJson, FOnActionDone Done)
{
	if (!IsValid(Owner) || !IsValid(Owner->Config))
	{
		Done.ExecuteIfBound(false, TEXT("invalid owner/config"));
		return;
	}

	FString Text;
	{
		TSharedPtr<FJsonObject> Params;
		const auto Reader = TJsonReaderFactory<>::Create(ParamsJson);
		if (!FJsonSerializer::Deserialize(Reader, Params) || !Params.IsValid()
			|| !Params->TryGetStringField(TEXT("text"), Text))
		{
			UE_LOG(LogMindAction, Warning, TEXT("Speak: invalid params: %s"), *ParamsJson);
			Done.ExecuteIfBound(false, TEXT("invalid params"));
			return;
		}
	}
	if (Text.TrimStartAndEnd().IsEmpty())
	{
		UE_LOG(LogMindAction, Warning, TEXT("Speak: empty text"));
		Done.ExecuteIfBound(false, TEXT("empty text"));
		return;
	}

	AActor* Speaker = UMindSpeechHelpers::ResolveSpeechActor(Owner->GetOwner());
	if (!Speaker)
	{
		UE_LOG(LogMindAction, Warning, TEXT("Speak: ResolveSpeechActor returned null"));
		Done.ExecuteIfBound(false, TEXT("no speech actor"));
		return;
	}

	const FString ApiKey = UMinimaxACELibrary::GetMinimaxApiKeyFromProjectEnv();
	if (ApiKey.IsEmpty())
	{
		UE_LOG(LogMindAction, Warning, TEXT("Speak: minimax api key missing"));
		Done.ExecuteIfBound(false, TEXT("no api key"));
		return;
	}
	const FString Voice = Owner->Config->MinimaxVoiceId;
	const FName   Prov  = Owner->Config->A2FProviderName;

	UMinimaxACELibrary::TriggerMinimaxSpeech(
		Owner,                                          // WorldContextObject
		Speaker,                                        // AActor* (可见 MetaHuman child)
		Text,                                           // FString —— 不允许 FText::FromString
		ApiKey,
		Voice.IsEmpty() ? TEXT("male-qn-qingse") : Voice,
		TEXT("https://api.minimaxi.com/v1/t2a_v2"),     // 显式默认；不允许 {} 空字串
		Prov);

	// TTS fire-and-forget；MVP 接受 Done(true) 立即返回
	Done.ExecuteIfBound(true, FString::Printf(TEXT("said: %s"), *Text.Left(40)));
}
