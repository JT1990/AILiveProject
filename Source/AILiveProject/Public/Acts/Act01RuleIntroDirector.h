#pragma once

// =============================================================================
// 中文教学：Act01RuleIntroDirector.h —— 「第一幕」剧情导演 Actor
//
// 一个 Director Actor 是「关卡剧情节点」的代码化表达：
//   - 摆在场景里、可由 Player Controller / 别的 Actor 通过 BP 调用
//   - 内部用状态机串联一系列子动作（开门 → NPC 走到 TV → 播放视频）
//   - 完成或失败时通过 dynamic multicast delegate 通知监听者
//
// 状态机 EAct01State：
//   Idle → OpeningDoors → NPCsMovingToTV → PlayingVideo → (Idle，发完成事件)
//
// 关键 UE 概念：
//
//   1) AActor 派生
//      Director 是 AActor 而非 UObject —— 因为它要「摆进关卡」+「能在 PIE
//      时收到 BeginPlay」。AActor 自带 Tick / SpawnActor / 复制等能力。
//
//   2) UCLASS(BlueprintType, Blueprintable)
//      `BlueprintType` 让 BP 变量可以是这个类型。`Blueprintable` 让美术能
//      派生 BP 子类。两个一起用 = 全开放给 BP。
//
//   3) DECLARE_DYNAMIC_MULTICAST_DELEGATE
//      DYNAMIC + MULTICAST 是 UE 的「BP 可见广播事件」：
//        - DYNAMIC：能序列化 / 在 BP 里 bind / 通过反射调用（比 raw delegate 慢但灵活）
//        - MULTICAST：可绑定多个监听者
//      OneParam 后缀变种带一个参数（这里是 FString Reason）。
//
//   4) TSoftObjectPtr / TSubclassOf
//      `TSoftObjectPtr<AActor>`：弱指针 + 路径。资产在编辑器里可指定但不会
//        强制加载，需要时调 LoadSynchronous() / Async load。
//      `TSubclassOf<AActor>`：编辑器里只能选这个基类的子类，类型安全的「类指针」。
//
//   5) UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "...")
//      EditAnywhere：编辑器属性面板可改默认值 + 实例 override
//      BlueprintReadWrite：BP 既可读又可写
//      Category：编辑器面板归类
//
//   6) UPROPERTY(BlueprintAssignable)
//      让 dynamic delegate 在 BP 里能 bind（通过节点连线监听）。
//
// 阅读建议：
//   1) 看构造函数 + 默认值，了解可配置参数
//   2) 看状态切换函数 OpenDoors → MoveNPCsToTV → PlayVideo
//   3) 看 OnAct01Completed / OnAct01Failed 的触发时机
// =============================================================================

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "Act01RuleIntroDirector.generated.h"

// 前向声明：减少头依赖。完整定义在各自的头里。
class AMediaPlate;
class USceneComponent;
class UEnvQuery;
struct IConsoleCommand;

// 完成事件：无参（只通知「成了」）
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FAct01CompletedDelegate);
// 失败事件：带原因（FString Reason 透传给 BP 监听器）
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAct01FailedDelegate, FString, Reason);

/** Act01 的运行状态机；控制开门、NPC 移动到电视前、播放视频这条剧情链。 */
UENUM(BlueprintType)
enum class EAct01State : uint8
{
	/** 空闲状态：Act01 尚未运行，允许 BeginAct01 启动。 */
	Idle UMETA(ToolTip = "空闲状态：Act01 尚未运行，允许 BeginAct01 启动。"),
	/** 开门阶段：牢房门正在播放打开动画。 */
	OpeningDoors UMETA(ToolTip = "开门阶段：牢房门正在播放打开动画。"),
	/** NPC 移动阶段：NPC 正在散布/移动到电视前的目标位置。 */
	NPCsMovingToTV UMETA(ToolTip = "NPC 移动阶段：NPC 正在散布/移动到电视前的目标位置。"),
	/** 播放视频阶段：NPC 已到位，电视视频正在播放或等待播放结束。 */
	PlayingVideo UMETA(ToolTip = "播放视频阶段：NPC 已到位，电视视频正在播放或等待播放结束。"),
};

UCLASS(BlueprintType, Blueprintable)
class AILIVEPROJECT_API AAct01RuleIntroDirector : public AActor
{
	GENERATED_BODY()

public:
	AAct01RuleIntroDirector();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Input")
	bool bEnableKeyTrigger = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Input")
	FKey StartKey;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSubclassOf<AActor> NPCMoverClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSoftObjectPtr<AActor> NavTargetActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSoftObjectPtr<AActor> NavLookTargetActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSoftObjectPtr<AMediaPlate> MediaPlateActor;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSubclassOf<AActor> NavTargetClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	TSubclassOf<AActor> NavLookTargetClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	FName NavTargetActorLabel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Actors")
	FName NavLookTargetActorLabel;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Doors")
	TArray<FName> CellDoorActorNames;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Doors")
	FName CellDoorMeshComponentName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Doors")
	FRotator DoorOpenRelativeRotation;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Doors")
	float DoorAnimationSeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Movement")
	TObjectPtr<UEnvQuery> ScatterQueryAsset;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Movement")
	float MovementTimeoutSeconds = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Movement")
	float ArrivalAcceptanceRadius = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Movement")
	float ArrivalSettleSeconds = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Movement")
	float ScatterDispatchDelaySeconds = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Video")
	float VideoFallbackTimeoutSeconds = 600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AILiveProject|Act01|Debug")
	bool bDebugPrintScreen = true;

	UPROPERTY(BlueprintAssignable, Category = "AILiveProject|Act01")
	FAct01CompletedDelegate OnAct01Completed;

	UPROPERTY(BlueprintAssignable, Category = "AILiveProject|Act01")
	FAct01FailedDelegate OnAct01Failed;

	// 启动剧情。中文教学：BlueprintCallable 让 BP/UI 按钮能调；返回 false 表示
	// 当前 Idle 状态不允许启动（例如已经在播放中）。
	UFUNCTION(BlueprintCallable, Category = "AILiveProject|Act01")
	bool BeginAct01();

	// 中途取消。强制把状态机踢回 Idle，不会触发 OnAct01Completed/Failed。
	UFUNCTION(BlueprintCallable, Category = "AILiveProject|Act01")
	void CancelAct01();

	// 中文教学：BlueprintPure 是「无副作用查询函数」—— 在 BP 节点上没有白色
	// 执行流引脚，可以放在表达式里直接读。inline 实现让调用零开销。
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "AILiveProject|Act01")
	bool IsAct01Running() const { return SceneState != EAct01State::Idle; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "AILiveProject|Act01")
	EAct01State GetSceneState() const { return SceneState; }

protected:
	// AActor 三个生命周期钩子。中文教学：
	//   BeginPlay：关卡里实例 spawn 后第一帧调用一次（可初始化、订阅事件）
	//   Tick     ：每帧调用（默认 60 FPS = 每秒 ~60 次）；DeltaSeconds 是与上帧的间隔秒
	//   EndPlay  ：被销毁或关卡结束时调用（可清理订阅、关线程）
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	struct FCachedDoor
	{
		TWeakObjectPtr<USceneComponent> Component;
		FRotator ClosedRelativeRotation = FRotator::ZeroRotator;
		FRotator ClosedWorldRotation = FRotator::ZeroRotator;
	};

	void CacheInitialNPCTransforms();
	void CacheCellDoorComponents();
	USceneComponent* FindCellDoorComponent(AActor* CellActor) const;
	AActor* ResolveActorByClassAndLabel(TSubclassOf<AActor> InClass, FName Label) const;
	void ResolveTargetActors(bool bLogResolution);

	void RegisterDebugConsoleCommands();
	void UnregisterDebugConsoleCommands();
	void BeginAct01Console();

	void StartOpeningDoors();
	void TickOpenDoors(float DeltaSeconds);
	void ResetDoorsToClosed();

	void StartNPCMovement();
	void TickNPCMovement(float DeltaSeconds);
	bool AreAllNPCsIdle() const;

	void StartVideo();
	void TickVideoFallback(float DeltaSeconds);
	void UnbindVideoDelegate();

	UFUNCTION()
	void HandleVideoEnded();

	void CompleteAct01();
	void FailAct01(const FString& Reason);
	void ResetToInitialPositions();

	void DebugMessage(const FString& Message, const FLinearColor& Color = FLinearColor::White) const;

	// 写一条 orchestrator.round_resolved 事件（phase=setup, visibility=["public"]）。
	// 返回 false 表示 EventStore 不可用或 AppendEvent 失败，由调用方决定是否升级为 FailAct01。
	bool AppendOrchestratorRoundResolved(const FString& Text);

	UPROPERTY(Transient)
	TObjectPtr<AActor> NavTargetCached;

	UPROPERTY(Transient)
	TObjectPtr<AActor> NavLookTargetCached;

	UPROPERTY(Transient)
	TObjectPtr<AMediaPlate> MediaPlateCached;

	TMap<TWeakObjectPtr<AActor>, FTransform> InitialNPCTransforms;
	TArray<FCachedDoor> CachedDoors;

	float DoorAnimationElapsed = 0.0f;
	float MovementElapsed = 0.0f;
	float MovementSettleElapsed = 0.0f;
	float VideoElapsed = 0.0f;
	float HeartbeatElapsed = 0.0f;
	bool bArrivalSettling = false;
	bool bVideoDelegateBound = false;
	EAct01State SceneState = EAct01State::Idle;

	IConsoleCommand* StartCommand = nullptr;
	IConsoleCommand* CancelCommand = nullptr;
};
