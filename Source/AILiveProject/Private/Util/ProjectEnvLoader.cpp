// =============================================================================
// 中文教学：ProjectEnvLoader.cpp —— .env 读取器实现
//
// 重要 C++ / UE 概念集中讲一下，因为本文件几乎是「线程安全单例缓存」的
// 教科书例子：
//
//   1) 匿名命名空间 `namespace { ... }`
//      把全局符号「藏」在本编译单元（.cpp）内，等价于老 C 的 `static`。
//      作用：EnvCS / CachedEnv / bLoaded / LoadIfNeeded 这些是模块内部状态，
//      不希望被其它 .cpp 的同名符号污染。
//
//   2) FCriticalSection + FScopeLock（RAII 临界区）
//      多线程安全：FCriticalSection = Windows CRITICAL_SECTION 的 UE 跨平台封装。
//      FScopeLock 在构造时 Lock，析构时 Unlock —— 离开作用域自动解锁，哪怕
//      函数中途 return 或抛异常也不会忘解锁。这是「RAII（Resource Acquisition
//      Is Initialization）」最经典的应用场景，C++ 语言哲学的核心。
//
//   3) DEFINE_LOG_CATEGORY_STATIC
//      声明一个本文件私有的日志类别（LogProjectEnv）。UE_LOG(LogXxx, Verbosity, Fmt, ...)
//      会把日志带上 [LogXxx] 前缀，方便 Output Log 里过滤。
//      Verbosity 等级（高→低）：Fatal / Error / Warning / Display / Log / Verbose / VeryVerbose。
//      这里用 Log（默认开关）和 Warning（默认开关）。
//
//   4) FPaths::ProjectDir() / TEXT(".env")
//      `/` 是 FString 的目录拼接运算符（UE 重载，跨平台处理斜杠方向）。
//      ProjectDir() 返回 .uproject 所在目录的绝对路径，末尾自带 `/`。
//
//   5) `Line.Split(TEXT("="), &Key, &Value)`
//      在第一个 = 处切分，分别填充 Key 和 Value 的输出参数。指针传出参数是
//      UE 老风格；现代 C++ 更喜欢返回 TPair / TOptional。
// =============================================================================

#include "Util/ProjectEnvLoader.h"

#include "HAL/CriticalSection.h"  // FCriticalSection / FScopeLock（线程同步）
#include "Misc/FileHelper.h"      // FFileHelper::LoadFileToString
#include "Misc/Paths.h"           // FPaths::ProjectDir()

DEFINE_LOG_CATEGORY_STATIC(LogProjectEnv, Log, All);

namespace
{
	// ── 模块内部状态（仅本 .cpp 可见）─────────────────────────────────────
	FCriticalSection EnvCS;            // 保护下面两个变量的并发读写
	TMap<FString, FString> CachedEnv;  // 缓存解析过的 key→value
	bool bLoaded = false;              // 是否已经做过一次磁盘读取（懒加载）

	// 第一次访问时读盘，后续 cache 命中。线程安全。
	void LoadIfNeeded()
	{
		FScopeLock Lock(&EnvCS);  // RAII 加锁：进入函数即锁，return 即解锁
		if (bLoaded)
		{
			return;
		}
		// 读 <ProjectDir>/.env
		const FString Path = FPaths::ProjectDir() / TEXT(".env");
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *Path))
		{
			// 文件不存在或读不出来：警告一下，标记为已加载（避免反复尝试）
			UE_LOG(LogProjectEnv, Warning, TEXT("Could not read .env at %s"), *Path);
			bLoaded = true;
			return;
		}

		// 按行切：UE 提供的工具，处理 \r\n / \n 都行
		TArray<FString> Lines;
		Contents.ParseIntoArrayLines(Lines);
		for (FString& Line : Lines)
		{
			Line.TrimStartAndEndInline();   // 去前后空白
			if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
			{
				continue;                   // 跳过空行和 # 开头的注释
			}
			FString Key, Value;
			// 按第一个 "=" 切，左边是 Key，右边是 Value
			if (Line.Split(TEXT("="), &Key, &Value))
			{
				Key.TrimStartAndEndInline();
				Value.TrimStartAndEndInline();
				Value.TrimQuotesInline();   // 去掉值两端的引号（兼容 key="value" 写法）
				CachedEnv.Add(Key, Value);
			}
		}
		UE_LOG(LogProjectEnv, Log, TEXT(".env loaded with %d entries"), CachedEnv.Num());
		bLoaded = true;
	}
}

namespace ProjectEnvLoader
{
	FString Get(const FString& Key)
	{
		LoadIfNeeded();           // 懒加载：确保第一次访问触发读盘
		FScopeLock Lock(&EnvCS);  // 即使没改 cache，遍历也要锁（防 Reload 并发清空）
		// case-insensitive lookup
		// 中文教学：TMap 默认大小写敏感；这里要做大小写不敏感查找，所以直接
		// 线性遍历比较。.env 条目不会很多，性能可以忽略。
		for (const TPair<FString, FString>& Pair : CachedEnv)
		{
			if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase))
			{
				return Pair.Value;
			}
		}
		return FString();         // 没找到返回空串
	}

	void Reload()
	{
		FScopeLock Lock(&EnvCS);
		CachedEnv.Reset();        // 清空 cache（保留容量）
		bLoaded = false;          // 让下次 Get 重新读盘
	}
}
