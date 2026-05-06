#pragma once

// =============================================================================
// 中文教学：OpenAIChatClient.h —— 兼容 OpenAI Chat Completions 协议的 HTTP 客户端
//
// 这是什么：
//   一个轻量 HTTP 客户端，把「OpenAI 风格」的 Chat Completions 请求（messages /
//   temperature / max_tokens / response_format=json_object 等字段）发给任意兼容
//   该协议的 endpoint：DeepSeek、GLM、Qwen3 等大模型厂商都对外暴露这套接口。
//   AILive 的 Reasoner→Parser 双 LLM 串联（PRD T7）就靠这个客户端发请求。
//
// 设计选择：阻塞式调用 RequestBlocking（同步等结果返回）
//   - 在游戏线程调会卡帧 —— 所以调用方都在「GameThread 之外的线程」上跑（见
//     AILiveParserSelfCheck.cpp 的 AsyncTask + ENamedThreads::AnyBackgroundThreadNormalTask）
//   - 用阻塞接口比写 callback 简单：调用方代码线性
//
// 后台线程小词表（本项目里这三种叫法都出现过，含义都是「不在 GameThread 上跑」）：
//   - `Async(EAsyncExecution::ThreadPool, lambda)`            —— 拿 TFuture 等结果用，
//                                                              MinimaxACELibrary.cpp 用这个
//   - `AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, lambda)`
//                                                            —— TaskGraph 调度，
//                                                              AILiveParserSelfCheck.cpp 用这个
//   - `TaskGraph` / `FRunnable`                               —— 底层概念，本项目未直接用
//   选哪种取决于：是否需要等结果（Future）、是否要指定线程类别。
//
// C++ 知识点：
//   - 没有 UCLASS/USTRUCT，纯 C++ 结构体（不进 UE 反射），因为不需要在蓝图里用，
//     纯 C++ 内部使用。这种「轻量 POD」结构在 UE 项目里也很常见
//   - struct vs class 在 C++ 里只有默认可见性差别（struct 默认 public，
//     class 默认 private），其它一样
// =============================================================================

#include "CoreMinimal.h"

namespace OpenAIChat
{
	// 请求参数。FRequest 是个普通 C++ 结构体（POD），不需要 UE 反射。
	struct FRequest
	{
		FString ApiKey;                      // API 密钥（HTTP Authorization 头里 Bearer ）
		FString Endpoint;                    // 完整 URL，例如 https://api.deepseek.com/chat/completions
		FString Model;                       // 模型名，例如 "deepseek-chat" / "glm-4-air"
		FString SystemPrompt;                // system role 的指令 prompt（可空）
		FString UserPrompt;                  // user role 的用户输入
		float   Temperature = 0.7f;          // 采样温度。Parser 调用时设 0 求确定性
		int32   MaxTokens   = 800;           // 期望最长输出 token 数
		bool    bResponseFormatJson = true;  // 设 true → 请求 LLM 强制返回有效 JSON 字符串
		FString ThinkingType;                // 部分模型（如 GLM v4）支持的「thinking」字段
		float   TimeoutSec  = 60.f;          // HTTP 超时（秒）
	};

	// 调用结果。bSuccess 表示「HTTP 200 + 解析出 choices[0].message」全成功才置 true。
	struct FResult
	{
		bool    bSuccess = false;             // 总体成功标志
		int32   HttpStatus = 0;               // HTTP 状态码
		FString ErrorMessage;                 // 失败时的人话原因（用于日志/UI）
		FString RawContent;                   // choices[0].message.content（LLM 主回复）
		FString RawResponsePayload;           // 整个 HTTP 响应体（debug 用）
		FString ReasoningContent;             // choices[0].message.reasoning_content（DeepSeek/QwQ 等的「思维链」字段）
		FString FinishReason;                 // "stop" / "length" / "tool_calls" 等
		FString ParsedJson;                   // 从 RawContent 中提取的第一个 JSON 对象（见 ExtractFirstJsonObject）
		int32   PromptTokens = 0;             // 计费/统计：输入 tokens 数
		int32   CompletionTokens = 0;         // 计费/统计：输出 tokens 数
		float   LatencyMs = 0.f;              // 端到端耗时（毫秒）
		bool    bRetriedWithoutResponseFormat = false;  // 触发了「response_format=json_object 被拒 → 重试不带该字段」回退
	};

	/**
	 * 阻塞式发起一次 Chat Completions 请求；调完返回 FResult。
	 * 中文教学：这函数会原地等 HTTP 完成（用 ProcessRequestUntilComplete），
	 * 别在游戏线程直接调，会冻屏。调用方应放到后台线程或 AsyncTask 里。
	 */
	AILIVEPROJECT_API FResult RequestBlocking(const FRequest& Req);

	/**
	 * 从 LLM 输出文本里提取第一个完整 JSON 对象（带括号匹配的最小子串）。
	 * 中文教学：LLM 经常在 JSON 前后加 ```json``` markdown 围栏或解释文字，
	 * 直接 Deserialize 整段会失败。用这个函数先把 { ... } 抠出来再解析。
	 * 算法：从第一个 '{' 开始，记录嵌套深度，遇到回到 0 的 '}' 即结束；
	 * 期间过滤字符串字面量内的 { }（用 bInString 标志）。
	 */
	AILIVEPROJECT_API FString ExtractFirstJsonObject(const FString& Text);
}
