# T15 — 出牌 / 拿枪 Montage + Notify 钩子

## 目标
给骗子酒馆加表演动画桥接：出牌时 NPC 做出"放牌"动作 / Reveal 阶段做"举枪"动作 / Roulette 失败者倒地或惊愕。**polish 任务，不影响游戏机制**。

## 前置
T14（M2 跑通）

## DoD
- [ ] 用引擎 / GASP / MetaHuman 自带 montage 选取 3 个动作（不自制动画）：
  - `AM_PlaceCardOnTable`（出牌：单手向桌面伸 + 收）— 选最接近的 GASP montage
  - `AM_TableSlam_Or_Point`（质疑：拍桌 / 指向）
  - `AM_HoldGesture` 或 `AM_Stagger`（轮盘失败：踉跄）
- [ ] **接口化（不直接 Cast 调 BP function）**：新增 `IMindPerformableInterface`：
  ```cpp
  UINTERFACE(MinimalAPI, Blueprintable)
  class UMindPerformableInterface : public UInterface { GENERATED_BODY() };
  class IMindPerformableInterface {
      GENERATED_BODY()
  public:
      UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="AI Live")
      void PlayMindMontage(UAnimMontage* Montage, FName Section);
  };
  ```
- [ ] `BP_NPC_MH_Character`（父类）实现该接口，BP 内 `PlayMontage` 即可
- [ ] C++ Action 通过 `IMindPerformableInterface::Execute_PlayMindMontage(Actor, Montage, Section)` 调用，避免直接 Cast/调 BP function
- [ ] `UMindAction_PlayCards::Execute` 加：Speak 之前 `Owner->GetOwner()->PlayMindMontage(AM_PlaceCardOnTable)`
- [ ] `UMindAction_Challenge::Execute` 加：`PlayMindMontage(AM_TableSlam_Or_Point)`
- [ ] GM 在 `Phase_Roulette` 命中后给该 NPC 调 `PlayMindMontage(AM_Stagger)`
- [ ] Montage notify（BPCall 或 NotifyEnd）→ 触发 `Done` 回调（让 LLM 等动作完成再下一步）

## 关键文件
- 修改 `BP_NPC_MH_Character`（父类，影响所有 8 NPC）
- 修改 `MindAction_PlayCards.cpp` / `MindAction_Challenge.cpp`
- 修改 `MindGameMaster_LiarsBar.cpp`（Phase_Roulette）
- DataAsset 引用：可在 `DA_GameConfig_LiarsBar` 加 `TSoftObjectPtr<UAnimMontage>` 字段，不硬编码路径

## 关键 API / 伪代码

```cpp
// BP_NPC_MH_Character (父类) — 用 Monolith MCP 加 BP function
UFUNCTION(BlueprintCallable)
void PlayMindMontage(UAnimMontage* Montage, FName Section = NAME_None);
// 实现：Mesh->GetAnimInstance()->Montage_Play(Montage); 可选 JumpToSection
```

```cpp
// MindAction_PlayCards::Execute 改造
{
    // 1. 播 montage
    if (auto* MontagePicker = Owner->Config->MontagePicker.LoadSynchronous()) {
        // 简化：直接通过 GameConfig DataAsset 拿 Montage
    }
    Owner->GetOwner()->PlayMindMontage(GameConfig->PlaceCardMontage);

    // 2. Speak（已有）
    // 3. Done（已有）
}
```

## 验收信号
- 跑一局骗子酒馆，能看到 NPC 出牌时手伸向桌面（即使桌上没有真的扑克牌物件）
- Challenge 时 NPC 拍桌或指向出牌人
- Roulette 失败时 NPC 踉跄
- 没有破坏 M2 的游戏推进
- 动画异常 / Montage 找不到时能 graceful fallback（无动画也能跑）

## 不在范围
- 真实扑克牌 mesh（不做）
- 手枪道具 mesh（不做）
- 复杂动画状态机调整（保持 Override Slot）

## 风险
- GASP 的 motion matching 与 Montage 同时播会冲突——Montage 走 UpperBody slot，下半身保持 motion matching
- 动画选取依赖 GASP 自带资源，找不到合适的就 fallback 用引擎 default Mannequin 动画
- Montage 长度可能与 Speak TTS 时长不同步——M3 不强求精确同步，能看到动作即可
