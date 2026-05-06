#pragma once

// =============================================================================
// 中文教学：AILiveListenerFilter.h —— 「intended → public」衍生前的过滤层
//
// 设计意图（来自 principles §5.3）：
//   抢麦赢家把 speech.intended 转换成 speech.public 之前，要做「礼貌/合规」过滤：
//   把 intended 文本经过一个独立模型再润色一遍（去除攻击性、合规拒绝等）。
//   MVP 阶段不接入真 LLM，本函数直通：原样返回 text。后续接入 LLM 时签名不变，
//   RunTick 主循环代码不动，只换实现。
//
// 函数签名细节：
//   Apply(InIntendedPayloadJson, OutScore)
//     输入：完整 intended 事件 payload JSON（含 text / intended_action / addressed_to_hint）
//     输出：返回值是过滤后 text；OutScore（输出参数）是分数（MVP 总是 0.0）
//
// C++ 知识点：
//   - 输出参数 `float& OutScore`：通过引用让函数额外返回一个值。
//     现代 C++ 更喜欢 `std::tuple<FString, float>` 或 `TPair<FString, float>`，
//     但 UE 老风格惯用引用输出参数（更明确、避免临时对象）
// =============================================================================

#include "CoreMinimal.h"

namespace ListenerFilter
{
	/**
	 * MVP 直通：返回 InIntendedPayloadJson.text 原文，OutScore=0.0。
	 * principles §5.3「intended → public 衍生前阻断」；下阶段引入真 LLM 时
	 * 本函数保持签名不变，RunTick 调用点稳定。
	 *
	 * 中文教学：注意 OutScore 是「输出参数」，调用方需要先声明再传引用：
	 *     float Score = 0.f;
	 *     FString Public = ListenerFilter::Apply(IntendedJson, Score);
	 */
	AILIVEPROJECT_API FString Apply(const FString& InIntendedPayloadJson, float& OutScore);
}
