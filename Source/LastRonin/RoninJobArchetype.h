// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "RoninTypes.h" // Required to parse EElementalType and EHunterClass definitions cleanly
#include "RoninJobArchetype.generated.h"

/**
 * Data-Driven template configurations defining a standardized RPG Job Class type.
 * This offloads class configuration entirely to separate assets in your content browser.
 */
UCLASS(BlueprintType)
class LASTRONIN_API URoninJobArchetype : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    // --- CLASS ARCHETYPE IDENTITY ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Job Identity")
    EHunterClass HunterClass;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Job Identity")
    FString ClassDisplayName = TEXT("Commoner");

    // --- SEED PRODUCTION WORKFORCE QUOTAS ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Simulation Balancing")
    int32 TargetPopulationQuota = 20;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Simulation Balancing")
    float DailyProductionEfficiency = 1.0f;

    // --- BASE RPG STATS TEMPLATE ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Base Attributes")
    int32 BasePowerLevel = 1;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Base Attributes")
    float BaseStamina = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Base Attributes")
    float BaseBlood = 100.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Base Attributes")
    int32 MinGenerationAge = 18;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Base Attributes")
    int32 MaxGenerationAge = 65;

    // --- LOOT & GUARANTEED STARTING GEAR ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Starting Inventory")
    TMap<FString, int32> GuaranteedStartingItems;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Starting Traits")
    FGameplayTagContainer DefaultTraits;

    // --- MESH / MATERIAL ASSET DIRECTORIES ---
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual Resources")
    FString DefaultSkeletalMeshPath;

    // =======================================================================
    // FIXED: EXPOSED PROFILE ELEMENTAL AFFINITY SOURCE CONTAINER
    // =======================================================================
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Job Magic Attributes")
    TArray<EElementalType> AllowedElements;
};