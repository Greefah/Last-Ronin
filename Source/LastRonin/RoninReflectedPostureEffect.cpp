#include "RoninReflectedPostureEffect.h"
#include "RoninAttributeSet.h"

URoninReflectedPostureEffect::URoninReflectedPostureEffect()
{
    DurationPolicy = EGameplayEffectDurationType::Instant;
    FSetByCallerFloat ReflectedMagnitude;
    ReflectedMagnitude.DataTag = FGameplayTag::RequestGameplayTag(TEXT("Data.Damage.Posture"));
    FGameplayModifierInfo Modifier;
    Modifier.Attribute = URoninAttributeSet::GetPostureAttribute();
    Modifier.ModifierOp = EGameplayModOp::Additive;
    Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(ReflectedMagnitude);
    Modifiers.Add(Modifier);
}
