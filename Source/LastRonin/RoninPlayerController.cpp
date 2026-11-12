#include "RoninPlayerController.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/InputComponent.h"
#include "UObject/UnrealType.h"
#include "RoninGameMode.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "BaseHunter.h"
#include "EconomySystem.h"
#include "RoninEconomyAccess.h"

namespace
{
    bool IsCommerceNameValid(const FString& Name)
    {
        return !Name.IsEmpty() && Name.Len() <= 256;
    }
}

void ARoninPlayerController::ClientCommerceResult_Implementation(FName Action, bool bSuccess, int32 UnitPrice)
{
    OnCommerceResult(Action, bSuccess, UnitPrice);
}

void ARoninPlayerController::FinishEconomyRequest(FName Action, bool bSuccess)
{
    if (bSuccess && GetGameInstance())
    {
        ARoninCharacter* Pawn = nullptr;
        FHunterProfile* Profile = nullptr;
        if (FRoninEconomyAccess::ResolveCaller(this, GetGameInstance(), Pawn, Profile))
        {
            Pawn->PlayerDNA.Inventory = Profile->Inventory;
            Pawn->PlayerDNA.Wealth = Profile->Wealth;
            if (UNemesisManager* DB = GetGameInstance()->GetSubsystem<UNemesisManager>()) DB->SaveWorldState();
        }
    }
    // Success reports the mutation result, not a guarantee that a world-save write succeeded.
    ClientCommerceResult(Action, bSuccess, 0);
}

void ARoninPlayerController::ServerRequestMerchantQuote_Implementation(ABaseHunter* Merchant, const FString& ItemID, bool bBuying)
{
    const int32 Price = IsValid(Merchant) && Merchant->GetWorld() == GetWorld() && IsCommerceNameValid(ItemID)
        ? Merchant->CalculateDynamicPrice(this, ItemID, bBuying) : 0;
    ClientCommerceResult(TEXT("MerchantQuote"), Price > 0, Price);
}

void ARoninPlayerController::ServerRequestMerchantTransaction_Implementation(ABaseHunter* Merchant, const FString& ItemID, int32 Quantity, bool bBuying)
{
    const bool bSuccess = IsValid(Merchant) && Merchant->GetWorld() == GetWorld() &&
        IsCommerceNameValid(ItemID) && Quantity > 0 && Merchant->ProcessTransaction(this, ItemID, Quantity, bBuying);
    ClientCommerceResult(TEXT("MerchantTransaction"), bSuccess, 0);
}

void ARoninPlayerController::ServerRequestPersonalCraft_Implementation(const FString& RecipeName)
{
    UEconomySystem* Economy = GetGameInstance() ? GetGameInstance()->GetSubsystem<UEconomySystem>() : nullptr;
    FinishEconomyRequest(TEXT("PersonalCraft"), Economy && IsCommerceNameValid(RecipeName) && Economy->CraftItem(this, RecipeName));
}

void ARoninPlayerController::ServerRequestSettlementTransfer_Implementation(const FString& RegionID, const FString& ItemID, int32 Quantity, bool bWithdraw)
{
    UEconomySystem* Economy = GetGameInstance() ? GetGameInstance()->GetSubsystem<UEconomySystem>() : nullptr;
    bool bSuccess = false;
    if (Economy && IsCommerceNameValid(RegionID) && IsCommerceNameValid(ItemID) && Quantity > 0)
    {
        bSuccess = bWithdraw ? Economy->WithdrawFromSettlementStockpile(this, RegionID, ItemID, Quantity)
            : Economy->DepositToSettlementStockpile(this, RegionID, ItemID, Quantity);
    }
    FinishEconomyRequest(bWithdraw ? FName(TEXT("SettlementWithdraw")) : FName(TEXT("SettlementDeposit")), bSuccess);
}

void ARoninPlayerController::ServerRequestSettlementCraft_Implementation(const FString& RegionID, const FString& RecipeName)
{
    UEconomySystem* Economy = GetGameInstance() ? GetGameInstance()->GetSubsystem<UEconomySystem>() : nullptr;
    FinishEconomyRequest(TEXT("SettlementCraft"), Economy && IsCommerceNameValid(RegionID) && IsCommerceNameValid(RecipeName) &&
        Economy->CraftItemFromSettlementStockpile(this, RegionID, RecipeName));
}

void ARoninPlayerController::ServerRequestSettlementBuild_Implementation(const FString& RegionID, const FString& BuildingID, FGameplayTag BuildingType, FVector Location)
{
    UEconomySystem* Economy = GetGameInstance() ? GetGameInstance()->GetSubsystem<UEconomySystem>() : nullptr;
    FinishEconomyRequest(TEXT("SettlementBuild"), Economy && IsCommerceNameValid(RegionID) && IsCommerceNameValid(BuildingID) &&
        BuildingType.IsValid() && Economy->BuildSettlementStructure(this, RegionID, BuildingID, BuildingType, Location));
}

void ARoninPlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();

    if (!InputComponent) return;
    // These are legacy action bindings; the flag belongs to each binding, not the Enhanced Input context.
    InputComponent->BindAction("PauseGame", IE_Pressed, this, &ARoninPlayerController::TogglePauseMenu).bExecuteWhenPaused = true;
    InputComponent->BindAction("MenuCycleLeft", IE_Pressed, this, &ARoninPlayerController::CycleTabLeft).bExecuteWhenPaused = true;
    InputComponent->BindAction("MenuCycleRight", IE_Pressed, this, &ARoninPlayerController::CycleTabRight).bExecuteWhenPaused = true;
}

bool ARoninPlayerController::OpenPauseMenu()
{
    if (bIsEndingPlay || bIsClosingMenu || !GetWorld()) return false;
    if (bIsMenuOpen) return true;
    if (!PauseMenuWidgetClass) return false;

    PauseMenuInstance = CreateWidget<UUserWidget>(this, PauseMenuWidgetClass);
    if (!IsValid(PauseMenuInstance)) return false;

    PauseMenuInstance->AddToViewport(100);

    // Never acquire ownership of a pause that was already active.
    // Opening the menu can still succeed when the game denies pausing.
    bOwnsMenuPause = !IsPaused() && SetPause(true);

    // Both game and UI receive input; retain the paused legacy action bindings.
    FInputModeGameAndUI InputMode;
    InputMode.SetWidgetToFocus(PauseMenuInstance->TakeWidget());
    InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    SetInputMode(InputMode);

    bShowMouseCursor = true;
    bIsMenuOpen = true;
    return true;
}

bool ARoninPlayerController::ClosePauseMenu()
{
    if (bIsClosingMenu) return false;
    bIsClosingMenu = true;

    // Clear state before removing the widget, which can invoke widget callbacks.
    UUserWidget* MenuToRemove = PauseMenuInstance;
    PauseMenuInstance = nullptr;
    bIsMenuOpen = false;
    const bool bReleasePause = bOwnsMenuPause;
    bOwnsMenuPause = false;

    if (IsValid(MenuToRemove))
    {
        MenuToRemove->RemoveFromParent();
    }

    if (GetWorld())
    {
        // Also check the current pauser so a different controller's pause is untouched.
        if (bReleasePause && IsPaused() && DefaultCanUnpause())
        {
            SetPause(false);
        }

        FInputModeGameOnly InputMode;
        SetInputMode(InputMode);
    }

    bShowMouseCursor = false;
    bIsClosingMenu = false;
    return true; // Reports menu closure, not whether the world resumed.
}

void ARoninPlayerController::TogglePauseMenu()
{
    if (bIsMenuOpen)
    {
        ClosePauseMenu();
    }
    else
    {
        OpenPauseMenu();
    }
}

void ARoninPlayerController::PawnLeavingGame()
{
    if (HasAuthority() && GetWorld()) {
        if (ARoninGameMode* Mode = Cast<ARoninGameMode>(GetWorld()->GetAuthGameMode()))
            Mode->PreparePlayerLogout(this);
    }
    // A successful/prepared handoff detached the body. Unknown game modes retain
    // the engine's normal cleanup behavior instead of silently leaking a pawn.
    Super::PawnLeavingGame();
}

void ARoninPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    bIsEndingPlay = true;
    ClosePauseMenu();
    Super::EndPlay(EndPlayReason);
}

void ARoninPlayerController::CycleTabLeft()
{
    if (bIsMenuOpen)
    {
        UpdateActiveMenuTab(-1);
    }
}

void ARoninPlayerController::CycleTabRight()
{
    if (bIsMenuOpen)
    {
        UpdateActiveMenuTab(1);
    }
}

void ARoninPlayerController::UpdateActiveMenuTab(int32 Direction)
{
    if (!PauseMenuInstance) return;

    // Call a custom blueprint function on your master widget to handle the Widget Switcher mechanics
    UFunction* CycleFunc = PauseMenuInstance->FindFunction(TabCycleFunctionName);
    if (!CycleFunc) return;
    // Retain the existing BlueprintCycleTab API, but only invoke exactly one by-value int32 input.
    bool bValidSignature = CycleFunc->NumParms == 1 && CycleFunc->ParmsSize == sizeof(int32);
    int32 InputCount = 0;
    for (TFieldIterator<FProperty> It(CycleFunc); It; ++It) {
        if (!It->HasAnyPropertyFlags(CPF_Parm)) continue;
        ++InputCount;
        bValidSignature = bValidSignature && CastField<FIntProperty>(*It) != nullptr &&
            !It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm | CPF_ReferenceParm) &&
            It->ArrayDim == 1 && It->GetOffset_ForUFunction() == 0;
    }
    if (!bValidSignature || InputCount != 1) {
        UE_LOG(LogTemp, Warning, TEXT("BlueprintCycleTab must accept one by-value int32 and return nothing."));
        return;
    }
    struct FCycleParams { int32 Dir; };
    FCycleParams Params{Direction};
    PauseMenuInstance->ProcessEvent(CycleFunc, &Params);
}