#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RoninTypes.h"
#include "EconomySystem.generated.h" // ABSOLUTE LAST INCLUDE

class APlayerController;

UCLASS()
class LASTRONIN_API UEconomySystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nemesis|Crafting")
    TMap<FString, FRecipe> CraftingRecipes;

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Crafting")
    void RegisterMasterRecipe(FString RecipeName, FRecipe Recipe);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Crafting")
    bool CraftItem(APlayerController* Caller, FString RecipeName);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Inventory")
    bool ConsumeItem(FString CharacterID, FString ItemName);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Settlement")
    bool DepositToSettlementStockpile(APlayerController* Caller, FString RegionID, FString ItemName, int32 Quantity);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Settlement")
    bool WithdrawFromSettlementStockpile(APlayerController* Caller, FString RegionID, FString ItemName, int32 Quantity);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Settlement")
    bool BuildSettlementStructure(APlayerController* Caller, FString RegionID, FString BuildingID, FGameplayTag BuildingType, FVector SpawnLocation);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Crafting")
    bool CraftItemFromSettlementStockpile(APlayerController* Caller, FString RegionID, FString RecipeName);

    void ProcessSettlementProductionTick(float DaysPassed);

    // Stockpile is canonical for primary resources. Safely drains legacy named
    // SharedStockpile entries, including later legacy writes, without mirroring counts.
    // Manager trade/raid/aggregation paths must also adopt this boundary: their raw
    // SharedStockpile reads cannot see canonical balances. Failure changes neither ledger.
    static float* GetPrimaryResourceField(FStockpile& Stockpile, const FString& ResourceName);
    static bool ResolvePrimaryResourceBalance(FRegionData& Region, const FString& ResourceName, float*& OutBalance);
    static bool IsValidItemMetadata(const FItemData& Item);
    // Safety gate for the existing manager valuation; no new balance/valuation policy.
    static bool CanRecalculateProfileNetWealth(const FHunterProfile& Profile,
        const TMap<FString, FItemData>& Inventory, const TMap<FString, FRegionData>& Regions);
    static bool HaveMatchingItemMetadata(const FItemData& A, const FItemData& B);
    static bool TryAdjustInventory(TMap<FString, FItemData>& Inventory, const FString& ItemName,
        int64 Delta, const FItemData& Metadata);
};