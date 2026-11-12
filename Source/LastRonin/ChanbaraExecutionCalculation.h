#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "ChanbaraExecutionCalculation.generated.h"

/**
 * Custom GAS Execution Calculation to process complex tactical math
 * for sword clashes, directional parries, and lethal executions.
 */
UCLASS()
class LASTRONIN_API UChanbaraExecutionCalculation : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()

public:
    UChanbaraExecutionCalculation();

    // Core execution pipeline driver override mandated by GAS
    virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams, FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;
};