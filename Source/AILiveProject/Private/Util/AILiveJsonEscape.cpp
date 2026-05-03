#include "Util/AILiveJsonEscape.h"

namespace AILiveUtil
{

FString EscapeJsonString(const FString& InText)
{
	FString Out;
	Out.Reserve(InText.Len() + 2);
	Out.AppendChar(TEXT('"'));
	for (int32 i = 0; i < InText.Len(); ++i)
	{
		const TCHAR Ch = InText[i];
		switch (Ch)
		{
		case TEXT('"'):  Out += TEXT("\\\""); break;
		case TEXT('\\'): Out += TEXT("\\\\"); break;
		case TEXT('\b'): Out += TEXT("\\b");  break;
		case TEXT('\f'): Out += TEXT("\\f");  break;
		case TEXT('\n'): Out += TEXT("\\n");  break;
		case TEXT('\r'): Out += TEXT("\\r");  break;
		case TEXT('\t'): Out += TEXT("\\t");  break;
		default:
			if (Ch < 0x20)
			{
				Out += FString::Printf(TEXT("\\u%04x"), (uint32)Ch);
			}
			else
			{
				Out.AppendChar(Ch);
			}
			break;
		}
	}
	Out.AppendChar(TEXT('"'));
	return Out;
}

} // namespace AILiveUtil
