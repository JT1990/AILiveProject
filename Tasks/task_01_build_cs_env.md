# T01 — Build.cs 模块依赖 + `.env` 通用读取

## 变更说明
- `.env` 已存在并使用 **`XXX_API_KEY` / `XXX_API_BASE` 风格命名**（用户提供）：
  ```
  NEO4J_URI=bolt://localhost:7687
  NEO4J_USER=neo4j
  NEO4J_PASSWORD=storynext123
  DEEPSEEK_API_BASE=https://api.deepseek.com/v1
  DEEPSEEK_API_KEY=sk-...
  EMBEDDING_API_BASE=http://localhost:11434/v1
  EMBEDDING_MODEL_NAME=qwen3-embedding:8b
  minimax=sk-api-...   # 历史遗留
  ```
- 后续所有 `ApiKeyEnvName` 字段统一用大写 `DEEPSEEK_API_KEY` / `GLM_API_KEY` 等
- 新增 `SmartObjectsModule` + `GameplayInteractionsModule` 模块依赖（T19.7 用）

## 目标
启用决策系统所需的 UE 模块，并把 `.env` 解析从仅支持 `minimax` 改为通用 key-value 读取。

## 前置
无（最早任务）

## DoD
- [ ] `AILiveProject.Build.cs` 加入：`AIModule`, `NavigationSystem`, `GameplayTasks`, `UMG`, `Slate`, `SlateCore`, **`SmartObjectsModule`, `GameplayInteractionsModule`**
- [ ] `MinimaxACELibrary` 新增 `GetEnvValueFromProjectEnv(FString KeyName)` BlueprintCallable，**带进程内 cache（避免每次决策都开文件读）**
- [ ] `GetMinimaxApiKeyFromProjectEnv` 改为 `return GetEnvValueFromProjectEnv(TEXT("minimax"))` 的薄 wrapper（保持向后兼容）
- [ ] **新增 `VerifyEnvSafety()` 函数**：检查 `.gitignore` 包含 `.env`，否则启动时 `UE_LOG(LogTemp, Error, ...)`，避免泄露
- [ ] 模块 `StartupModule` 中调一次 `VerifyEnvSafety`
- [ ] **日志 key mask**：所有日志打印 API key 的位置（如 T05 DeepSeek trace）必须 mask 中间字符（只保留前 4 + 后 4），添加 helper `MaskKey(FString)`
- [ ] `.env` 已包含必需 keys（**用户已提供**）：`DEEPSEEK_API_KEY` / `EMBEDDING_API_BASE` / `EMBEDDING_MODEL_NAME` / `NEO4J_URI` / `NEO4J_USER` / `NEO4J_PASSWORD` / `minimax`
- [ ] T18.5 接 GLM 时再加 `GLM_API_KEY` / `GLM_API_BASE`
- [ ] **`.env` 在 `.gitignore` 中（手动验证）**
- [ ] 完整 UBT 重建通过：`Build.bat AILiveProjectEditor Win64 Development`

## 关键文件
- 修改 `Source/AILiveProject/AILiveProject.Build.cs`
- 修改 `Source/AILiveProject/Public/MinimaxACELibrary.h`
- 修改 `Source/AILiveProject/Private/MinimaxACELibrary.cpp`
- 用户操作：编辑 `D:/Project/Unreal/AILiveProject/.env`

## 关键 API / 伪代码

```cpp
// MinimaxACELibrary.h
UFUNCTION(BlueprintCallable, Category="AI Live|Env")
static FString GetEnvValueFromProjectEnv(const FString& KeyName);

UFUNCTION(BlueprintCallable, Category="AI Live|Minimax")  // 保留旧函数
static FString GetMinimaxApiKeyFromProjectEnv();

// 启动时 sanity check
UFUNCTION(BlueprintCallable, Category="AI Live|Env")
static bool VerifyEnvSafety();  // 验证 .env 在 .gitignore 里
```

```cpp
// .cpp 实现思路
namespace { TMap<FString, FString> EnvCache; bool bCacheLoaded = false; }

static void LoadEnvCacheOnce() {
    if (bCacheLoaded) return;
    const FString EnvPath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".env"));
    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *EnvPath)) { bCacheLoaded = true; return; }
    TArray<FString> Lines; Content.ParseIntoArrayLines(Lines);
    for (auto& L : Lines) {
        FString K, V;
        if (L.Split(TEXT("="), &K, &V)) EnvCache.Add(K.TrimStartAndEnd().ToLower(), V.TrimStartAndEnd());
    }
    bCacheLoaded = true;
}

FString UMinimaxACELibrary::GetEnvValueFromProjectEnv(const FString& KeyName) {
    LoadEnvCacheOnce();
    return EnvCache.FindRef(KeyName.ToLower());
}

bool UMinimaxACELibrary::VerifyEnvSafety() {
    // 读 .gitignore，检查是否包含 .env 行（首列匹配，忽略注释）
    // 不包含则 UE_LOG(Error, "SECURITY: .env not in .gitignore!")
    // 返回 true/false
}
```

**日志 mask key**：所有打印 API key 的位置（如 DeepSeek 调用 trace）必须 mask 中间字符：
```cpp
FString MaskKey(const FString& K) {
    if (K.Len() < 12) return TEXT("****");
    return K.Left(4) + TEXT("...") + K.Right(4);
}
```

## 验收信号
- UE 编辑器打开后，新建一个测试 BP，调 `Get Env Value From Project Env("deepseek")`，Print String 看到 `sk-...`
- 重新调 `Get Minimax Api Key From Project Env`，仍能取到原 minimax key（回归测试）
- Output Log 没有新增 warning / error

## 不在范围
- DeepSeek 实际 API 调用（T05）
- 任何 Mind/GameMaster 类（T02/T03）

## 风险
- 用户的 `.env` 已经存在 `minimax=`，注意原解析逻辑不要破坏
- 若 `.env` 文件 BOM/编码问题导致空字符串，加日志提示
