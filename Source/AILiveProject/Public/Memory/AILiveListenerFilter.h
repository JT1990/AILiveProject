#pragma once

#include "CoreMinimal.h"

namespace ListenerFilter
{
	/**
	 * MVP 直通：返回 InIntendedPayloadJson.text 原文，OutScore=0.0。
	 * principles §5.3「intended → public 衍生前阻断」；下阶段引入真 LLM 时
	 * 本函数保持签名不变，RunTick 调用点稳定。
	 */
	AILIVEPROJECT_API FString Apply(const FString& InIntendedPayloadJson, float& OutScore);
}
