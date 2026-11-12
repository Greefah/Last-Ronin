#include "RoninStanceAbility.h"

#include "AbilitySystemComponent.h"
#include "Engine/World.h"
#include "GameplayTagContainer.h"
#include "RoninCharacter.h"
#include "TimerManager.h"

// -----------------------------------------------------------------------
// All four stance tags � these must exist in your project's
// GameplayTags.ini or a native tag declaration block.
// State.Stance.Flash     ? fast light attacks, reduced guard window
// State.Stance.Heavy     ? slow, high posture damage, armored startup
// State.Stance.Forbidden ? high risk/reward, bypasses block entirely
// State.Stance.Defensive ? enlarged parry frames, reduced self-damage
// -----------------------------------------------------------------------
URoninStanceAbility::URoninStanceAbility()
{
    InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
    NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

    // Shared block: cannot switch stances while already mid-transition
    ActivationBlockedTags.AddTag(
        FGameplayTag::RequestGameplayTag(TEXT("State.Stance.Transitioning")));

    // Cannot stance-switch during an active execution finisher
    ActivationBlockedTags.AddTag(
        FGameplayTag::RequestGameplayTag(TEXT("State.Combat.IsExecutionVulnerable")));
}

// -----------------------------------------------------------------------
bool URoninStanceAbility::ValidateStanceActivation(const FGameplayAbilityActorInfo* ActorInfo) const
{
    if (!ActorInfo || !FMath::IsFinite(StanceTransitionCooldown) || StanceTransitionCooldown < 0.f)
        return false;

    UAbilitySystemComponent* ASC = ActorInfo->AbilitySystemComponent.Get();
    const ARoninCharacter* RoninChar = Cast<ARoninCharacter>(ActorInfo->AvatarActor.Get());
    if (!IsValid(ASC) || !IsValid(RoninChar) || !RoninChar->GetWorld()
        || RoninChar->GetAbilitySystemComponent() != ASC || RoninChar->bHasInitiatedDeath
        || RoninChar->bRestoringCombatState || RoninChar->bReplayingCombat)
        return false;

    switch (TargetStance)
    {
    case ESamuraiStance::Flash:
    case ESamuraiStance::Heavy:
    case ESamuraiStance::Forbidden:
    case ESamuraiStance::Defensive:
        break;
    default:
        return false;
    }

    if (!GetTagForStance(TargetStance).IsValid())
        return false;

    if (TargetStance == ESamuraiStance::Forbidden)
    {
        const FGameplayTag UnlockTag = FGameplayTag::RequestGameplayTag(
            TEXT("Trait.Hereditary.ForbiddenArts"), false);
        if (!UnlockTag.IsValid() || !ASC->HasMatchingGameplayTag(UnlockTag))
            return false;
    }

    return true;
}

bool URoninStanceAbility::CanActivateAbility(
    const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayTagContainer* SourceTags,
    const FGameplayTagContainer* TargetTags,
    FGameplayTagContainer* OptionalRelevantTags) const
{
    return ValidateStanceActivation(ActorInfo)
        && Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
}

// -----------------------------------------------------------------------
void URoninStanceAbility::ActivateAbility(
    const FGameplayAbilitySpecHandle Handle,
    const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo,
    const FGameplayEventData* TriggerEventData)
{
    if (!ValidateStanceActivation(ActorInfo))
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }

    Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
    if (!IsActive())
        return;

    // Revalidate after the activation callback, before any cost or stance mutation.
    if (!ValidateStanceActivation(ActorInfo)
        || !CommitAbility(Handle, ActorInfo, ActivationInfo, nullptr))
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
        return;
    }
    if (!IsActive())
        return;

    UAbilitySystemComponent* ASC = ActorInfo->AbilitySystemComponent.Get();
    ARoninCharacter* RoninChar = Cast<ARoninCharacter>(ActorInfo->AvatarActor.Get());
    UWorld* World = RoninChar->GetWorld();
    const FGameplayTag TransitionTag =
        FGameplayTag::RequestGameplayTag(TEXT("State.Stance.Transitioning"));

    TransitionASC = ASC;
    bOwnsTransitionTag = true;
    bOwnsReplicatedTransitionTag = RoninChar->HasAuthority();
    if (bOwnsReplicatedTransitionTag)
        ASC->AddReplicatedLooseGameplayTag(TransitionTag);
    ASC->AddLooseGameplayTag(TransitionTag);
    if (!IsActive())
        return;

    // The character owns stance tags and the single persistent passive across abilities.
    RoninChar->SetCurrentStance(TargetStance);
    if (!IsActive())
        return;
    if (RoninChar->HasAuthority())
        RoninChar->ReplaceStancePassive(StancePassiveEffectClass, GetAbilityLevel());
    if (!IsActive())
        return;

    UE_LOG(LogTemp, Log,
        TEXT("CHANBARA STANCE: %s entered stance [%s]"),
        *GetNameSafe(RoninChar),
        *GetTagForStance(TargetStance).ToString());

    if (StanceTransitionCooldown > 0.f)
    {
        World->GetTimerManager().SetTimer(TransitionTimer, this,
            &URoninStanceAbility::FinishTransition, StanceTransitionCooldown, false);
    }
    else
    {
        // Zero is valid, but SetTimer with a zero rate never fires.
        FinishTransition();
    }
}

// -----------------------------------------------------------------------
void URoninStanceAbility::ClearAllStanceTags(UAbilitySystemComponent* ASC) const
{
    // Explicitly remove all four stance tags so no overlap ever occurs.
    // Each is removed once; RemoveLooseGameplayTag is a no-op if not present.
    static const TArray<FName> StanceTagNames = {
        TEXT("State.Stance.Flash"),
        TEXT("State.Stance.Heavy"),
        TEXT("State.Stance.Forbidden"),
        TEXT("State.Stance.Defensive")
    };

    for (const FName& TagName : StanceTagNames)
    {
        FGameplayTag Tag = FGameplayTag::RequestGameplayTag(TagName, false);
        if (Tag.IsValid() && ASC->HasMatchingGameplayTag(Tag))
        {
            ASC->RemoveLooseGameplayTag(Tag);
        }
    }
}

void URoninStanceAbility::FinishTransition()
{
    if (IsActive()) EndAbility(GetCurrentAbilitySpecHandle(), GetCurrentActorInfo(), GetCurrentActivationInfo(), true, false);
}

void URoninStanceAbility::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
    if (UWorld* World = GetWorld())
        World->GetTimerManager().ClearTimer(TransitionTimer);

    UAbilitySystemComponent* ASC = TransitionASC.Get();
    const bool bRemoveLocalTag = bOwnsTransitionTag;
    const bool bRemoveReplicatedTag = bOwnsReplicatedTransitionTag;
    bOwnsTransitionTag = false;
    bOwnsReplicatedTransitionTag = false;
    TransitionASC.Reset();

    if (ASC && (bRemoveLocalTag || bRemoveReplicatedTag))
    {
        const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(TEXT("State.Stance.Transitioning"));
        if (bRemoveReplicatedTag)
            ASC->RemoveReplicatedLooseGameplayTag(Tag);
        if (bRemoveLocalTag)
            ASC->RemoveLooseGameplayTag(Tag);
    }
    Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

// -----------------------------------------------------------------------
FGameplayTag URoninStanceAbility::GetTagForStance(ESamuraiStance Stance) const
{
    switch (Stance)
    {
    case ESamuraiStance::Flash:
        return FGameplayTag::RequestGameplayTag(TEXT("State.Stance.Flash"), false);
    case ESamuraiStance::Heavy:
        return FGameplayTag::RequestGameplayTag(TEXT("State.Stance.Heavy"), false);
    case ESamuraiStance::Forbidden:
        return FGameplayTag::RequestGameplayTag(TEXT("State.Stance.Forbidden"), false);
    case ESamuraiStance::Defensive:
        return FGameplayTag::RequestGameplayTag(TEXT("State.Stance.Defensive"), false);
    default:
        return FGameplayTag();
    }
}