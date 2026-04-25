#include "AILiveProject.h"

#include "MinimaxACELibrary.h"

DEFINE_LOG_CATEGORY_STATIC(LogAILiveProject, Log, All);

void FAILiveProjectModule::StartupModule()
{
	UE_LOG(LogAILiveProject, Log, TEXT("AILiveProject module started"));
	UMinimaxACELibrary::VerifyEnvSafety();
}

void FAILiveProjectModule::ShutdownModule()
{
}

IMPLEMENT_PRIMARY_GAME_MODULE(FAILiveProjectModule, AILiveProject, "AILiveProject");
