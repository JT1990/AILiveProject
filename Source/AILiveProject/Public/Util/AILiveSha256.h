#pragma once

// =============================================================================
// 中文教学：AILiveSha256.h —— SHA-256 摘要工具
//
// 解决什么问题：
//   AILive 的事件存储 (AILiveEventStoreSubsystem) 用「Hash 链」串联所有事件，
//   每条新事件的 hash = SHA256(序列化文本 + 上一条事件的 hash)，类似区块链。
//   这能实现两件事：
//     1) 数据完整性校验（VerifyHashChain，T9 协议）
//     2) 防篡改（改任何一条事件，后续所有 hash 都会变）
//   本工具就是这条链的密码学原语。
//
// 为什么是 OpenSSL 而不是 UE 自带的 FSHA1：
//   UE 自带 FSHA1 / FMD5（在 Misc/SecureHash.h），但本项目刻意禁止使用 SHA-1
//   —— SHA-1 早已不安全。SHA-256 在 UE 5.x 中没有官方 API，所以直接走
//   OpenSSL 模块（在 .Build.cs 里已加入 PublicDependencyModuleNames）。
//
// C++ 知识点：
//   - 「fingerprint（指纹）」是密码学惯用术语，指对任意内容的固定长度摘要
//   - SHA-256 输出 32 字节 = 64 个小写 hex 字符。本函数返回值就是这 64 字符
// =============================================================================

#include "CoreMinimal.h"

namespace AILiveUtil
{
	/**
	 * SHA-256 over UTF-8 bytes of InText. Returns 64 lowercase hex chars.
	 * Uses OpenSSL (same algorithm as the EventStore hash chain).
	 * Always use this — SHA-1 helpers are forbidden project-wide.
	 *
	 * 中文教学：
	 *   输入: "hello"
	 *   输出: "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
	 *   - 内部先把 InText 转成 UTF-8 字节序列，再喂给 OpenSSL 的 SHA256
	 *   - 输出全小写 hex；如需大写，调用方用 .ToUpper()
	 *   - 同输入 → 同输出（确定性）；任何一字节差异 → 输出完全不同（雪崩效应）
	 */
	AILIVEPROJECT_API FString Sha256Fingerprint(const FString& InText);
}
