
#include "EconomySystem.h"

#include "RoninEconomyAccess.h"
#include "NemesisManager.h"
#include "Engine/GameInstance.h"
#include "Kismet/KismetSystemLibrary.h"
#include "SettlementHub.h"
#include "Kismet/GameplayStatics.h"

float* UEconomySystem::GetPrimaryResourceField(FStockpile& Stockpile, const FString& ResourceName)
{
    if (ResourceName.Equals(TEXT("Rice"), ESearchCase::IgnoreCase)) return &Stockpile.Rice;
    if (ResourceName.Equals(TEXT("Lumber"), ESearchCase::IgnoreCase)) return &Stockpile.Lumber;
    if (ResourceName.Equals(TEXT("Fish"), ESearchCase::IgnoreCase)) return &Stockpile.Fish;
    if (ResourceName.Equals(TEXT("Iron"), ESearchCase::IgnoreCase)) return &Stockpile.Iron;
    return nullptr;
}

namespace
{
    bool HasSettlementAccess(ARoninCharacter* Pawn, const FHunterProfile& Profile,
        const FString& RegionID, const FRegionData& Region, ASettlementHub* Hub,
        const FVector* BuildLocation = nullptr)
    {
        if (RegionID.IsEmpty() || Region.RegionID != RegionID ||
            !FRoninEconomyAccess::HasSettlementRights(Profile, Region) ||
            !IsValid(Pawn) || !Pawn->HasAuthority() || Pawn->IsActorBeingDestroyed() || !Pawn->GetWorld() ||
            !IsValid(Hub) || !Hub->HasAuthority() || Hub->IsActorBeingDestroyed() ||
            Hub->GetWorld() != Pawn->GetWorld() || Hub->RegionID != RegionID) return false;
        const FVector PawnLocation = Pawn->GetActorLocation();
        const FVector HubLocation = Hub->GetActorLocation();
        if (!FRoninEconomyAccess::IsWithinRange(PawnLocation, HubLocation,
            FRoninEconomyAccess::InteractionRange)) return false;
        return !BuildLocation ||
            (FRoninEconomyAccess::IsWithinRange(*BuildLocation, HubLocation, FRoninEconomyAccess::BuildingRange) &&
             FRoninEconomyAccess::IsWithinRange(*BuildLocation, PawnLocation, FRoninEconomyAccess::BuildingRange));
    }

    ASettlementHub* FindAccessibleSettlementHub(ARoninCharacter* Pawn, const FHunterProfile& Profile,
        const FString& RegionID, const FRegionData& Region, const FVector* BuildLocation = nullptr)
    {
        if (!IsValid(Pawn) || !Pawn->GetWorld() || RegionID.IsEmpty() || Region.RegionID != RegionID ||
            !FRoninEconomyAccess::HasSettlementRights(Profile, Region)) return nullptr;
        TArray<AActor*> Hubs;
        UGameplayStatics::GetAllActorsOfClass(Pawn->GetWorld(), ASettlementHub::StaticClass(), Hubs);
        for (AActor* Actor : Hubs)
        {
            ASettlementHub* Hub = Cast<ASettlementHub>(Actor);
            if (HasSettlementAccess(Pawn, Profile, RegionID, Region, Hub, BuildLocation)) return Hub;
        }
        return nullptr; // No valid authoritative in-range hub means no mutation.
    }

    bool IsExactStockBalance(double Value)
    {
        return FMath::IsFinite(Value) && Value >= 0.0 && Value <= MAX_flt &&
            static_cast<double>(static_cast<float>(Value)) == Value;
    }

    FString StockMetadataKey(const FString& Name)
    {
        for (const TCHAR* Primary : { TEXT("Rice"), TEXT("Lumber"), TEXT("Fish"), TEXT("Iron") })
            if (Name.Equals(Primary, ESearchCase::IgnoreCase)) return FString(Primary);
        return Name;
    }

    FItemData PrimaryUnitMetadata(const FString& Name)
    {
        FItemData Item;
        Item.ItemName = Name;
        Item.Type = EItemType::Resource;
        Item.Potency = 1.0f; // Per unit, never withdrawn quantity.
        Item.Quantity = 0;
        Item.Rarity = ERoninRarity::Common;
        return Item;
    }

    bool IsValidRecipe(const FString& Name, const FRecipe& Recipe)
    {
        if (Name.IsEmpty() || Recipe.Ingredients.Num() == 0 || Recipe.Result.Quantity <= 0 ||
            !UEconomySystem::IsValidItemMetadata(Recipe.Result)) return false;
        for (const auto& Ingredient : Recipe.Ingredients)
            if (Ingredient.Key.IsEmpty() || Ingredient.Value <= 0) return false;
        return true;
    }

    bool AdjustRegionCount(FRegionData& Region, const FString& Name, int64 Delta)
    {
        if (Name.IsEmpty() || Delta < -static_cast<int64>(MAX_int32) || Delta > MAX_int32) return false;
        float* Primary = UEconomySystem::GetPrimaryResourceField(Region.Stockpile, Name);
        if (Primary)
        {
            if (!UEconomySystem::ResolvePrimaryResourceBalance(Region, Name, Primary)) return false;
            const double Current = *Primary;
            const double Next = Current + static_cast<double>(Delta);
            if (!IsExactStockBalance(Next) || Next - Current != static_cast<double>(Delta) ||
                Next - static_cast<double>(Delta) != Current) return false;
            *Primary = static_cast<float>(Next);
            return true;
        }
        const int32* Existing = Region.SharedStockpile.Find(Name);
        const int64 Current = Existing ? *Existing : 0;
        const int64 Next = Current + Delta;
        if (Current < 0 || Next < 0 || Next > MAX_int32) return false;
        if (Next == 0) Region.SharedStockpile.Remove(Name);
        else Region.SharedStockpile.Add(Name, static_cast<int32>(Next));
        return true;
    }

    bool ReadRegionMetadata(FRegionData& Region, const FString& Name, FItemData& Out)
    {
        float* Primary = UEconomySystem::GetPrimaryResourceField(Region.Stockpile, Name);
        if (Primary && !UEconomySystem::ResolvePrimaryResourceBalance(Region, Name, Primary)) return false;
        if (Name == TEXT("Copper_Mon"))
        {
            Out = PrimaryUnitMetadata(Name); // Currency potency is always per single coin.
        }
        else if (const FItemData* Stored = Region.SharedStockpileMetadata.Find(StockMetadataKey(Name)))
        {
            if (Stored->ItemName != StockMetadataKey(Name) || !UEconomySystem::IsValidItemMetadata(*Stored)) return false;
            Out = *Stored;
        }
        else
        {
            // Unknown legacy arbitrary items cannot safely be reconstructed.
            if (!Primary) return false;
            Out = PrimaryUnitMetadata(Name);
        }
        Out.ItemName = Name;
        Out.Quantity = 0;
        return true;
    }

    bool StoreRegionMetadata(FRegionData& Region, const FString& Name, const FItemData& Incoming)
    {
        if (!UEconomySystem::IsValidItemMetadata(Incoming)) return false;
        float* Primary = UEconomySystem::GetPrimaryResourceField(Region.Stockpile, Name);
        double Current = 0.0;
        if (Primary)
        {
            if (!UEconomySystem::ResolvePrimaryResourceBalance(Region, Name, Primary)) return false;
            Current = *Primary;
        }
        else if (const int32* Count = Region.SharedStockpile.Find(Name))
        {
            if (*Count < 0) return false;
            Current = *Count;
        }
        FItemData Previous;
        if (Current > 0.0 && (!ReadRegionMetadata(Region, Name, Previous) ||
            !UEconomySystem::HaveMatchingItemMetadata(Previous, Incoming))) return false;
        FItemData Metadata = Incoming;
        Metadata.ItemName = StockMetadataKey(Name);
        Metadata.Quantity = 0;
        Region.SharedStockpileMetadata.Add(Metadata.ItemName, Metadata);
        return true;
    }

    void CommitRegionStock(FRegionData& Region, const FRegionData& Working)
    {
        Region.Stockpile = Working.Stockpile;
        Region.SharedStockpile = Working.SharedStockpile;
        Region.SharedStockpileMetadata = Working.SharedStockpileMetadata;
    }
}

bool UEconomySystem::ResolvePrimaryResourceBalance(FRegionData& Region, const FString& ResourceName, float*& OutBalance)
{
    OutBalance = GetPrimaryResourceField(Region.Stockpile, ResourceName);
    if (!OutBalance || !FMath::IsFinite(*OutBalance) || *OutBalance < 0.0f) return false;
    const FString CanonicalName = StockMetadataKey(ResourceName);
    FItemData Metadata = PrimaryUnitMetadata(CanonicalName);
    bool bHasMetadata = false;
    // Metadata aliases must agree before any count or metadata entry is changed.
    for (const auto& Pair : Region.SharedStockpileMetadata)
    {
        if (!Pair.Key.Equals(CanonicalName, ESearchCase::IgnoreCase)) continue;
        if (!Pair.Value.ItemName.Equals(CanonicalName, ESearchCase::IgnoreCase) ||
            !IsValidItemMetadata(Pair.Value) ||
            (bHasMetadata && !HaveMatchingItemMetadata(Metadata, Pair.Value))) return false;
        Metadata = Pair.Value;
        bHasMetadata = true;
    }
    int64 LegacyTotal = 0;
    for (const auto& Pair : Region.SharedStockpile)
    {
        if (!Pair.Key.Equals(CanonicalName, ESearchCase::IgnoreCase)) continue;
        if (Pair.Value < 0) return false;
        LegacyTotal += static_cast<int64>(Pair.Value); // At most int32 map entries of int32 counts.
    }
    if (LegacyTotal > (static_cast<int64>(1) << 53)) return false;
    const double Current = *OutBalance;
    auto LedgerMetadataMatches = [&](const FString& LedgerName)
    {
        const FItemData* Stored = Region.SharedStockpileMetadata.Find(LedgerName);
        if (!Stored) Stored = Region.SharedStockpileMetadata.Find(CanonicalName);
        const FItemData Unit = Stored ? *Stored : PrimaryUnitMetadata(CanonicalName);
        return HaveMatchingItemMetadata(Metadata, Unit);
    };
    if (Current > 0.0 && !LedgerMetadataMatches(CanonicalName)) return false;
    for (const auto& Pair : Region.SharedStockpile)
        if (Pair.Value > 0 && Pair.Key.Equals(CanonicalName, ESearchCase::IgnoreCase) &&
            !LedgerMetadataMatches(Pair.Key)) return false;
    const double Combined = Current + static_cast<double>(LegacyTotal);
    if (!IsExactStockBalance(Combined) || Combined - Current != static_cast<double>(LegacyTotal) ||
        Combined - static_cast<double>(LegacyTotal) != Current) return false;
    // Metadata-free legacy primary units use the existing unit metadata fallback.
    Metadata.ItemName = CanonicalName;
    Metadata.Quantity = 0;
    *OutBalance = static_cast<float>(Combined);
    // Erase only incorporated balances: repeated resolution never double credits.
    for (auto It = Region.SharedStockpile.CreateIterator(); It; ++It)
        if (It.Key().Equals(CanonicalName, ESearchCase::IgnoreCase)) It.RemoveCurrent();
    for (auto It = Region.SharedStockpileMetadata.CreateIterator(); It; ++It)
        if (It.Key().Equals(CanonicalName, ESearchCase::IgnoreCase)) It.RemoveCurrent();
    if (Combined > 0.0 || bHasMetadata)
        Region.SharedStockpileMetadata.Add(CanonicalName, Metadata);
    return true;
}

bool UEconomySystem::IsValidItemMetadata(const FItemData& Item)
{
    return !Item.ItemName.IsEmpty() && FMath::IsFinite(Item.Potency) && Item.Potency >= 0.0f &&
        (Item.ItemName != TEXT("Copper_Mon") ||
            (Item.Type == EItemType::Resource && Item.Potency == 1.0f && Item.Rarity == ERoninRarity::Common)) &&
        static_cast<double>(Item.Potency) <= MAX_int32 &&
        static_cast<uint8>(Item.Type) <= static_cast<uint8>(EItemType::RareYokaiDrop) &&
        static_cast<uint8>(Item.Rarity) <= static_cast<uint8>(ERoninRarity::Legacy);
}

bool UEconomySystem::CanRecalculateProfileNetWealth(const FHunterProfile& Profile,
    const TMap<FString, FItemData>& Inventory, const TMap<FString, FRegionData>& Regions)
{
    // Mirror only the current manager's arithmetic as a safety gate. The manager
    // still owns and performs valuation; callers must update this gate if that changes.
    int64 Total = 0;
    for (const auto& Pair : Inventory)
    {
        const FItemData& Item = Pair.Value;
        if (Item.Quantity < 0) return false;
        int64 Value = Item.Quantity;
        if (Pair.Key != TEXT("Copper_Mon"))
        {
            if (!IsValidItemMetadata(Item)) return false;
            Value = static_cast<int64>(Item.Potency) * static_cast<int64>(Item.Quantity);
        }
        // Individual products and each aggregate must fit the existing int32 metric.
        if (Value < 0 || Value > MAX_int32) return false;
        Total += Value;
        if (Total > MAX_int32) return false;
    }
    if (!Profile.ClanID.IsEmpty() && Profile.ClanID != TEXT("Neutral"))
    {
        for (const auto& Pair : Regions)
        {
            const FRegionData& Region = Pair.Value;
            if (Region.ControllingClanID != Profile.ClanID) continue;
            const double DistrictValue = Region.DistrictValue;
            if (!FMath::IsFinite(DistrictValue) || DistrictValue < MIN_int32 || DistrictValue > MAX_int32) return false;
            Total += static_cast<int64>(DistrictValue);
            if (Total < MIN_int32 || Total > MAX_int32) return false;
        }
    }
    return true;
}

bool UEconomySystem::HaveMatchingItemMetadata(const FItemData& A, const FItemData& B)
{
    return IsValidItemMetadata(A) && IsValidItemMetadata(B) &&
        A.Type == B.Type && A.Potency == B.Potency && A.Rarity == B.Rarity;
}

bool UEconomySystem::TryAdjustInventory(TMap<FString, FItemData>& Inventory, const FString& ItemName,
    int64 Delta, const FItemData& Metadata)
{
    if (ItemName.IsEmpty() || Metadata.ItemName != ItemName ||
        Delta < -static_cast<int64>(MAX_int32) || Delta > MAX_int32) return false;
    const FItemData UnitMetadata = ItemName == TEXT("Copper_Mon") ? PrimaryUnitMetadata(ItemName) : Metadata;
    if (!IsValidItemMetadata(UnitMetadata)) return false;
    const FItemData* Existing = Inventory.Find(ItemName);
    const int64 Current = Existing ? Existing->Quantity : 0;
    const int64 Next = Current + Delta;
    if (Current < 0 || Next < 0 || Next > MAX_int32) return false;
    // Older currency entries may contain only a count; normalize their metadata.
    if (Existing && ItemName != TEXT("Copper_Mon") &&
        (Existing->ItemName != ItemName || !HaveMatchingItemMetadata(*Existing, Metadata))) return false;
    if (Next == 0) Inventory.Remove(ItemName);
    else
    {
        FItemData Updated = UnitMetadata; // Source-derived, no default Quantity=1 increment.
        Updated.Quantity = static_cast<int32>(Next);
        Inventory.Add(ItemName, Updated);
    }
    return true;
}

void UEconomySystem::Initialize(FSubsystemCollectionBase& Collection) {
    Super::Initialize(Collection);

    // Initial recipe definitions
    FRecipe RecoveryBrew;
    RecoveryBrew.RecipeName = TEXT("Medicinal Green Tea");
    RecoveryBrew.Ingredients.Add(TEXT("Wild Herbs"), 2);
    RecoveryBrew.Result = { TEXT("Medicinal Green Tea"), EItemType::Food, 35.0f, 1, ERoninRarity::Common };
    RegisterMasterRecipe(TEXT("Medicinal Green Tea"), RecoveryBrew);

    FRecipe SteelKatana;
    SteelKatana.RecipeName = TEXT("Folded Steel Katana");
    SteelKatana.Ingredients.Add(TEXT("Iron"), 4);
    SteelKatana.Ingredients.Add(TEXT("Lumber"), 2);
    SteelKatana.Result = { TEXT("Folded Steel Katana"), EItemType::Weapon, 45.0f, 1, ERoninRarity::Rare };
    RegisterMasterRecipe(TEXT("Folded Steel Katana"), SteelKatana);
}

void UEconomySystem::RegisterMasterRecipe(FString RecipeName, FRecipe Recipe)
{
    if (!IsValidRecipe(RecipeName, Recipe))
    {
        UE_LOG(LogTemp, Warning, TEXT("RegisterMasterRecipe: invalid recipe '%s' rejected"), *RecipeName);
        return;
    }
    CraftingRecipes.FindOrAdd(RecipeName) = Recipe;
}

bool UEconomySystem::CraftItem(APlayerController* Caller, FString RecipeName)
{
    UGameInstance* GI = GetGameInstance();
    ARoninCharacter* Pawn = nullptr;
    FHunterProfile* Profile = nullptr;
    if (!FRoninEconomyAccess::ResolveCaller(Caller, GI, Pawn, Profile)) return false;
    UNemesisManager* DB = GI->GetSubsystem<UNemesisManager>();
    const FString CharacterID = Profile->UniqueID;
    const FRecipe* Recipe = CraftingRecipes.Find(RecipeName);
    if (!Recipe || !IsValidRecipe(RecipeName, *Recipe)) return false;
    // Synchronous preflight only: no further simulation synchronization or latent work.
    TMap<FString, FItemData> WorkingInventory = Profile->Inventory;
    for (const auto& Pair : Recipe->Ingredients)
    {
        // All ingredients, including Iron/Lumber, come only from personal inventory.
        const FItemData* Found = WorkingInventory.Find(Pair.Key);
        if (!Found || Found->Quantity < Pair.Value) return false;
        const FItemData Source = *Found;
        if (!TryAdjustInventory(WorkingInventory, Pair.Key, -static_cast<int64>(Pair.Value), Source)) return false;
    }
    const FItemData& Result = Recipe->Result;
    if (!TryAdjustInventory(WorkingInventory, Result.ItemName, static_cast<int64>(Result.Quantity), Result)) return false;
    // No live ingredient consumption occurs until output metadata/count preflight succeeds.
    if (!CanRecalculateProfileNetWealth(*Profile, WorkingInventory, DB->MapRegions)) return false;
    Profile->Inventory = MoveTemp(WorkingInventory);
    DB->RecalculateProfileNetWealth(CharacterID);
    UE_LOG(LogTemp, Log, TEXT("CraftItem: '%s' crafted for '%s'"), *RecipeName, *CharacterID);
    return true;
}

bool UEconomySystem::ConsumeItem(FString CharacterID, FString ItemName)
{
    UGameInstance* GI = GetGameInstance();
    UNemesisManager* DB = GI ? GI->GetSubsystem<UNemesisManager>() : nullptr;
    if (!DB) return false;
    DB->SynchronizeSimulation();
    CharacterID = DB->ResolveCharacterID(CharacterID);
    FHunterProfile* Profile = DB->ActiveNemeses.Find(CharacterID);
    if (!Profile || !Profile->bIsAlive) return false;
    const FItemData* Found = Profile->Inventory.Find(ItemName);
    if (!Found || Found->Quantity <= 0 || Found->ItemName != ItemName ||
        !IsValidItemMetadata(*Found) || Found->Potency <= 0.0f) return false;
    const FItemData Item = *Found;
    TMap<FString, FItemData> WorkingInventory = Profile->Inventory;
    if (!TryAdjustInventory(WorkingInventory, ItemName, -1, Item)) return false;
    if (Item.Type == EItemType::Resource)
    {
        if (ItemName == TEXT("Copper_Mon") || static_cast<double>(Item.Potency) >= static_cast<double>(MAX_int32) + 0.5) return false;
        const int64 Credit = FMath::RoundToInt64(static_cast<double>(Item.Potency));
        if (Credit <= 0 || Credit > MAX_int32) return false;
        const FItemData Currency = PrimaryUnitMetadata(TEXT("Copper_Mon")); // Quantity explicitly zero.
        if (!TryAdjustInventory(WorkingInventory, Currency.ItemName, Credit, Currency)) return false;
    }
    if ((Item.Type == EItemType::Food && !FMath::IsFinite(Profile->Hunger)) ||
        (Item.Type == EItemType::Drink && !FMath::IsFinite(Profile->Thirst))) return false;
    if (!CanRecalculateProfileNetWealth(*Profile, WorkingInventory, DB->MapRegions)) return false;
    switch (Item.Type)
    {
    case EItemType::Food:
        Profile->Hunger = static_cast<float>(FMath::Clamp(static_cast<double>(Profile->Hunger) + Item.Potency, 0.0, 100.0));
        break;
    case EItemType::Drink:
        Profile->Thirst = static_cast<float>(FMath::Clamp(static_cast<double>(Profile->Thirst) + Item.Potency, 0.0, 100.0));
        break;
    case EItemType::Resource:
        // Copper_Mon credit is already checked. Wealth remains derived.
        break;
    case EItemType::Weapon:
    case EItemType::Armor:
    case EItemType::RareYokaiDrop:
        Profile->LootedLegacyGear.Add(Item.ItemName);
        break;
    default:
        break;
    }
    Profile->Inventory = MoveTemp(WorkingInventory);
    DB->RecalculateProfileNetWealth(CharacterID);
    UE_LOG(LogTemp, Log, TEXT("ConsumeItem: %s consumed '%s'"), *CharacterID, *ItemName);
    return true;
}

// Deposit an item from a character inventory into a region stockpile (Rice/Lumber/Iron/Fish).
bool UEconomySystem::DepositToSettlementStockpile(APlayerController* Caller, FString RegionID, FString ItemName, int32 Quantity)
{
    if (Quantity <= 0) return false;
    UGameInstance* GI = GetGameInstance();
    ARoninCharacter* Pawn = nullptr;
    FHunterProfile* ResolvedProfile = nullptr;
    if (!FRoninEconomyAccess::ResolveCaller(Caller, GI, Pawn, ResolvedProfile)) return false;
    UNemesisManager* DB = GI->GetSubsystem<UNemesisManager>();
    FRegionData* ResolvedRegion = DB->MapRegions.Find(RegionID);
    if (!ResolvedRegion) return false;
    FHunterProfile& Profile = *ResolvedProfile;
    FRegionData& Region = *ResolvedRegion;
    ASettlementHub* Hub = FindAccessibleSettlementHub(Pawn, Profile, RegionID, Region);
    if (!Hub) return false;
    const FString CharacterID = Profile.UniqueID;

    // Canonical caller, rights and actual server locations are validated before working state.
    const FItemData* Found = Profile.Inventory.Find(ItemName);
    if (!Found || Found->Quantity < Quantity || Found->ItemName != ItemName) return false;
    const FItemData Source = ItemName == TEXT("Copper_Mon") ? PrimaryUnitMetadata(ItemName) : *Found;
    if (!IsValidItemMetadata(Source)) return false;
    FRegionData WorkingRegion = Region;
    TMap<FString, FItemData> WorkingInventory = Profile.Inventory;
    if (!StoreRegionMetadata(WorkingRegion, ItemName, Source) ||
        !AdjustRegionCount(WorkingRegion, ItemName, static_cast<int64>(Quantity)) ||
        !TryAdjustInventory(WorkingInventory, ItemName, -static_cast<int64>(Quantity), Source) ||
        !CanRecalculateProfileNetWealth(Profile, WorkingInventory, DB->MapRegions)) return false;
    // Synchronous preflight: no latent work or further simulation synchronization.
    if (!HasSettlementAccess(Pawn, Profile, RegionID, Region, Hub)) return false;
    CommitRegionStock(Region, WorkingRegion);
    Profile.Inventory = MoveTemp(WorkingInventory);
    DB->RecalculateProfileNetWealth(CharacterID);

    UE_LOG(LogTemp, Log, TEXT("Deposit: %d x '%s' deposited by '%s' to region '%s'"), Quantity, *ItemName, *CharacterID, *RegionID);
    return true;
}

// Withdraw items from region stockpile into a character's inventory
bool UEconomySystem::WithdrawFromSettlementStockpile(APlayerController* Caller, FString RegionID, FString ItemName, int32 Quantity)
{
    if (Quantity <= 0) return false;
    UGameInstance* GI = GetGameInstance();
    ARoninCharacter* Pawn = nullptr;
    FHunterProfile* ResolvedProfile = nullptr;
    if (!FRoninEconomyAccess::ResolveCaller(Caller, GI, Pawn, ResolvedProfile)) return false;
    UNemesisManager* DB = GI->GetSubsystem<UNemesisManager>();
    FRegionData* ResolvedRegion = DB->MapRegions.Find(RegionID);
    if (!ResolvedRegion) return false;
    FHunterProfile& Profile = *ResolvedProfile;
    FRegionData& Region = *ResolvedRegion;
    ASettlementHub* Hub = FindAccessibleSettlementHub(Pawn, Profile, RegionID, Region);
    if (!Hub) return false;
    const FString CharacterID = Profile.UniqueID;

    FRegionData WorkingRegion = Region;
    TMap<FString, FItemData> WorkingInventory = Profile.Inventory;
    FItemData Metadata;
    if (!ReadRegionMetadata(WorkingRegion, ItemName, Metadata) ||
        !AdjustRegionCount(WorkingRegion, ItemName, -static_cast<int64>(Quantity)) ||
        !TryAdjustInventory(WorkingInventory, ItemName, static_cast<int64>(Quantity), Metadata) ||
        !CanRecalculateProfileNetWealth(Profile, WorkingInventory, DB->MapRegions)) return false;
    if (!HasSettlementAccess(Pawn, Profile, RegionID, Region, Hub)) return false;
    CommitRegionStock(Region, WorkingRegion);
    Profile.Inventory = MoveTemp(WorkingInventory);

    DB->RecalculateProfileNetWealth(CharacterID);
    UE_LOG(LogTemp, Log, TEXT("Withdraw: %d x '%s' withdrawn by '%s' from region '%s'"), Quantity, *ItemName, *CharacterID, *RegionID);
    return true;
}

// Build a basic settlement structure (appends to region structures)
bool UEconomySystem::BuildSettlementStructure(APlayerController* Caller, FString RegionID, FString BuildingID, FGameplayTag BuildingType, FVector SpawnLocation)
{
    if (BuildingID.IsEmpty() || !FMath::IsFinite(SpawnLocation.X) ||
        !FMath::IsFinite(SpawnLocation.Y) || !FMath::IsFinite(SpawnLocation.Z)) return false;
    UGameInstance* GI = GetGameInstance();
    ARoninCharacter* Pawn = nullptr;
    FHunterProfile* Profile = nullptr;
    if (!FRoninEconomyAccess::ResolveCaller(Caller, GI, Pawn, Profile)) return false;
    UNemesisManager* DB = GI->GetSubsystem<UNemesisManager>();
    FRegionData* ResolvedRegion = DB->MapRegions.Find(RegionID);
    if (!ResolvedRegion) return false;
    FRegionData& Region = *ResolvedRegion;
    ASettlementHub* Hub = FindAccessibleSettlementHub(Pawn, *Profile, RegionID, Region, &SpawnLocation);
    if (!Hub) return false;
    for (const FBuildingData& Building : Region.Structures)
        if (Building.BuildingID == BuildingID) return false;

    FBuildingData NewBuilding;
    NewBuilding.BuildingID = BuildingID;
    NewBuilding.BuildingTypeTag = BuildingType;
    NewBuilding.WorldLocation = SpawnLocation;
    NewBuilding.CurrentHealth = 500.0f;
    NewBuilding.bIsFunctional = true;
    TArray<FBuildingData> WorkingStructures = Region.Structures;
    WorkingStructures.Add(NewBuilding);

    if (!HasSettlementAccess(Pawn, *Profile, RegionID, Region, Hub, &SpawnLocation)) return false;
    Region.Structures = MoveTemp(WorkingStructures);

    UE_LOG(LogTemp, Log, TEXT("BuildSettlementStructure: '%s' added to region '%s'"), *BuildingID, *RegionID);
    return true;
}

// Craft using the region stockpile (consumes region stockpile resources where ingredients match resource types)
bool UEconomySystem::CraftItemFromSettlementStockpile(APlayerController* Caller, FString RegionID, FString RecipeName)
{
    UGameInstance* GI = GetGameInstance();
    ARoninCharacter* Pawn = nullptr;
    FHunterProfile* Profile = nullptr;
    if (!FRoninEconomyAccess::ResolveCaller(Caller, GI, Pawn, Profile)) return false;
    UNemesisManager* DB = GI->GetSubsystem<UNemesisManager>();
    FRegionData* ResolvedRegion = DB->MapRegions.Find(RegionID);
    if (!ResolvedRegion) return false;
    FRegionData& Region = *ResolvedRegion;
    ASettlementHub* Hub = FindAccessibleSettlementHub(Pawn, *Profile, RegionID, Region);
    if (!Hub) return false;
    const FRecipe* FoundRecipe = CraftingRecipes.Find(RecipeName);
    if (!FoundRecipe || !IsValidRecipe(RecipeName, *FoundRecipe)) return false;
    const FRecipe& Recipe = *FoundRecipe;
    FRegionData WorkingRegion = Region;
    // Owned working balances permit ingredient/result overlap without partial consumption.
    for (const auto& Pair : Recipe.Ingredients)
        if (!AdjustRegionCount(WorkingRegion, Pair.Key, -static_cast<int64>(Pair.Value))) return false;
    const FItemData& Result = Recipe.Result;
    if (!StoreRegionMetadata(WorkingRegion, Result.ItemName, Result) ||
        !AdjustRegionCount(WorkingRegion, Result.ItemName, static_cast<int64>(Result.Quantity))) return false;
    if (!HasSettlementAccess(Pawn, *Profile, RegionID, Region, Hub)) return false;
    CommitRegionStock(Region, WorkingRegion);

    UE_LOG(LogTemp, Log, TEXT("CraftItemFromSettlementStockpile: '%s' crafted in region '%s'"), *RecipeName, *RegionID);
    return true;
}

void UEconomySystem::ProcessSettlementProductionTick(float DaysPassed)
{
    if (!FMath::IsFinite(DaysPassed) || DaysPassed <= 0.0f) return;
    const double Yield = 30.0 * static_cast<double>(DaysPassed);
    if (!FMath::IsFinite(Yield) || Yield >= static_cast<double>(MAX_int32) + 0.5) return;
    const int64 RoutedQuantity = FMath::RoundToInt64(Yield);
    if (RoutedQuantity < 0 || RoutedQuantity > MAX_int32) return;
    UGameInstance* GI = GetGameInstance();
    UNemesisManager* DB = GI ? GI->GetSubsystem<UNemesisManager>() : nullptr;
    if (!DB) return;
    DB->SynchronizeSimulation();

    auto ValidParent = [DB](const FString& ChildID, const FString& ParentID, bool bRequireSameClan)
    {
        const FRegionData* Child = DB->MapRegions.Find(ChildID);
        const FRegionData* Parent = DB->MapRegions.Find(ParentID);
        if (!Child || !Parent || ChildID == ParentID ||
            Child->RegionType == ESubRegionType::MilitaryFortress ||
            Parent->RegionType != ESubRegionType::MilitaryFortress ||
            (bRequireSameClan && Child->ControllingClanID != Parent->ControllingClanID)) return false;
        TSet<FString> Visited;
        Visited.Add(ChildID);
        FString Cursor = ParentID;
        while (!Cursor.IsEmpty())
        {
            if (Visited.Contains(Cursor)) return false;
            Visited.Add(Cursor);
            const FRegionData* Node = DB->MapRegions.Find(Cursor);
            if (!Node) return false;
            Cursor = Node->ParentFortressRegionID;
        }
        return true;
    };

    // Preserve existing 30/day commodity routing; do not introduce another yield policy.
    for (auto& VillagePair : DB->MapRegions)
    {
        FRegionData& Outpost = VillagePair.Value;
        if (!ValidParent(VillagePair.Key, Outpost.ParentFortressRegionID, true)) continue;
        FString CommodityKey = TEXT("Rice");
        if (Outpost.RegionType == ESubRegionType::IronMine) CommodityKey = TEXT("Iron");
        else if (Outpost.RegionType == ESubRegionType::LumberCamp) CommodityKey = TEXT("Lumber");
        else if (Outpost.RegionType == ESubRegionType::FishingVillage) CommodityKey = TEXT("Fish");
        FRegionData& Destination = DB->MapRegions.FindChecked(Outpost.ParentFortressRegionID);
        FRegionData Working = Destination;
        if (RoutedQuantity > 0 && StoreRegionMetadata(Working, CommodityKey, PrimaryUnitMetadata(CommodityKey)) &&
            AdjustRegionCount(Working, CommodityKey, RoutedQuantity)) CommitRegionStock(Destination, Working);
    }

    for (auto& FortPair : DB->MapRegions)
    {
        FRegionData& Fortress = FortPair.Value;
        if (Fortress.RegionType != ESubRegionType::MilitaryFortress || Fortress.ControllingClanID == TEXT("Neutral")) continue;
        FRegionData Working = Fortress;
        float* Iron = nullptr;
        float* Lumber = nullptr;
        float* Rice = nullptr;
        if (!ResolvePrimaryResourceBalance(Working, TEXT("Iron"), Iron) ||
            !ResolvePrimaryResourceBalance(Working, TEXT("Lumber"), Lumber) ||
            !ResolvePrimaryResourceBalance(Working, TEXT("Rice"), Rice)) continue;
        // Preserve the daily 60/60 -> +200 path, separately from the siege 50/50 -> +250 path.
        const double NewMax = static_cast<double>(Working.MaxFortressHealth) + 200.0;
        if (*Iron >= 60.0f && *Lumber >= 60.0f &&
            FMath::IsFinite(Working.MaxFortressHealth) && Working.MaxFortressHealth > 0.0f &&
            FMath::IsFinite(Working.FortressHealth) && Working.FortressHealth >= 0.0f &&
            Working.FortressHealth <= Working.MaxFortressHealth && IsExactStockBalance(NewMax) &&
            NewMax - Working.MaxFortressHealth == 200.0)
        {
            FRegionData Reinforced = Working;
            const double NewHealth = FMath::Min(static_cast<double>(Working.FortressHealth) + 200.0, NewMax);
            if (IsExactStockBalance(NewHealth) && AdjustRegionCount(Reinforced, TEXT("Iron"), -60) &&
                AdjustRegionCount(Reinforced, TEXT("Lumber"), -60))
            {
                CommitRegionStock(Working, Reinforced);
                Working.MaxFortressHealth = static_cast<float>(NewMax);
                Working.FortressHealth = static_cast<float>(NewHealth);
                UE_LOG(LogTemp, Log, TEXT("ECONOMY: Fortress %s reinforced. Max Wall Health: %f"), *Fortress.RegionID, Working.MaxFortressHealth);
            }
        }
        const bool bIsStarving = Working.Stockpile.Rice < 15.0f;
        const double FoodAfter = FMath::Max(0.0, static_cast<double>(Working.Stockpile.Rice) - 15.0);
        if (!IsExactStockBalance(FoodAfter) ||
            static_cast<double>(Working.Stockpile.Rice) - FoodAfter != FMath::Min(15.0, static_cast<double>(Working.Stockpile.Rice))) continue;
        Working.Stockpile.Rice = static_cast<float>(FoodAfter);
        CommitRegionStock(Fortress, Working);
        Fortress.MaxFortressHealth = Working.MaxFortressHealth;
        Fortress.FortressHealth = Working.FortressHealth;
        // Existing starvation seizure policy retained, with hierarchy validation only.
        if (bIsStarving)
        {
            for (auto& OppPair : DB->MapRegions)
            {
                FRegionData& Opponent = OppPair.Value;
                if (Opponent.ControllingClanID != Fortress.ControllingClanID &&
                    Opponent.RegionType == ESubRegionType::FarmingVillage && ValidParent(OppPair.Key, FortPair.Key, false))
                {
                    Opponent.ControllingClanID = Fortress.ControllingClanID;
                    Opponent.ParentFortressRegionID = FortPair.Key;
                    Opponent.Status = ETerritoryStatus::War;
                    Opponent.DangerLevel = 100.0f;
                    UE_LOG(LogTemp, Warning, TEXT("ECONOMY: Clan '%s' seized outstation %s due to starvation!"), *Fortress.ControllingClanID, *Opponent.RegionName);
                    break;
                }
            }
        }
    }

    // Preserve consolidation, with paired preflight; never zero a donor after failed credit.
    for (auto& Pair : DB->MapRegions)
    {
        FRegionData& Child = Pair.Value;
        if (!ValidParent(Pair.Key, Child.ParentFortressRegionID, true)) continue;
        FRegionData& Parent = DB->MapRegions.FindChecked(Child.ParentFortressRegionID);
        FRegionData From = Child;
        FRegionData To = Parent;
        bool bValid = true;
        for (const TCHAR* Name : { TEXT("Rice"), TEXT("Iron"), TEXT("Lumber"), TEXT("Fish") })
        {
            float* Source = nullptr;
            float* Target = nullptr;
            if (!ResolvePrimaryResourceBalance(From, Name, Source) || !ResolvePrimaryResourceBalance(To, Name, Target)) { bValid = false; break; }
            const double Amount = *Source;
            if (Amount == 0.0) continue;
            FItemData Metadata;
            const double Combined = static_cast<double>(*Target) + Amount;
            if (!IsExactStockBalance(Combined) || Combined - *Target != Amount || Combined - Amount != *Target ||
                !ReadRegionMetadata(From, Name, Metadata) || !StoreRegionMetadata(To, Name, Metadata)) { bValid = false; break; }
            *Target = static_cast<float>(Combined);
            *Source = 0.0f;
        }
        if (bValid) { CommitRegionStock(Child, From); CommitRegionStock(Parent, To); }
    }
    // Health and stockpile changes also require a refresh, even without conquest.
    TArray<AActor*> Hubs;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), ASettlementHub::StaticClass(), Hubs);
    for (AActor* Actor : Hubs)
        if (ASettlementHub* Hub = Cast<ASettlementHub>(Actor)) Hub->SyncWithBackendDatabase();
}