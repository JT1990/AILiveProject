// =============================================================================
// 中文教学：AILiveListenerFilter.cpp —— MVP 直通实现
//
// 现在干的事：
//   1) 解析 InIntendedPayloadJson 为 FJsonObject
//   2) 取出 "text" 字段返回
//   3) OutScore 永远 0.0
//
// 后续接入真 LLM 时，会改成：
//   1) 把 intended.text 喂给一个独立的 listener LLM
//   2) listener 返回过滤后版本 + safety score
//   3) 写回 OutScore
// =============================================================================

#include "Memory/AILiveListenerFilter.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace ListenerFilter
{
	FString Apply(const FString& InIntendedPayloadJson, float& OutScore)
	{
		OutScore = 0.f;     // MVP 永远 0；后续接 LLM 时会写真分数
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<TCHAR>> Reader =
			TJsonReaderFactory<TCHAR>::Create(InIntendedPayloadJson);
		// JSON 解析成功且能取到 "text" 字段 → 返回；否则空串
		if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
		{
			FString T;
			if (Obj->TryGetStringField(TEXT("text"), T)) return T;
		}
		return FString();
	}
}
