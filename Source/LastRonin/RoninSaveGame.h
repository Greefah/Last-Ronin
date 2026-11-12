#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "RoninTypes.h"
#include "RoninSaveGame.generated.h"

UCLASS()
class LASTRONIN_API URoninSaveGame : public USaveGame
{
    GENERATED_BODY()

public:
    // --- CHARACTER DATA ---
    UPROPERTY() TMap<FString, FHunterProfile> SavedNemeses;
    UPROPERTY() TArray<FRumorData> SavedRumors;
    UPROPERTY() int32 SavedGeneration;

    // --- WORLD & KINGDOM DATA ---
    UPROPERTY() TMap<FString, FRegionData> SavedRegions;

    // --- TIME DATA ---
    UPROPERTY() int32 SavedYear;
    UPROPERTY() int32 SavedDay;
    UPROPERTY() float SavedDayCounter;

    // Missing in older slots: version zero keeps legacy diplomacy initialization.
    UPROPERTY() int32 SaveVersion = 0;
    UPROPERTY() TMap<FString, float> SavedClanFriction;

    // Version two records the child contribution already included in each fortress maximum.
    // Legacy slots default to an empty map; no intrinsic balance value is guessed.
    UPROPERTY() TMap<FString, float> SavedFortressChildBonuses;

    // Independent quest schema; zero means a legacy slot with no quest snapshot.
    // These contain only stable identities and values, never asset/runtime object pointers.
    UPROPERTY() int32 QuestSaveVersion = 0;
    UPROPERTY() TMap<FName, uint8> SavedQuestStates;
    UPROPERTY() TMap<FString, int32> SavedQuestObjectiveCounts;

    // Non-mutating preflight. Do not normalize corrupt values into a new valid slot.
    // Legacy PLAYER-only records and arbitrary non-GUID string identities are supported.
    bool IsWorldSnapshotValid() const
    {
        auto FiniteVector = [](const FVector& V)
        {
            return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
        };
        if (SaveVersion < 0 || SaveVersion > 2 || SavedGeneration < 1 ||
            SavedDay < 1 || SavedDay > 365 ||
            !FMath::IsFinite(SavedDayCounter) || SavedDayCounter < 0.f) return false;
        for (const auto& Pair : SavedNemeses)
        {
            const FHunterProfile& P = Pair.Value;
            if (Pair.Key.IsEmpty() || P.UniqueID.IsEmpty() ||
                (Pair.Key != TEXT("PLAYER") && Pair.Key != P.UniqueID) ||
                P.Age < 0 || P.LegacyGeneration < 1 ||
                !FiniteVector(P.WorldLocation) || !FiniteVector(P.ActiveTravelRoute.DestinationTarget) ||
                !FMath::IsFinite(P.ActiveTravelRoute.TravelSpeed) || P.ActiveTravelRoute.TravelSpeed < 0.f)
                return false;
            for (float Value : {P.Karma, P.Heat, P.Notoriety, P.InheritedFame, P.Hunger,
                P.Thirst, P.Potential, P.Stamina, P.Blood})
                if (!FMath::IsFinite(Value)) return false;
            for (const auto& Item : P.Inventory)
                if (Item.Key.IsEmpty() || Item.Value.Quantity < 0 || !FMath::IsFinite(Item.Value.Potency)) return false;
            for (const auto& Vendetta : P.VendettaList)
                if (!FMath::IsFinite(Vendetta.Value)) return false;
        }
        for (const auto& Pair : SavedRegions)
        {
            const FRegionData& R = Pair.Value;
            if (Pair.Key.IsEmpty() || R.RegionID != Pair.Key) return false;
            for (float Value : {R.FortressHealth, R.MaxFortressHealth, R.DistrictValue,
                R.DangerLevel, R.LocalHeat, R.LocalFortificationValue,
                R.Stockpile.Rice, R.Stockpile.Iron, R.Stockpile.Lumber, R.Stockpile.Fish})
                if (!FMath::IsFinite(Value)) return false;
            for (const auto& Item : R.SharedStockpile)
                if (Item.Key.IsEmpty() || Item.Value < 0) return false;
            for (const auto& Item : R.SharedStockpileMetadata)
                if (Item.Key.IsEmpty() || !FMath::IsFinite(Item.Value.Potency)) return false;
            for (const FBuildingData& Building : R.Structures)
                if (!FiniteVector(Building.WorldLocation) || !FMath::IsFinite(Building.CurrentHealth)) return false;
        }
        for (const FRumorData& Rumor : SavedRumors)
            if (!FiniteVector(Rumor.LastKnownLocation) || !FMath::IsFinite(Rumor.SpreadLevel)) return false;
        for (const auto& Pair : SavedClanFriction)
            if (!FMath::IsFinite(Pair.Value)) return false;
        for (const auto& Pair : SavedFortressChildBonuses)
            if (!FMath::IsFinite(Pair.Value) || Pair.Value < 0.f) return false;
        return true;
    }

    URoninSaveGame()
    {
        SavedGeneration = 1;
        SavedYear = 1570;
        SavedDay = 1;
        SavedDayCounter = 0.f;
    }
};