#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "RoninTypes.h"
#include "RoninJobArchetype.h" 
#include "Engine/StreamableManager.h"
#include "Async/AsyncWork.h" 
#include "NemesisManager.generated.h" 

class ARoninCharacter;
class AAIController;
class ABaseHunter;
class FAsyncFeudalSimulationWorker;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnYearPassed, int32, NewYear);

UCLASS()
class LASTRONIN_API UNemesisManager : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Serialize(FArchive& Ar) override;

    // --- PERSISTENCE & DATA TRACKERS ---
    UPROPERTY(BlueprintReadOnly, Category = "Nemesis")
    TMap<FString, FHunterProfile> ActiveNemeses;

    UPROPERTY(BlueprintReadOnly, Category = "Nemesis")
    TMap<FString, FRegionData> MapRegions;

    UPROPERTY(BlueprintReadOnly, Category = "Nemesis")
    TArray<FRumorData> ActiveRumors;

    UPROPERTY(BlueprintReadOnly, Category = "Nemesis")
    int32 Generation = 1;

    // --- DATA-DRIVEN REGISTRY AND BALANCING MATRIX ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nemesis|Job Registry Matrix")
    TMap<EHunterClass, URoninJobArchetype*> MasterJobRegistry;

    // Optional registered Primary Assets; an empty/unknown mapping uses the existing mesh loader.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nemesis|Spawning")
    TMap<EHunterClass, FPrimaryAssetId> HunterClassPrimaryAssets;

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Job Registry")
    void PopulateJobRegistry(UDataTable* JobDataTable);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nemesis|Simulation")
    int32 MaxWorldPopulationCap = 500;

    // FIXED: Formally exposed seeding container property declaration inside the class scope boundary
    UPROPERTY(EditDefaultsOnly, Category = "Nemesis|Configuration Assets")
    UDataTable* RegionSeedingTable;

    // --- MASS SCALABILITY PARAMETERS ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nemesis|Mass Scale")
    int32 SpatialGridCellSize = 10000;

    UPROPERTY(BlueprintReadOnly, Category = "Nemesis|Mass Scale")
    TMap<FIntVector2D, FSpatialCellGroup> SpatialGridMatrix;

    UPROPERTY(BlueprintReadOnly, Category = "Nemesis|Diplomacy")
    TMap<FString, float> GlobalClanFriction;

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Diplomacy")
    void ModifyFactionStanding(FString ClanA, FString ClanB, float DeltaStanding);

    UFUNCTION(BlueprintPure, Category = "Nemesis|Diplomacy")
    float GetFactionStanding(FString ClanA, FString ClanB) const;

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Vendettas")
    void RegisterCharacterExecution(FString VictimID, FString KillerID);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Social")
    void EnslaveCharacter(FString TargetCharacterID, FString MasterCharacterID);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nemesis|Spawning")
    float SpawningRadius = 15000.0f;

    UPROPERTY()
    TMap<FString, ABaseHunter*> SpawnedPhysicalHunters;

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Spawning")
    void ScanAndManagePhysicalPopulation(APawn* PlayerPawn);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Feudal Matrix")
    bool ReassignVillageTributary(const FString& VillageRegionID, const FString& NewFortressRegionID);

    /** Calculates the closest structural region key tracking a given world coordinate vector */
    UFUNCTION(BlueprintPure, Category = "Nemesis|Demographics")
    FString GetRegionIDFromCoordinates(FVector Location) const;

    /** Runs a daily simulation tick evaluating localized population splits and percent-capped births */
    UFUNCTION(BlueprintCallable, Category = "Nemesis|Demographics")
    void ProcessGenerationalBirthAndHousingPass();

    UPROPERTY(BlueprintReadWrite, Category = "Nemesis|Survival")
    float HungerLevel = 0.0f;

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Survival")
    void ProcessSurvivalMetrics(ARoninCharacter* Player, float DeltaTime);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Survival")
    void PerformRest(ARoninCharacter* Player, float NutritionValue);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Legacy")
    void RecordPlayerDeath(FString KillerID);

    UPROPERTY(BlueprintReadOnly, Category = "Nemesis|Time")
    int32 CurrentYear = 1570;

    UPROPERTY(BlueprintReadOnly, Category = "Nemesis|Time")
    int32 CurrentDay = 1;

    UPROPERTY(BlueprintAssignable, Category = "Nemesis|Events")
    FOnYearPassed OnYearPassed;

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Time")
    void AdvanceWorldClock(float DeltaSeconds, float TimeMultiplier = 1.0f);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Kingdom")
    void ClaimRegion(FString RegionID, FString NewOwnerID);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Kingdom")
    void ProcessUsurperConstruction(FString RegionID);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Rumors")
    void GenerateRumorsForCrime(FString CrimeLocation, float KarmaImpact);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Rumors")
    void ProcessRumorAndSpawnAvenger(UWorld* WorldContext, const FString& InstigatorID, const FString& Crime, FVector Location, float HeatGenerated);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nemesis|Spawning")
    TSubclassOf<class ACharacter> BaseHunterClass;

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Social")
    void ModifyKarma(FString CharacterID, float Amount);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Social")
    void AddHeat(FString CharacterID, float Amount);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Sync")
    void SyncPlayerProfile(FHunterProfile P);

    void RegisterHunter(const FHunterProfile& D);

    // Native integration helpers: PLAYER-only legacy records remain valid.
    FString ResolveCharacterID(const FString& CharacterID) const;
    bool IsRedundantPlayerAlias(const FString& CharacterID) const;
    void RefreshPlayerAlias();

    // Native C++ only, game thread only; no parameters or return value.
    // Blocks for the owned worker, merges only current-world/epoch and unchanged-baseline
    // unrealized NPC movement results, then releases the task. No task is a no-op.
    // Call before external persistence snapshots or direct registry/world replacement.
    // Does not advance the clock, save, invalidate streaming, or snapshot physical actors.
    void SynchronizeSimulation();

    UFUNCTION(BlueprintCallable, Category = "Nemesis|System")
    bool GetHunterProfile(FString HunterID, FHunterProfile& OutProfile);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Rumors")
    FRumorData GetMostRelevantRumor(FVector CurrentLocation);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|AI")
    void UpdateAIHuntTarget(AAIController* HunterAI);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|SaveLoad")
    void SaveWorldState();

    UFUNCTION(BlueprintCallable, Category = "Nemesis|SaveLoad")
    void LoadWorldState();

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Reputation")
    void UpdateReputation(float KarmaShift, float NotorietyGain);

    /** Global asset management subsystem core driver handle */
    FStreamableManager AssetStreamerPipeline;

    /** Dynamic brain evaluation router to switch AI profiles between passive logic and Chanbara styles */
    UFUNCTION(BlueprintCallable, Category = "Nemesis|AI")
    void AssignClassAppropriateAIBrain(class ABaseHunter* SpawnedNPC);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Reputation")
    void GenerateHeroicRumor(FString Location, FString ActionDescription, float FameIncrease);

    void EnforcePopulationEquilibrium();
    FString SelectCullingCandidate();

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Dynasty")
    void AbsorbHeirAsPlayer(FString ChosenHeirID);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Generation")
    FString GenerateRandomHunter(FString OriginClan, bool bGenerateAILore = true);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Generation")
    void SeedWorldPopulation(int32 StartingPopulation = 500);
   
    UFUNCTION(BlueprintCallable, Category = "Nemesis|Generation")
    void ResetPopulationGenerator(int32 NewTargetPopulation = 500);

    void DeactivateHunter(ABaseHunter* HunterToPool);
    ABaseHunter* GetHunterFromPool(FVector SpawnLocation);

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Kingdom")
    void AggregateSubRegionDataToParents();

    UFUNCTION(BlueprintCallable, Category = "Nemesis|Economy")
    void RecalculateProfileNetWealth(FString CharacterID);

protected:
    void RunBackgroundWarAndDiplomacy(float DaysPassed);

private:
    float DayCounter = 0.f;
    // Only a successful load or the explicit destructive reset may clear rejection.
    bool bRejectedWorldLoad = false;
    bool bLoadedWorldState = false;
    void TickWorldSystems(float DaysPassed);
    void SpawnAvenger(UWorld* WorldContext, const FString& InstigatorID, FVector Location);
    void ModifyActiveNemesis(FString ID, float ValueModifier);

    void LinkParentalLineage(FHunterProfile& ChildNPC);
    FVector GenerateRandomMapCoordinates();
    FString GetClassAppropriateMesh(EHunterClass CharacterClass);

    UPROPERTY()
    TArray<ABaseHunter*> InactiveHuntersPool;

    // Last applied child contribution, separate from the region's intrinsic maximum.
    UPROPERTY()
    TMap<FString, float> FortressChildBonuses;

    void InvalidateWorldRequests();
    bool IsInPlayerInterest(const FVector& Location, float Radius) const;
    void RealizeHunter(const FString& CharacterID);
    void AsyncLoadPrimaryAsset(const FString& CharacterID);
    void RequestHunterMesh(const FString& CharacterID, const FGuid& RequestID);

    uint64 WorldEpoch = 0;
    bool bDeinitializing = false;
    TWeakObjectPtr<UWorld> StreamingWorld;
    TArray<TWeakObjectPtr<APawn>> TrackedServerPlayers;
    TMap<FString, TSharedPtr<FStreamableHandle>> PendingMeshRequests;
    TMap<FString, FGuid> PendingMeshIdentities;
    TSet<FString> PendingPrimaryAssetIDs;

    FCriticalSection DataMutex;
    TWeakObjectPtr<UWorld> SimulationWorld;
    FAsyncTask<FAsyncFeudalSimulationWorker>* ActiveSimulationThreadTask = nullptr;
};