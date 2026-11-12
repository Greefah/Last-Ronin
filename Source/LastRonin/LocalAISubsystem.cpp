#include "LocalAISubsystem.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"

void ULocalAISubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
#if PLATFORM_WINDOWS
    if (bOwnsServerProcess && ServerProcessHandle.IsValid())
    {
        if (FPlatformProcess::IsProcRunning(ServerProcessHandle)) return;
        FPlatformProcess::CloseProc(ServerProcessHandle);
        ServerProcessHandle = FProcHandle();
        bOwnsServerProcess = false;
    }
    const FString ServerExePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + TEXT("ThirdParty/AI_Models/llama-server.exe"));
    const FString ModelPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() + TEXT("ThirdParty/AI_Models/gemma-2-2b-it-Q4_K_M.gguf"));
    if (!IFileManager::Get().FileExists(*ServerExePath) || !IFileManager::Get().FileExists(*ModelPath))
    {
        Readiness = ELocalAIReadiness::MissingFiles;
        UE_LOG(LogTemp, Warning, TEXT("LOCAL AI: Launch skipped; default executable or model file is missing."));
        return;
    }
    const FString Args = FString::Printf(TEXT("-m \"%s\" -c 2048 -ngl 99"), *ModelPath);
    uint32 ProcessID = 0;
    ServerProcessHandle = FPlatformProcess::CreateProc(
        *ServerExePath, *Args, true, true, true, &ProcessID, 0, nullptr, nullptr);
    bOwnsServerProcess = ServerProcessHandle.IsValid();
    if (bOwnsServerProcess)
    {
        Readiness = ELocalAIReadiness::Starting;
        // Handle acquisition is not a readiness assertion; no polling or blocking wait.
        UE_LOG(LogTemp, Log, TEXT("LOCAL AI: Owned server process handle acquired; HTTP readiness is not verified."));
    }
    else
    {
        Readiness = ELocalAIReadiness::Failed;
        UE_LOG(LogTemp, Error, TEXT("LOCAL AI: CreateProc returned no valid server handle."));
    }
#else
    Readiness = ELocalAIReadiness::UnsupportedPlatform;
    UE_LOG(LogTemp, Warning, TEXT("LOCAL AI: Bundled Windows executable is not launched on this platform."));
#endif
}

void ULocalAISubsystem::Deinitialize()
{
#if PLATFORM_WINDOWS
    if (bOwnsServerProcess && ServerProcessHandle.IsValid())
    {
        if (FPlatformProcess::IsProcRunning(ServerProcessHandle))
            FPlatformProcess::TerminateProc(ServerProcessHandle);
        FPlatformProcess::CloseProc(ServerProcessHandle);
        ServerProcessHandle = FProcHandle();
    }
#endif
    bOwnsServerProcess = false;
    Readiness = ELocalAIReadiness::Stopped;
    Super::Deinitialize();
}