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
