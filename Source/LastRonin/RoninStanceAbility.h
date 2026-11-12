#pragma once
#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "Engine/TimerHandle.h"
#include "RoninTypes.h"
#include "RoninStanceAbility.generated.h"

/**
 * Stance-switching ability for the Chanbara combat system.
 * Each stance grants a unique gameplay tag set and attribute modifier
 * that the ChanbaraExecutionCalculation reads during damage resolution.
 */
UCLASS(BlueprintType, Blueprintable)
class LASTRONIN_API URoninStanceAbility : public UGameplayAbility
{
    GENERATED_BODY()

public:
    URoninStanceAbility();

    // The stance this ability instance activates
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Chanbara|Stance")
    ESamuraiStance TargetStance = ESamuraiStance::Flash;

    // Optional gameplay effect to apply persistent stance buffs (speed, guard window, etc.)
    UPROPERTY(EditDefaultsOnly, Category = "Chanbara|Stance")
    TSubclassOf<UGameplayEffect> StancePassiveEffectClass;

    // How long the stance transition animation window lasts in seconds (blocks re-activation)
    UPROPERTY(EditDefaultsOnly, Category = "Chanbara|Stance")
    float StanceTransitionCooldown = 0.4f;

protected:
    virtual void ActivateAbility(
        const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayAbilityActivationInfo ActivationInfo,
        const FGameplayEventData* TriggerEventData) override;

    virtual bool CanActivateAbility(
        const FGameplayAbilitySpecHandle Handle,
        const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayTagContainer* SourceTags,
        const FGameplayTagContainer* TargetTags,
        FGameplayTagContainer* OptionalRelevantTags) const override;

    virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

public:
    // Legacy API retained for existing callers; activation never uses this broad cleanup.
    // Character-owned stance changes must go through SetCurrentStance instead.
    void ClearAllStanceTags(UAbilitySystemComponent* ASC) const;

private:
    bool ValidateStanceActivation(const FGameplayAbilityActorInfo* ActorInfo) const;
    void FinishTransition();
    FTimerHandle TransitionTimer;
    TWeakObjectPtr<UAbilitySystemComponent> TransitionASC;
    bool bOwnsTransitionTag = false;
    bool bOwnsReplicatedTransitionTag = false;

    // Returns the gameplay tag that corresponds to the requested stance
    FGameplayTag GetTagForStance(ESamuraiStance Stance) const;
};