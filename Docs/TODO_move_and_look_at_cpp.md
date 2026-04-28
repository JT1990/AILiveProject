# MoveAndLookAt C++ 接入待办

日期：2026-04-29

## 当前结论

保留 `SandboxCharacter_Mover.MoveAndLookAt` 蓝图实现，不要重写。

理由：

- 当前移动链路已经 PIE 验证，`AIController::MoveToLocation`、PathFollowing、NavMover、CharacterMover 的核心执行本来就在引擎 C++。
- `MoveAndLookAt` 依赖 `SandboxCharacter_Mover` 现有 GASP / Mover 输入链，尤其是 `Get_OrientationIntent` 的到达后朝向覆盖。
- M 键只是 `L_prison` 的测试触发器，不是长期产品入口。

## 下一步

如果接入 Mind / LLM 决策层，新增一个薄 C++ `UBlueprintFunctionLibrary` 或 `UActorComponent` 作为命令入口。

要求：

- 先由 C++ 命令入口调用现有 BP `MoveAndLookAt`。
- 不直接替换 GASP / Mover 图。
- 不把 `Get_OrientationIntent`、Mover 输入产出、Visual-override 相关链路迁到 C++。

## 迁移条件

等移动行为明显变复杂后，再把调度层迁到 C++。

适合迁到 C++ 的内容：

- 移动命令调度
- 取消移动
- 超时处理
- 失败错误码
- 统一日志
- Mind / LLM 行为状态回传

仍保留在蓝图的内容：

- `Get_OrientationIntent`
- 贴着 GASP / Mover 的动画输入逻辑
- 现有 `SandboxCharacter_Mover` 父类图中的运动模式分支

除非决定重构角色父类，否则不要把这些贴着 GASP / Mover 的动画输入逻辑迁到 C++。
