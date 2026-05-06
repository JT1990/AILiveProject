#pragma once

// =============================================================================
// 中文教学：AILiveJsonEscape.h —— JSON 字符串转义工具
//
// 解决什么问题：
//   有时候我们要手工拼一段 JSON 字符串（不走 FJsonObject）。比如 EventStore
//   把错误信息 "user said: \"hi\nworld\""  原样塞进 JSON 字段时，`"`、`\`、
//   `\n` 这些字符必须按 JSON 规范转义，否则 JSON 不合法。这个函数就负责把
//   一段任意 FString 转成「带引号的 JSON 字符串字面量」。
//
// C++ / UE 知识点：
//   - `namespace AILiveUtil { ... }`：命名空间，把工具函数收纳到一起避免重名
//   - `AILIVEPROJECT_API`：跨模块导出宏，由 UBT 自动生成。Public/ 头里要被
//     其它模块用的函数都得加这个；不加会在 Editor 里报「unresolved external
//     symbol」链接错误。命名规则是 `<模块大写>_API`。
//   - `const FString&`：常量引用传参，避免拷贝整串文本（FString 其实底层是
//     共享引用计数，但写成 const& 仍然是惯用法）。
// =============================================================================

#include "CoreMinimal.h"  // FString / TArray / 基础容器都在这个伞形头里

namespace AILiveUtil
{
	/**
	 * Returns a JSON-quoted string literal for InText (including surrounding "").
	 * Escapes \, ", control chars (\b/\f/\n/\r/\t and \u00XX for the rest).
	 * Used by AppendSystemParseFailure to safely embed arbitrary error reasons /
	 * payload snippets into a hand-built JSON payload string.
	 *
	 * 中文教学：
	 *   输入 hello "world"\n   → 输出 "hello \"world\"\n"（注意：返回值已含外层双引号）
	 *   - 反斜杠 \ 转成 \\
	 *   - 双引号 " 转成 \"
	 *   - 控制字符（< 0x20）转成 \uXXXX 形式
	 *   特殊：本函数不处理 UTF-16 代理对，对一般中文/emoji 是直通的（FString
	 *   内部是 UTF-16，TCHAR 等于一个码元，足够多数场景使用）。
	 */
	AILIVEPROJECT_API FString EscapeJsonString(const FString& InText);
}
