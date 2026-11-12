#include "BaseHunter.h"
#include "EconomySystem.h"
#include "RoninCharacter.h"
#include "DeepSeek_AI.h"
#include "Engine/GameInstance.h"
#include "Blueprint/UserWidget.h"
#include "NemesisManager.h" 
#include "DialogueSystem.h"
#include "Kismet/GameplayStatics.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "RoninEconomyAccess.h"

namespace
{
    bool ResolveMerchantCaller(const ABaseHunter* MerchantActor, APlayerController* BuyerController,
        UGameInstance* GI, ARoninCharacter*& OutBuyerPawn, FHunterProfile*& OutBuyer,
        FHunterProfile*& OutMerchant)
    {
        OutBuyerPawn = nullptr;
        OutBuyer = nullptr;
        OutMerchant = nullptr;
        if (!IsValid(MerchantActor) || !MerchantActor->HasAuthority() ||
            MerchantActor->IsActorBeingDestroyed() || !MerchantActor->GetWorld() ||
            MerchantActor->HunterDNA.UniqueID.IsEmpty() || MerchantActor->HunterDNA.UniqueID == TEXT("PLAYER") ||
            !FRoninEconomyAccess::ResolveCaller(BuyerController, GI, OutBuyerPawn, OutBuyer)) return false;
        UNemesisManager* DB = GI->GetSubsystem<UNemesisManager>();
        OutMerchant = DB ? DB->ActiveNemeses.Find(MerchantActor->HunterDNA.UniqueID) : nullptr;
        return OutMerchant && OutMerchant->UniqueID == MerchantActor->HunterDNA.UniqueID &&
            OutMerchant->UniqueID != OutBuyer->UniqueID && OutMerchant->bIsAlive &&
            FMath::IsFinite(OutMerchant->Blood) && OutMerchant->Blood > 0.f &&
            MerchantActor->GetWorld() == OutBuyerPawn->GetWorld() &&
            FRoninEconomyAccess::IsWithinRange(MerchantActor->GetActorLocation(),
                OutBuyerPawn->GetActorLocation(), FRoninEconomyAccess::InteractionRange);
    }
}

ABaseHunter::ABaseHunter()
{
    PrimaryActorTick.bCanEverTick = true;
}

void ABaseHunter::BeginPlay()
{
    Super::BeginPlay();
    OnDNAUpdated();
}

void ABaseHunter::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    HunterDNA.WorldLocation = GetActorLocation();
}

void ABaseHunter::InitializeFromDNA(FHunterProfile InDNA)
{
    HunterDNA = InDNA;

    if (GetMesh() && !HunterDNA.AsyncSkeletalMeshHandle.IsNull())
    {
        USkeletalMesh* LoadedMesh = HunterDNA.AsyncSkeletalMeshHandle.Get();

        if (!LoadedMesh)
        {
            LoadedMesh = HunterDNA.AsyncSkeletalMeshHandle.LoadSynchronous();
        }

        if (LoadedMesh)
        {
            GetMesh()->SetSkeletalMesh(LoadedMesh);
            GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered;
            GetMesh()->bEnableUpdateRateOptimizations = true;
        }
    }

    if (GetCharacterMovement())
    {
        GetCharacterMovement()->bAutoUpdateTickRegistration = true;
        GetCharacterMovement()->bComponentShouldUpdatePhysicsVolume = false;

        if (HunterDNA.Age < 15 || HunterDNA.HunterClass == EHunterClass::Child)
        {
            GetCharacterMovement()->MaxWalkSpeed = 300.0f;
        }
        else if (HunterDNA.HunterClass == EHunterClass::BanditChief || HunterDNA.HunterClass == EHunterClass::Soldier || HunterDNA.HunterClass == EHunterClass::RoninKing)
        {
            GetCharacterMovement()->MaxWalkSpeed = 600.0f;
        }
        else
        {
            GetCharacterMovement()->MaxWalkSpeed = 450.0f;
        }
    }

    OnDNAUpdated();
}

TMap<FString, FItemData> ABaseHunter::GetCurrentShopInventory() const
{
    UGameInstance* GI = GetGameInstance();
    UNemesisManager* DB = GI ? GI->GetSubsystem<UNemesisManager>() : nullptr;
    if (!DB || HunterDNA.UniqueID.IsEmpty()) return TMap<FString, FItemData>();
    DB->SynchronizeSimulation();
    const FHunterProfile* Merchant = DB->ActiveNemeses.Find(HunterDNA.UniqueID);
    return Merchant && Merchant->bIsAlive ? Merchant->Inventory : TMap<FString, FItemData>();
}

void ABaseHunter::OpenMerchantInterface(APlayerController* PlayerController)
{
    ARoninCharacter* BuyerPawn = nullptr;
    FHunterProfile* Buyer = nullptr;
    FHunterProfile* Merchant = nullptr;
    if (!ResolveMerchantCaller(this, PlayerController, GetGameInstance(), BuyerPawn, Buyer, Merchant)) return;
    HunterDNA.Inventory = Merchant->Inventory;
    HunterDNA.Wealth = Merchant->Wealth;
    UE_LOG(LogTemp, Log, TEXT("COMMERCE TRACE: Initializing shop interaction loop with %s (Class: %d)"),
        *Merchant->HunterName, (int32)Merchant->HunterClass);
    OnInteractionTriggered(PlayerController);
}

int32 ABaseHunter::CalculateDynamicPrice(APlayerController* BuyerController, FString ItemID, bool bPlayerBuying) const
{
    if (!HasAuthority() || ItemID.IsEmpty() || ItemID.Equals(TEXT("Copper_Mon"), ESearchCase::IgnoreCase)) return 0;
    ARoninCharacter* BuyerPawn = nullptr;
    FHunterProfile* Player = nullptr;
    FHunterProfile* Merchant = nullptr;
    if (!ResolveMerchantCaller(this, BuyerController, GetGameInstance(), BuyerPawn, Player, Merchant)) return 0;
    // Sell quotes use the player's actual item, even when the merchant has no such stock.
    const FItemData* Item = bPlayerBuying ? Merchant->Inventory.Find(ItemID) :
        (Player ? Player->Inventory.Find(ItemID) : nullptr);
    if (!Item || Item->Quantity <= 0 || Item->ItemName != ItemID ||
        !UEconomySystem::IsValidItemMetadata(*Item) || Item->Potency <= 0.0f ||
        static_cast<double>(Item->Potency) > MAX_int32) return 0;
    const int64 TruncatedBase = static_cast<int64>(Item->Potency); // Finite/bounded above.
    if (TruncatedBase <= 0) return 0;
    double RarityMultiplier = 1.0;
    switch (Item->Rarity)
    {
    case ERoninRarity::Rare: RarityMultiplier = 2.5; break;
    case ERoninRarity::Legendary: RarityMultiplier = 6.0; break;
    case ERoninRarity::Legacy: RarityMultiplier = 15.0; break;
    default: break;
    }
    double FameModifier = 1.0;
    double ScarcityModifier = 1.0;
    const FItemData* MerchantItem = Merchant->Inventory.Find(ItemID);
    const int32 MerchantStockCount = MerchantItem ? MerchantItem->Quantity : 0;
    if (MerchantStockCount < 0) return 0;
    if (Player)
    {
        if (!FMath::IsFinite(Player->InheritedFame)) return 0;
        const double Fame = Player->InheritedFame;
        if (bPlayerBuying)
        {
            FameModifier = FMath::Clamp(1.5 - Fame / 100.0, 0.5, 1.5);
            ScarcityModifier = FMath::Clamp(1.0 + 3.0 / FMath::Max(1, MerchantStockCount), 1.0, 3.0);
        }
        else
        {
            FameModifier = FMath::Clamp(0.5 + Fame / 100.0, 0.5, 1.5);
            ScarcityModifier = FMath::Clamp(1.5 - static_cast<double>(MerchantStockCount) / 10.0, 0.2, 1.2);
        }
    }
    const double FinalPrice = static_cast<double>(TruncatedBase) * RarityMultiplier * FameModifier * ScarcityModifier;
    if (!FMath::IsFinite(FinalPrice) || FinalPrice <= 0.0 || FinalPrice >= static_cast<double>(MAX_int32) + 0.5) return 0;
    const int64 RoundedPrice = FMath::RoundToInt64(FinalPrice);
    return RoundedPrice > 0 && RoundedPrice <= MAX_int32 ? static_cast<int32>(RoundedPrice) : 0;
}

bool ABaseHunter::ProcessTransaction(APlayerController* BuyerController, FString ItemID, int32 Quantity, bool bPlayerBuying)
{
    if (!HasAuthority()) return false;
    if (Quantity <= 0 || ItemID.IsEmpty() || ItemID.Equals(TEXT("Copper_Mon"), ESearchCase::IgnoreCase)) return false;
    UGameInstance* GI = GetGameInstance();
    const int32 UnitPrice = CalculateDynamicPrice(BuyerController, ItemID, bPlayerBuying);
    if (UnitPrice <= 0) return false;
    ARoninCharacter* BuyerPawn = nullptr;
    FHunterProfile* Player = nullptr;
    FHunterProfile* Merchant = nullptr;
    if (!ResolveMerchantCaller(this, BuyerController, GI, BuyerPawn, Player, Merchant)) return false;
    UNemesisManager* NemesisSys = GI->GetSubsystem<UNemesisManager>();
    const FString BuyerID = Player->UniqueID;
    const FString MerchantID = Merchant->UniqueID;
    const FItemData* Found = bPlayerBuying ? Merchant->Inventory.Find(ItemID) : Player->Inventory.Find(ItemID);
    if (!Found || Found->Quantity < Quantity || Found->ItemName != ItemID ||
        !UEconomySystem::IsValidItemMetadata(*Found) || Found->Potency <= 0.0f) return false;
    FItemData Metadata = *Found;
    Metadata.Quantity = 0; // Destination starts at zero, with source type/potency/rarity.
    const int64 TotalPrice = static_cast<int64>(UnitPrice) * static_cast<int64>(Quantity);
    if (TotalPrice <= 0 || TotalPrice > MAX_int32) return false;
    FItemData Currency;
    Currency.ItemName = TEXT("Copper_Mon");
    Currency.Type = EItemType::Resource;
    Currency.Potency = 1.0f;
    Currency.Rarity = ERoninRarity::Common;
    Currency.Quantity = 0;
    TMap<FString, FItemData> PlayerInventory = Player->Inventory;
    TMap<FString, FItemData> MerchantInventory = Merchant->Inventory;
    const int64 ItemDelta = bPlayerBuying ? static_cast<int64>(Quantity) : -static_cast<int64>(Quantity);
    const int64 MoneyDelta = bPlayerBuying ? -TotalPrice : TotalPrice;
    // Includes payer affordability in both directions and all four destination bounds.
    if (!UEconomySystem::TryAdjustInventory(PlayerInventory, ItemID, ItemDelta, Metadata) ||
        !UEconomySystem::TryAdjustInventory(MerchantInventory, ItemID, -ItemDelta, Metadata) ||
        !UEconomySystem::TryAdjustInventory(PlayerInventory, Currency.ItemName, MoneyDelta, Currency) ||
        !UEconomySystem::TryAdjustInventory(MerchantInventory, Currency.ItemName, -MoneyDelta, Currency) ||
        !UEconomySystem::CanRecalculateProfileNetWealth(*Player, PlayerInventory, NemesisSys->MapRegions) ||
        !UEconomySystem::CanRecalculateProfileNetWealth(*Merchant, MerchantInventory, NemesisSys->MapRegions)) return false;
    // Repeat authority, bound identity, living merchant, world, and range proof at mutation.
    // No callbacks or latent work intervene between preflight, this proof, and the merge.
    ARoninCharacter* CurrentBuyerPawn = nullptr;
    FHunterProfile* CurrentBuyer = nullptr;
    FHunterProfile* CurrentMerchant = nullptr;
    if (!ResolveMerchantCaller(this, BuyerController, GI, CurrentBuyerPawn, CurrentBuyer, CurrentMerchant) ||
        CurrentBuyerPawn != BuyerPawn || CurrentBuyer != Player || CurrentMerchant != Merchant ||
        CurrentBuyer->UniqueID != BuyerID || CurrentMerchant->UniqueID != MerchantID) return false;
    // Merge only commerce-owned fields into current backend profiles, never stale whole DNA.
    Player->Inventory = MoveTemp(PlayerInventory);
    Merchant->Inventory = MoveTemp(MerchantInventory);
    NemesisSys->RecalculateProfileNetWealth(BuyerID);
    NemesisSys->RecalculateProfileNetWealth(MerchantID);
    BuyerPawn->PlayerDNA.Inventory = Player->Inventory;
    BuyerPawn->PlayerDNA.Wealth = Player->Wealth;
    HunterDNA.Inventory = Merchant->Inventory;
    HunterDNA.Wealth = Merchant->Wealth;
    NemesisSys->SaveWorldState();
    return true;
}

void ABaseHunter::UpdateAIStrategy()
{
    if (UDeepSeekSubsystem* DeepSeekSys = GetGameInstance()->GetSubsystem<UDeepSeekSubsystem>())
    {
        ARoninCharacter* PlayerCharacter = Cast<ARoninCharacter>(UGameplayStatics::GetPlayerCharacter(this, 0));

        if (PlayerCharacter)
        {
            TArray<FString> KnownAbilities;
            KnownAbilities.Add(TEXT("Flash Strike"));

            DeepSeekSys->GenerateCombatStrategy(TEXT("Samurai"), KnownAbilities);
        }
    }
}

void ABaseHunter::ProcessSurvivalMetrics(float DeltaTime)
{
    if (!HasAuthority() || !FMath::IsFinite(DeltaTime) || DeltaTime <= 0.0f || HunterDNA.UniqueID.IsEmpty()) return;
    UGameInstance* GI = GetGameInstance();
    UNemesisManager* DB = GI ? GI->GetSubsystem<UNemesisManager>() : nullptr;
    if (!DB) return;
    DB->SynchronizeSimulation();
    FHunterProfile* Profile = DB->ActiveNemeses.Find(HunterDNA.UniqueID);
    if (!Profile || !Profile->bIsAlive || !FMath::IsFinite(Profile->Hunger)) return;
    // Same 0.01/second deficit as manager survival, expressed as profile fullness.
    // Explicit hook only: Tick does not add a second survival simulation pass.
    Profile->Hunger = static_cast<float>(FMath::Clamp(
        static_cast<double>(Profile->Hunger) - static_cast<double>(DeltaTime) * 0.01, 0.0, 100.0));
    HunterDNA.Hunger = Profile->Hunger;
}

void ABaseHunter::PerformRest(float NutritionValue)
{
    if (!HasAuthority() || !FMath::IsFinite(NutritionValue) || NutritionValue <= 0.0f || HunterDNA.UniqueID.IsEmpty()) return;
    UGameInstance* GI = GetGameInstance();
    UNemesisManager* DB = GI ? GI->GetSubsystem<UNemesisManager>() : nullptr;
    if (!DB) return;
    DB->SynchronizeSimulation();
    FHunterProfile* Profile = DB->ActiveNemeses.Find(HunterDNA.UniqueID);
    if (!Profile || !Profile->bIsAlive || !FMath::IsFinite(Profile->Hunger)) return;
    // Existing rest nutrition convention; does not mint inventory or consume arbitrary food.
    Profile->Hunger = static_cast<float>(FMath::Clamp(
        static_cast<double>(Profile->Hunger) + NutritionValue, 0.0, 100.0));
    HunterDNA.Hunger = Profile->Hunger;
}

void ABaseHunter::BeginDialogue()
{
    UGameInstance* GI = GetGameInstance();
    if (!GI || HunterDNA.UniqueID.IsEmpty()) return;
    if (UDialogueSystem* DialogueSystem = GI->GetSubsystem<UDialogueSystem>())
    {
        DialogueSystem->StartDialogue(HunterDNA.UniqueID, TEXT("A wary samurai guarding the mountain pass."));
    }
}