---
name: a2f-metahuman-contract
description: Audio2Face / MetaHuman 角色集成的硬契约 — 每个 A2F 驱动的角色必须同时具备 UACEAudioCurveSourceComponent 与 Face AnimBP 中的 ApplyACEAnimation 节点，否则音频播但脸不动（静默失败）。Triggers on A2F, Audio2Face, MetaHuman 嘴型/口型/lip-sync, TTS, ACE, MinimaxSpeech, TriggerMinimaxSpeech, UACEAudioCurveSourceComponent, ApplyACEAnimation, Face AnimBP, PrewarmA2F, 新增 A2F 角色, 口型不同步, 嘴巴不动 排查. 改动 A2F / MetaHuman / TTS 链路、新挂 A2F 角色、或排查口型不同步时务必加载本 skill。
---

# A2F 角色契约

每个 A2F 驱动的角色**两样都必须有**：

1. 可见 skeletal mesh actor 上有 `UACEAudioCurveSourceComponent`（`UMinimaxACELibrary::TriggerMinimaxSpeech` 会自动挂）。
2. **Face AnimBP 里有 `ApplyACEAnimation` 节点**，从该组件读 curve。

缺节点则音频播但脸不动，这是静默失败模式。口型不同步时优先查这里。

## 相关 C++ 入口（参考）

详细签名见 `CLAUDE.md` 的 "C++ glue 层" 段：

- `UMinimaxACELibrary::TriggerMinimaxSpeech` — 当前唯一 BP TTS 入口，典型延迟 1–4 s。
- `UMinimaxACELibrary::PrewarmA2F` — BeginPlay 调一次，避免首次调用时的 TRT 编译延迟。
