#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HAL/PlatformProcess.h"
#include "LocalAISubsystem.generated.h"

// Last observed state, not a continuously monitored health guarantee.
UENUM(BlueprintType)
enum class ELocalAIReadiness : uint8
{
    NotStarted, Starting, Responding, Failed, MissingFiles, UnsupportedPlatform, Stopped
};

UCLASS()
class LASTRONIN_API ULocalAISubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintPure, Category = "AI|Local")
    ELocalAIReadiness GetReadiness() const { return Readiness; }

    // Called only after an actual local HTTP completion result has been validated.
    // Observational only: a later result may replace Failed, including for an external service.
    // No timer, probe or service loop; shutdown remains terminal for this subsystem instance.
    void RecordLocalResponse(bool bValidatedResponse)
    {
        if (Readiness != ELocalAIReadiness::Stopped)
            Readiness = bValidatedResponse ? ELocalAIReadiness::Responding : ELocalAIReadiness::Failed;
    }

private:
    ELocalAIReadiness Readiness = ELocalAIReadiness::NotStarted;
    // Only a handle created by this subsystem may be terminated or closed here.
    FProcHandle ServerProcessHandle;
    bool bOwnsServerProcess = false;
};