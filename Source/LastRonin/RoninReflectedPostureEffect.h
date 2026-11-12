#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "RoninReflectedPostureEffect.generated.h"

/** Instant, modifier-only reflection. Never invokes the Chanbara damage execution. */
UCLASS()
class LASTRONIN_API URoninReflectedPostureEffect : public UGameplayEffect
{
    GENERATED_BODY()
public:
    URoninReflectedPostureEffect();
};
