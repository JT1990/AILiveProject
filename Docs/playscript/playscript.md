游戏发生在一个监狱内，共有10个AI角色，在关卡大纲中的对象名为：`BP_NPC_MH_CharACTer_1`~`BP_NPC_MH_CharACTer_10`，在以下对话中简称为 `NPC1`~`NPC10`。

## 规则介绍：

`ACT*` 是需要编排的
`SKIP*` 是已知应有逻辑，但是在编排时跳过。`SKIP*` 只是简单示意，也可能有错误/缺失，此次的目的之一就是完善SKIP的逻辑。
`待补充动画`：继续必备待补补充动画，从 `Mixamo.com` 下载动画资产。
`GameHoster`的职责：1. 代码逻辑上广播任务给NPC，2.编排时语音播报。
`逐幕编排`：从开始逐步推演，没通过一幕再推进下一幕。

## 剧本如下：

### ACT01：游戏规则介绍

初始位置：10个NPC各自在单独的监狱牢房内。

逻辑描述：

1. 打开牢门
   - SM_blocking_prison_Cube_270.SM_Door (Pitch=0.000000,Yaw=0.000000,Roll=-120.000000)
   - SM_blocking_prison_Cube_269.SM_Door (Pitch=0.000000,Yaw=0.000000,Roll=-120.000000)
   - SM_blocking_prison_Cube_268.SM_Door (Pitch=0.000000,Yaw=0.000000,Roll=-120.000000)
   - SM_blocking_prison_Cube_259.SM_Door (Pitch=0.000000,Yaw=0.000000,Roll=-120.000000)
   - SM_blocking_prison_Cube_285.SM_Door (Pitch=0.000000,Yaw=0.000000,Roll=-120.000000)
   - SM_blocking_prison_Cube_286.SM_Door (Pitch=0.000000,Yaw=0.000000,Roll=-120.000000)
   - SM_blocking_prison_Cube_351.SM_Door (Pitch=0.000000,Yaw=0.000000,Roll=-120.000000)
   - SM_blocking_prison_Cube_352.SM_Door (Pitch=0.000000,Yaw=0.000000,Roll=-120.000000)
   - SM_blocking_prison_Cube_271.SM_Door (Pitch=0.000000,Yaw=0.000000,Roll=-120.000000)
   - SM_blocking_prison_Cube_353.SM_Door (Pitch=0.000000,Yaw=0.000000,Roll=-120.000000)

2. 让 10个NPC移动到 `BP_NavTarget_1`，并且看向墙上的电视机 `BP_NavLookTarget_TV`。
3. 播放视频 `MediaPlate`，使用的是UE内置的媒体组件。 游戏规则 @Docs\playscript\zombie-game-rule.md。
4. 直至播放结束。

### ACT02：10个NPC接收游戏规则

将游戏规则广播给10个NPC，10个NPC并行请求。

游戏规则文字内容： @Docs\playscript\zombie-game-rule.md

prompt应包含：

- 你是AI，以 AI 身份出场，AI知道自己是AI，不扮演人类——不赋予人类职业、教育、地域、年龄、姓名格式等背景叙事。只赋予外观符号供观众识别：名字（AI 语义）、昵称、性别（声线与形象呈现）、声线、类人虚拟形象。这是 AI 的外壳，不是人类身份。
- 游戏规则，
