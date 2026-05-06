// =============================================================================
// 中文教学：AILiveSha256.cpp —— SHA-256 摘要的实现
//
// C++ / UE 知识点：
//   1) THIRD_PARTY_INCLUDES_START / _END
//      第三方库（OpenSSL、SQLite 等）的头文件经常用到 UE 自家代码不允许的写法
//      （比如 reset 宏定义、特殊 warning 等）。这对宏临时关闭 UE 的严格警告
//      检查；包完之后再恢复。这是 UE 与 C 库交互的标准模式。
//
//   2) FTCHARToUTF8
//      UE 内部 FString 是 UTF-16，但密码学算法（SHA-256）按字节工作，所以
//      需要先转成 UTF-8 字节流。FTCHARToUTF8 是 UE 提供的一次性转换器：
//        FTCHARToUTF8 Utf8(*InText);  // 构造时即转换
//        Utf8.Get()                   // 拿 const ANSICHAR* (= const char*)
//        Utf8.Length()                // 拿字节长度
//      它在栈上分配缓冲（短串）或堆分配（长串），构造体作用域结束自动释放。
//
//   3) `*InText` 解引用 FString → const TCHAR*
//      FString 重载了 `operator*` 拿到内部 C 字符串指针，等价于 InText.GetCharArray().GetData()
//
//   4) BytesToHex(p, len)
//      UE 工具函数（在 Misc/SecureHash.h 里），把 byte 数组转成大写 hex
//      字符串，然后 .ToLower() 转小写以匹配 EventStore 协议约定。
// =============================================================================

#include "Util/AILiveSha256.h"

#include "Misc/SecureHash.h" // BytesToHex

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>     // 提供 SHA256 函数和 SHA256_DIGEST_LENGTH 常量（=32）
THIRD_PARTY_INCLUDES_END

namespace AILiveUtil
{

FString Sha256Fingerprint(const FString& InText)
{
	// 步骤 1：把 UTF-16 文本转成 UTF-8 字节流。SHA 算法不关心编码，只看字节。
	const FTCHARToUTF8 Utf8(*InText);

	// 步骤 2：调 OpenSSL 一次性接口算摘要。Digest 是 32 字节固定输出。
	uint8 Digest[SHA256_DIGEST_LENGTH];  // SHA256_DIGEST_LENGTH 来自 openssl/sha.h，值为 32
	SHA256((const unsigned char*)Utf8.Get(), (size_t)Utf8.Length(), Digest);

	// 步骤 3：32 字节摘要 → 64 字符的小写 hex 字符串
	return BytesToHex(Digest, SHA256_DIGEST_LENGTH).ToLower();
}

} // namespace AILiveUtil
