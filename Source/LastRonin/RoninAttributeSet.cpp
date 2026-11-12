#include "RoninAttributeSet.h"
#include "GameplayEffectExtension.h"
#include "RoninCharacter.h"
#include "AbilitySystemComponent.h"
#include "GameplayTagContainer.h"
#include "Net/UnrealNetwork.h"

URoninAttributeSet::URoninAttributeSet()
{
    InitBlood(100.0f);
    InitPosture(0.0f);
    InitStamina(100.0f);
    InitMaxStamina(100.0f);
    InitPotential(100.0f);
}

void URoninAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
    Super::PreAttributeChange(Attribute, NewValue);

    ClampAttributeValue(Attribute, NewValue);
}

void URoninAttributeSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
    Super::PreAttributeBaseChange(Attribute, NewValue);
    ClampAttributeValue(Attribute, NewValue);
}

void URoninAttributeSet::ClampAttributeValue(const FGameplayAttribute& Attribute, float& NewValue) const
{
    if (Attribute != GetBloodAttribute() && Attribute != GetPostureAttribute() &&
        Attribute != GetStaminaAttribute() && Attribute != GetMaxStaminaAttribute() &&
        Attribute != GetPotentialAttribute()) return;

    if (!FMath::IsFinite(NewValue)) NewValue = 0.f;
    if (Attribute == GetBloodAttribute())
        NewValue = FMath::Clamp(NewValue, 0.f, 1000.f);
    else if (Attribute == GetMaxStaminaAttribute())
        NewValue = FMath::Max(NewValue, 1.f);
    else if (Attribute == GetStaminaAttribute())
    {
        const float SafeMax = FMath::IsFinite(GetMaxStamina()) ? FMath::Max(GetMaxStamina(), 1.f) : 1.f;
        NewValue = FMath::Clamp(NewValue, 0.f, SafeMax);
    }
    else
        NewValue = FMath::Clamp(NewValue, 0.f, 100.f);
}

void URoninAttributeSet::PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
    Super::PostAttributeChange(Attribute, OldValue, NewValue);
    if (Attribute != GetMaxStaminaAttribute()) return;

    float ClampedStamina = GetStamina();
    ClampAttributeValue(GetStaminaAttribute(), ClampedStamina);
    if (!FMath::IsFinite(GetStamina()) || ClampedStamina != GetStamina())
        SetStamina(ClampedStamina);

    UAbilitySystemComponent* ASC = GetOwningAbilitySystemComponent();
    ARoninCharacter* Character = IsValid(ASC) ? Cast<ARoninCharacter>(ASC->GetAvatarActor()) : nullptr;
    if (IsValid(Character) && !Character->bRestoringCombatState && !Character->bReplayingCombat && !Character->bHasInitiatedDeath)
        Character->PlayerDNA.Stamina = GetStamina();
}

void URoninAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
    Super::PostGameplayEffectExecute(Data);

    UAbilitySystemComponent* TargetASC = &Data.Target;
    if (!IsValid(TargetASC)) return;

    ARoninCharacter* TargetCharacter = Cast<ARoninCharacter>(TargetASC->GetAvatarActor());
    if (TargetCharacter && (TargetCharacter->bRestoringCombatState || TargetCharacter->bReplayingCombat || TargetCharacter->bHasInitiatedDeath)) return;

    const FGameplayAttribute& Attribute = Data.EvaluatedData.Attribute;
    if (Attribute == GetPostureAttribute())
    {
        float Value = GetPosture();
        ClampAttributeValue(Attribute, Value);
        SetPosture(Value);
        if (!IsValid(TargetASC)) return;
        TargetCharacter = Cast<ARoninCharacter>(TargetASC->GetAvatarActor());
        if (TargetCharacter && (TargetCharacter->bRestoringCombatState || TargetCharacter->bReplayingCombat || TargetCharacter->bHasInitiatedDeath)) return;
        // Never accumulate loose counts on repeated hits; the character owns its contribution.
        if (TargetASC->IsOwnerActorAuthoritative())
        {
            if (IsValid(TargetCharacter))
            {
                ClearFallbackExecutionVulnerability();
                TargetCharacter->UpdateExecutionVulnerability(GetPosture() >= 100.f);
            }
            else
                UpdateFallbackExecutionVulnerability(TargetASC, GetPosture() >= 100.f);
        }
    }
    else if (Attribute == GetBloodAttribute())
    {
        float Value = GetBlood();
        ClampAttributeValue(Attribute, Value);
        SetBlood(Value);
        if (IsValid(TargetCharacter))
        {
            TargetCharacter->PlayerDNA.Blood = GetBlood();
            // HandleDeath owns the authority check; retain replay/restore/one-shot guards.
            if (GetBlood() <= 0.f && !TargetCharacter->bRestoringCombatState &&
                !TargetCharacter->bReplayingCombat && !TargetCharacter->bHasInitiatedDeath)
                TargetCharacter->HandleDeath();
        }
    }
    else if (Attribute == GetStaminaAttribute())
    {
        float Value = GetStamina();
        ClampAttributeValue(Attribute, Value);
        SetStamina(Value);
        if (IsValid(TargetCharacter)) TargetCharacter->PlayerDNA.Stamina = GetStamina();
    }
    else if (Attribute == GetMaxStaminaAttribute())
    {
        float Value = GetMaxStamina();
        ClampAttributeValue(Attribute, Value);
        SetMaxStamina(Value);
        float StaminaValue = GetStamina();
        ClampAttributeValue(GetStaminaAttribute(), StaminaValue);
        SetStamina(StaminaValue);
        if (IsValid(TargetCharacter)) TargetCharacter->PlayerDNA.Stamina = GetStamina();
    }
    else if (Attribute == GetPotentialAttribute())
    {
        float Value = GetPotential();
        ClampAttributeValue(Attribute, Value);
        SetPotential(Value);
        if (IsValid(TargetCharacter)) TargetCharacter->PlayerDNA.Potential = GetPotential();
    }
}

void URoninAttributeSet::UpdateFallbackExecutionVulnerability(UAbilitySystemComponent* ASC, bool bVulnerable)
{
    if (FallbackVulnerabilityASC.Get() != ASC || !bVulnerable)
        ClearFallbackExecutionVulnerability();
    if (!bVulnerable || bOwnsFallbackVulnerabilityTag || !IsValid(ASC)) return;
    const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(TEXT("State.Combat.IsExecutionVulnerable"), false);
    if (!Tag.IsValid()) return;
    FallbackVulnerabilityASC = ASC;
    bOwnsFallbackVulnerabilityTag = true;
    ASC->AddLooseGameplayTag(Tag, 1);
}

void URoninAttributeSet::ClearFallbackExecutionVulnerability()
{
    UAbilitySystemComponent* ASC = FallbackVulnerabilityASC.Get();
    const bool bOwned = bOwnsFallbackVulnerabilityTag;
    bOwnsFallbackVulnerabilityTag = false;
    FallbackVulnerabilityASC.Reset();
    if (bOwned && IsValid(ASC))
    {
        const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(TEXT("State.Combat.IsExecutionVulnerable"), false);
        if (Tag.IsValid()) ASC->RemoveLooseGameplayTag(Tag, 1);
    }
}

void URoninAttributeSet::BeginDestroy()
{
    ClearFallbackExecutionVulnerability();
    Super::BeginDestroy();
}

void URoninAttributeSet::OnRep_Blood(const FGameplayAttributeData& OldBlood) { GAMEPLAYATTRIBUTE_REPNOTIFY(URoninAttributeSet, Blood, OldBlood); }
void URoninAttributeSet::OnRep_Posture(const FGameplayAttributeData& OldPosture) { GAMEPLAYATTRIBUTE_REPNOTIFY(URoninAttributeSet, Posture, OldPosture); }
void URoninAttributeSet::OnRep_Stamina(const FGameplayAttributeData& OldStamina) { GAMEPLAYATTRIBUTE_REPNOTIFY(URoninAttributeSet, Stamina, OldStamina); }
void URoninAttributeSet::OnRep_MaxStamina(const FGameplayAttributeData& OldMaxStamina) { GAMEPLAYATTRIBUTE_REPNOTIFY(URoninAttributeSet, MaxStamina, OldMaxStamina); }
void URoninAttributeSet::OnRep_Potential(const FGameplayAttributeData& OldPotential) { GAMEPLAYATTRIBUTE_REPNOTIFY(URoninAttributeSet, Potential, OldPotential); }

void URoninAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    DOREPLIFETIME_CONDITION_NOTIFY(URoninAttributeSet, Blood, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(URoninAttributeSet, Posture, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(URoninAttributeSet, Stamina, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(URoninAttributeSet, MaxStamina, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(URoninAttributeSet, Potential, COND_None, REPNOTIFY_Always);
}