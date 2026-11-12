#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "GameplayTagContainer.h"
#include "RoninPlayerController.generated.h"

class UUserWidget;
class ABaseHunter;

UCLASS()
class LASTRONIN_API ARoninPlayerController : public APlayerController
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "Ronin|UI") bool OpenPauseMenu();
    UFUNCTION(BlueprintCallable, Category = "Ronin|UI") bool ClosePauseMenu();

    // Requests carry targets, not buyer IDs. The server always resolves this controller's pawn.
    UFUNCTION(Server, Reliable, BlueprintCallable, Category = "Ronin|Commerce")
    void ServerRequestMerchantQuote(ABaseHunter* Merchant, const FString& ItemID, bool bBuying);
    UFUNCTION(Server, Reliable, BlueprintCallable, Category = "Ronin|Commerce")
    void ServerRequestMerchantTransaction(ABaseHunter* Merchant, const FString& ItemID, int32 Quantity, bool bBuying);
    UFUNCTION(Server, Reliable, BlueprintCallable, Category = "Ronin|Commerce")
    void ServerRequestPersonalCraft(const FString& RecipeName);
    UFUNCTION(Server, Reliable, BlueprintCallable, Category = "Ronin|Commerce")
    void ServerRequestSettlementTransfer(const FString& RegionID, const FString& ItemID, int32 Quantity, bool bWithdraw);
    UFUNCTION(Server, Reliable, BlueprintCallable, Category = "Ronin|Commerce")
    void ServerRequestSettlementCraft(const FString& RegionID, const FString& RecipeName);
    UFUNCTION(Server, Reliable, BlueprintCallable, Category = "Ronin|Commerce")
    void ServerRequestSettlementBuild(const FString& RegionID, const FString& BuildingID, FGameplayTag BuildingType, FVector Location);
    UFUNCTION(BlueprintImplementableEvent, Category = "Ronin|Commerce")
    void OnCommerceResult(FName Action, bool bSuccess, int32 UnitPrice);


protected:
    virtual void SetupInputComponent() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void PawnLeavingGame() override;
    UPROPERTY(EditDefaultsOnly, Category = "UI") FName TabCycleFunctionName = TEXT("BlueprintCycleTab");

    // Input Actions
    void TogglePauseMenu();
    void CycleTabLeft();
    void CycleTabRight();

    UPROPERTY(EditDefaultsOnly, Category = "UI")
    TSubclassOf<UUserWidget> PauseMenuWidgetClass;

private:
    UFUNCTION(Client, Reliable)
    void ClientCommerceResult(FName Action, bool bSuccess, int32 UnitPrice);
    void FinishEconomyRequest(FName Action, bool bSuccess);

    UPROPERTY()
    UUserWidget* PauseMenuInstance = nullptr;

    bool bIsMenuOpen = false;
    bool bOwnsMenuPause = false;
    bool bIsClosingMenu = false;
    bool bIsEndingPlay = false;

    // Helper to send index shifts to the blueprint widget switcher
    void UpdateActiveMenuTab(int32 Direction);
};