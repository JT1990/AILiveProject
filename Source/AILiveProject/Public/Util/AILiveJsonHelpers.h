#pragma once

// =============================================================================
// 中文教学：AILiveJsonHelpers.h —— 「umbrella header」总入口
//
// 这是什么：
//   一个不写任何代码、只 #include 别人的「伞形头文件」。如果你在某处需要
//   AILiveUtil 命名空间下的 EscapeJsonString + Sha256Fingerprint 两套工具，
//   只要 `#include "Util/AILiveJsonHelpers.h"` 一行就拿全。
//
// 为什么这样拆：
//   T3 阶段把工具拆成了 AILiveJsonEscape.h（JSON 转义） + AILiveSha256.h
//   （SHA-256 摘要）两个独立头，关注点更清晰。但调用方往往两者都要，所以
//   再提供这个 umbrella 让调用点保持简洁。
//
// C++ 知识点：
//   - #pragma once: 头文件防重复包含的现代写法（替代老式的 #ifndef X / #define X / #endif）
//   - 头文件不应该有可执行代码，只放声明 / 内联 / 模板，符合 C++ 单一定义规则（ODR）
// =============================================================================

// Umbrella for AILiveUtil JSON / hash helpers reused by EventStore and Director
// write paths. T3 split the helpers into AILiveJsonEscape.h + AILiveSha256.h;
// this header forwards both so callers can include a single file and the
// task-card "涉及文件" inventory matches the on-disk layout.

#include "Util/AILiveJsonEscape.h"  // 见 AILiveJsonEscape.h：JSON 字符串转义
#include "Util/AILiveSha256.h"      // 见 AILiveSha256.h：UTF-8 文本的 SHA-256 hex 摘要
