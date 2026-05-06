#pragma once

// =============================================================================
// 中文教学：AILiveAgent.h —— Agent 标记接口（marker interface）
//
// 这是什么：
//   一个空接口（interface）—— 不定义任何方法，仅作「类型标记」。Actor 实现这个
//   接口表明「我是一个 AI Live Agent，请把我当一等公民对待」。
//   将来感知/记忆/社交图谱系统会查询「实现 IAILiveAgent 的 actor」做特殊处理。
//
// 关键 UE 概念 ——「双类」接口模式：
//
//   UE 的 UInterface 模式很特殊。每个接口要写两个类：
//     - UAILiveAgent : public UInterface   ← UE 反射用，必须叫 U 前缀
//     - IAILiveAgent                       ← 实际接口契约，必须叫 I 前缀
//
//   为什么？UE 反射系统只识别 UObject 派生类，但 C++ 多重继承 UObject 会
//   有钻石问题。所以解决方案是：UInterface 派生类只是「反射占位」，真正的
//   方法签名定义在与之并列的 IInterface 类里。Actor 多重继承 AActor + IAILiveAgent
//   即可（IAILiveAgent 不是 UObject，不会有钻石问题）。
//
//   实现方：
//     class AMyNPC : public AActor, public IAILiveAgent { ... };
//
//   检查实现：
//     if (Actor->Implements<UAILiveAgent>()) { ... }     // 注意是 U 前缀
//     IAILiveAgent* IFace = Cast<IAILiveAgent>(Actor);   // 然后用 I 前缀做调用
//
//   UINTERFACE(BlueprintType, Blueprintable)
//     BlueprintType：BP 变量可以是这个类型
//     Blueprintable：BP 类可以实现这个接口
// =============================================================================

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "AILiveAgent.generated.h"

// UE 反射占位类。实例化它没意义；只是让 UHT 生成必要的元数据。
UINTERFACE(BlueprintType, Blueprintable)
class AILIVEPROJECT_API UAILiveAgent : public UInterface
{
	GENERATED_BODY()
};

/**
 * Marker interface for AI Live agents. Implementing actors become first-class
 * citizens in perception, memory and social-graph systems. The interface is
 * intentionally empty for now; future milestones will add team / identity hooks.
 *
 * 中文教学：实际接口契约。Actor 多重继承它即获得「AI Live agent」标签身份。
 * 当前空 ——后续里程碑会加 team / identity 等查询方法。
 */
class AILIVEPROJECT_API IAILiveAgent
{
	GENERATED_BODY()
};
