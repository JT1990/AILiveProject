// =============================================================================
// 中文教学：AILiveJsonEscape.cpp —— EscapeJsonString 的实现
//
// 阅读这个文件之前，先看 AILiveJsonEscape.h 的教学注释了解输入/输出。
//
// C++ 知识点：
//   - `Out.Reserve(N)`：提前分配 N 个 TCHAR 的空间，避免逐次扩容拷贝（性能优化）
//   - `TEXT("...")`：UE 宏，根据平台决定是 char 还是 wchar_t 字面量；FString
//     内部是 UTF-16，所以这里所有字面量都加 TEXT 包一层
//   - `FString::Printf(TEXT("\\u%04x"), value)`：UE 版 sprintf，类型安全
//   - `static_cast`/`(uint32)Ch`：C 风格强转。UE 老代码这么写很常见；新代码
//     更建议用 static_cast<uint32>(Ch)
// =============================================================================

#include "Util/AILiveJsonEscape.h"

namespace AILiveUtil
{

FString EscapeJsonString(const FString& InText)
{
	FString Out;
	Out.Reserve(InText.Len() + 2);  // +2 给两端引号占位，提前分配避免反复 realloc
	Out.AppendChar(TEXT('"'));      // 输出最左侧引号
	for (int32 i = 0; i < InText.Len(); ++i)
	{
		const TCHAR Ch = InText[i];
		switch (Ch)
		{
		// JSON 规范要求转义的 7 个字符：双引号、反斜杠、\b、\f、\n、\r、\t
		// 注：以下行尾不能写「反斜杠+引号」做示例 —— 因为 // 行注释中行尾的反斜杠
		// 会被 C++ 解析为「行继续符（line continuation）」，把下一行吃进注释，触发 C4010。
		case TEXT('"'):  Out += TEXT("\\\""); break;  // 双引号转义
		case TEXT('\\'): Out += TEXT("\\\\"); break;  // 反斜杠转义
		case TEXT('\b'): Out += TEXT("\\b");  break;  // 退格
		case TEXT('\f'): Out += TEXT("\\f");  break;  // 换页
		case TEXT('\n'): Out += TEXT("\\n");  break;  // 换行
		case TEXT('\r'): Out += TEXT("\\r");  break;  // 回车
		case TEXT('\t'): Out += TEXT("\\t");  break;  // 制表符 Tab
		default:
			if (Ch < 0x20)
			{
				// 其它 ASCII 控制字符（0x00 ~ 0x1F 中未列入上面的）→ \u00XX
				Out += FString::Printf(TEXT("\\u%04x"), (uint32)Ch);
			}
			else
			{
				// 普通字符（含中文、emoji 等高位字符）原样输出
				Out.AppendChar(Ch);
			}
			break;
		}
	}
	Out.AppendChar(TEXT('"'));      // 输出最右侧引号，闭合
	return Out;
}

} // namespace AILiveUtil
