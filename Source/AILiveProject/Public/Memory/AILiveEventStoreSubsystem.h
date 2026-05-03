#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SQLiteDatabase.h"
#include "Memory/AILiveEventTypes.h"
#include "AILiveEventStoreSubsystem.generated.h"

UCLASS()
class AILIVEPROJECT_API UAILiveEventStoreSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool BeginGame(const FString& InGameId);
	void EndGame();

	bool IsGameOpen() const { return Db.IsValid(); }
	const FString& GetCurrentGameId() const { return CurrentGameId; }

	bool AppendEvent(const FAILiveEvent& InEvent);
	TArray<FAILiveEvent> QueryEventsByActor(const FString& Actor, int32 LimitCount) const;
	bool VerifyHashChain(int64& OutFirstBadSeq) const;

	// 通过主连接读 _meta.db.schema_meta 全表。WAL 同进程二次连接会拿不到 -shm
	// 共享映射（实测 ReadOnly / ReadWrite 都回 SQLITE_IOERR），故不开新连接，
	// 走已 open 的 MetaDb。仅在 IsGameOpen() 时可调。
	bool QueryMetaSchemaRegistry(TMap<FString, FString>& OutKVs);

	static constexpr int32 kCurrentSchemaVersion = 1;
	static const TCHAR* const kGenesisHash;

private:
	void ApplyPragmas(FSQLiteDatabase& InDb);
	bool EnsureSchema(FSQLiteDatabase& InDb, bool bIsMetaDb);
	bool RunMigrations(FSQLiteDatabase& InDb, int32 FromVersion, int32 ToVersion, bool bIsMetaDb, const TCHAR* FtsTokenizer);
	bool EnsureMetaRegistry(FSQLiteDatabase& InMetaDb);

	FString DetectFtsTokenizer(FSQLiteDatabase& InDb);

	FSQLiteDatabase Db;
	FSQLiteDatabase MetaDb;
	FString CurrentGameId;
};
