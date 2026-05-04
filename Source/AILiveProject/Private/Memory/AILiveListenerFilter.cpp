#include "Memory/AILiveListenerFilter.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace ListenerFilter
{
	FString Apply(const FString& InIntendedPayloadJson, float& OutScore)
	{
		OutScore = 0.f;
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<TCHAR>> Reader =
			TJsonReaderFactory<TCHAR>::Create(InIntendedPayloadJson);
		if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
		{
			FString T;
			if (Obj->TryGetStringField(TEXT("text"), T)) return T;
		}
		return FString();
	}
}
