#pragma once

// =============================================================================
// 中文教学：SightMemoryComponent.h —— NPC 视觉「记忆」组件
//
// 这是什么：
//   一个 UActorComponent，挂在 NPC actor 上，订阅 AIPerception 的 sight 事件，
//   把「最近看到过谁、最后一次在哪」记下来。提供 BP 查询接口：
//     - GetLastSeenLocation(Target) → 上次看到 Target 的位置
//     - IsCurrentlyVisible(Target)  → 此刻是否还看得见
//     - GetCurrentlyVisibleActors() → 当前能看到的全部 actor 列表
//   两个事件：
//     - OnSightEnter ：刚看到一个新 actor
//     - OnSightExit  ：丢失一个 actor 的视野（带最后位置）
//
// 关键 UE 概念：
//
//   1) UActorComponent
//      Actor 的「插件」：单独一份功能模块。挂上即生效，不需要继承 Actor 类。
//      "ClassGroup=(AILiveProject)"：编辑器添加组件菜单里的归类
//      "BlueprintSpawnableComponent"：BP 里能在「Add Component」按钮里看到
//      "Blueprintable"：可派生 BP 子类
//
//   2) DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams
//      与 OneParam 同理，多一个参数。这里第一个 AActor*，第二个 FVector。
//      参数名（Other / Location）会出现在 BP 节点的 pin 上。
//
//   3) FAIStimulus
//      AIPerception 系统传过来的感知事件结构。包含：
//        - StimulusLocation：感知到的位置
//        - WasSuccessfullySensed()：当前是否仍然在感知
//        - Age：上次刺激发生到现在的秒数
//
//   4) FTimerHandle + AttemptBindPerceptionDelegate 重试机制
//      AIController 有可能比组件晚就位。所以 BeginPlay 第一次 bind 失败时不报错，
//      用 timer 每 0.1s 重试一次，最多 30 次（3 秒），覆盖正常 spawn 时序差。
//      MaxBindRetry / BindRetryInterval 是 static constexpr 常量。
// =============================================================================

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "Perception/AIPerceptionTypes.h"   // FAIStimulus
#include "UObject/WeakObjectPtr.h"
#include "SightMemoryComponent.generated.h"

// 跨文件可见的日志类别（DECLARE_EXTERN）。在 .cpp 用 DEFINE_LOG_CATEGORY 实例化。
DECLARE_LOG_CATEGORY_EXTERN(LogSightMemory, Log, All);

// BP 可见的多播事件。Other 参数 + 位置参数。
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSightEnterDelegate, AActor*, Other, FVector, Location);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSightExitDelegate, AActor*, Other, FVector, LastSeenLocation);

UCLASS(ClassGroup=(AILiveProject), meta=(BlueprintSpawnableComponent), Blueprintable)
class AILIVEPROJECT_API USightMemoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USightMemoryComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AILiveProject|SightMemory")
	bool bDebugPrintScreen = false;

	UPROPERTY(BlueprintAssignable, Category="AILiveProject|SightMemory")
	FOnSightEnterDelegate OnSightEnter;

	UPROPERTY(BlueprintAssignable, Category="AILiveProject|SightMemory")
	FOnSightExitDelegate OnSightExit;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="AILiveProject|SightMemory")
	bool GetLastSeenLocation(AActor* Target, FVector& OutLocation) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="AILiveProject|SightMemory")
	bool IsCurrentlyVisible(AActor* Target) const;

	UFUNCTION(BlueprintCallable, Category="AILiveProject|SightMemory")
	void GetCurrentlyVisibleActors(TArray<AActor*>& OutActors) const;

	UFUNCTION(BlueprintCallable, Category="AILiveProject|SightMemory")
	void GetAllLastSeenLocations(TMap<AActor*, FVector>& OutLocations) const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void OnPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus);

	void AttemptBindPerceptionDelegate();

private:
	TMap<TWeakObjectPtr<AActor>, FVector> LastSeenLocations;
	TSet<TWeakObjectPtr<AActor>> CurrentlyVisibleActors;

	bool bDelegateBound = false;
	int32 BindRetryCount = 0;
	static constexpr int32 MaxBindRetry = 30;
	static constexpr float BindRetryInterval = 0.1f;
	FTimerHandle BindRetryHandle;
};
