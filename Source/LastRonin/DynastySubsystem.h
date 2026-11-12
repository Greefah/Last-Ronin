#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RoninTypes.h"
#include "DynastySubsystem.generated.h" // ABSOLUTE LAST INCLUDE

UCLASS()
class LASTRONIN_API UDynastySubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "Nemesis|Dynasty")
    void ConceiveHeir(FString MotherID);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Social")
    void UpdateRelationshipTier(FString CharacterA, FString CharacterB, int32 NewAffinity);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Social")
    FString ProposeMarriage(FString ProposerID, FString TargetID);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|UI")
    TArray<FFamilyNode> GetFamilyTree(const FHunterProfile& TargetProfile);

    void RunBackgroundLifeSimulation(float DaysPassed);
};