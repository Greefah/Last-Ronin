#include "ChanbaraExecutionCalculation.h"
#include "AbilitySystemComponent.h"
#include "RoninAttributeSet.h"
#include "GameplayTagContainer.h"
#include "GameFramework/Actor.h"
#include "RoninCharacter.h"
#include "RoninReflectedPostureEffect.h"

struct FRoninCombatCaptureDef
{
    DECLARE_ATTRIBUTE_CAPTUREDEF(Blood);
    DECLARE_ATTRIBUTE_CAPTUREDEF(Posture);

    FRoninCombatCaptureDef()
    {
        DEFINE_ATTRIBUTE_CAPTUREDEF(URoninAttributeSet, Blood, Target, false);
        DEFINE_ATTRIBUTE_CAPTUREDEF(URoninAttributeSet, Posture, Target, false);
    }
};

static const FRoninCombatCaptureDef& CombatCapture()
{
    static FRoninCombatCaptureDef CaptureDef;
    return CaptureDef;
}

UChanbaraExecutionCalculation::UChanbaraExecutionCalculation()
{
    RelevantAttributesToCapture.Add(CombatCapture().BloodDef);
    RelevantAttributesToCapture.Add(CombatCapture().PostureDef);
}

void UChanbaraExecutionCalculation::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams, FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
    UAbilitySystemComponent* TargetASC = ExecutionParams.GetTargetAbilitySystemComponent();
    UAbilitySystemComponent* SourceASC = ExecutionParams.GetSourceAbilitySystemComponent();

    AActor* SourceActor = SourceASC ? SourceASC->GetAvatarActor() : nullptr;
    AActor* TargetActor = TargetASC ? TargetASC->GetAvatarActor() : nullptr;

    // Executions are authority-observed outcomes, not deterministic predicted events.
    if (!IsValid(SourceASC) || !IsValid(TargetASC) || SourceASC == TargetASC ||
        !IsValid(SourceActor) || !IsValid(TargetActor) || SourceActor == TargetActor ||
        SourceActor->GetWorld() != TargetActor->GetWorld() ||
        !SourceASC->IsOwnerActorAuthoritative() || !TargetASC->IsOwnerActorAuthoritative() ||
        !TargetASC->HasAttributeSetForAttribute(URoninAttributeSet::GetBloodAttribute()) ||
        !TargetASC->HasAttributeSetForAttribute(URoninAttributeSet::GetPostureAttribute())) return;

    ARoninCharacter* Attacker = Cast<ARoninCharacter>(SourceActor);
    ARoninCharacter* Defender = Cast<ARoninCharacter>(TargetActor);
    if ((Attacker && (Attacker->bRestoringCombatState || Attacker->bReplayingCombat || Attacker->bHasInitiatedDeath)) ||
        (Defender && (Defender->bRestoringCombatState || Defender->bReplayingCombat || Defender->bHasInitiatedDeath))) return;

    const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();
    const auto NonNegativeFinite = [](float Value) { return FMath::IsFinite(Value) ? FMath::Max(Value, 0.f) : 0.f; };
    const float BaseDamage = NonNegativeFinite(Spec.GetSetByCallerMagnitude(
        FGameplayTag::RequestGameplayTag(TEXT("Data.Damage.Physical")), false, 0.f));
    const float BasePostureDamage = NonNegativeFinite(Spec.GetSetByCallerMagnitude(
        FGameplayTag::RequestGameplayTag(TEXT("Data.Damage.Posture")), false, 0.f));
    if (BaseDamage <= 0.f && BasePostureDamage <= 0.f) return;

    // Facing is relative to the attacker's position, never the attacker's forward.
    FVector DirectionToAttacker = SourceActor->GetActorLocation() - TargetActor->GetActorLocation();
    FVector DefenderForward = TargetActor->GetActorForwardVector();
    DirectionToAttacker.Z = 0.0;
    DefenderForward.Z = 0.0;
    if (DirectionToAttacker.ContainsNaN() || DefenderForward.ContainsNaN() ||
        !DirectionToAttacker.Normalize() || !DefenderForward.Normalize()) return;
    const bool bIsFacingAttacker = FVector::DotProduct(DefenderForward, DirectionToAttacker) >= 0.5;
    const bool bHasPerfectParryFramesActive = TargetASC->HasMatchingGameplayTag(
        FGameplayTag::RequestGameplayTag(TEXT("State.Combat.PerfectParryFrames")));
    const bool bIsActivelyBlocking = TargetASC->HasMatchingGameplayTag(
        FGameplayTag::RequestGameplayTag(TEXT("State.Combat.IsBlocking")));

    // =======================================================================
    // SYSTEMIC CLASH & COUNTER CALCULATION ENGINE
    // =======================================================================
    float AppliedHealthDamage = 0.0f;
    float AppliedPostureDamage = 0.0f;

    if (bIsFacingAttacker && bHasPerfectParryFramesActive)
    {
        // 1. SUCCESSFUL PERFECT PARRY EVENT TRIGGER
        AppliedHealthDamage = 0.0f;
        AppliedPostureDamage = 0.0f;

        // Modifier-only transient instant effect: no damage execution and no recursion.
        if (BasePostureDamage > 0.f && SourceASC->HasAttributeSetForAttribute(URoninAttributeSet::GetPostureAttribute()))
        {
            URoninReflectedPostureEffect* ReflectionEffect = NewObject<URoninReflectedPostureEffect>(GetTransientPackage());
            FGameplayEffectSpec ReflectionSpec(ReflectionEffect, TargetASC->MakeEffectContext(), 1.f);
            const float ReflectedPosture = static_cast<float>(FMath::Min(static_cast<double>(BasePostureDamage) * 1.5, static_cast<double>(MAX_flt)));
            ReflectionSpec.SetSetByCallerMagnitude(FGameplayTag::RequestGameplayTag(TEXT("Data.Damage.Posture")), ReflectedPosture);
            TargetASC->ApplyGameplayEffectSpecToTarget(ReflectionSpec, SourceASC);
        }

        if (IsValid(Attacker)) Attacker->RequestImpactFreeze(4);
        if (IsValid(Defender)) Defender->RequestImpactFreeze(4);
    }
    else if (bIsFacingAttacker && bIsActivelyBlocking)
    {
        // 2. STANDARD SWORD GUARD BLOCK (CHIP REDUCTION PASS)
        AppliedHealthDamage = BaseDamage * 0.05f;
        AppliedPostureDamage = BasePostureDamage;
        if (IsValid(Attacker)) Attacker->RequestImpactFreeze(4);
        if (IsValid(Defender)) Defender->RequestImpactFreeze(4);
    }
    else
    {
        // 3. UNGUARDED FLANK HIT OR CAUGHT OUTSIDE TIMING ARC
        AppliedHealthDamage = BaseDamage;
        AppliedPostureDamage = BasePostureDamage * 0.5f;
    }

    // =======================================================================
    // FINALIZE DAMAGE MODIFIER APPLICATION REGISTERS
    // =======================================================================
    if (AppliedHealthDamage > 0.0f)
    {
        OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(URoninAttributeSet::GetBloodAttribute(), EGameplayModOp::Additive, -AppliedHealthDamage));
    }

    if (AppliedPostureDamage > 0.0f)
    {
        OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(URoninAttributeSet::GetPostureAttribute(), EGameplayModOp::Additive, AppliedPostureDamage));
    }
}