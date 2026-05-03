#include "Util/AILiveSha256.h"

#include "Misc/SecureHash.h" // BytesToHex

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace AILiveUtil
{

FString Sha256Fingerprint(const FString& InText)
{
	const FTCHARToUTF8 Utf8(*InText);

	uint8 Digest[SHA256_DIGEST_LENGTH];
	SHA256((const unsigned char*)Utf8.Get(), (size_t)Utf8.Length(), Digest);

	return BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
}

} // namespace AILiveUtil
