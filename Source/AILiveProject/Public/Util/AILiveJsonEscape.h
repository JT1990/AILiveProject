#pragma once

#include "CoreMinimal.h"

namespace AILiveUtil
{
	/**
	 * Returns a JSON-quoted string literal for InText (including surrounding "").
	 * Escapes \, ", control chars (\b/\f/\n/\r/\t and \u00XX for the rest).
	 * Used by AppendSystemParseFailure to safely embed arbitrary error reasons /
	 * payload snippets into a hand-built JSON payload string.
	 */
	AILIVEPROJECT_API FString EscapeJsonString(const FString& InText);
}
