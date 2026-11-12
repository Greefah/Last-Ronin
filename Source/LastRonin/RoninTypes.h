#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "GameplayTagContainer.h"
#include "Engine/DataTable.h"
#include "RoninTypes.generated.h"

// =======================================================================
// 1. ENUMS (Global State Definitions)
// =======================================================================

UENUM(BlueprintType)
enum class ERoninGender : uint8 { Male, Female, NonBinary, YokaiEntity };

UENUM(BlueprintType)
enum class EHunterClass : uint8 {
    Nameless, Bandit, Hunter, BanditChief, KingHunter, RoninKing, Thrall, Usurper, GodTier,
    Child, Farmer, Citizen, Soldier, Blacksmith, Merchant, Chef, Doctor, Fisherman, Yokai
};

UENUM(BlueprintType)
enum class EMoralAlignment : uint8 { LawfulGood, ChaoticGood, TrueNeutral, LawfulEvil, ChaoticEvil };

UENUM(BlueprintType)
enum class ERoninRarity : uint8 { Common, Rare, Legendary, Legacy };

UENUM(BlueprintType)
enum class EElementalType : uint8 { None, Fire, Water, Ice, Earth, Air, Lightning, Light, Dark, Blood };

UENUM(BlueprintType)
enum class ESamuraiStance : uint8 { Flash, Heavy, Forbidden, Defensive };

UENUM(BlueprintType)
enum class ETerritoryStatus : uint8 { Peace, Contested, War, Siege, Usurped };

UENUM(BlueprintType)
enum class EFamilyTie : uint8 {
    None, Spouse, Parent, Child, Sibling,
    Master, Slave, Enemy, Rival,
    Student, Friend, BestFriend,
    Grandparent, Aunt, Uncle
};

UENUM(BlueprintType)
enum class ERumorType : uint8 { Crime, HeroicDeed, Folklore };

UENUM(BlueprintType)
enum class EItemType : uint8 { Resource, Food, Drink, Weapon, Armor, RareYokaiDrop };

UENUM(BlueprintType)
enum class ESubRegionType : uint8 {
    MilitaryFortress    UMETA(DisplayName = "Parent Administrative Fortress"),
    FarmingVillage      UMETA(DisplayName = "Resource Node - Produces Rice"),
    IronMine            UMETA(DisplayName = "Resource Node - Produces Iron Ore"),
    LumberCamp          UMETA(DisplayName = "Resource Node - Produces Lumber"),
    GuardPost           UMETA(DisplayName = "Defensive Node - Enhances Fortification"),
    HiddenCamp          UMETA(DisplayName = "Contested Node - Generates Unrest/Heat"),
    FishingVillage      UMETA(DisplayName = "Resource Node - Produces Fish")
};

UENUM(BlueprintType)
enum class EVillageSpecialty : uint8 { Rice, Lumber, Fish, Iron };

// =======================================================================
// 2. STOCKPILE & GRID STRUCTURES
// =======================================================================

USTRUCT(BlueprintType)
struct FStockpile {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Rice = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Lumber = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Fish = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Iron = 0.0f;
};

USTRUCT(BlueprintType)
struct FIntVector2D {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 X = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Y = 0;
    bool operator==(const FIntVector2D& Other) const { return X == Other.X && Y == Other.Y; }
};

FORCEINLINE uint32 GetTypeHash(const FIntVector2D& Key) { return HashCombine(GetTypeHash(Key.X), GetTypeHash(Key.Y)); }

USTRUCT(BlueprintType)
struct FFeudalTravelRoute {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector DestinationTarget = FVector::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float   TravelSpeed = 400.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool    bIsTravelling = false;
};

// =======================================================================
// 3. DATA STRUCTURES
// =======================================================================

USTRUCT(BlueprintType)
struct FBuildingData {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString      BuildingID;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FGameplayTag BuildingTypeTag;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float        CurrentHealth = 500.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool         bIsFunctional = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector      WorldLocation = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct FItemData {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString      ItemName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) EItemType    Type = EItemType::Resource;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float        Potency = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32        Quantity = 1;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ERoninRarity Rarity = ERoninRarity::Common;
};

USTRUCT(BlueprintType)
struct FRecipe {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString             RecipeName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TMap<FString, int32> Ingredients;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FItemData           Result;
};

// Groups all character UniqueIDs occupying a single spatial grid cell.
// Populated each frame by NemesisManager::ScanAndManagePhysicalPopulation.
USTRUCT(BlueprintType)
struct FSpatialCellGroup {
    GENERATED_BODY()
    UPROPERTY(BlueprintReadWrite, Category = "Nemesis|Mass Scale")
    TSet<FString> CharacterIDs;
};

USTRUCT(BlueprintType)
struct FRegionData {
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString          RegionID;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString          RegionName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString          ControllingClanID = TEXT("Neutral");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float            FortressHealth = 1000.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float            MaxFortressHealth = 1000.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ETerritoryStatus Status = ETerritoryStatus::Peace;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float            DistrictValue = 0.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float            DangerLevel = 0.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FBuildingData> Structures;

    // Canonical primary resource balances (Rice/Iron/Lumber/Fish).
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FStockpile Stockpile;

    // Arbitrary item counts and unreconciled legacy primary balances.
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TMap<FString, int32> SharedStockpile;

    // Metadata only: Quantity is zero; never a second count ledger.
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TMap<FString, FItemData> SharedStockpileMetadata;

    UPROPERTY(EditAnywhere, BlueprintReadWrite) EVillageSpecialty ResourceSpecialty = EVillageSpecialty::Rice;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Diplomacy")
    TMap<FString, int32> ActiveTradeRoutes;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settlement|Hierarchy")
    ESubRegionType RegionType = ESubRegionType::FarmingVillage;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settlement|Hierarchy")
    FString ParentFortressRegionID = TEXT("");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settlement|Metrics")
    float LocalHeat = 0.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Settlement|Metrics")
    float LocalFortificationValue = 100.0f;
};

USTRUCT(BlueprintType)
struct FRelationshipData {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32      Affinity = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) EFamilyTie TieType = EFamilyTie::None;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool       bIsDating = false;
};

USTRUCT(BlueprintType)
struct FRumorData {
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ERumorType Type = ERumorType::Crime;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString    SubjectID = TEXT("");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector    LastKnownLocation = FVector::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float      SpreadLevel = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString    Fact;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString    Folklore;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString    Misinformation;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString    RawRumorText;
};

USTRUCT(BlueprintType)
struct FHunterMemory {
    GENERATED_BODY()
    UPROPERTY(BlueprintReadWrite) FString   ActionLabel = TEXT("");
    UPROPERTY(BlueprintReadWrite) FDateTime TimeStamp;
    UPROPERTY(BlueprintReadWrite) int32     GenerationAtTime = 1;
};

USTRUCT(BlueprintType)
struct FBiographyData
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biography") FString OriginStory;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biography") FString NotableDeeds;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biography") FString CurrentRumor;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biography") FString LastWords;
};

// Forward declaration � full definition lives in RoninJobArchetype.h
class URoninJobArchetype;

USTRUCT(BlueprintType)
struct FHunterProfile {
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString UniqueID = TEXT("");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString HunterName = TEXT("Nameless");
    UPROPERTY(EditAnywhere, BlueprintReadWrite) ERoninGender    Gender = ERoninGender::Male;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString         ClanID = TEXT("Neutral");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hunter DNA|Status")
    bool bIsAlive = true;

    UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Hunter DNA")
    EMoralAlignment Alignment = EMoralAlignment::TrueNeutral;

    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector WorldLocation = FVector::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool    bIsNamed = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32   PowerLevel = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Karma = 0.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Heat = 0.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Notoriety = 0.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float InheritedFame = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Wealth = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Hunger = 100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Thirst = 100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Potential = 100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Stamina = 100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) float Blood = 100.f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Age = 18;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 LegacyGeneration = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FHunterMemory>            MemoryBank;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<FString>                  LootedLegacyGear;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TMap<FString, FRelationshipData> RelationshipWeb;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) TMap<FString, FItemData>         Inventory;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Profile|Lore")
    FBiographyData Biography;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hunter DNA|Visuals")
    FString SkeletalMeshPath = TEXT("");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hunter DNA|Job Rules")
    URoninJobArchetype* JobClassData = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hunter DNA|Mass Scale")
    FFeudalTravelRoute ActiveTravelRoute;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hunter DNA|Mass Scale")
    FIntVector2D AssignedSpatialGridCell;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hunter DNA|Visuals")
    TSoftObjectPtr<USkeletalMesh> AsyncSkeletalMeshHandle;

    FHunterProfile()
    {
        bIsAlive = true;
        ElementalAffinities.Empty();
        ElementalAffinities.Add(EElementalType::None);
    }

    void PruneDanglingReferences(const FString& TargetID)
    {
        if (RelationshipWeb.Contains(TargetID)) RelationshipWeb.Remove(TargetID);
        if (VendettaList.Contains(TargetID))    VendettaList.Remove(TargetID);
    }

    UPROPERTY(EditAnywhere, BlueprintReadWrite) TArray<EElementalType> ElementalAffinities;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) EHunterClass HunterClass = EHunterClass::Nameless;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nemesis|Multiplayer") bool    bIsRealPlayer = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nemesis|Multiplayer") FString UniquePlayerAccountID;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nemesis|Dynasty")     bool    bIsOfflineAI = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hunter DNA|Traits")
    FGameplayTagContainer InheritedTraits;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hunter DNA|Vendettas")
    TMap<FString, float> VendettaList;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hunter DNA|Lineage")
    FString MotherID = TEXT("");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hunter DNA|Lineage")
    FString FatherID = TEXT("");
};

USTRUCT(BlueprintType)
struct FFamilyNode {
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) FHunterProfile Profile;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) EFamilyTie     TieType;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32          Affinity;
};

// =======================================================================
// 4. DATA-DRIVEN SEEDING OVERLAYS (Authoritative Global Scope)
// =======================================================================

USTRUCT(BlueprintType)
struct FRoninRegionRow : public FTableRowBase
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region Seeding Blueprint")
    FString RegionID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region Seeding Blueprint")
    FString RegionName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region Seeding Blueprint")
    ESubRegionType RegionType = ESubRegionType::FarmingVillage;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region Seeding Blueprint")
    FString ParentFortressRegionID = TEXT("");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region Seeding Blueprint")
    float BaseMaxFortressHealth = 1000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Region Seeding Blueprint")
    float InitialDistrictValue = 50.0f;
};

// -----------------------------------------------------------------------
// DataTable row for Job Registry mapping to URoninJobArchetype assets
// -----------------------------------------------------------------------
USTRUCT(BlueprintType)
struct FJobRegistryRow : public FTableRowBase
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Job Registry")
    URoninJobArchetype* JobArchetype = nullptr;
};

// =======================================================================
// 5. ADDITIVE VALUE-ONLY PRODUCER INTERFACES
// =======================================================================

/** A derived view only: never writes back to the profile or changes its IDs. */
USTRUCT(BlueprintType)
struct LASTRONIN_API FRoninPersona
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ronin|Persona")
    float Karma = 0.f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ronin|Persona")
    float Heat = -100.f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ronin|Persona")
    float Fame = 0.f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ronin|Persona")
    float HudHeat = 0.f;

    static FRoninPersona FromProfile(const FHunterProfile& Profile)
    {
        FRoninPersona Result;
        Result.Karma = ClampSigned(Profile.Karma);
        Result.HudHeat = FMath::Clamp(FMath::IsFinite(Profile.Heat) ? Profile.Heat : 0.f, 0.f, 100.f);
        Result.Heat = Result.HudHeat * 2.f - 100.f;
        Result.Fame = ClampSigned(Profile.InheritedFame);
        return Result;
    }

    static float ClampSigned(float Value)
    {
        return FMath::Clamp(FMath::IsFinite(Value) ? Value : 0.f, -100.f, 100.f);
    }
};

/** GUID interop only. Legacy/non-GUID string IDs remain authoritative and unchanged. */
struct LASTRONIN_API FRoninGuidAdapter
{
    // Failure leaves OutGuid unchanged. No generated or hashed replacement IDs.
    static bool TryParse(const FString& CharacterID, FGuid& OutGuid)
    {
        FGuid Parsed;
        if (!FGuid::Parse(CharacterID, Parsed))
        {
            return false;
        }
        OutGuid = Parsed;
        return true;
    }

    // Explicit formatting for new GUID IDs, not a round-trip of an existing string.
    static FString ToString(const FGuid& Guid)
    {
        return Guid.ToString(EGuidFormats::DigitsWithHyphens);
    }
};

/** Owned simulation values. Contains no UObject, weak/soft object, or borrowed references. */
USTRUCT(BlueprintType)
struct LASTRONIN_API FNPCSimPacket
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Simulation")
    FString CharacterID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Simulation")
    FVector WorldLocation = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Simulation")
    FFeudalTravelRoute TravelRoute;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Simulation")
    FIntVector2D SpatialCell;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Simulation")
    FString ClanID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Simulation")
    FString MotherID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Simulation")
    FString FatherID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Simulation")
    int32 Wealth = 0;

    // Raw profile heat, not the signed persona view.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Simulation")
    float Heat = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Simulation")
    bool bIsAlive = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Simulation")
    bool bIsRealized = false;

    // Reflected for native serialization; uint64 is not exposed as a Blueprint pin.
    UPROPERTY()
    uint64 WorldEpoch = 0;

    UPROPERTY()
    uint64 Revision = 0;

    // Epoch/revision and realization state must be supplied by the owning producer.
    static FNPCSimPacket FromProfile(const FHunterProfile& Profile, uint64 InWorldEpoch,
        uint64 InRevision, bool bInIsRealized)
    {
        FNPCSimPacket Result;
        Result.CharacterID = Profile.UniqueID;
        Result.WorldLocation = Profile.WorldLocation;
        Result.TravelRoute = Profile.ActiveTravelRoute;
        Result.SpatialCell = Profile.AssignedSpatialGridCell;
        Result.ClanID = Profile.ClanID;
        Result.MotherID = Profile.MotherID;
        Result.FatherID = Profile.FatherID;
        Result.Wealth = Profile.Wealth;
        Result.Heat = Profile.Heat;
        Result.bIsAlive = Profile.bIsAlive;
        Result.bIsRealized = bInIsRealized;
        Result.WorldEpoch = InWorldEpoch;
        Result.Revision = InRevision;
        return Result;
    }
};
