#pragma once

// Umbrella for AILiveUtil JSON / hash helpers reused by EventStore and Director
// write paths. T3 split the helpers into AILiveJsonEscape.h + AILiveSha256.h;
// this header forwards both so callers can include a single file and the
// task-card "涉及文件" inventory matches the on-disk layout.

#include "Util/AILiveJsonEscape.h"
#include "Util/AILiveSha256.h"
