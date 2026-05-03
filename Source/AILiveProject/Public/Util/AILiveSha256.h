#pragma once

#include "CoreMinimal.h"

namespace AILiveUtil
{
	/**
	 * SHA-256 over UTF-8 bytes of InText. Returns 64 lowercase hex chars.
	 * Uses OpenSSL (same algorithm as the EventStore hash chain).
	 * Always use this — SHA-1 helpers are forbidden project-wide.
	 */
	AILIVEPROJECT_API FString Sha256Fingerprint(const FString& InText);
}
