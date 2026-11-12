#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayTagContainer.h"
#include "RoninCounterAbility.generated.h"

class UAbilitySystemComponent;
class UGameplayEffect;

/** Automated reaction ability executing swift parriable counter strikes */
UCLASS()
class LASTRONIN_API URoninCounterAbility : public UGameplayAbility
{
    GENERATED_BODY()

public:
    URoninCounterAbility();

    UPROPERTY(EditDefaultsOnly, Category = "Chanbara|Combat")
    TSubclassOf<UGameplayEffect> CounterDamageEffectClass;

    UPROPERTY(EditDefaultsOnly, Category = "Chanbara|Combat", meta = (ClampMin = "0")) float CounterPhysicalDamage = 20.f;
    UPROPERTY(EditDefaultsOnly, Category = "Chanbara|Combat", meta = (ClampMin = "0")) float CounterPostureDamage = 30.f;

protected:
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
    virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

private:
    void ClearOwnedCounterWindow();
    TWeakObjectPtr<UAbilitySystemComponent> CounterWindowASC;
    FGameplayTag OwnedCounterWindowTag;
    uint64 CounterActivationSerial = 0;
};