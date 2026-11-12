#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "RoninTypes.h"
#include "BaseHunter.generated.h"

// Forward declaration of global classes in clean global scope
class UNemesisManager;
class APlayerController;

UCLASS(BlueprintType, Blueprintable)
class LASTRONIN_API ABaseHunter : public ACharacter
{
    GENERATED_BODY()

public:
    ABaseHunter();

protected:
    virtual void BeginPlay() override;

public:
    virtual void Tick(float DeltaTime) override;

    // Persisted data profile matching this specific physical instance
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hunter DNA")
    FHunterProfile HunterDNA;

    UFUNCTION(BlueprintPure, Category = "Clan")
    FString GetClanID() const { return HunterDNA.ClanID; }

    // Initializes this character's mesh and movement attributes from profile data
    UFUNCTION(BlueprintCallable, Category = "Hunter DNA")
    void InitializeFromDNA(FHunterProfile InDNA);

    // --- MERCHANT & COMMERCE CORE HOOKS ---

    // Triggers the localized commercial UI pipeline based on this character's specific job class
    UFUNCTION(BlueprintCallable, Category = "Merchant|Commerce")
    void OpenMerchantInterface(APlayerController* PlayerController);

    // Clean pass-through helper node to expose this character's data maps to UMG wrap slots
    UFUNCTION(BlueprintPure, Category = "Merchant|Commerce")
    TMap<FString, FItemData> GetCurrentShopInventory() const;

    // Server-only balanced transfer for the controller's authorized living buyer.
    UFUNCTION(BlueprintCallable, Category = "Merchant|Commerce")
    bool ProcessTransaction(APlayerController* BuyerController, FString ItemID, int32 Quantity, bool bPlayerBuying);

    // Server-only quote; invalid identity, world, or interaction range returns zero.
    UFUNCTION(BlueprintPure, Category = "Merchant|Commerce")
    int32 CalculateDynamicPrice(APlayerController* BuyerController, FString ItemID, bool bPlayerBuying) const;

    // FIXED: Swapped typo UCLASS back to UFUNCTION so UHT parses the class body successfully
    UFUNCTION(BlueprintImplementableEvent, Category = "Merchant|Commerce")
    void OnInteractionTriggered(APlayerController* PlayerController);

    UFUNCTION(BlueprintImplementableEvent, Category = "Hunter DNA")
    void OnDNAUpdated();

    // --- SYSTEMS LOGIC OVERRIDES ---
    UFUNCTION(BlueprintCallable, Category = "AI")
    void UpdateAIStrategy();

    UFUNCTION(BlueprintCallable, Category = "Survival")
    void ProcessSurvivalMetrics(float DeltaTime);

    UFUNCTION(BlueprintCallable, Category = "Survival")
    void PerformRest(float NutritionValue);

    UFUNCTION(BlueprintCallable, Category = "Dialogue")
    void BeginDialogue();
};