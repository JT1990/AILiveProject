#pragma once

#include "CoreMinimal.h"

namespace AILiveUtil
{
	/**
	 * SHA-256 over UTF-8 bytes of InText. Returns 64 lowercase hex chars.
	 * Uses OpenSSL EVP_sha256 (same algorithm as the EventStore hash chain).
	 * SHA-1 is forbidden project-wide; never reach for FSHA1.
	 */
	AILIVEPROJECT_API FString Sha256Fingerprint(const FString& InText);
}
