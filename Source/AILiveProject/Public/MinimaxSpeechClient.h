#pragma once

// =============================================================================
// 中文教学：MinimaxSpeechClient.h —— MiniMax「文本转语音 (TTS)」HTTP 客户端
//
// 这是什么：
//   一个轻量阻塞式 HTTP 客户端，把一段文字发给 MiniMax 的 t2a_v2 接口，
//   返回 16-bit PCM 音频样本。供 MinimaxACELibrary 在后台线程调用。
//
// 设计与 OpenAIChatClient 类似：
//   - 不进 UE 反射（纯 C++ struct + 命名空间函数）
//   - 阻塞接口（ProcessRequestUntilComplete）
//   - 调用方负责丢后台线程
//
// PCM 音频常识（教学）：
//   - 16-bit signed PCM：每个采样点是 int16（-32768 ~ 32767）
//   - SampleRate（采样率）：每秒多少个采样点。16000 Hz 是语音 TTS 常用值（电话品质）
//   - Mono（单声道）：NumChannels = 1
//   - Duration = Samples.Num() / SampleRate
//   - 数据流向：MiniMax 返回 hex 字符串 → 解码成 byte[] → 重解释成 int16[]
// =============================================================================

#include "CoreMinimal.h"

namespace MinimaxSpeech
{
	// 请求参数。中文教学：默认值都是 MiniMax 当前推荐值，调用方一般只填
	// ApiKey + Text + VoiceId 三项。
	struct FRequest
	{
		FString ApiKey;                                                       // 凭证（Authorization: Bearer ...）
		FString Text;                                                         // 要合成的文字（UTF-8 中文支持）
		FString VoiceId   = TEXT("male-qn-qingse");                           // 声线 ID，MiniMax 平台预设
		FString Model     = TEXT("speech-2.8-turbo");                         // 模型版本
		FString Endpoint  = TEXT("https://api.minimaxi.com/v1/t2a_v2");
		FString LanguageBoost = TEXT("Chinese");                              // 提示语言，让识别器更准
		int32   SampleRate = 16000;                                           // 采样率
		float   Speed      = 1.f;                                             // 语速倍率
		float   Volume     = 1.f;                                             // 音量
		int32   Pitch      = 0;                                               // 音高调整
	};

	struct FResult
	{
		bool          bSuccess = false;
		FString       ErrorMessage;
		FString       TraceId;            // MiniMax 平台返回的 trace ID（出问题报障用）
		TArray<int16> Samples;            // PCM16 采样点数组
		int32         SampleRate  = 0;
		int32         NumChannels = 1;
		float         DurationSec = 0.f;  // 音频长度（秒）
	};

	// 阻塞式调用。中文教学：内部 HTTP POST → 等响应 → 解析 JSON 取 hex audio →
	// 解码 hex 成 byte[] → 重解释成 int16[]。⚠️ 不要在游戏线程调。
	AILIVEPROJECT_API FResult RequestBlocking(const FRequest& Req);
}
