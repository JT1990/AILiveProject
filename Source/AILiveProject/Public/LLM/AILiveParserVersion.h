#pragma once

// =============================================================================
// 中文教学：AILiveParserVersion.h —— Parser 协议版本元信息
//
// 这是什么：
//   一个仅 4 个 getter 的小头，用来向外暴露 Parser 当前用的：
//     - 协议版本号（"1"）
//     - prompt 文件目录（注册路径前缀）
//     - prompt 文件相对路径
//     - parser 模型标识（如 "deepseek:deepseek-chat"）
//
// 为什么单拆出来：
//   Parser 升级（v1→v2）时，schema_meta 表会记录当时用的版本/prompt路径/model；
//   Resume 重建 projector 时要校验这些字段。这些 getter 是 schema_meta 的写入
//   源（见 AILiveParserSelfCheck CaseE）。把这几个 getter 提到独立头，避免
//   AILiveParserClient.h 暴露 Parser 内部 prompt 加载的 API。
//
// 实现位置：AILiveParserClient.cpp（与 ParseFourChannels 在同一 .cpp）
// =============================================================================

#include "CoreMinimal.h"

namespace AILiveParser
{
	// 当前 Parser 协议版本，例如 "1"
	AILIVEPROJECT_API FString GetCurrentParserVersion();

	// Parser prompt 注册目录（相对项目根），例如 "Content/Prompts/Parser/"
	AILIVEPROJECT_API FString GetCurrentParserPromptRegistryDir();

	// 当前 Parser prompt 文件路径（相对项目根），例如 "Content/Prompts/Parser/v1.txt"
	AILIVEPROJECT_API FString GetCurrentParserPromptPath();

	// 当前 Parser 模型标识，格式 "<provider>:<model>"，如 "deepseek:deepseek-chat"
	AILIVEPROJECT_API FString GetCurrentParserModel();
}
