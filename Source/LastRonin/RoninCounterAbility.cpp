#include "RoninCounterAbility.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h" 
#include "GameFramework/Character.h"
#include "GameplayEffect.h"
#include "Misc/ScopeExit.h"
#include "RoninCharacter.h"

URoninCounterAbility::URoninCounterAbility()
{
    InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
    NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

    // FIXED: Correct native GAS framework type 'FAbilityTriggerData' linked cleanly
    FAbilityTriggerData SourceTrigger;
    SourceTrigger.TriggerTag = FGameplayTag::RequestGameplayTag(TEXT("Event.Chanbara.InstantCounter"));
    SourceTrigger.TriggerSource = EGameplayAbilityTriggerSource::GameplayEvent;

    AbilityTriggers.Add(SourceTrigger);
}

void URoninCounterAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
    // EndAbility itself requires actor info; never dereference a missing activation context.
    if (!ActorInfo) return;
    const uint64 ActivationSerial = ++CounterActivationSerial;
    Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
    if (ActivationSerial != CounterActivationSerial || !IsEndAbilityValid(Handle, ActorInfo)) return;

    const auto Finish = [&](bool bCancelled)
    {
        if (ActivationSerial == CounterActivationSerial && IsEndAbilityValid(Handle, ActorInfo))
            EndAbility(Handle, ActorInfo, ActivationInfo, true, bCancelled);
    };
    UAbilitySystemComponent* MyASC = ActorInfo->AbilitySystemComponent.Get();
    AActor* MyAvatar = ActorInfo->AvatarActor.Get();
    const AActor* EventTarget = TriggerEventData ? TriggerEventData->Target.Get() : nullptr;
    if (!IsValid(MyASC) || !IsValid(MyAvatar) || !IsValid(EventTarget) ||
        !MyAvatar->GetWorld() || MyAvatar == EventTarget ||
        MyAvatar->GetWorld() != EventTarget->GetWorld() || !CounterDamageEffectClass)
    {
        Finish(true);
        return;
    }

    UAbilitySystemComponent* EnemyASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(EventTarget);
    AActor* EnemyAvatar = IsValid(EnemyASC) ? EnemyASC->GetAvatarActor() : nullptr;
    const auto CombatantsValid = [&]()
    {
        if (!IsValid(MyASC) || !IsValid(EnemyASC) || MyASC == EnemyASC ||
            !IsValid(MyAvatar) || !IsValid(EnemyAvatar) || MyAvatar == EnemyAvatar ||
            !MyAvatar->GetWorld() || MyAvatar->GetWorld() != EnemyAvatar->GetWorld() ||
            MyASC->GetAvatarActor() != MyAvatar || EnemyASC->GetAvatarActor() != EnemyAvatar) return false;
        const ARoninCharacter* SourceCharacter = Cast<ARoninCharacter>(MyAvatar);
        const ARoninCharacter* TargetCharacter = Cast<ARoninCharacter>(EnemyAvatar);
        return !(SourceCharacter && (SourceCharacter->bRestoringCombatState || SourceCharacter->bReplayingCombat || SourceCharacter->bHasInitiatedDeath)) &&
            !(TargetCharacter && (TargetCharacter->bRestoringCombatState || TargetCharacter->bReplayingCombat || TargetCharacter->bHasInitiatedDeath));
    };
    const FGameplayTag WindowTag = FGameplayTag::RequestGameplayTag(TEXT("State.Combat.PerfectParryFrames"), false);
    const FGameplayTag PhysicalTag = FGameplayTag::RequestGameplayTag(TEXT("Data.Damage.Physical"), false);
    const FGameplayTag PostureTag = FGameplayTag::RequestGameplayTag(TEXT("Data.Damage.Posture"), false);
    if (!CombatantsValid() || !WindowTag.IsValid() || !PhysicalTag.IsValid() || !PostureTag.IsValid())
    {
        Finish(true);
        return;
    }

    FGameplayEffectSpecHandle DamageSpec = MyASC->MakeOutgoingSpec(CounterDamageEffectClass, 1.f, MyASC->MakeEffectContext());
    if (!DamageSpec.IsValid())
    {
        Finish(true);
        return;
    }
    const auto NonNegativeFinite = [](float Value) { return FMath::IsFinite(Value) ? FMath::Max(Value, 0.f) : 0.f; };
    DamageSpec.Data->SetSetByCallerMagnitude(PhysicalTag, NonNegativeFinite(CounterPhysicalDamage));
    DamageSpec.Data->SetSetByCallerMagnitude(PostureTag, NonNegativeFinite(CounterPostureDamage));

    // Preserve native predicted cost/cooldown; damage execution remains authority-only.
    if (!CommitAbility(Handle, ActorInfo, ActivationInfo, nullptr))
    {
        Finish(true);
        return;
    }
    if (ActivationSerial != CounterActivationSerial || !IsEndAbilityValid(Handle, ActorInfo)) return;
    if (!CombatantsValid())
    {
        Finish(true);
        return;
    }

    if (ACharacter* MyChar = Cast<ACharacter>(MyAvatar))
    {
        const FVector Direction = EnemyAvatar->GetActorLocation() - MyAvatar->GetActorLocation();
        if (!Direction.ContainsNaN() && !Direction.IsNearlyZero())
            MyChar->SetActorRotation(Direction.GetSafeNormal2D().Rotation());
    }

    if (MyASC->IsOwnerActorAuthoritative() && EnemyASC->IsOwnerActorAuthoritative())
    {
        // Cleanup also runs on reentrant cancellation; only our one contribution is removed.
        ON_SCOPE_EXIT
        {
            if (ActivationSerial == CounterActivationSerial) ClearOwnedCounterWindow();
        };
        CounterWindowASC = MyASC;
        OwnedCounterWindowTag = WindowTag;
        MyASC->AddLooseGameplayTag(WindowTag, 1);
        if (ActivationSerial == CounterActivationSerial && IsEndAbilityValid(Handle, ActorInfo) &&
            CombatantsValid() && OwnedCounterWindowTag == WindowTag)
        {
            MyASC->ApplyGameplayEffectSpecToTarget(*DamageSpec.Data.Get(), EnemyASC);
        }
        // No active ability-spec pointer or event payload is retained across Apply callbacks.
    }
    Finish(false);
}

void URoninCounterAbility::ClearOwnedCounterWindow()
{
    UAbilitySystemComponent* ASC = CounterWindowASC.Get();
    const FGameplayTag Tag = OwnedCounterWindowTag;
    CounterWindowASC.Reset();
    OwnedCounterWindowTag = FGameplayTag();
    if (IsValid(ASC) && Tag.IsValid()) ASC->RemoveLooseGameplayTag(Tag, 1);
}

void URoninCounterAbility::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
    if (!ActorInfo || !IsEndAbilityValid(Handle, ActorInfo)) return;
    const uint64 ActivationSerial = CounterActivationSerial;
    ClearOwnedCounterWindow();
    // Tag callbacks may already have ended or retriggered this instance.
    if (ActivationSerial == CounterActivationSerial && IsEndAbilityValid(Handle, ActorInfo))
        Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}