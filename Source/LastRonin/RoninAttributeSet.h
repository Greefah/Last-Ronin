#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "RoninAttributeSet.generated.h"

#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
    GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
    GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

UCLASS()
class LASTRONIN_API URoninAttributeSet : public UAttributeSet
{
    GENERATED_BODY()

public:
    URoninAttributeSet();

    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
    virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
    virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;
    virtual void PostGameplayEffectExecute(const struct FGameplayEffectModCallbackData& Data) override;
    virtual void BeginDestroy() override;

    // --- DEFINITIVE LIFE & STRUCTURAL INTEGRITY ATTRIBUTES ---
    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Health", ReplicatedUsing = OnRep_Blood)
    FGameplayAttributeData Blood;
    ATTRIBUTE_ACCESSORS(URoninAttributeSet, Blood);

    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Stance", ReplicatedUsing = OnRep_Posture)
    FGameplayAttributeData Posture;
    ATTRIBUTE_ACCESSORS(URoninAttributeSet, Posture);

    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Energy", ReplicatedUsing = OnRep_Stamina)
    FGameplayAttributeData Stamina;
    ATTRIBUTE_ACCESSORS(URoninAttributeSet, Stamina);

    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Energy", ReplicatedUsing = OnRep_MaxStamina)
    FGameplayAttributeData MaxStamina;
    ATTRIBUTE_ACCESSORS(URoninAttributeSet, MaxStamina);

    UPROPERTY(BlueprintReadOnly, Category = "Attributes|Magic", ReplicatedUsing = OnRep_Potential)
    FGameplayAttributeData Potential;
    ATTRIBUTE_ACCESSORS(URoninAttributeSet, Potential);

protected:
    UFUNCTION() virtual void OnRep_Blood(const FGameplayAttributeData& OldBlood);
    UFUNCTION() virtual void OnRep_Posture(const FGameplayAttributeData& OldPosture);
    UFUNCTION() virtual void OnRep_Stamina(const FGameplayAttributeData& OldStamina);
    UFUNCTION() virtual void OnRep_MaxStamina(const FGameplayAttributeData& OldMaxStamina);
    UFUNCTION() virtual void OnRep_Potential(const FGameplayAttributeData& OldPotential);

private:
    void ClampAttributeValue(const FGameplayAttribute& Attribute, float& NewValue) const;
    void UpdateFallbackExecutionVulnerability(UAbilitySystemComponent* ASC, bool bVulnerable);
    void ClearFallbackExecutionVulnerability();
    bool bOwnsFallbackVulnerabilityTag = false;
    TWeakObjectPtr<UAbilitySystemComponent> FallbackVulnerabilityASC;
};