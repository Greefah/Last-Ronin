#include "NemesisManager.h"
#include "DynastySubsystem.h"
#include "EconomySystem.h"
#include "RoninCharacter.h"
#include "BaseHunter.h"
#include "DeepSeek_AI.h"
#include "SettlementHub.h"
#include "DialogueSystem.h"
#include "AIController.h"
#include "BrainComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "RoninSaveGame.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Async/AsyncWork.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "Engine/AssetManager.h"
#include "URoninQuestManager.h"
#include "Subsystems/SubsystemCollection.h"
#include "RoninGameMode.h"
#include "GameFramework/PlayerController.h"

namespace
{
    bool TrySpatialCell(const FVector& Location, int32 ConfiguredSize, FIntVector2D& Cell)
    {
        if (!FMath::IsFinite(Location.X) || !FMath::IsFinite(Location.Y) || !FMath::IsFinite(Location.Z)) return false;
        const int32 Size = FMath::Max(1, ConfiguredSize);
        const double X = FMath::FloorToDouble(Location.X / Size);
        const double Y = FMath::FloorToDouble(Location.Y / Size);
        if (X < MIN_int32 || X > MAX_int32 || Y < MIN_int32 || Y > MAX_int32) return false;
        Cell = FIntVector2D(static_cast<int32>(X), static_cast<int32>(Y));
        return true;
    }

    // Copies make legacy migration plus debit/credit atomic across both region ledgers.
    bool TransferManagerStock(FRegionData& Source, FRegionData& Target, const FString& Name, double Amount)
    {
        if (&Source == &Target || !FMath::IsFinite(Amount) || Amount <= 0.0) return false;
        FRegionData From = Source;
        FRegionData To = Target;
        double SourceCount = 0.0;
        double TargetCount = 0.0;
        float* FromPrimary = UEconomySystem::GetPrimaryResourceField(From.Stockpile, Name);
        float* ToPrimary = nullptr;
        if (FromPrimary)
        {
            if (!UEconomySystem::ResolvePrimaryResourceBalance(From, Name, FromPrimary) ||
                !UEconomySystem::ResolvePrimaryResourceBalance(To, Name, ToPrimary)) return false;
            SourceCount = *FromPrimary;
            TargetCount = *ToPrimary;
            const double Debit = SourceCount - Amount;
            const double Credit = TargetCount + Amount;
            if (!FMath::IsFinite(Credit) || Debit < 0.0 || Credit > MAX_flt ||
                static_cast<double>(static_cast<float>(Debit)) != Debit ||
                static_cast<double>(static_cast<float>(Credit)) != Credit ||
                SourceCount - Debit != Amount || Credit - TargetCount != Amount ||
                Debit + Amount != SourceCount || Credit - Amount != TargetCount) return false;
            *FromPrimary = static_cast<float>(Debit);
            *ToPrimary = static_cast<float>(Credit);
        }
        else
        {
            SourceCount = From.SharedStockpile.FindRef(Name);
            TargetCount = To.SharedStockpile.FindRef(Name);
            if (Amount > MAX_int32 || Amount != static_cast<int32>(Amount) ||
                SourceCount < Amount || TargetCount < 0.0 || TargetCount + Amount > MAX_int32) return false;
            const int32 Remaining = static_cast<int32>(SourceCount - Amount);
            if (Remaining == 0) From.SharedStockpile.Remove(Name);
            else From.SharedStockpile.Add(Name, Remaining);
            To.SharedStockpile.Add(Name, static_cast<int32>(TargetCount + Amount));
        }
        const FItemData* SourceMetadata = From.SharedStockpileMetadata.Find(Name);
        const FItemData* TargetMetadata = To.SharedStockpileMetadata.Find(Name);
        if ((SourceMetadata && !UEconomySystem::IsValidItemMetadata(*SourceMetadata)) ||
            (TargetMetadata && !UEconomySystem::IsValidItemMetadata(*TargetMetadata))) return false;
        if (TargetCount > 0.0 && SourceMetadata && TargetMetadata &&
            !UEconomySystem::HaveMatchingItemMetadata(*SourceMetadata, *TargetMetadata)) return false;
        if (SourceMetadata && (TargetCount == 0.0 || !TargetMetadata))
        {
            FItemData Metadata = *SourceMetadata;
            Metadata.ItemName = Name;
            Metadata.Quantity = 0;
            To.SharedStockpileMetadata.Add(Name, Metadata);
        }
        Source.Stockpile = From.Stockpile;
        Source.SharedStockpile = MoveTemp(From.SharedStockpile);
        Source.SharedStockpileMetadata = MoveTemp(From.SharedStockpileMetadata);
        Target.Stockpile = To.Stockpile;
        Target.SharedStockpile = MoveTemp(To.SharedStockpile);
        Target.SharedStockpileMetadata = MoveTemp(To.SharedStockpileMetadata);
        return true;
    }
}

class FAsyncFeudalSimulationWorker : public FNonAbandonableTask
{
    friend class FAsyncTask<FAsyncFeudalSimulationWorker>;

public:
    FAsyncFeudalSimulationWorker(const TArray<FNPCSimPacket>& InPackets, float InDaysPassed, int32 InCellSize)
        : Inputs(InPackets), Outputs(InPackets), DaysPassed(InDaysPassed), CellSize(FMath::Max(1, InCellSize))
    {
    }

    FORCEINLINE TStatId GetStatId() const
    {
        RETURN_QUICK_DECLARE_CYCLE_STAT(FAsyncFeudalSimulationWorker, STATGROUP_ThreadPoolAsyncTasks);
    }

    void DoWork()
    {
        // Only owned values are touched here. Profile maps and UObjects stay on the game thread.
        if (!FMath::IsFinite(DaysPassed) || DaysPassed <= 0.0f) return;
        for (FNPCSimPacket& Packet : Outputs)
        {
            if (!Packet.bIsAlive || Packet.bIsRealized ||
                !TrySpatialCell(Packet.WorldLocation, CellSize, Packet.SpatialCell) ||
                !FMath::IsFinite(Packet.TravelRoute.DestinationTarget.X) ||
                !FMath::IsFinite(Packet.TravelRoute.DestinationTarget.Y) ||
                !FMath::IsFinite(Packet.TravelRoute.DestinationTarget.Z) ||
                !FMath::IsFinite(Packet.TravelRoute.TravelSpeed) || Packet.TravelRoute.TravelSpeed < 0.f) continue;
            // Stable per character/day, independent of map iteration and worker scheduling.
            uint32 Seed = 2166136261u;
            for (const TCHAR Character : Packet.CharacterID)
                Seed = (Seed ^ static_cast<uint32>(Character)) * 16777619u;
            Seed ^= static_cast<uint32>(Packet.Revision);
            Seed ^= static_cast<uint32>(Packet.Revision >> 32);
            FRandomStream Random(static_cast<int32>(Seed & 0x7fffffffu));
            FFeudalTravelRoute& Route = Packet.TravelRoute;
            if (Route.bIsTravelling)
            {
                FVector Direction = Route.DestinationTarget - Packet.WorldLocation;
                const double DistanceLeft = Direction.Size();
                const double DeltaMove = FMath::Max(0.0f, Route.TravelSpeed) * DaysPassed * 24.0;
                if (DistanceLeft <= DeltaMove)
                {
                    Packet.WorldLocation = Route.DestinationTarget;
                    Route.bIsTravelling = false;
                }
                else
                {
                    Direction.Normalize();
                    Packet.WorldLocation += Direction * DeltaMove;
                }
            }
            else if (Random.RandRange(0, 1000) > 980)
            {
                const double X = Random.FRandRange(-60000.0, 60000.0);
                const double Y = Random.FRandRange(-60000.0, 60000.0);
                Route.DestinationTarget = FVector(X, Y, 120.0);
                Route.bIsTravelling = true;
                Route.TravelSpeed = static_cast<float>(Random.FRandRange(350.0, 550.0));
            }
            if (!TrySpatialCell(Packet.WorldLocation, CellSize, Packet.SpatialCell))
                Packet = Inputs[&Packet - Outputs.GetData()];
        }
    }

    TArray<FNPCSimPacket> Inputs;
    TArray<FNPCSimPacket> Outputs;

private:
    float DaysPassed;
    int32 CellSize;
};

void UNemesisManager::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    Collection.InitializeDependency<URoninQuestManager>();

    // Attempt to restore saved world state first.
    LoadWorldState();
    // A rejected existing slot is valuable: neither seed nor autosave over it.
    if (bRejectedWorldLoad) return;

    // If we have no saved world, seed. A successfully loaded empty world stays empty.
    bool bUsedFallbackSeeding = false;

    if (MapRegions.Num() == 0 && !bLoadedWorldState)
    {
        if (RegionSeedingTable && RegionSeedingTable->GetRowStruct() == FRoninRegionRow::StaticStruct())
        {
            // Seed from DataTable (authoritative); never reinterpret an unrelated row schema.
            TArray<FRoninRegionRow*> Rows;
            RegionSeedingTable->GetAllRows<FRoninRegionRow>(TEXT("Master Map Initialization Pass"), Rows);

            for (const FRoninRegionRow* Row : Rows)
            {
                if (!Row || Row->RegionID.IsEmpty()) continue;
                if (MapRegions.Contains(Row->RegionID)) continue; // defensive

                FRegionData NewRegion;
                NewRegion.RegionID = Row->RegionID;
                NewRegion.RegionName = Row->RegionName;
                NewRegion.RegionType = Row->RegionType;
                NewRegion.ParentFortressRegionID = Row->ParentFortressRegionID;
                NewRegion.MaxFortressHealth = Row->BaseMaxFortressHealth;
                NewRegion.FortressHealth = Row->BaseMaxFortressHealth;
                NewRegion.DistrictValue = Row->InitialDistrictValue;
                NewRegion.ControllingClanID = TEXT("Neutral");
                NewRegion.Status = ETerritoryStatus::Peace;

                MapRegions.Add(NewRegion.RegionID, NewRegion);
            }

            UE_LOG(LogTemp, Log, TEXT("NemesisManager: seeded %d regions from RegionSeedingTable."), MapRegions.Num());
        }
        if (MapRegions.Num() == 0)
        {
            // Snapshot of the 26 authored DT_MapRegions rows; saved regions always take precedence.
            bUsedFallbackSeeding = true;
            struct FRegionSeed
            {
                const TCHAR* ID;
                const TCHAR* Name;
                ESubRegionType Type;
                const TCHAR* Parent;
                float MaxHealth;
                float Value;
            };
            const FRegionSeed Seeds[] = {
                {TEXT("R_Fortress_01"), TEXT("Kurokawa Keep"), ESubRegionType::MilitaryFortress, TEXT(""), 1500.f, 120.f},
                {TEXT("R_Fortress_02"), TEXT("Shiroyama Hold"), ESubRegionType::MilitaryFortress, TEXT(""), 1500.f, 110.f},
                {TEXT("R_Fortress_03"), TEXT("Mizunoe Bastion"), ESubRegionType::MilitaryFortress, TEXT(""), 1500.f, 115.f},
                {TEXT("R_Fortress_04"), TEXT("Tsuchioka Citadel"), ESubRegionType::MilitaryFortress, TEXT(""), 1500.f, 105.f},
                {TEXT("R_Fortress_05"), TEXT("Akagane Stronghold"), ESubRegionType::MilitaryFortress, TEXT(""), 1500.f, 125.f},
                {TEXT("R_Fortress_06"), TEXT("Seiryū Watch"), ESubRegionType::MilitaryFortress, TEXT(""), 1500.f, 100.f},
                {TEXT("R_Village_01"), TEXT("Hana Village"), ESubRegionType::FarmingVillage, TEXT("R_Fortress_01"), 200.f, 30.f},
                {TEXT("R_Village_02"), TEXT("Ishigane Hamlet"), ESubRegionType::IronMine, TEXT("R_Fortress_01"), 200.f, 25.f},
                {TEXT("R_Village_03"), TEXT("Yamato Lumber Camp"), ESubRegionType::LumberCamp, TEXT("R_Fortress_01"), 200.f, 20.f},
                {TEXT("R_Village_04"), TEXT("Minato Hamlet"), ESubRegionType::FishingVillage, TEXT("R_Fortress_01"), 200.f, 15.f},
                {TEXT("R_Village_05"), TEXT("Sakura Village"), ESubRegionType::FarmingVillage, TEXT("R_Fortress_02"), 200.f, 28.f},
                {TEXT("R_Village_06"), TEXT("Nishi Hamlet"), ESubRegionType::LumberCamp, TEXT("R_Fortress_02"), 200.f, 22.f},
                {TEXT("R_Village_07"), TEXT("Kawa Village"), ESubRegionType::FishingVillage, TEXT("R_Fortress_02"), 200.f, 18.f},
                {TEXT("R_Village_08"), TEXT("Tanaka Village"), ESubRegionType::FarmingVillage, TEXT("R_Fortress_02"), 200.f, 12.f},
                {TEXT("R_Village_09"), TEXT("Fuji Village"), ESubRegionType::FarmingVillage, TEXT("R_Fortress_03"), 200.f, 32.f},
                {TEXT("R_Village_10"), TEXT("Oka Hamlet"), ESubRegionType::IronMine, TEXT("R_Fortress_03"), 200.f, 20.f},
                {TEXT("R_Village_11"), TEXT("Sugi Village"), ESubRegionType::LumberCamp, TEXT("R_Fortress_03"), 200.f, 16.f},
                {TEXT("R_Village_12"), TEXT("Mori Hamlet"), ESubRegionType::FishingVillage, TEXT("R_Fortress_03"), 200.f, 10.f},
                {TEXT("R_Village_13"), TEXT("Sora Village"), ESubRegionType::FarmingVillage, TEXT("R_Fortress_04"), 200.f, 26.f},
                {TEXT("R_Village_14"), TEXT("Hama Hamlet"), ESubRegionType::FishingVillage, TEXT("R_Fortress_04"), 200.f, 14.f},
                {TEXT("R_Village_15"), TEXT("Kishi Village"), ESubRegionType::LumberCamp, TEXT("R_Fortress_04"), 200.f, 11.f},
                {TEXT("R_Village_16"), TEXT("Koba Village"), ESubRegionType::IronMine, TEXT("R_Fortress_05"), 200.f, 35.f},
                {TEXT("R_Village_17"), TEXT("Taka Hamlet"), ESubRegionType::FarmingVillage, TEXT("R_Fortress_05"), 200.f, 20.f},
                {TEXT("R_Village_18"), TEXT("Yama Village"), ESubRegionType::LumberCamp, TEXT("R_Fortress_05"), 200.f, 18.f},
                {TEXT("R_Village_19"), TEXT("Ueda Village"), ESubRegionType::FarmingVillage, TEXT("R_Fortress_06"), 200.f, 22.f},
                {TEXT("R_Village_20"), TEXT("Mizu Village"), ESubRegionType::FishingVillage, TEXT("R_Fortress_06"), 200.f, 13.f}
            };
            for (const FRegionSeed& Seed : Seeds)
            {
                FRegionData Region;
                Region.RegionID = Seed.ID;
                Region.RegionName = Seed.Name;
                Region.RegionType = Seed.Type;
                Region.ParentFortressRegionID = Seed.Parent;
                Region.MaxFortressHealth = Seed.MaxHealth;
                Region.FortressHealth = Seed.MaxHealth;
                Region.DistrictValue = Seed.Value;
                Region.ControllingClanID = TEXT("Neutral");
                Region.Status = ETerritoryStatus::Peace;
                MapRegions.Add(Region.RegionID, Region);
            }
            UE_LOG(LogTemp, Warning, TEXT("NemesisManager: missing, incompatible or empty region table; applied 26-region fallback. Existing saved regions are never replaced."));
        }
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("NemesisManager: Loaded %d regions from saved world state; seeding skipped."), MapRegions.Num());
    }

    if (!GlobalClanFriction.Contains(TEXT("BloodMoon_Player")))
        GlobalClanFriction.Add(TEXT("BloodMoon_Player"), 0.0f);
    if (!GlobalClanFriction.Contains(TEXT("Samurai_Shogunate")))
        GlobalClanFriction.Add(TEXT("Samurai_Shogunate"), 25.0f);

    if (GEngine)
    {
        FString BootMsg = FString::Printf(
            TEXT("NEMESIS REGISTRY ONLINE: Loaded %d Dynamic Regions | %d Profiles Active"),
            MapRegions.Num(),
            ActiveNemeses.Num()
        );
        GEngine->AddOnScreenDebugMessage(-1, 10.0f, FColor::Green, BootMsg);
    }

    // Process-lifetime registration routes through the invoking world, never an old subsystem.
    static FAutoConsoleCommandWithWorld DumpRegionsCommand(
        TEXT("DumpRegions"),
        TEXT("Prints a summary of MapRegions and ActiveNemeses to the log."),
        FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
        {
            UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
            UNemesisManager* Manager = GI ? GI->GetSubsystem<UNemesisManager>() : nullptr;
            if (!Manager || Manager->bDeinitializing) return;
            UE_LOG(LogTemp, Log, TEXT("=== DUMP REGIONS ==="));
            for (const auto& Pair : Manager->MapRegions)
            {
                UE_LOG(LogTemp, Log, TEXT("Region: %s, Owner: %s"),
                    *Pair.Value.RegionID, *Pair.Value.ControllingClanID);
            }
            UE_LOG(LogTemp, Log, TEXT("=== DUMP NEMESES ==="));
            int32 Count = 0;
            for (const auto& Pair : Manager->ActiveNemeses)
            {
                UE_LOG(LogTemp, Log, TEXT("Hunter: %s, Age: %d"),
                    *Pair.Value.HunterName, Pair.Value.Age);
                if (++Count >= 300) break;
            }
        }));

    // Only save automatically if we seeded from the authoritative DataTable or loaded from disk.
    if (!bUsedFallbackSeeding)
    {
        SaveWorldState();
    }
}

void UNemesisManager::SynchronizeSimulation()
{
    check(IsInGameThread());
    if (!ActiveSimulationThreadTask) return;
    ActiveSimulationThreadTask->EnsureCompletion();
    const FAsyncFeudalSimulationWorker& Worker = ActiveSimulationThreadTask->GetTask();
    const bool bCurrentWorld = !bDeinitializing && SimulationWorld.IsValid() &&
        SimulationWorld.Get() == GetWorld();
    for (int32 Index = 0; bCurrentWorld && Index < Worker.Outputs.Num(); ++Index)
    {
        if (!Worker.Inputs.IsValidIndex(Index)) break;
        const FNPCSimPacket& Before = Worker.Inputs[Index];
        const FNPCSimPacket& After = Worker.Outputs[Index];
        const uint64 CurrentRevision = (static_cast<uint64>(static_cast<uint32>(CurrentYear)) << 32) |
            static_cast<uint32>(CurrentDay);
        if (Before.WorldEpoch != WorldEpoch || After.WorldEpoch != WorldEpoch ||
            Before.CharacterID != After.CharacterID || Before.Revision != After.Revision ||
            Before.Revision != CurrentRevision) continue;
        FHunterProfile* Profile = ActiveNemeses.Find(After.CharacterID);
        if (!Profile || !Profile->bIsAlive || Profile->bIsRealPlayer ||
            After.CharacterID == TEXT("PLAYER") || SpawnedPhysicalHunters.Contains(After.CharacterID)) continue;
        const FFeudalTravelRoute& Route = Profile->ActiveTravelRoute;
        if (Profile->UniqueID != Before.CharacterID ||
            Profile->WorldLocation != Before.WorldLocation ||
            !(Profile->AssignedSpatialGridCell == Before.SpatialCell) ||
            Profile->ClanID != Before.ClanID || Profile->MotherID != Before.MotherID ||
            Profile->FatherID != Before.FatherID || Profile->Wealth != Before.Wealth ||
            Profile->Heat != Before.Heat || Profile->bIsAlive != Before.bIsAlive || Before.bIsRealized ||
            Route.DestinationTarget != Before.TravelRoute.DestinationTarget ||
            Route.TravelSpeed != Before.TravelRoute.TravelSpeed ||
            Route.bIsTravelling != Before.TravelRoute.bIsTravelling) continue;
        FIntVector2D CheckedCell;
        if (!TrySpatialCell(After.WorldLocation, SpatialGridCellSize, CheckedCell) ||
            !(CheckedCell == After.SpatialCell)) continue;
        Profile->WorldLocation = After.WorldLocation;
        Profile->ActiveTravelRoute = After.TravelRoute;
        Profile->AssignedSpatialGridCell = CheckedCell;
    }
    delete ActiveSimulationThreadTask;
    ActiveSimulationThreadTask = nullptr;
}

void UNemesisManager::Serialize(FArchive& Ar)
{
    // An active worker is owned by the game thread; never serialize a half-applied result.
    if (ActiveSimulationThreadTask) SynchronizeSimulation();
    Super::Serialize(Ar);
}

void UNemesisManager::InvalidateWorldRequests()
{
    check(IsInGameThread());
    ++WorldEpoch; // Invalidate delegates before cancellation can run any completion work.
    TMap<FString, TSharedPtr<FStreamableHandle>> Requests = MoveTemp(PendingMeshRequests);
    const TSet<FString> PrimaryRequests = MoveTemp(PendingPrimaryAssetIDs);
    PendingMeshRequests.Empty();
    PendingMeshIdentities.Empty();
    PendingPrimaryAssetIDs.Empty();
    TrackedServerPlayers.Empty();
    StreamingWorld = GetWorld();
    for (auto& Pair : Requests)
    {
        // AssetManager may share this primary load with other characters/consumers.
        // Drop our interest via epoch/identity guards; never cancel or unload it globally.
        if (!PrimaryRequests.Contains(Pair.Key) && Pair.Value.IsValid()) Pair.Value->CancelHandle();
    }
}

void UNemesisManager::Deinitialize()
{
    bDeinitializing = true;
    SynchronizeSimulation();
    InvalidateWorldRequests();
    // Remove ownership before Destroy can invoke external end-play callbacks.
    TMap<FString, ABaseHunter*> Bodies = MoveTemp(SpawnedPhysicalHunters);
    TArray<ABaseHunter*> Pool = MoveTemp(InactiveHuntersPool);
    SpawnedPhysicalHunters.Empty();
    InactiveHuntersPool.Empty();
    SpatialGridMatrix.Empty();
    for (auto& Pair : Bodies) if (IsValid(Pair.Value)) Pair.Value->Destroy();
    for (ABaseHunter* Body : Pool) if (IsValid(Body)) Body->Destroy();
    Super::Deinitialize();
}

void UNemesisManager::AggregateSubRegionDataToParents()
{
    FScopeLock Lock(&DataMutex);

    TMap<FString, float>  AggregatedHeat;
    TMap<FString, int32>  SubRegionCounts;
    TMap<FString, float>  TotalFortificationBonus;

    for (auto& RegionPair : MapRegions)
    {
        FRegionData& SubRegion = RegionPair.Value;
        const FString ParentID = SubRegion.ParentFortressRegionID;
        FRegionData* Parent = MapRegions.Find(ParentID);
        if (!Parent || Parent == &SubRegion || SubRegion.RegionType == ESubRegionType::MilitaryFortress ||
            Parent->RegionType != ESubRegionType::MilitaryFortress) continue;

        SubRegionCounts.FindOrAdd(ParentID)++;
        AggregatedHeat.FindOrAdd(ParentID) += SubRegion.LocalHeat;
        TotalFortificationBonus.FindOrAdd(ParentID) += SubRegion.LocalFortificationValue;

        TArray<FString> Names = { TEXT("Rice"), TEXT("Lumber"), TEXT("Fish"), TEXT("Iron") };
        for (const auto& Item : SubRegion.SharedStockpile)
            if (!UEconomySystem::GetPrimaryResourceField(SubRegion.Stockpile, Item.Key)) Names.Add(Item.Key);
        for (const FString& Name : Names)
        {
            FRegionData Working = SubRegion;
            float* Balance = UEconomySystem::GetPrimaryResourceField(Working.Stockpile, Name);
            double Amount = Working.SharedStockpile.FindRef(Name);
            if (Balance)
            {
                if (!UEconomySystem::ResolvePrimaryResourceBalance(Working, Name, Balance)) continue;
                Amount = *Balance;
            }
            if (Amount > 0.0) TransferManagerStock(SubRegion, *Parent, Name, Amount);
        }
    }

    for (auto& RegionPair : MapRegions)
    {
        FRegionData& FortressRegion = RegionPair.Value;

        if (FortressRegion.RegionType != ESubRegionType::MilitaryFortress)
            continue;

        FString FortressID = FortressRegion.RegionID;
        int32 ChildCount = SubRegionCounts.FindRef(FortressID);

        if (AggregatedHeat.Contains(FortressID))
        {
            float AveragedHeat = AggregatedHeat[FortressID] / (float)ChildCount;
            FortressRegion.DangerLevel = FMath::Clamp(AveragedHeat, 0.0f, 100.0f);
        }

        const float StoredPreviousBonus = FortressChildBonuses.FindRef(FortressID);
        const float GatheredBonus = TotalFortificationBonus.FindRef(FortressID);
        const float PreviousBonus = FMath::IsFinite(StoredPreviousBonus) ? StoredPreviousBonus : 0.0f;
        const float NewBonus = FMath::IsFinite(GatheredBonus) ? FMath::Max(0.0f, GatheredBonus) : 0.0f;
        const double OldMax = FMath::IsFinite(FortressRegion.MaxFortressHealth)
            ? static_cast<double>(FortressRegion.MaxFortressHealth) : 0.0;
        // Preserve the current intrinsic max, whether seeded, loaded, or legitimately upgraded.
        FortressRegion.MaxFortressHealth = static_cast<float>(FMath::Clamp(
            OldMax - PreviousBonus + NewBonus, 0.0, static_cast<double>(MAX_flt)));
        FortressRegion.FortressHealth = FMath::Clamp(
            FMath::IsFinite(FortressRegion.FortressHealth) ? FortressRegion.FortressHealth : 0.0f,
            0.0f, FortressRegion.MaxFortressHealth);
        FortressChildBonuses.Add(FortressID, NewBonus);
    }
}

void UNemesisManager::RecalculateProfileNetWealth(FString CharacterID)
{
    CharacterID = ResolveCharacterID(CharacterID);
    FScopeLock Lock(&DataMutex);

    if (!ActiveNemeses.Contains(CharacterID)) return;
    FHunterProfile& Profile = ActiveNemeses[CharacterID];
    // All callers, including daily ticks and legacy integrations, use the same bounds gate.
    // Invalid data leaves the previous derived metric intact rather than overflowing.
    if (!UEconomySystem::CanRecalculateProfileNetWealth(Profile, Profile.Inventory, MapRegions)) return;

    int32 AggregatedNetValue = 0;

    for (const auto& ItemPair : Profile.Inventory)
    {
        const FItemData& Item = ItemPair.Value;

        if (ItemPair.Key.Equals(TEXT("Copper_Mon")))
        {
            AggregatedNetValue += Item.Quantity;
        }
        else
        {
            AggregatedNetValue += static_cast<int32>(Item.Potency) * Item.Quantity;
        }
    }

    if (!Profile.ClanID.Equals(TEXT("Neutral")) && !Profile.ClanID.IsEmpty())
    {
        for (const auto& RegionPair : MapRegions)
        {
            if (RegionPair.Value.ControllingClanID.Equals(Profile.ClanID))
            {
                AggregatedNetValue += static_cast<int32>(RegionPair.Value.DistrictValue);
            }
        }
    }

    Profile.Wealth = AggregatedNetValue;
    RefreshPlayerAlias();
}

void UNemesisManager::UpdateReputation(float KarmaShift, float NotorietyGain)
{
    if (!FMath::IsFinite(KarmaShift) || !FMath::IsFinite(NotorietyGain)) return;
    const FString PlayerID = ResolveCharacterID(TEXT("PLAYER"));
    if (!ActiveNemeses.Contains(PlayerID)) return;

    FHunterProfile& Player = ActiveNemeses[PlayerID];
    Player.Karma = FMath::Clamp(Player.Karma + KarmaShift, -100.f, 100.f);

    float KarmaMultiplier = 1.0f + (FMath::Abs(Player.Karma) / 100.f);
    float FinalGain = NotorietyGain * KarmaMultiplier;

    Player.Notoriety = FMath::Clamp(Player.Notoriety + FinalGain, 0.f, 1000.f);
    RefreshPlayerAlias();
}

void UNemesisManager::SyncPlayerProfile(FHunterProfile P)
{
    if (!IsInGameThread() || P.UniqueID.IsEmpty()) return;
    SynchronizeSimulation();
    // Each account owns its stable key. PLAYER remains a compatibility view of
    // the first player, never an alias that every connected pawn may overwrite.
    const FHunterProfile* Legacy = ActiveNemeses.Find(TEXT("PLAYER"));
    const bool bOwnsLegacyAlias = !Legacy || Legacy->UniqueID == P.UniqueID;
    if (const FHunterProfile* Existing = ActiveNemeses.Find(P.UniqueID))
        if (!Existing->bIsAlive) P.bIsAlive = false;
    ActiveNemeses.Add(P.UniqueID, P);
    if (bOwnsLegacyAlias) ActiveNemeses.Add(TEXT("PLAYER"), P);
}

bool UNemesisManager::IsRedundantPlayerAlias(const FString& CharacterID) const
{
    if (CharacterID != TEXT("PLAYER")) return false;
    const FHunterProfile* Alias = ActiveNemeses.Find(CharacterID);
    if (!Alias || Alias->UniqueID.IsEmpty() || Alias->UniqueID == CharacterID) return false;
    const FHunterProfile* Canonical = ActiveNemeses.Find(Alias->UniqueID);
    return Canonical && Canonical->UniqueID == Alias->UniqueID;
}

FString UNemesisManager::ResolveCharacterID(const FString& CharacterID) const
{
    return IsRedundantPlayerAlias(CharacterID) ? ActiveNemeses.FindChecked(CharacterID).UniqueID : CharacterID;
}

void UNemesisManager::RefreshPlayerAlias()
{
    if (!IsRedundantPlayerAlias(TEXT("PLAYER"))) return;
    const FString ID = ActiveNemeses.FindChecked(TEXT("PLAYER")).UniqueID;
    const FHunterProfile Copy = ActiveNemeses.FindChecked(ID);
    ActiveNemeses.Add(TEXT("PLAYER"), Copy);
}

void UNemesisManager::SaveWorldState()
{
    if (bRejectedWorldLoad || bDeinitializing) return;
    if (!IsInGameThread()) return;
    if (URoninSaveGame* S = Cast<URoninSaveGame>(UGameplayStatics::CreateSaveGameObject(URoninSaveGame::StaticClass())))
    {
        URoninQuestManager* Quests = GetGameInstance() ? GetGameInstance()->GetSubsystem<URoninQuestManager>() : nullptr;
        if (!Quests || !Quests->ExportSaveData(S))
        {
            UE_LOG(LogTemp, Error, TEXT("WorldState save aborted: quest snapshot was not available or valid."));
            return;
        }
        SynchronizeSimulation();
        S->SavedNemeses = ActiveNemeses;
        // Realized bodies publish only movement and monotonic death, not stale social/economic DNA.
        for (const auto& Pair : SpawnedPhysicalHunters)
        {
            ABaseHunter* Body = Pair.Value;
            FHunterProfile* Saved = S->SavedNemeses.Find(Pair.Key);
            if (!IsValid(Body) || Body->GetWorld() != GetWorld() || !Saved ||
                Body->HunterDNA.UniqueID != Saved->UniqueID ||
                Body->HunterDNA.LegacyGeneration != Saved->LegacyGeneration) continue;
            FIntVector2D Cell;
            const FVector Location = Body->GetActorLocation();
            if (!TrySpatialCell(Location, SpatialGridCellSize, Cell)) return;
            Saved->WorldLocation = Location;
            Saved->AssignedSpatialGridCell = Cell;
            if (!Body->HunterDNA.bIsAlive) Saved->bIsAlive = false;
        }
        if (IsRedundantPlayerAlias(TEXT("PLAYER")))
        {
            const FString ID = ResolveCharacterID(TEXT("PLAYER"));
            const FHunterProfile Canonical = S->SavedNemeses.FindChecked(ID);
            S->SavedNemeses.Add(TEXT("PLAYER"), Canonical);
        }
        S->SavedRegions = MapRegions;
        S->SavedRumors = ActiveRumors;
        S->SavedYear = CurrentYear;
        S->SavedDay = CurrentDay;
        S->SavedDayCounter = DayCounter;
        S->SavedGeneration = Generation;
        S->SavedClanFriction = GlobalClanFriction;
        S->SavedFortressChildBonuses = FortressChildBonuses;
        S->SaveVersion = 2;
        if (!S->IsWorldSnapshotValid())
        {
            UE_LOG(LogTemp, Error, TEXT("WorldState save aborted: invalid live world snapshot; existing slot retained."));
            return;
        }
        if (!UGameplayStatics::SaveGameToSlot(S, TEXT("WorldState"), 0))
        {
            UE_LOG(LogTemp, Error, TEXT("WorldState save failed; previous persistence is not confirmed."));
        }
    }
}

void UNemesisManager::LoadWorldState()
{
    if (!IsInGameThread() || bDeinitializing) return;
    if (!UGameplayStatics::DoesSaveGameExist(TEXT("WorldState"), 0)) return;
    URoninSaveGame* L = Cast<URoninSaveGame>(UGameplayStatics::LoadGameFromSlot(TEXT("WorldState"), 0));
    if (!L || !L->IsWorldSnapshotValid())
    {
        bRejectedWorldLoad = true;
        UE_LOG(LogTemp, Error, TEXT("WorldState load rejected: unreadable, incompatible or invalid world snapshot. Slot writes blocked until successful load or explicit reset."));
        return;
    }
    {
        URoninQuestManager* Quests = GetGameInstance() ? GetGameInstance()->GetSubsystem<URoninQuestManager>() : nullptr;
        if (!Quests || !Quests->ImportSaveData(L))
        {
            bRejectedWorldLoad = true;
            UE_LOG(LogTemp, Error, TEXT("WorldState load rejected: incompatible or invalid quest snapshot; live records retained and slot writes blocked."));
            return;
        }
        InvalidateWorldRequests();
        SynchronizeSimulation(); // Epoch changed: drain and discard, never merge into the replacement.
        bRejectedWorldLoad = false;
        bLoadedWorldState = true;
        TMap<FString, ABaseHunter*> Bodies = MoveTemp(SpawnedPhysicalHunters);
        TArray<ABaseHunter*> Pool = MoveTemp(InactiveHuntersPool);
        SpawnedPhysicalHunters.Empty();
        InactiveHuntersPool.Empty();
        for (auto& Pair : Bodies) if (IsValid(Pair.Value)) Pair.Value->Destroy();
        for (ABaseHunter* Hunter : Pool) if (IsValid(Hunter)) Hunter->Destroy();
        SpatialGridMatrix.Empty();
        ActiveNemeses = L->SavedNemeses;
        MapRegions = L->SavedRegions;
        ActiveRumors = L->SavedRumors;
        CurrentYear = L->SavedYear;
        CurrentDay = FMath::Clamp(L->SavedDay, 1, 365);
        DayCounter = FMath::IsFinite(L->SavedDayCounter) ? FMath::Max(0.0f, L->SavedDayCounter) : 0.0f;
        Generation = FMath::Max(1, L->SavedGeneration);
        if (L->SaveVersion >= 1) GlobalClanFriction = L->SavedClanFriction;
        FortressChildBonuses.Empty();
        if (L->SaveVersion >= 2) FortressChildBonuses = L->SavedFortressChildBonuses;
    }
}

void UNemesisManager::ModifyFactionStanding(FString ClanA, FString ClanB, float DeltaStanding)
{
    if (!FMath::IsFinite(DeltaStanding)) return;
    FString Key = ClanA + TEXT("_") + ClanB;
    float   CurrentStanding = GlobalClanFriction.Contains(Key) ? GlobalClanFriction[Key] : 0.0f;
    GlobalClanFriction.Add(Key, FMath::Clamp(CurrentStanding + DeltaStanding, -100.0f, 100.0f));

    FString InverseKey = ClanB + TEXT("_") + ClanA;
    GlobalClanFriction.Add(InverseKey, FMath::Clamp(CurrentStanding + DeltaStanding, -100.0f, 100.0f));
}

float UNemesisManager::GetFactionStanding(FString ClanA, FString ClanB) const
{
    if (ClanA.Equals(ClanB)) return 100.0f;
    FString Key = ClanA + TEXT("_") + ClanB;
    return GlobalClanFriction.Contains(Key) ? GlobalClanFriction[Key] : 0.0f;
}

void UNemesisManager::GenerateRumorsForCrime(FString CrimeLocation, float KarmaImpact)
{
    UpdateReputation(KarmaImpact, FMath::Abs(KarmaImpact) * 0.5f);

    FVector RumorLocation = FVector::ZeroVector;
    if (MapRegions.Contains(CrimeLocation) && MapRegions[CrimeLocation].Structures.Num() > 0)
    {
        RumorLocation = MapRegions[CrimeLocation].Structures[0].WorldLocation;
    }
    else if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(this, 0))
    {
        RumorLocation = PlayerChar->GetActorLocation();
    }

    if (UDeepSeekSubsystem* AI = GetGameInstance()->GetSubsystem<UDeepSeekSubsystem>())
    {
        FString Prompt = FString::Printf(
            TEXT("The Ronin committed a crime at %s. Impact: %f. Write a Fact, a Folklore Tall-Tale, and dangerous Misinformation."),
            *CrimeLocation, KarmaImpact);

        FOnDeepSeekResponse ResponseDelegate;
        const TWeakObjectPtr<UNemesisManager> WeakThis(this);
        const TWeakObjectPtr<UWorld> RequestWorld(GetWorld());
        const uint64 RequestEpoch = WorldEpoch;
        ResponseDelegate.BindLambda([WeakThis, RequestWorld, RequestEpoch, RumorLocation](FString Response)
            {
                UNemesisManager* Manager = WeakThis.Get();
                if (!Manager || Manager->bDeinitializing || Manager->WorldEpoch != RequestEpoch ||
                    !RequestWorld.IsValid() || Manager->GetWorld() != RequestWorld.Get()) return;
                FRumorData NewRumor;
                NewRumor.Type = ERumorType::Crime;
                NewRumor.LastKnownLocation = RumorLocation;
                NewRumor.RawRumorText = Response;
                NewRumor.Fact = Response;
                Manager->ActiveRumors.Add(NewRumor);
            });
        AI->SendPromptToDeepSeek(Prompt, ResponseDelegate);
    }
}

void UNemesisManager::GenerateHeroicRumor(FString Location, FString ActionDescription, float FameIncrease)
{
    if (!FMath::IsFinite(FameIncrease)) return;
    UpdateReputation(FameIncrease, FameIncrease);

    FVector RumorLocation = FVector::ZeroVector;
    if (MapRegions.Contains(Location) && MapRegions[Location].Structures.Num() > 0)
    {
        RumorLocation = MapRegions[Location].Structures[0].WorldLocation;
    }
    else if (ACharacter* PlayerChar = UGameplayStatics::GetPlayerCharacter(this, 0))
    {
        RumorLocation = PlayerChar->GetActorLocation();
    }

    if (UDeepSeekSubsystem* AI = GetGameInstance()->GetSubsystem<UDeepSeekSubsystem>())
    {
        FString Prompt = FString::Printf(
            TEXT("The Ronin performed a heroic deed: %s at %s. Write a brief Fact, a mythic Folklore legend, and jealous Misinformation from rivals."),
            *ActionDescription, *Location);

        FOnDeepSeekResponse ResponseDelegate;
        const TWeakObjectPtr<UNemesisManager> WeakThis(this);
        const TWeakObjectPtr<UWorld> RequestWorld(GetWorld());
        const uint64 RequestEpoch = WorldEpoch;
        const int32 RequestGeneration = Generation;
        const FHunterProfile* OriginalPlayer = ActiveNemeses.Find(TEXT("PLAYER"));
        const FString SubjectID = OriginalPlayer ? OriginalPlayer->UniqueID : FString();
        const int32 SubjectGeneration = OriginalPlayer ? OriginalPlayer->LegacyGeneration : 0;
        ResponseDelegate.BindLambda([WeakThis, RequestWorld, RequestEpoch, RequestGeneration,
            SubjectID, SubjectGeneration, FameIncrease, RumorLocation](FString Response)
            {
                UNemesisManager* Manager = WeakThis.Get();
                if (!Manager || Manager->bDeinitializing || Manager->WorldEpoch != RequestEpoch ||
                    !RequestWorld.IsValid() || Manager->GetWorld() != RequestWorld.Get()) return;
                FRumorData NewRumor;
                NewRumor.Type = ERumorType::HeroicDeed;
                NewRumor.SubjectID = SubjectID;
                NewRumor.SpreadLevel = FameIncrease;
                NewRumor.LastKnownLocation = RumorLocation;
                NewRumor.RawRumorText = Response;
                NewRumor.Fact = Response;
                Manager->ActiveRumors.Add(NewRumor);

                if (SubjectID.IsEmpty()) return;
                FHunterProfile* Original = nullptr;
                if (Manager->Generation == RequestGeneration)
                {
                    FHunterProfile* Player = Manager->ActiveNemeses.Find(TEXT("PLAYER"));
                    if (Player && Player->UniqueID == SubjectID && Player->LegacyGeneration == SubjectGeneration)
                        Original = Manager->ActiveNemeses.Find(Manager->ResolveCharacterID(TEXT("PLAYER")));
                }
                // A retained historical entity can receive its own fame, never a future PLAYER alias.
                if (!Original && SubjectID != TEXT("PLAYER"))
                    Original = Manager->ActiveNemeses.Find(SubjectID);
                if (Original && Original->UniqueID == SubjectID && Original->LegacyGeneration == SubjectGeneration &&
                    FMath::IsFinite(Original->InheritedFame) && FMath::IsFinite(FameIncrease))
                {
                    const double Fame = static_cast<double>(Original->InheritedFame) + FameIncrease;
                    if (Fame >= -static_cast<double>(MAX_flt) && Fame <= MAX_flt)
                        Original->InheritedFame = static_cast<float>(Fame);
                    Manager->RefreshPlayerAlias();
                }
            });
        AI->SendPromptToDeepSeek(Prompt, ResponseDelegate);
    }
}

FRumorData UNemesisManager::GetMostRelevantRumor(FVector CurrentLocation)
{
    FRumorData HottestRumor;
    float MaxSpread = -1.0f;
    for (const FRumorData& Rumor : ActiveRumors)
    {
        if (Rumor.SpreadLevel > MaxSpread)
        {
            MaxSpread = Rumor.SpreadLevel;
            HottestRumor = Rumor;
        }
    }
    return HottestRumor;
}

void UNemesisManager::ProcessSurvivalMetrics(ARoninCharacter* Player, float DeltaTime)
{
    if (!FMath::IsFinite(DeltaTime) || DeltaTime < 0.f || !FMath::IsFinite(HungerLevel)) return;
    const double NextHunger = static_cast<double>(HungerLevel) + static_cast<double>(DeltaTime) * 0.01;
    if (NextHunger <= MAX_flt) HungerLevel = static_cast<float>(NextHunger);
}

void UNemesisManager::PerformRest(ARoninCharacter* Player, float NutritionValue)
{
    if (!FMath::IsFinite(NutritionValue) || NutritionValue < 0.f || !FMath::IsFinite(HungerLevel)) return;
    HungerLevel = FMath::Max(0.f, HungerLevel - NutritionValue);
}

void UNemesisManager::RecordPlayerDeath(FString KillerID)
{
    if (Generation < MAX_int32) ++Generation;
    SaveWorldState();
}

FString UNemesisManager::GetRegionIDFromCoordinates(FVector Location) const
{
    FString ClosestRegion = TEXT("");
    float   MinDistance = MAX_FLT;

    for (const auto& Pair : MapRegions)
    {
        FVector TargetCenter = FVector::ZeroVector;
        if (Pair.Value.Structures.Num() > 0)
        {
            TargetCenter = Pair.Value.Structures[0].WorldLocation;
        }

        float CurrentDist = FVector::DistSquared(Location, TargetCenter);
        if (CurrentDist < MinDistance)
        {
            MinDistance = CurrentDist;
            ClosestRegion = Pair.Key;
        }
    }
    return ClosestRegion;
}

void UNemesisManager::ProcessGenerationalBirthAndHousingPass()
{
    FScopeLock Lock(&DataMutex);

    TMap<FString, int32> RegionPopulationCounters;
    TMap<FString, int32> RegionCivilianCounters;

    for (const auto& Pair : ActiveNemeses)
    {
        const FHunterProfile& Citizen = Pair.Value;
        if (!Citizen.bIsAlive || IsRedundantPlayerAlias(Pair.Key)) continue;

        FString LocalRegionID = GetRegionIDFromCoordinates(Citizen.WorldLocation);
        if (LocalRegionID.IsEmpty()) continue;

        RegionPopulationCounters.FindOrAdd(LocalRegionID)++;

        bool bIsCivilian = (
            Citizen.HunterClass == EHunterClass::Child ||
            Citizen.HunterClass == EHunterClass::Farmer ||
            Citizen.HunterClass == EHunterClass::Citizen ||
            Citizen.HunterClass == EHunterClass::Merchant ||
            Citizen.HunterClass == EHunterClass::Chef ||
            Citizen.HunterClass == EHunterClass::Blacksmith ||
            Citizen.HunterClass == EHunterClass::Doctor ||
            Citizen.HunterClass == EHunterClass::Fisherman
            );

        if (bIsCivilian) RegionCivilianCounters.FindOrAdd(LocalRegionID)++;
    }

    TArray<FHunterProfile> NewBornProfiles;

    for (auto& ParentPair : ActiveNemeses)
    {
        FHunterProfile& Mother = ParentPair.Value;
        if (IsRedundantPlayerAlias(ParentPair.Key) || !Mother.bIsAlive ||
            Mother.Gender != ERoninGender::Female || Mother.Age < 18 || Mother.Age > 45) continue;

        FString MotherRegionID = GetRegionIDFromCoordinates(Mother.WorldLocation);
        if (!MapRegions.Contains(MotherRegionID)) continue;

        int32 TotalPop = RegionPopulationCounters.FindOrAdd(MotherRegionID);
        int32 CivilianPop = RegionCivilianCounters.FindOrAdd(MotherRegionID);
        float CurrentCivilianRatio = (TotalPop > 0) ? (static_cast<float>(CivilianPop) / static_cast<float>(TotalPop)) : 0.0f;

        ESubRegionType CurrentZoneType = MapRegions[MotherRegionID].RegionType;

        if (CurrentZoneType == ESubRegionType::MilitaryFortress && CurrentCivilianRatio >= 0.35f) continue;
        if (CurrentZoneType != ESubRegionType::MilitaryFortress && CurrentCivilianRatio >= 0.70f) continue;

        if (FMath::FRand() <= 0.035f)
        {
            FHunterProfile ChildProfile;
            ChildProfile.UniqueID = FGuid::NewGuid().ToString();
            ChildProfile.HunterName = FString::Printf(TEXT("Heir %s"), *Mother.HunterName);
            ChildProfile.Age = 0;
            ChildProfile.HunterClass = EHunterClass::Child;
            ChildProfile.bIsAlive = true;
            ChildProfile.ClanID = Mother.ClanID;
            ChildProfile.MotherID = ParentPair.Key;
            ChildProfile.RelationshipWeb.FindOrAdd(ChildProfile.MotherID).TieType = EFamilyTie::Parent;
            for (const auto& Relation : Mother.RelationshipWeb)
            {
                if (Relation.Value.TieType != EFamilyTie::Spouse) continue;
                const FString FatherID = ResolveCharacterID(Relation.Key);
                const FHunterProfile* Father = ActiveNemeses.Find(FatherID);
                if (!Father || FatherID == ParentPair.Key || !Father->bIsAlive ||
                    Father->Age < 18 || Father->Gender != ERoninGender::Male) continue;
                const FRelationshipData* Reciprocal = Father->RelationshipWeb.Find(ParentPair.Key);
                if (!Reciprocal || Reciprocal->TieType != EFamilyTie::Spouse) continue;
                ChildProfile.FatherID = FatherID;
                ChildProfile.RelationshipWeb.FindOrAdd(FatherID).TieType = EFamilyTie::Parent;
                break;
            }
            ChildProfile.WorldLocation = Mother.WorldLocation + FVector(
                FMath::RandRange(-200.0f, 200.0f),
                FMath::RandRange(-200.0f, 200.0f), 0.0f);
            ChildProfile.AssignedSpatialGridCell = Mother.AssignedSpatialGridCell;
            ChildProfile.ElementalAffinities.Add(EElementalType::None);
            ChildProfile.SkeletalMeshPath = GetClassAppropriateMesh(EHunterClass::Child);
            ChildProfile.AsyncSkeletalMeshHandle = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(ChildProfile.SkeletalMeshPath));

            NewBornProfiles.Add(ChildProfile);
            RegionPopulationCounters[MotherRegionID]++;
            RegionCivilianCounters[MotherRegionID]++;
        }
    }

    for (const FHunterProfile& Baby : NewBornProfiles)
    {
        ActiveNemeses.Add(Baby.UniqueID, Baby);
        if (FHunterProfile* Mother = ActiveNemeses.Find(Baby.MotherID))
            Mother->RelationshipWeb.FindOrAdd(Baby.UniqueID).TieType = EFamilyTie::Child;
        if (FHunterProfile* Father = ActiveNemeses.Find(Baby.FatherID))
            Father->RelationshipWeb.FindOrAdd(Baby.UniqueID).TieType = EFamilyTie::Child;
        UE_LOG(LogTemp, Log, TEXT("LINEAGE ENGINE: A child (%s) born via regional percentage conditions."), *Baby.UniqueID);
    }
}

void UNemesisManager::RunBackgroundWarAndDiplomacy(float DaysPassed)
{
    // VENDETTA MAP INTERPOLATION LOOP (BLOOD FEUDS) � non-economic systems remain here
    for (auto& HunterPair : ActiveNemeses)
    {
        FHunterProfile& Fighter = HunterPair.Value;
        if (!Fighter.bIsAlive || IsRedundantPlayerAlias(HunterPair.Key)) continue;

        for (auto& KillVendetta : Fighter.VendettaList)
        {
            FString TargetID = KillVendetta.Key;
            float   WeightValue = KillVendetta.Value;

            if (WeightValue >= 80.0f && ActiveNemeses.Contains(TargetID))
            {
                FString EnemyClan = ActiveNemeses[TargetID].ClanID;
                if (!Fighter.ClanID.Equals(EnemyClan))
                {
                    GlobalClanFriction.FindOrAdd(Fighter.ClanID + TEXT("_") + EnemyClan) = -100.0f;

                    UE_LOG(LogTemp, Warning,
                        TEXT("VENDETTA: Blood Feud between %s and %s triggered a global war pass!"),
                        *Fighter.HunterName, *ActiveNemeses[TargetID].HunterName);
                }
            }
        }
    }

    ProcessGenerationalBirthAndHousingPass();
}

void UNemesisManager::RegisterHunter(const FHunterProfile& D)
{
    if (!IsInGameThread() || bDeinitializing || D.UniqueID.IsEmpty()) return;
    // Own the input before draining: callers may pass a reference into the registry.
    FHunterProfile Profile = D;
    SynchronizeSimulation();
    const FString ID = ResolveCharacterID(Profile.UniqueID);
    Profile.UniqueID = ID;
    if (const FHunterProfile* Existing = ActiveNemeses.Find(ID))
        if (!Existing->bIsAlive) Profile.bIsAlive = false;
    const TSharedPtr<FStreamableHandle> Request = PendingMeshRequests.FindRef(ID);
    const bool bPrimary = PendingPrimaryAssetIDs.Contains(ID);
    PendingMeshIdentities.Remove(ID);
    PendingMeshRequests.Remove(ID);
    PendingPrimaryAssetIDs.Remove(ID);
    if (!bPrimary && Request.IsValid()) Request->CancelHandle();
    ActiveNemeses.Add(ID, Profile);
    RefreshPlayerAlias();
}

void UNemesisManager::AdvanceWorldClock(float DeltaSeconds, float TimeMultiplier)
{
    if (!IsInGameThread() || bDeinitializing || bRejectedWorldLoad ||
        !GetWorld() || GetWorld()->GetNetMode() == NM_Client ||
        CurrentDay < 1 || CurrentDay > 365 || !FMath::IsFinite(DayCounter) || DayCounter < 0.f ||
        !FMath::IsFinite(DeltaSeconds) || !FMath::IsFinite(TimeMultiplier) ||
        DeltaSeconds < 0.0f || TimeMultiplier < 0.0f) return;
    const double AddedTime = static_cast<double>(DeltaSeconds) * TimeMultiplier;
    if (!FMath::IsFinite(AddedTime) || AddedTime > MAX_flt - DayCounter) return;
    DayCounter += static_cast<float>(AddedTime);
    // Bound work per call without discarding backlog or changing daily probabilities.
    int32 DaysProcessed = 0;
    while (DayCounter >= 24.0f && DaysProcessed < 8)
    {
        // Stop at the representable calendar boundary without consuming backlog.
        if ((CurrentDay == 365 && CurrentYear == MAX_int32) || DayCounter - 24.0f == DayCounter) break;
        SynchronizeSimulation();
        const uint64 TickEpoch = WorldEpoch;
        const TWeakObjectPtr<UWorld> TickWorld(GetWorld());
        DayCounter -= 24.0f;
        ++CurrentDay;
        if (CurrentDay > 365)
        {
            CurrentDay -= 365;
            ++CurrentYear;
            for (auto& Pair : ActiveNemeses)
                if (!IsRedundantPlayerAlias(Pair.Key) && Pair.Value.Age < MAX_int32) ++Pair.Value.Age;
            RefreshPlayerAlias();
            OnYearPassed.Broadcast(CurrentYear);
            if (bDeinitializing || WorldEpoch != TickEpoch || !TickWorld.IsValid() || GetWorld() != TickWorld.Get()) return;
        }
        TickWorldSystems(1.0f);
        ++DaysProcessed;
    }
    if (DaysProcessed > 0) SaveWorldState();
}

void UNemesisManager::TickWorldSystems(float DaysPassed)
{
    SynchronizeSimulation();
    AggregateSubRegionDataToParents();

    for (int32 i = ActiveRumors.Num() - 1; i >= 0; --i)
    {
        ActiveRumors[i].SpreadLevel -= DaysPassed * 5.0f;
        if (ActiveRumors[i].SpreadLevel <= 0) ActiveRumors.RemoveAt(i);
    }

    RunBackgroundWarAndDiplomacy(DaysPassed);

    if (UDynastySubsystem* DynastySys = GetGameInstance()->GetSubsystem<UDynastySubsystem>())
        DynastySys->RunBackgroundLifeSimulation(DaysPassed);

    if (UEconomySystem* EconSys = GetGameInstance()->GetSubsystem<UEconomySystem>())
        EconSys->ProcessSettlementProductionTick(DaysPassed);

    // Diplomatic trade routes
    for (auto& FortPair : MapRegions)
    {
        FRegionData& Fortress = FortPair.Value;
        if (Fortress.RegionType != ESubRegionType::MilitaryFortress) continue;

        for (auto& TradeRoute : Fortress.ActiveTradeRoutes)
        {
            FString TargetFortressID = TradeRoute.Key;
            int32   Volume = TradeRoute.Value;

            if (MapRegions.Contains(TargetFortressID))
            {
                FRegionData& Target = MapRegions[TargetFortressID];

                FRegionData Working = Fortress;
                float* Rice = nullptr;
                if (Volume > 0 && Target.RegionType == ESubRegionType::MilitaryFortress &&
                    UEconomySystem::ResolvePrimaryResourceBalance(Working, TEXT("Rice"), Rice) &&
                    *Rice > 100.0f && TransferManagerStock(Fortress, Target, TEXT("Rice"), Volume))
                {
                    UE_LOG(LogTemp, Log, TEXT("DIPLOMACY: %s sent %d Rice to %s"),
                        *Fortress.RegionID, Volume, *TargetFortressID);
                }
            }
        }
    }

    // Bandit camp raids
    for (auto& BanditPair : MapRegions)
    {
        FRegionData& BanditCamp = BanditPair.Value;
        if (BanditCamp.RegionType != ESubRegionType::HiddenCamp) continue;

        FVector BanditPos = BanditCamp.Structures.Num() > 0
            ? BanditCamp.Structures[0].WorldLocation : FVector::ZeroVector;

        for (auto& TargetPair : MapRegions)
        {
            FRegionData& TargetVillage = TargetPair.Value;
            if (TargetVillage.RegionType != ESubRegionType::FarmingVillage) continue;

            FVector VillagePos = TargetVillage.Structures.Num() > 0
                ? TargetVillage.Structures[0].WorldLocation : FVector::ZeroVector;

            if (FVector::Dist(BanditPos, VillagePos) < 5000.0f)
            {
                FString Specialty = (TargetVillage.ResourceSpecialty == EVillageSpecialty::Fish)
                    ? TEXT("Fish") : TEXT("Rice");

                FRegionData Working = TargetVillage;
                float* Available = nullptr;
                if (!UEconomySystem::ResolvePrimaryResourceBalance(Working, Specialty, Available)) continue;
                const double Stolen = FMath::Min(static_cast<double>(*Available), 5.0);
                if (TransferManagerStock(TargetVillage, BanditCamp, Specialty, Stolen))
                {
                    UE_LOG(LogTemp, Warning, TEXT("RAID: Bandit camp %s stole %f %s from %s!"),
                        *BanditCamp.RegionID, Stolen, *Specialty, *TargetVillage.RegionID);
                }
                break;
            }
        }
    }

    for (auto& NemesisPair : ActiveNemeses)
    {
        if (NemesisPair.Value.bIsAlive && !IsRedundantPlayerAlias(NemesisPair.Key))
            RecalculateProfileNetWealth(NemesisPair.Key);
    }
    RefreshPlayerAlias();

    TArray<FNPCSimPacket> Packets;
    Packets.Reserve(ActiveNemeses.Num());
    for (const auto& Pair : ActiveNemeses)
    {
        if (Pair.Key == TEXT("PLAYER") || Pair.Value.bIsRealPlayer || !Pair.Value.bIsAlive ||
            SpawnedPhysicalHunters.Contains(Pair.Key)) continue;
        const uint64 CalendarRevision = (static_cast<uint64>(static_cast<uint32>(CurrentYear)) << 32) |
            static_cast<uint32>(CurrentDay);
        FNPCSimPacket Packet = FNPCSimPacket::FromProfile(Pair.Value, WorldEpoch, CalendarRevision, false);
        Packet.CharacterID = Pair.Key;
        Packets.Add(MoveTemp(Packet));
    }
    SimulationWorld = GetWorld();
    ActiveSimulationThreadTask = new FAsyncTask<FAsyncFeudalSimulationWorker>(
        Packets, DaysPassed, SpatialGridCellSize);
    ActiveSimulationThreadTask->StartBackgroundTask();

    UE_LOG(LogTemp, Warning, TEXT("=== ASYNC SIMULATION HEARTBEAT TICK ==="));
    UE_LOG(LogTemp, Warning, TEXT("Simulated Time Step: +%f Days (Current Year: %d)"), DaysPassed, CurrentYear);
    UE_LOG(LogTemp, Warning, TEXT("Total Registry Capacity: %d Profiles"), ActiveNemeses.Num());
    UE_LOG(LogTemp, Warning, TEXT("======================================="));
}

void UNemesisManager::SeedWorldPopulation(int32 StartingPopulation)
{
    if (!IsInGameThread() || bDeinitializing || bRejectedWorldLoad || ActiveNemeses.Num() > 0) return;

    int32 TargetTotal = (StartingPopulation > 0) ? StartingPopulation : 500;
    TArray<URoninJobArchetype*> PredefinedArchetypePool;

    for (const auto& JobPair : MasterJobRegistry)
    {
        URoninJobArchetype* Archetype = JobPair.Value;
        if (Archetype)
        {
            const int32 Count = FMath::Clamp(Archetype->TargetPopulationQuota, 0, TargetTotal - PredefinedArchetypePool.Num());
            for (int32 i = 0; i < Count; ++i) PredefinedArchetypePool.Add(Archetype);
        }
    }
 

    TArray<FString> FirstNames = { TEXT("Jin"),TEXT("Mugen"),TEXT("Kenshin"),TEXT("Sanjiro"),TEXT("Kojiro"),TEXT("Hanzo"),TEXT("Ryuzo") };
    TArray<FString> LastNames = { TEXT("Sakai"),TEXT("Himura"),TEXT("Miyamoto"),TEXT("Hattori"),TEXT("Shimazu"),TEXT("Oda") };
    TArray<FString> Clans = { TEXT("Samurai_Shogunate"),TEXT("BloodMoon_Player"),TEXT("Shadow_Syndicate"),TEXT("Neutral") };

    TArray<FString> RegionKeys;
    MapRegions.GetKeys(RegionKeys);

    UE_LOG(LogTemp, Warning, TEXT("FEUDAL ENGINE: Seeding %d characters..."), TargetTotal);

    for (int32 i = 0; i < TargetTotal; ++i)
    {
        FHunterProfile NewHunter;
        NewHunter.UniqueID = FGuid::NewGuid().ToString();

        FString RandomFirst = FirstNames[FMath::RandRange(0, FirstNames.Num() - 1)];
        FString RandomLast = LastNames[FMath::RandRange(0, LastNames.Num() - 1)];
        NewHunter.HunterName = RandomFirst + TEXT(" ") + RandomLast;
        NewHunter.ClanID = Clans[FMath::RandRange(0, Clans.Num() - 1)];
        NewHunter.bIsAlive = true;
        NewHunter.LegacyGeneration = 1;

        ESubRegionType TargetRegionType = ESubRegionType::FarmingVillage;

        if (RegionKeys.Num() > 0 && i < (TargetTotal * 0.4f))
        {
            FString AssignedRegionKey = RegionKeys[FMath::RandRange(0, RegionKeys.Num() - 1)];
            NewHunter.WorldLocation = GenerateRandomMapCoordinates();
            TargetRegionType = MapRegions[AssignedRegionKey].RegionType;
        }
        else
        {
            float BoundX = FMath::RandRange(-60000.0f, 60000.0f);
            float BoundY = FMath::RandRange(-60000.0f, 60000.0f);
            NewHunter.WorldLocation = FVector(BoundX, BoundY, 5000.0f);
            TargetRegionType = (FMath::FRand() <= 0.20f) ? ESubRegionType::MilitaryFortress : ESubRegionType::FarmingVillage;
        }

        float PopulationRoll = FMath::FRand();
        bool  bSpawnAsCivilian = (TargetRegionType == ESubRegionType::MilitaryFortress)
            ? (PopulationRoll <= 0.35f) : (PopulationRoll <= 0.70f);

        if (bSpawnAsCivilian)
        {
            TArray<EHunterClass> CivilianClasses = {
                EHunterClass::Citizen, EHunterClass::Farmer, EHunterClass::Child,
                EHunterClass::Merchant, EHunterClass::Chef, EHunterClass::Blacksmith
            };
            NewHunter.HunterClass = CivilianClasses[FMath::RandRange(0, CivilianClasses.Num() - 1)];
            NewHunter.Age = (NewHunter.HunterClass == EHunterClass::Child)
                ? FMath::RandRange(4, 14) : FMath::RandRange(18, 65);
        }
        else
        {
            TArray<EHunterClass> CombatClasses = {
                EHunterClass::Soldier, EHunterClass::Bandit,
                EHunterClass::Hunter,  EHunterClass::BanditChief
            };
            NewHunter.HunterClass = CombatClasses[FMath::RandRange(0, CombatClasses.Num() - 1)];
            NewHunter.Age = FMath::RandRange(18, 48);
        }

        if (PredefinedArchetypePool.IsValidIndex(i) &&
            PredefinedArchetypePool[i]->HunterClass == NewHunter.HunterClass)
        {
            URoninJobArchetype* SelectedJob = PredefinedArchetypePool[i];
            NewHunter.JobClassData = SelectedJob;
            NewHunter.PowerLevel = SelectedJob->BasePowerLevel;
            NewHunter.Blood = SelectedJob->BaseBlood;
            NewHunter.Stamina = SelectedJob->BaseStamina;
            NewHunter.SkeletalMeshPath = SelectedJob->DefaultSkeletalMeshPath;
        }
        else
        {
            NewHunter.PowerLevel = (NewHunter.HunterClass == EHunterClass::Child) ? 0 : 1;
            NewHunter.Blood = 100.0f;
            NewHunter.Stamina = 100.0f;
            NewHunter.SkeletalMeshPath = GetClassAppropriateMesh(NewHunter.HunterClass);
        }

        NewHunter.AsyncSkeletalMeshHandle = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(NewHunter.SkeletalMeshPath));

        if (NewHunter.HunterClass == EHunterClass::Soldier ||
            NewHunter.HunterClass == EHunterClass::BanditChief)
        {
            TArray<EElementalType> EliteElements = { EElementalType::Fire, EElementalType::Lightning };
            NewHunter.ElementalAffinities.Add(EliteElements[FMath::RandRange(0, EliteElements.Num() - 1)]);
        }
        else
        {
            NewHunter.ElementalAffinities.Add(EElementalType::None);
        }

        if (!TrySpatialCell(NewHunter.WorldLocation, SpatialGridCellSize, NewHunter.AssignedSpatialGridCell)) continue;

        NewHunter.FatherID = TEXT("None");
        NewHunter.MotherID = TEXT("None");

        // High-rarity feudal item distributions
        if (NewHunter.HunterClass == EHunterClass::Soldier)
        {
            FItemData GuardKatana;
            GuardKatana.ItemName = TEXT("Shogunate Steel Katana");
            GuardKatana.Type = EItemType::Weapon;
            GuardKatana.Potency = 45.0f;
            GuardKatana.Quantity = 1;
            GuardKatana.Rarity = ERoninRarity::Rare;
            NewHunter.Inventory.Add(GuardKatana.ItemName, GuardKatana);
        }
        else if (NewHunter.HunterClass == EHunterClass::BanditChief)
        {
            FItemData WarlordBlade;
            WarlordBlade.ItemName = TEXT("Muramasa Blood-Cursed Edge");
            WarlordBlade.Type = EItemType::Weapon;
            WarlordBlade.Potency = 85.0f;
            WarlordBlade.Quantity = 1;
            WarlordBlade.Rarity = ERoninRarity::Legendary;
            NewHunter.Inventory.Add(WarlordBlade.ItemName, WarlordBlade);
        }

        TArray<EMoralAlignment> Alignments = {
            EMoralAlignment::LawfulGood, EMoralAlignment::ChaoticGood,
            EMoralAlignment::TrueNeutral, EMoralAlignment::LawfulEvil, EMoralAlignment::ChaoticEvil
        };

        if (NewHunter.HunterClass == EHunterClass::Bandit || NewHunter.HunterClass == EHunterClass::BanditChief)
        {
            NewHunter.Alignment = (FMath::FRand() > 0.5f) ? EMoralAlignment::ChaoticEvil : EMoralAlignment::LawfulEvil;
        }
        else if (NewHunter.HunterClass == EHunterClass::Farmer || NewHunter.HunterClass == EHunterClass::Citizen)
        {
            NewHunter.Alignment = EMoralAlignment::TrueNeutral;
        }
        else
        {
            NewHunter.Alignment = Alignments[FMath::RandRange(0, Alignments.Num() - 1)];
        }

        if (NewHunter.Alignment == EMoralAlignment::ChaoticEvil || NewHunter.Alignment == EMoralAlignment::LawfulEvil)
        {
            NewHunter.Karma = FMath::RandRange(-100.0f, -20.0f);
        }
        else if (NewHunter.Alignment == EMoralAlignment::LawfulGood || NewHunter.Alignment == EMoralAlignment::ChaoticGood)
        {
            NewHunter.Karma = FMath::RandRange(20.0f, 100.0f);
        }
        else
        {
            NewHunter.Karma = FMath::RandRange(-15.0f, 15.0f);
        }
        RegisterHunter(NewHunter);
    }

    SaveWorldState();
}

void UNemesisManager::PopulateJobRegistry(UDataTable* JobDataTable)
{
    if (!JobDataTable) return;

    TArray<FJobRegistryRow*> Rows;
    JobDataTable->GetAllRows<FJobRegistryRow>(TEXT("PopulateJobRegistry"), Rows);

    for (FJobRegistryRow* Row : Rows)
    {
        if (Row && Row->JobArchetype)
        {
            MasterJobRegistry.Add(Row->JobArchetype->HunterClass, Row->JobArchetype);
        }
    }
}

void UNemesisManager::ResetPopulationGenerator(int32 NewTargetPopulation)
{
    SynchronizeSimulation();
    InvalidateWorldRequests();
    FScopeLock Lock(&DataMutex);

    UE_LOG(LogTemp, Warning, TEXT("MASS PURGE: Initializing total population database reset..."));

    TMap<FString, ABaseHunter*> Bodies = MoveTemp(SpawnedPhysicalHunters);
    TArray<ABaseHunter*> Pool = MoveTemp(InactiveHuntersPool);
    SpawnedPhysicalHunters.Empty();
    InactiveHuntersPool.Empty();
    for (auto& Pair : Bodies) if (IsValid(Pair.Value)) Pair.Value->Destroy();
    for (ABaseHunter* PooledHunter : Pool) if (IsValid(PooledHunter)) PooledHunter->Destroy();

    ActiveNemeses.Empty();
    ActiveRumors.Empty();
    SpatialGridMatrix.Empty();

    Generation = 1;
    CurrentYear = 1570;
    CurrentDay = 1;
    DayCounter = 0.0f;

    if (UGameplayStatics::DoesSaveGameExist(TEXT("WorldState"), 0))
    {
        UGameplayStatics::DeleteGameInSlot(TEXT("WorldState"), 0);
        UE_LOG(LogTemp, Warning, TEXT("MASS PURGE: Save slot 'WorldState' erased from disk."));
    }

    bRejectedWorldLoad = false; // Explicit reset is intentional permission to replace the slot.
    bLoadedWorldState = false;
    SeedWorldPopulation(NewTargetPopulation);

    UE_LOG(LogTemp, Warning, TEXT("MASS PURGE: Fresh database generated. Capacity: %d Profiles."),
        ActiveNemeses.Num());
}

void UNemesisManager::AssignClassAppropriateAIBrain(ABaseHunter* SpawnedNPC)
{
    if (!IsValid(SpawnedNPC) || !SpawnedNPC->HunterDNA.bIsAlive) return;
    if (!SpawnedNPC->GetController()) SpawnedNPC->SpawnDefaultController();

    EHunterClass ClassType = SpawnedNPC->HunterDNA.HunterClass;
    UCharacterMovementComponent* MoveComp = SpawnedNPC->GetCharacterMovement();
    SpawnedNPC->SetActorTickEnabled(true);
    if (MoveComp)
    {
        MoveComp->SetComponentTickEnabled(true);
        MoveComp->SetMovementMode(MOVE_Walking);
    }
    if (AAIController* AI = Cast<AAIController>(SpawnedNPC->GetController()))
    {
        AI->SetActorTickEnabled(true);
        AI->StopMovement();
        if (UBrainComponent* Brain = AI->GetBrainComponent())
            Brain->SetComponentTickEnabled(true);
    }

    if (ClassType == EHunterClass::Farmer ||
        ClassType == EHunterClass::Citizen ||
        ClassType == EHunterClass::Child)
    {
        SpawnedNPC->SetActorTickEnabled(true);

        if (MoveComp)
        {
            MoveComp->bComponentShouldUpdatePhysicsVolume = false;
            MoveComp->bUseRVOAvoidance = false;
            MoveComp->SetComponentTickEnabled(true);
        }

        if (AAIController* AIC = Cast<AAIController>(SpawnedNPC->GetController()))
        {
            if (AIC->GetBrainComponent())
                AIC->GetBrainComponent()->StopLogic(TEXT("Passive Civilian Role Assignment"));

            FVector RandomWanderPoint = SpawnedNPC->GetActorLocation() +
                FVector(FMath::RandRange(-2000, 2000), FMath::RandRange(-2000, 2000), 0);
            AIC->MoveToLocation(RandomWanderPoint, 50.0f, false, true, false, false, nullptr, false);
        }
    }
    else
    {
        SpawnedNPC->SetActorTickEnabled(true);

        if (MoveComp)
        {
            MoveComp->bComponentShouldUpdatePhysicsVolume = true;
            MoveComp->bUseRVOAvoidance = true;
            MoveComp->SetComponentTickEnabled(true);
        }

        if (AAIController* AIC = Cast<AAIController>(SpawnedNPC->GetController()))
        {
            if (AIC->GetBrainComponent())
                AIC->GetBrainComponent()->RestartLogic();
        }
    }
}

void UNemesisManager::ProcessUsurperConstruction(FString RegionID)
{
    if (MapRegions.Contains(RegionID))
        UE_LOG(LogTemp, Warning, TEXT("The Usurper is building in: %s"), *RegionID);
}

void UNemesisManager::UpdateAIHuntTarget(AAIController* HunterAI)
{
    if (!HunterAI || ActiveRumors.Num() == 0) return;
    HunterAI->MoveToLocation(ActiveRumors[0].LastKnownLocation);
}

void UNemesisManager::ModifyActiveNemesis(FString ID, float ValueModifier)
{
    if (!FMath::IsFinite(ValueModifier)) return;
    ID = ResolveCharacterID(ID);
    if (ActiveNemeses.Contains(ID))
        ActiveNemeses[ID].Heat = FMath::Clamp(ActiveNemeses[ID].Heat + ValueModifier, 0.f, 100.f);
    RefreshPlayerAlias();
}

bool UNemesisManager::GetHunterProfile(FString HunterID, FHunterProfile& OutProfile)
{
    if (FHunterProfile* FoundProfile = ActiveNemeses.Find(ResolveCharacterID(HunterID)))
    {
        OutProfile = *FoundProfile;
        return true;
    }
    return false;
}

void UNemesisManager::AddHeat(FString CharacterID, float Amount)
{
    ModifyActiveNemesis(CharacterID, Amount);
}

void UNemesisManager::ModifyKarma(FString CharacterID, float Amount)
{
    if (!FMath::IsFinite(Amount)) return;
    if (CharacterID == TEXT("PLAYER"))
    {
        UpdateReputation(Amount, FMath::Abs(Amount) * 0.5f);
    }
    else if (ActiveNemeses.Contains(CharacterID))
    {
        ActiveNemeses[CharacterID].Karma =
            FMath::Clamp(ActiveNemeses[CharacterID].Karma + Amount, -100.f, 100.f);
        RefreshPlayerAlias();
    }
}

void UNemesisManager::ClaimRegion(FString RegionID, FString NewOwnerID)
{
    if (MapRegions.Contains(RegionID))
        MapRegions[RegionID].ControllingClanID = NewOwnerID;
}

void UNemesisManager::EnforcePopulationEquilibrium()
{
    int32 LivingCount = 0;
    for (const auto& Pair : ActiveNemeses)
        if (Pair.Value.bIsAlive && !IsRedundantPlayerAlias(Pair.Key)) ++LivingCount;

    while (LivingCount > MaxWorldPopulationCap)
    {
        FString DeadTargetID = SelectCullingCandidate();
        if (!DeadTargetID.IsEmpty())
        {
            ActiveNemeses[DeadTargetID].bIsAlive = false;
            if (ActiveNemeses[DeadTargetID].HunterClass == EHunterClass::Nameless &&
                !ActiveNemeses[DeadTargetID].bIsNamed)
            {
                for (auto& RemainingPair : ActiveNemeses)
                    RemainingPair.Value.PruneDanglingReferences(DeadTargetID);
            }
            LivingCount--;
        }
        else { break; }
    }
}

FString UNemesisManager::SelectCullingCandidate()
{
    FString BestCandidateID = TEXT("");
    int32   MaxAgeEvaluated = -1;

    for (const auto& Pair : ActiveNemeses)
    {
        const FHunterProfile& Profile = Pair.Value;
        if (!Profile.bIsAlive || Profile.bIsRealPlayer || IsRedundantPlayerAlias(Pair.Key) ||
            Profile.UniqueID.Equals(TEXT("PLAYER")) || Profile.HunterClass >= EHunterClass::BanditChief) continue;

        if (Profile.Age > 65 && Profile.Age > MaxAgeEvaluated)
        {
            MaxAgeEvaluated = Profile.Age;
            BestCandidateID = Pair.Key;
        }
    }

    if (BestCandidateID.IsEmpty())
    {
        for (const auto& Pair : ActiveNemeses)
        {
            if (Pair.Value.bIsAlive && !Pair.Value.bIsRealPlayer && !IsRedundantPlayerAlias(Pair.Key) &&
                !Pair.Value.UniqueID.Equals(TEXT("PLAYER")) &&
                Pair.Value.HunterClass == EHunterClass::Nameless)
                return Pair.Key;
        }
    }

    return BestCandidateID;
}

void UNemesisManager::RegisterCharacterExecution(FString VictimID, FString KillerID)
{
    VictimID = ResolveCharacterID(VictimID);
    KillerID = ResolveCharacterID(KillerID);
    if (!ActiveNemeses.Contains(VictimID) || !ActiveNemeses[VictimID].bIsAlive || KillerID.IsEmpty()) return;

    FHunterProfile& Victim = ActiveNemeses[VictimID];
    FString         VictimClan = Victim.ClanID;
    Victim.bIsAlive = false;
    Victim.MemoryBank.Add(FHunterMemory{
        FString::Printf(TEXT("Executed by %s."), *KillerID), FDateTime::Now(), Generation });

    for (auto& Pair : ActiveNemeses)
    {
        FHunterProfile& PotentialRelative = Pair.Value;
        if (!PotentialRelative.bIsAlive || Pair.Key.Equals(VictimID) || IsRedundantPlayerAlias(Pair.Key)) continue;

        if (PotentialRelative.RelationshipWeb.Contains(VictimID))
        {
            EFamilyTie Tie = PotentialRelative.RelationshipWeb[VictimID].TieType;
            if (Tie == EFamilyTie::Parent || Tie == EFamilyTie::Child ||
                Tie == EFamilyTie::Sibling || Tie == EFamilyTie::Spouse ||
                Tie == EFamilyTie::Slave)
            {
                PotentialRelative.VendettaList.FindOrAdd(KillerID) += 90.0f;
                PotentialRelative.MemoryBank.Add(FHunterMemory{
                    TEXT("Blood Feud Activated!"), FDateTime::Now(), Generation });
                // Death changes affinity, not historical parent/child/spouse structure.
                PotentialRelative.RelationshipWeb[VictimID].Affinity = 0;
            }
        }

        if (!VictimClan.Equals(TEXT("Neutral")) && PotentialRelative.ClanID.Equals(VictimClan))
            PotentialRelative.VendettaList.FindOrAdd(KillerID) += 20.0f;
    }

    if (ActiveNemeses.Contains(KillerID))
    {
        FString KillerClan = ActiveNemeses[KillerID].ClanID;
        ModifyFactionStanding(VictimClan, KillerClan, -40.0f);
    }
    RefreshPlayerAlias();
}

void UNemesisManager::EnslaveCharacter(FString TargetCharacterID, FString MasterCharacterID)
{
    TargetCharacterID = ResolveCharacterID(TargetCharacterID);
    MasterCharacterID = ResolveCharacterID(MasterCharacterID);
    if (TargetCharacterID == MasterCharacterID || !ActiveNemeses.Contains(TargetCharacterID) || MasterCharacterID.IsEmpty()) return;
    FHunterProfile& ThrallProfile = ActiveNemeses[TargetCharacterID];
    if (!ThrallProfile.bIsAlive) return;

    FRelationshipData& MasterRelation = ThrallProfile.RelationshipWeb.FindOrAdd(MasterCharacterID);
    MasterRelation.TieType = EFamilyTie::Slave;
    MasterRelation.Affinity = 0;

    if (ActiveNemeses.Contains(MasterCharacterID))
    {
        FHunterProfile& MasterProfile = ActiveNemeses[MasterCharacterID];
        MasterProfile.RelationshipWeb.FindOrAdd(TargetCharacterID).TieType = EFamilyTie::Master;
        ThrallProfile.ClanID = MasterProfile.ClanID;
    }

    ThrallProfile.MemoryBank.Add(FHunterMemory{
        FString::Printf(TEXT("Subjugated into servitude by %s."), *MasterCharacterID),
        FDateTime::Now(), Generation });

    ModifyFactionStanding(ThrallProfile.ClanID, TEXT("PLAYER"), -25.0f);
    RefreshPlayerAlias();
    SaveWorldState();
}

void UNemesisManager::DeactivateHunter(ABaseHunter* HunterToPool)
{
    if (!IsValid(HunterToPool) || InactiveHuntersPool.Contains(HunterToPool)) return;
    const FString CharacterID = HunterToPool->HunterDNA.UniqueID;
    ABaseHunter* CurrentBody = SpawnedPhysicalHunters.FindRef(CharacterID);
    if (CurrentBody && CurrentBody != HunterToPool) { HunterToPool->Destroy(); return; }
    if (CurrentBody == HunterToPool) SpawnedPhysicalHunters.Remove(CharacterID);
    FHunterProfile* Profile = ActiveNemeses.Find(CharacterID);
    FIntVector2D Cell;
    const FVector Location = HunterToPool->GetActorLocation();
    if (bDeinitializing || HunterToPool->GetWorld() != GetWorld() || !Profile ||
        Profile->UniqueID != CharacterID || Profile->LegacyGeneration != HunterToPool->HunterDNA.LegacyGeneration ||
        !TrySpatialCell(Location, SpatialGridCellSize, Cell))
    {
        HunterToPool->Destroy();
        return;
    }
    // Publish only movement and monotonic death from the matching incarnation.
    Profile->WorldLocation = Location;
    Profile->AssignedSpatialGridCell = Cell;
    if (!HunterToPool->HunterDNA.bIsAlive) Profile->bIsAlive = false;
    if (!Profile || !Profile->bIsAlive || !HunterToPool->HunterDNA.bIsAlive)
    {
        HunterToPool->Destroy(); // A corpse must never be reused as a living identity.
        return;
    }
    if (AAIController* AI = Cast<AAIController>(HunterToPool->GetController()))
    {
        AI->StopMovement();
        if (UBrainComponent* Brain = AI->GetBrainComponent())
        {
            Brain->StopLogic(TEXT("Body pooled"));
            Brain->SetComponentTickEnabled(false);
        }
        AI->SetActorTickEnabled(false);
    }
    if (UCharacterMovementComponent* Movement = HunterToPool->GetCharacterMovement())
    {
        Movement->StopMovementImmediately();
        Movement->DisableMovement();
        Movement->SetComponentTickEnabled(false);
    }
    HunterToPool->SetActorHiddenInGame(true);
    HunterToPool->SetActorEnableCollision(false);
    HunterToPool->SetActorTickEnabled(false);
    InactiveHuntersPool.AddUnique(HunterToPool);
}

ABaseHunter* UNemesisManager::GetHunterFromPool(FVector SpawnLocation)
{
    UWorld* World = GetWorld();
    FIntVector2D CheckedCell;
    if (bDeinitializing || !World || World->GetNetMode() == NM_Client ||
        !TrySpatialCell(SpawnLocation, SpatialGridCellSize, CheckedCell)) return nullptr;

    FVector        TraceStart = FVector(SpawnLocation.X, SpawnLocation.Y, 50000.0f);
    FVector        TraceEnd = FVector(SpawnLocation.X, SpawnLocation.Y, -20000.0f);
    FHitResult     SurfaceHit;
    FCollisionQueryParams TraceParams;
    TraceParams.bTraceComplex = false;

    FVector FinalSnappedLocation = SpawnLocation;
    if (World->LineTraceSingleByChannel(SurfaceHit, TraceStart, TraceEnd, ECC_Visibility, TraceParams))
        FinalSnappedLocation.Z = SurfaceHit.ImpactPoint.Z + 20.0f;

    for (int32 i = InactiveHuntersPool.Num() - 1; i >= 0; --i)
    {
        ABaseHunter* ReusedHunter = InactiveHuntersPool[i];
        InactiveHuntersPool.RemoveAt(i);
        if (!IsValid(ReusedHunter)) continue;
        const FHunterProfile* Previous = ActiveNemeses.Find(ReusedHunter->HunterDNA.UniqueID);
        if (ReusedHunter->GetWorld() != World || !Previous || !Previous->bIsAlive ||
            Previous->UniqueID != ReusedHunter->HunterDNA.UniqueID ||
            Previous->LegacyGeneration != ReusedHunter->HunterDNA.LegacyGeneration ||
            !ReusedHunter->HunterDNA.bIsAlive)
        {
            ReusedHunter->Destroy();
            continue;
        }
        // Do not expose the previous owner's identity to a new realization.
        ReusedHunter->HunterDNA = FHunterProfile();
        ReusedHunter->SetActorLocation(FinalSnappedLocation);
        ReusedHunter->SetActorHiddenInGame(false);
        ReusedHunter->SetActorEnableCollision(true);
        ReusedHunter->SetActorTickEnabled(true);
        return ReusedHunter;
    }

    if (BaseHunterClass)
    {
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

        ACharacter* SpawnedBody = World->SpawnActor<ACharacter>(
            BaseHunterClass, FinalSnappedLocation, FRotator::ZeroRotator, SpawnParams);
        ABaseHunter* Hunter = Cast<ABaseHunter>(SpawnedBody);
        if (!Hunter && IsValid(SpawnedBody)) SpawnedBody->Destroy();
        return Hunter;
    }

    UE_LOG(LogTemp, Error, TEXT("NEMESIS CRITICAL: Spawning failed � BaseHunterClass is NULL!"));
    return nullptr;
}

bool UNemesisManager::IsInPlayerInterest(const FVector& Location, float Radius) const
{
    UWorld* World = GetWorld();
    if (!World || World->GetNetMode() == NM_Client || !FMath::IsFinite(Radius) || Radius < 0.0f)
        return false;
    for (const TWeakObjectPtr<APawn>& WeakPlayer : TrackedServerPlayers)
    {
        const APawn* Player = WeakPlayer.Get();
        if (Player && Player->GetWorld() == World && Player->IsPlayerControlled() &&
            FVector::DistSquared(Player->GetActorLocation(), Location) <= FMath::Square(static_cast<double>(Radius)))
            return true;
    }
    return false;
}

void UNemesisManager::RealizeHunter(const FString& CharacterID)
{
    UWorld* World = GetWorld();
    if (bDeinitializing || !World || World->GetNetMode() == NM_Client ||
        SpawnedPhysicalHunters.Contains(CharacterID)) return;
    const FHunterProfile* Profile = ActiveNemeses.Find(CharacterID);
    if (!Profile || !Profile->bIsAlive || Profile->bIsRealPlayer || CharacterID == TEXT("PLAYER") ||
        !IsInPlayerInterest(Profile->WorldLocation, SpawningRadius)) return;
    // Copy before actor initialization, which can invoke external Blueprint events.
    const FHunterProfile DNA = *Profile;
    const uint64 RequestEpoch = WorldEpoch;
    if (ABaseHunter* NewNPC = GetHunterFromPool(DNA.WorldLocation))
    {
        auto IsCurrent = [&]()
        {
            const FHunterProfile* Current = ActiveNemeses.Find(CharacterID);
            return !bDeinitializing && WorldEpoch == RequestEpoch && GetWorld() == World &&
                IsValid(NewNPC) && NewNPC->GetWorld() == World && Current && Current->bIsAlive &&
                Current->UniqueID == DNA.UniqueID && Current->LegacyGeneration == DNA.LegacyGeneration;
        };
        if (!IsCurrent() || SpawnedPhysicalHunters.Contains(CharacterID))
        {
            if (IsValid(NewNPC)) NewNPC->Destroy();
            return;
        }
        SpawnedPhysicalHunters.Add(CharacterID, NewNPC);
        NewNPC->InitializeFromDNA(DNA);
        if (!IsCurrent() || SpawnedPhysicalHunters.FindRef(CharacterID) != NewNPC ||
            !NewNPC->HunterDNA.bIsAlive || NewNPC->HunterDNA.UniqueID != CharacterID)
        {
            if (SpawnedPhysicalHunters.FindRef(CharacterID) == NewNPC) SpawnedPhysicalHunters.Remove(CharacterID);
            if (IsValid(NewNPC)) NewNPC->Destroy();
            return;
        }
        AssignClassAppropriateAIBrain(NewNPC);
    }
}

void UNemesisManager::AsyncLoadPrimaryAsset(const FString& CharacterID)
{
    UWorld* World = GetWorld();
    const FHunterProfile* Profile = ActiveNemeses.Find(CharacterID);
    if (bDeinitializing || !World || World->GetNetMode() == NM_Client ||
        PendingMeshRequests.Contains(CharacterID) || !Profile || !Profile->bIsAlive ||
        Profile->bIsRealPlayer || CharacterID == TEXT("PLAYER") ||
        !IsInPlayerInterest(Profile->WorldLocation, SpawningRadius)) return;

    const FGuid RequestID = FGuid::NewGuid();
    const FString SubjectID = Profile->UniqueID;
    const int32 SubjectGeneration = Profile->LegacyGeneration;
    const EHunterClass SubjectClass = Profile->HunterClass;
    const FSoftObjectPath MeshPath = Profile->AsyncSkeletalMeshHandle.ToSoftObjectPath();
    const FPrimaryAssetId PrimaryID = HunterClassPrimaryAssets.FindRef(SubjectClass);
    const TWeakObjectPtr<UNemesisManager> WeakThis(this);
    const TWeakObjectPtr<UWorld> RequestWorld(World);
    const uint64 RequestEpoch = WorldEpoch;
    PendingMeshIdentities.Add(CharacterID, RequestID);
    PendingMeshRequests.Add(CharacterID, TSharedPtr<FStreamableHandle>());

    auto ContinueWithMesh = [WeakThis, RequestWorld, RequestEpoch, CharacterID, RequestID,
        SubjectID, SubjectGeneration, SubjectClass, MeshPath, PrimaryID]()
    {
        UNemesisManager* Manager = WeakThis.Get();
        if (!Manager || Manager->PendingMeshIdentities.FindRef(CharacterID) != RequestID) return;
        const TSharedPtr<FStreamableHandle> PrimaryHandle = Manager->PendingMeshRequests.FindRef(CharacterID);
        Manager->PendingPrimaryAssetIDs.Remove(CharacterID);
        const FHunterProfile* Current = Manager->ActiveNemeses.Find(CharacterID);
        if (Manager->bDeinitializing || Manager->WorldEpoch != RequestEpoch || !RequestWorld.IsValid() ||
            Manager->GetWorld() != RequestWorld.Get() || !Current || !Current->bIsAlive ||
            Current->bIsRealPlayer || Current->UniqueID != SubjectID ||
            Current->LegacyGeneration != SubjectGeneration || Current->HunterClass != SubjectClass ||
            Current->AsyncSkeletalMeshHandle.ToSoftObjectPath() != MeshPath ||
            Manager->HunterClassPrimaryAssets.FindRef(SubjectClass) != PrimaryID)
        {
            Manager->PendingMeshRequests.Remove(CharacterID);
            Manager->PendingMeshIdentities.Remove(CharacterID);
            return;
        }
        // A Primary Asset is optional preloading, never an assumption about the mesh's schema.
        Manager->RequestHunterMesh(CharacterID, RequestID);
    };

    UAssetManager* Assets = UAssetManager::GetIfInitialized();
    if (!PrimaryID.IsValid() || !Assets || Assets->GetPrimaryAssetPath(PrimaryID).IsNull())
    {
        ContinueWithMesh();
        return;
    }
    PendingPrimaryAssetIDs.Add(CharacterID);
    TSharedPtr<FStreamableHandle> Handle = Assets->LoadPrimaryAsset(
        PrimaryID, TArray<FName>(), FStreamableDelegate::CreateLambda(ContinueWithMesh));
    // Completion may run inline and already install the fallback's handle.
    if (PendingMeshIdentities.FindRef(CharacterID) == RequestID && PendingPrimaryAssetIDs.Contains(CharacterID))
    {
        if (Handle.IsValid()) PendingMeshRequests.Add(CharacterID, Handle);
        else ContinueWithMesh();
    }
}

void UNemesisManager::RequestHunterMesh(const FString& CharacterID, const FGuid& RequestID)
{
    if (PendingMeshIdentities.FindRef(CharacterID) != RequestID) return;
    const FHunterProfile* Profile = ActiveNemeses.Find(CharacterID);
    UWorld* World = GetWorld();
    if (bDeinitializing || !World || World->GetNetMode() == NM_Client || !Profile ||
        !Profile->bIsAlive || Profile->bIsRealPlayer ||
        !IsInPlayerInterest(Profile->WorldLocation, SpawningRadius))
    {
        PendingMeshRequests.Remove(CharacterID);
        PendingMeshIdentities.Remove(CharacterID);
        return;
    }
    if (Profile->AsyncSkeletalMeshHandle.IsValid() || Profile->AsyncSkeletalMeshHandle.IsNull())
    {
        PendingMeshRequests.Remove(CharacterID);
        PendingMeshIdentities.Remove(CharacterID);
        RealizeHunter(CharacterID);
        return;
    }
    const FString SubjectID = Profile->UniqueID;
    const int32 SubjectGeneration = Profile->LegacyGeneration;
    const EHunterClass SubjectClass = Profile->HunterClass;
    const FSoftObjectPath MeshPath = Profile->AsyncSkeletalMeshHandle.ToSoftObjectPath();
    const TWeakObjectPtr<UNemesisManager> WeakThis(this);
    const TWeakObjectPtr<UWorld> RequestWorld(World);
    const uint64 RequestEpoch = WorldEpoch;
    TSharedPtr<FStreamableHandle> Handle = AssetStreamerPipeline.RequestAsyncLoad(MeshPath,
        FStreamableDelegate::CreateLambda([WeakThis, RequestWorld, RequestEpoch, CharacterID,
            RequestID, SubjectID, SubjectGeneration, SubjectClass, MeshPath]()
        {
            UNemesisManager* Manager = WeakThis.Get();
            if (!Manager || Manager->PendingMeshIdentities.FindRef(CharacterID) != RequestID) return;
            const TSharedPtr<FStreamableHandle> CompletedHandle = Manager->PendingMeshRequests.FindRef(CharacterID);
            Manager->PendingMeshRequests.Remove(CharacterID);
            Manager->PendingMeshIdentities.Remove(CharacterID);
            const FHunterProfile* Current = Manager->ActiveNemeses.Find(CharacterID);
            if (Manager->bDeinitializing || Manager->WorldEpoch != RequestEpoch || !RequestWorld.IsValid() ||
                Manager->GetWorld() != RequestWorld.Get() || !Current || !Current->bIsAlive ||
                Current->bIsRealPlayer || Current->UniqueID != SubjectID ||
                Current->LegacyGeneration != SubjectGeneration || Current->HunterClass != SubjectClass ||
                Current->AsyncSkeletalMeshHandle.ToSoftObjectPath() != MeshPath ||
                !Current->AsyncSkeletalMeshHandle.IsValid()) return;
            Manager->RealizeHunter(CharacterID);
        }));
    if (PendingMeshIdentities.FindRef(CharacterID) == RequestID)
    {
        if (Handle.IsValid()) PendingMeshRequests.Add(CharacterID, Handle);
        else
        {
            PendingMeshRequests.Remove(CharacterID);
            PendingMeshIdentities.Remove(CharacterID);
        }
    }
}

void UNemesisManager::ScanAndManagePhysicalPopulation(APawn* PlayerPawn)
{
    if (bDeinitializing || !IsValid(PlayerPawn) || !PlayerPawn->IsPlayerControlled() || !BaseHunterClass) return;
    UWorld* World = GetWorld();
    if (!World || PlayerPawn->GetWorld() != World || World->GetNetMode() == NM_Client ||
        !FMath::IsFinite(SpawningRadius) || SpawningRadius < 0.0f) return;
    SynchronizeSimulation(); // Never wait on the worker while holding DataMutex.
    if (StreamingWorld.Get() != World) InvalidateWorldRequests();
    TrackedServerPlayers.RemoveAll([World](const TWeakObjectPtr<APawn>& Player)
    {
        return !Player.IsValid() || Player->GetWorld() != World || !Player->IsPlayerControlled();
    });
    TrackedServerPlayers.AddUnique(TWeakObjectPtr<APawn>(PlayerPawn));

    const int32 CellSize = FMath::Max(1, SpatialGridCellSize);
    const double Span = FMath::CeilToDouble(static_cast<double>(SpawningRadius) / CellSize);
    if (Span >= MAX_int32) return;
    const int32 CellSpan = static_cast<int32>(Span);
    SpatialGridMatrix.Empty();
    for (auto& Pair : ActiveNemeses)
    {
        FHunterProfile& Profile = Pair.Value;
        if (!Profile.bIsAlive || Profile.bIsRealPlayer || Pair.Key == TEXT("PLAYER")) continue;
        if (!TrySpatialCell(Profile.WorldLocation, CellSize, Profile.AssignedSpatialGridCell)) continue;
        SpatialGridMatrix.FindOrAdd(Profile.AssignedSpatialGridCell).CharacterIDs.Add(Pair.Key);
    }

    TArray<FString> RetireIDs;
    for (const auto& Pair : SpawnedPhysicalHunters)
    {
        const FHunterProfile* Profile = ActiveNemeses.Find(Pair.Key);
        if (!IsValid(Pair.Value) || !Profile || !Profile->bIsAlive || !Pair.Value->HunterDNA.bIsAlive ||
            !IsInPlayerInterest(Pair.Value->GetActorLocation(), SpawningRadius + 2000.0f))
            RetireIDs.Add(Pair.Key);
    }
    for (const FString& ID : RetireIDs)
    {
        ABaseHunter* Body = SpawnedPhysicalHunters.FindRef(ID);
        SpawnedPhysicalHunters.Remove(ID);
        DeactivateHunter(Body);
    }

    TSet<FString> CandidateIDs;
    for (const TWeakObjectPtr<APawn>& WeakPlayer : TrackedServerPlayers)
    {
        const APawn* Player = WeakPlayer.Get();
        if (!Player) continue;
        const FVector PlayerLocation = Player->GetActorLocation();
        FIntVector2D PlayerCell;
        if (!TrySpatialCell(PlayerLocation, CellSize, PlayerCell)) continue;
        // Visit occupied cells, not an unbounded radius-squared set of empty coordinates.
        for (const auto& CellPair : SpatialGridMatrix)
        {
            const int64 DX = static_cast<int64>(CellPair.Key.X) - PlayerCell.X;
            const int64 DY = static_cast<int64>(CellPair.Key.Y) - PlayerCell.Y;
            if (DX >= -static_cast<int64>(CellSpan) && DX <= CellSpan &&
                DY >= -static_cast<int64>(CellSpan) && DY <= CellSpan)
                CandidateIDs.Append(CellPair.Value.CharacterIDs);
        }
    }
    for (const FString& ID : CandidateIDs)
    {
        if (SpawnedPhysicalHunters.Contains(ID) || PendingMeshRequests.Contains(ID)) continue;
        const FHunterProfile* Profile = ActiveNemeses.Find(ID);
        if (!Profile || !Profile->bIsAlive || Profile->bIsRealPlayer ||
            !IsInPlayerInterest(Profile->WorldLocation, SpawningRadius)) continue;
        AsyncLoadPrimaryAsset(ID);
    }
}

void UNemesisManager::AbsorbHeirAsPlayer(FString ChosenHeirID)
{
    // Compatibility entry point has no caller parameter: it may only operate on
    // the primary player's authoritative, dying pawn. It never rewrites IDs.
    if (!IsInGameThread() || bDeinitializing || !GetWorld() ||
        GetWorld()->GetNetMode() == NM_Client || ChosenHeirID.Len() > 256) return;
    ARoninGameMode* Mode = Cast<ARoninGameMode>(GetWorld()->GetAuthGameMode());
    FHunterProfile Primary;
    if (!Mode || !GetHunterProfile(TEXT("PLAYER"), Primary)) return;
    for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It) {
        APlayerController* PC = It->Get();
        ARoninCharacter* Pawn = IsValid(PC) ? Cast<ARoninCharacter>(PC->GetPawn()) : nullptr;
        if (!IsValid(Pawn) || !Pawn->bHasInitiatedDeath ||
            Pawn->PlayerDNA.UniqueID != Primary.UniqueID) continue;
        Mode->ReincarnateAsHeir(PC, Pawn->GetPlayerDNA(), ChosenHeirID);
        return;
    }
}

void UNemesisManager::LinkParentalLineage(FHunterProfile& ChildNPC)
{
    // The argument may be a map value: record its key and own all input before insertion.
    FString StoredChildKey;
    for (const auto& Pair : ActiveNemeses)
        if (&Pair.Value == &ChildNPC) { StoredChildKey = Pair.Key; break; }
    FHunterProfile Child = ChildNPC;
    if (Child.UniqueID.IsEmpty() || Child.Age < 0 || Child.Age > MAX_int32 - 35) return;
    SynchronizeSimulation();
    const FString ChildID = ResolveCharacterID(StoredChildKey.IsEmpty() ? Child.UniqueID : StoredChildKey);
    if (!StoredChildKey.IsEmpty())
        if (const FHunterProfile* Canonical = ActiveNemeses.Find(ChildID)) Child = *Canonical;
    if (Child.Age < 0 || Child.Age > MAX_int32 - 35) return;
    FString PossibleFather;
    FString PossibleMother;

    for (const auto& Pair : ActiveNemeses)
    {
        const FHunterProfile& PotentialParent = Pair.Value;
        if (IsRedundantPlayerAlias(Pair.Key) || Pair.Key == ChildID ||
            PotentialParent.ClanID != Child.ClanID ||
            PotentialParent.Age < Child.Age + 16 || !PotentialParent.bIsAlive) continue;
        if (PossibleFather.IsEmpty() && PotentialParent.Gender == ERoninGender::Male)
            PossibleFather = Pair.Key;
        else if (PossibleMother.IsEmpty() && PotentialParent.Gender == ERoninGender::Female)
            PossibleMother = Pair.Key;
        if (!PossibleFather.IsEmpty() && !PossibleMother.IsEmpty()) break;
    }

    if (PossibleFather.IsEmpty())
    {
        FHunterProfile HistoricFather;
        HistoricFather.UniqueID = FGuid::NewGuid().ToString();
        HistoricFather.ClanID = Child.ClanID;
        HistoricFather.HunterName = FString::Printf(TEXT("Elder %s Ancestor"), *Child.ClanID);
        HistoricFather.Age = Child.Age + FMath::RandRange(22, 35);
        HistoricFather.Gender = ERoninGender::Male;
        HistoricFather.HunterClass = Child.HunterClass;
        HistoricFather.Alignment = Child.Alignment;
        HistoricFather.WorldLocation = Child.WorldLocation +
            FVector(FMath::RandRange(-500.f, 500.f), FMath::RandRange(-500.f, 500.f), 0.f);
        HistoricFather.SkeletalMeshPath = GetClassAppropriateMesh(HistoricFather.HunterClass);
        HistoricFather.FatherID = TEXT("None - Ancestor Record");
        HistoricFather.MotherID = TEXT("None - Ancestor Record");
        PossibleFather = HistoricFather.UniqueID;
        ActiveNemeses.Add(PossibleFather, HistoricFather);
    }

    FHunterProfile* Target = StoredChildKey.IsEmpty() ? &ChildNPC : ActiveNemeses.Find(ChildID);
    if (!Target) return;
    Target->FatherID = PossibleFather;
    Target->MotherID = PossibleMother.IsEmpty() ? TEXT("Deceased / Historical") : PossibleMother;
    Target->RelationshipWeb.FindOrAdd(PossibleFather).TieType = EFamilyTie::Parent;
    if (!PossibleMother.IsEmpty()) Target->RelationshipWeb.FindOrAdd(PossibleMother).TieType = EFamilyTie::Parent;
    if (FHunterProfile* Father = ActiveNemeses.Find(PossibleFather))
        Father->RelationshipWeb.FindOrAdd(ChildID).TieType = EFamilyTie::Child;
    if (FHunterProfile* Mother = ActiveNemeses.Find(PossibleMother))
        Mother->RelationshipWeb.FindOrAdd(ChildID).TieType = EFamilyTie::Child;
    RefreshPlayerAlias();
}

FString UNemesisManager::GenerateRandomHunter(FString OriginClan, bool bGenerateAILore)
{
    if (!IsInGameThread() || bDeinitializing || bRejectedWorldLoad) return FString();
    FHunterProfile NewHunter;
    NewHunter.UniqueID = FGuid::NewGuid().ToString();

    TArray<FString> FirstNames = { TEXT("Jin"),TEXT("Mugen"),TEXT("Kenshin"),TEXT("Sanjiro"),TEXT("Kojiro"),TEXT("Hanzo"),TEXT("Ryuzo") };
    TArray<FString> LastNames = { TEXT("Sakai"),TEXT("Himura"),TEXT("Miyamoto"),TEXT("Hattori"),TEXT("Shimazu"),TEXT("Oda") };

    NewHunter.HunterName = FirstNames[FMath::RandRange(0, FirstNames.Num() - 1)]
        + TEXT(" ") + LastNames[FMath::RandRange(0, LastNames.Num() - 1)];
    NewHunter.ClanID = OriginClan;
    NewHunter.bIsAlive = true;
    NewHunter.LegacyGeneration = Generation;
    NewHunter.Karma = FMath::RandRange(-50.0f, 50.0f);
    NewHunter.Heat = FMath::RandRange(0.0f, 20.0f);

    if (MasterJobRegistry.Contains(EHunterClass::Nameless) && MasterJobRegistry[EHunterClass::Nameless])
    {
        URoninJobArchetype* DefaultJob = MasterJobRegistry[EHunterClass::Nameless];
        NewHunter.JobClassData = DefaultJob;
        NewHunter.HunterClass = DefaultJob->HunterClass;
        if (DefaultJob->MinGenerationAge < 0 || DefaultJob->MaxGenerationAge < DefaultJob->MinGenerationAge ||
            DefaultJob->MaxGenerationAge == MAX_int32) return FString();
        NewHunter.Age = FMath::RandRange(DefaultJob->MinGenerationAge, DefaultJob->MaxGenerationAge);
        NewHunter.PowerLevel = DefaultJob->BasePowerLevel;
        NewHunter.Blood = DefaultJob->BaseBlood;
        NewHunter.Stamina = DefaultJob->BaseStamina;
        NewHunter.SkeletalMeshPath = DefaultJob->DefaultSkeletalMeshPath;
    }
    else
    {
        NewHunter.HunterClass = EHunterClass::Nameless;
        NewHunter.Age = FMath::RandRange(18, 55);
        NewHunter.PowerLevel = 1;
        NewHunter.SkeletalMeshPath = GetClassAppropriateMesh(NewHunter.HunterClass);
    }

    NewHunter.WorldLocation = GenerateRandomMapCoordinates();
    NewHunter.FatherID = TEXT("None - Ancestor");
    NewHunter.MotherID = TEXT("None - Ancestor");

    if (!TrySpatialCell(NewHunter.WorldLocation, SpatialGridCellSize, NewHunter.AssignedSpatialGridCell)) return FString();

    NewHunter.AsyncSkeletalMeshHandle = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(NewHunter.SkeletalMeshPath));
    RegisterHunter(NewHunter); // Cached AI responses may invoke the delegate inline.

    if (bGenerateAILore)
    {
        if (UDeepSeekSubsystem* AI = GetGameInstance()->GetSubsystem<UDeepSeekSubsystem>())
        {
            FString BioPrompt = FString::Printf(
                TEXT("Create a gritty, 2-sentence Chanbara-style origin story for a wandering Ronin named %s from the %s clan."),
                *NewHunter.HunterName, *OriginClan);

            FOnDeepSeekResponse ResponseDelegate;
            FString NewHunterID = NewHunter.UniqueID;
            const TWeakObjectPtr<UNemesisManager> WeakThis(this);
            const TWeakObjectPtr<UWorld> RequestWorld(GetWorld());
            const uint64 RequestEpoch = WorldEpoch;
            ResponseDelegate.BindLambda([WeakThis, RequestWorld, RequestEpoch, NewHunterID](FString AIResult)
                {
                    UNemesisManager* Manager = WeakThis.Get();
                    if (!Manager || Manager->bDeinitializing || Manager->WorldEpoch != RequestEpoch ||
                        !RequestWorld.IsValid() || Manager->GetWorld() != RequestWorld.Get()) return;
                    FHunterProfile* Profile = Manager->ActiveNemeses.Find(NewHunterID);
                    if (Profile && Profile->bIsAlive && Profile->UniqueID == NewHunterID)
                    {
                        Profile->Biography.OriginStory = AIResult;
                        Manager->SaveWorldState();
                    }
                });
            AI->SendPromptToDeepSeek(BioPrompt, ResponseDelegate);
        }
    }

    SaveWorldState();
    return NewHunter.UniqueID;
}

void UNemesisManager::ProcessRumorAndSpawnAvenger(UWorld* WorldContext, const FString& InstigatorID,
    const FString& Crime, FVector Location, float HeatGenerated)
{
    FIntVector2D CheckedCell;
    if (bDeinitializing || !WorldContext || WorldContext != GetWorld() ||
        WorldContext->GetNetMode() == NM_Client || !FMath::IsFinite(HeatGenerated) ||
        !TrySpatialCell(Location, SpatialGridCellSize, CheckedCell)) return;
    FRumorData R;
    R.Type = ERumorType::Crime;
    R.SubjectID = InstigatorID;
    R.RawRumorText = Crime;
    R.Fact = Crime;
    R.SpreadLevel = HeatGenerated;
    R.LastKnownLocation = Location;
    ActiveRumors.Add(R);

    SpawnAvenger(WorldContext, InstigatorID, Location);
}

void UNemesisManager::SpawnAvenger(UWorld* WorldContext, const FString& InstigatorID, FVector Location)
{
    if (bDeinitializing || !WorldContext || WorldContext != GetWorld() ||
        WorldContext->GetNetMode() == NM_Client || InstigatorID.IsEmpty()) return;
    const FVector SpawnPos = Location + FVector(
        FMath::RandRange(-1000, 1000), FMath::RandRange(-1000, 1000), 100);

    FHunterProfile DNA;
    DNA.UniqueID = FGuid::NewGuid().ToString();
    DNA.HunterName = TEXT("Avenger");
    DNA.HunterClass = EHunterClass::Hunter;
    DNA.LegacyGeneration = Generation;
    DNA.WorldLocation = SpawnPos;
    DNA.SkeletalMeshPath = GetClassAppropriateMesh(DNA.HunterClass);
    DNA.AsyncSkeletalMeshHandle = TSoftObjectPtr<USkeletalMesh>(FSoftObjectPath(DNA.SkeletalMeshPath));
    DNA.VendettaList.Add(ResolveCharacterID(InstigatorID), 90.0f);
    const uint64 RequestEpoch = WorldEpoch;
    if (ABaseHunter* Avenger = GetHunterFromPool(SpawnPos))
    {
        if (!IsValid(Avenger) || bDeinitializing || WorldEpoch != RequestEpoch || GetWorld() != WorldContext)
        {
            if (IsValid(Avenger)) Avenger->Destroy();
            return;
        }
        DNA.WorldLocation = Avenger->GetActorLocation();
        if (!TrySpatialCell(DNA.WorldLocation, SpatialGridCellSize, DNA.AssignedSpatialGridCell))
        {
            Avenger->Destroy();
            return;
        }
        RegisterHunter(DNA);
        SpawnedPhysicalHunters.Add(DNA.UniqueID, Avenger);
        Avenger->InitializeFromDNA(DNA);
        const FHunterProfile* Current = ActiveNemeses.Find(DNA.UniqueID);
        if (bDeinitializing || WorldEpoch != RequestEpoch || GetWorld() != WorldContext ||
            !IsValid(Avenger) || !Current || !Current->bIsAlive ||
            Current->LegacyGeneration != DNA.LegacyGeneration ||
            SpawnedPhysicalHunters.FindRef(DNA.UniqueID) != Avenger)
        {
            if (SpawnedPhysicalHunters.FindRef(DNA.UniqueID) == Avenger) SpawnedPhysicalHunters.Remove(DNA.UniqueID);
            if (IsValid(Avenger)) Avenger->Destroy();
            return;
        }
        AssignClassAppropriateAIBrain(Avenger);
        SaveWorldState();
        UE_LOG(LogTemp, Log, TEXT("Avenger initialized and tracked: %s"), *DNA.UniqueID);
    }
}

bool UNemesisManager::ReassignVillageTributary(const FString& VillageRegionID, const FString& NewFortressRegionID)
{
    if (!IsInGameThread() || bDeinitializing || !GetWorld() || GetWorld()->GetNetMode() == NM_Client ||
        VillageRegionID.IsEmpty() || NewFortressRegionID.IsEmpty() || VillageRegionID == NewFortressRegionID) return false;
    SynchronizeSimulation();
    FScopeLock Lock(&DataMutex);

    if (!MapRegions.Contains(VillageRegionID) || !MapRegions.Contains(NewFortressRegionID))
    {
        UE_LOG(LogTemp, Error,
            TEXT("FEUDAL MATRIX: Reassignment failed. IDs not found. (Village: %s | Fortress: %s)"),
            *VillageRegionID, *NewFortressRegionID);
        return false;
    }

    FRegionData& VillageData = MapRegions[VillageRegionID];
    const FRegionData& NewFortressData = MapRegions[NewFortressRegionID];

    if (VillageData.RegionType == ESubRegionType::MilitaryFortress ||
        NewFortressData.RegionType != ESubRegionType::MilitaryFortress)
    {
        UE_LOG(LogTemp, Error, TEXT("FEUDAL MATRIX: Hierarchy violation. Source must be a node, destination a fortress."));
        return false;
    }

    TSet<FString> Ancestors;
    Ancestors.Add(VillageRegionID);
    FString Cursor = NewFortressRegionID;
    while (!Cursor.IsEmpty()) {
        if (Ancestors.Contains(Cursor)) return false;
        Ancestors.Add(Cursor);
        const FRegionData* Region = MapRegions.Find(Cursor);
        if (!Region) return false;
        Cursor = Region->ParentFortressRegionID;
    }
    FString OldFortressID = VillageData.ParentFortressRegionID;
    UE_LOG(LogTemp, Warning, TEXT("FEUDAL MATRIX: Severing bond between Node %s and Fortress %s"),
        *VillageRegionID, *OldFortressID);

    VillageData.ParentFortressRegionID = NewFortressRegionID;
    VillageData.ControllingClanID = NewFortressData.ControllingClanID;

    UE_LOG(LogTemp, Warning, TEXT("FEUDAL MATRIX: Node %s now pays tribute to Fortress %s under clan '%s'!"),
        *VillageRegionID, *NewFortressRegionID, *VillageData.ControllingClanID);

    AggregateSubRegionDataToParents();
    TArray<AActor*> TrackedHubs;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), ASettlementHub::StaticClass(), TrackedHubs);
    for (AActor* HubActor : TrackedHubs)
    {
        if (ASettlementHub* PhysicalHub = Cast<ASettlementHub>(HubActor))
            PhysicalHub->SyncWithBackendDatabase();
    }
    SaveWorldState();
    return true;
}

FVector UNemesisManager::GenerateRandomMapCoordinates()
{
    float BoundX = FMath::RandRange(-60000.0f, 60000.0f);
    float BoundY = FMath::RandRange(-60000.0f, 60000.0f);
    float GroundPlaneZ = 120.0f;
    return FVector(BoundX, BoundY, GroundPlaneZ);
}

FString UNemesisManager::GetClassAppropriateMesh(EHunterClass CharacterClass)
{
    switch (CharacterClass)
    {
    case EHunterClass::Soldier:
    case EHunterClass::Hunter:
        return TEXT("/Game/Characters/Samurai/Mesh/SK_Heavy_Samurai.SK_Heavy_Samurai");

    case EHunterClass::Bandit:
    case EHunterClass::BanditChief:
        return TEXT("/Game/Characters/Bandits/Mesh/SK_Bandit_Leader.SK_Bandit_Leader");

    case EHunterClass::Child:
        return TEXT("/Game/Characters/Children/Mesh/SK_Child_Mesh.SK_Child_Mesh");

    case EHunterClass::Farmer:
    case EHunterClass::Citizen:
    case EHunterClass::Nameless:
    default:
        return TEXT("/Game/Characters/Villagers/Mesh/SK_Peasant_Commoner.SK_Peasant_Commoner");
    }
}
