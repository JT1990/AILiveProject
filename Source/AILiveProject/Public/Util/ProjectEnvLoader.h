#pragma once

// =============================================================================
// 中文教学：ProjectEnvLoader.h —— 「.env 文件」读取器
//
// 这是什么：
//   开发期为了方便，本项目允许在工程根目录放一个 `.env` 文本文件，每行
//   `key=value` 形式存放配置（最典型的就是 MiniMax API Key）。本工具加载并
//   缓存这些键值对，供其它代码用 `ProjectEnvLoader::Get("minimax")` 取值。
//
//   📂 文件位置：D:\Project\Unreal\AILiveProject\.env
//   📝 示例内容：
//        minimax=eyJhbG...
//        # 这是注释
//        openai=sk-xxx
//
// 注意（CLAUDE.md 也强调）：
//   仅供本地开发使用。生产应该走 UDeveloperSettings 或某个 secret store。
//   .env 文件不要 commit 到 git（项目应有 .gitignore 排除规则）。
//
// 接口说明：
//   - Get(Key)  : 读取一项；不存在返回空 FString。键名大小写不敏感。
//   - Reload()  : 清缓存，下次 Get 会重新读盘。改完 .env 想立即生效就调这个。
// =============================================================================

#include "CoreMinimal.h"

namespace ProjectEnvLoader
{
	AILIVEPROJECT_API FString Get(const FString& Key);  // 拿值；找不到返回 ""
	AILIVEPROJECT_API void Reload();                    // 强制重新读盘
}
