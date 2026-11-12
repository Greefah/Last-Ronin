#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GameplayTagContainer.h"
#include "URoninQuestAsset.h"
#include "URoninQuestManager.generated.h"

class URoninSaveGame;

// Owned value snapshot. Objectives' CurrentState is runtime-only in this copy.
USTRUCT(BlueprintType)
struct LASTRONIN_API FRoninQuestRuntimeData
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Quest")
    FName QuestID;

    UPROPERTY(BlueprintReadOnly, Category = "Quest")
    FString QuestName;

    UPROPERTY(BlueprintReadOnly, Category = "Quest")
    FString QuestDescription;

    UPROPERTY(BlueprintReadOnly, Category = "Quest")
    FGameplayTag TriggerRumorTag;

    UPROPERTY(BlueprintReadOnly, Category = "Quest")
    ERoninQuestState State = ERoninQuestState::NotStarted;

    UPROPERTY(BlueprintReadOnly, Category = "Quest")
    TArray<FRoninQuestObjective> Objectives;

    UPROPERTY(BlueprintReadOnly, Category = "Quest")
    TMap<FString, int32> ObjectiveCounts;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FRoninQuestStateChanged, FName, QuestID, ERoninQuestState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FRoninQuestObjectiveProgress, FName, QuestID, FString, ObjectiveID, int32, CurrentCount, int32, RequiredCount, ERoninQuestState, ObjectiveState);

UCLASS()
class LASTRONIN_API URoninQuestManager : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    // Explicit registration only; validates and copies the definition. No asset discovery.
    // An already registered ID is accepted only for an equivalent definition (no replacement).
    UFUNCTION(BlueprintCallable, Category = "Ronin|Quests")
    bool RegisterQuestDefinition(URoninQuestAsset* QuestAsset);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Quests")
    bool StartQuest(FName QuestID);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Quests")
    void TriggerQuestFromRumor(FGameplayTag RumorTag);

    // int32 is inherently finite. Nonpositive amounts are ignored; counts saturate at RequiredCount.
    UFUNCTION(BlueprintCallable, Category = "Ronin|Quests")
    void UpdateObjectiveProgress(FGameplayTag ObjectiveTag, int32 Amount = 1);

    UFUNCTION(BlueprintPure, Category = "Ronin|Quests")
    ERoninQuestState GetQuestState(FName QuestID) const;

    // All getters copy values; no mutable runtime or asset references are returned.
    UFUNCTION(BlueprintPure, Category = "Ronin|Quests")
    bool GetQuestSnapshot(FName QuestID, FRoninQuestRuntimeData& OutSnapshot) const;

    UFUNCTION(BlueprintPure, Category = "Ronin|Quests")
    TArray<FRoninQuestRuntimeData> GetQuestSnapshots() const;

    // Game thread only. No disk IO, manager lookup, rewards or load-time delegates.
    // Parent must synchronize its simulation before snapshotting/replacing WorldState.
    UFUNCTION(BlueprintCallable, Category = "Ronin|Quests|Save")
    bool ExportSaveData(URoninSaveGame* SaveData) const;

    // Atomic replacement; registration may happen before or after import. False leaves runtime intact.
    UFUNCTION(BlueprintCallable, Category = "Ronin|Quests|Save")
    bool ImportSaveData(const URoninSaveGame* SaveData);

    UPROPERTY(BlueprintAssignable, Category = "Ronin|Quests")
    FRoninQuestStateChanged OnQuestStateChanged;

    UPROPERTY(BlueprintAssignable, Category = "Ronin|Quests")
    FRoninQuestObjectiveProgress OnObjectiveProgressChanged;

protected:
    // Legacy API fields retained as mirrors. RuntimeQuests is authoritative.
    UPROPERTY(BlueprintReadOnly, Category = "Quest")
    TMap<FName, ERoninQuestState> QuestStates;

    UPROPERTY(BlueprintReadOnly, Category = "Quest")
    TArray<URoninQuestAsset*> ActiveQuests;

private:
    void CompleteQuest(URoninQuestAsset* QuestAsset);
    void BroadcastQuestUpdate(FName QuestID, ERoninQuestState NewState);
    void RebuildLegacyMirrors();
    bool StartQuestInternal(FName QuestID);
    static bool HasCompletedRequiredObjectives(const FRoninQuestRuntimeData& Quest);
    static FString MakeObjectiveSaveKey(FName QuestID, const FString& ObjectiveID);
    static bool RestoreQuest(FRoninQuestRuntimeData& Quest,
        const TMap<FName, uint8>& States, const TMap<FString, int32>& Counts);

    UPROPERTY(Transient)
    TMap<FName, URoninQuestAsset*> RegisteredDefinitions;

    UPROPERTY(Transient)
    TMap<FName, FRoninQuestRuntimeData> RuntimeQuests;

    // Retain not-yet-registered definitions' save entries across load/save and late registration.
    TMap<FName, uint8> ImportedQuestStates;
    TMap<FString, int32> ImportedObjectiveCounts;

    // Mutators invoked by delegate callbacks are rejected, rather than invalidating live iterations.
    bool bDispatchingQuestEvents = false;
};