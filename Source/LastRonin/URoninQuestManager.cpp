#include "URoninQuestManager.h" // <--- MUST BE THE ABSOLUTE FIRST LINE
#include "RoninSaveGame.h"
#include "Templates/UnrealTemplate.h"

namespace
{
    constexpr int32 RoninQuestSaveVersion = 1;

    bool IsValidQuestState(uint8 State)
    {
        return State == static_cast<uint8>(ERoninQuestState::NotStarted) ||
            State == static_cast<uint8>(ERoninQuestState::InProgress) ||
            State == static_cast<uint8>(ERoninQuestState::Completed) ||
            State == static_cast<uint8>(ERoninQuestState::Failed);
    }

    bool EquivalentDefinition(const FRoninQuestRuntimeData& Existing, const URoninQuestAsset& Asset)
    {
        if (Existing.QuestID != Asset.QuestID || Existing.QuestName != Asset.QuestName ||
            Existing.QuestDescription != Asset.QuestDescription ||
            Existing.TriggerRumorTag != Asset.TriggerRumorTag ||
            Existing.Objectives.Num() != Asset.Objectives.Num()) return false;
        for (int32 Index = 0; Index < Existing.Objectives.Num(); ++Index)
        {
            const FRoninQuestObjective& A = Existing.Objectives[Index];
            const FRoninQuestObjective& B = Asset.Objectives[Index];
            if (A.ObjectiveID != B.ObjectiveID || A.Description != B.Description ||
                A.bIsOptional != B.bIsOptional || A.ObjectiveTag != B.ObjectiveTag ||
                A.RequiredCount != B.RequiredCount) return false;
        }
        // Legacy CurrentState is intentionally not definition input or runtime authority.
        return true;
    }

    struct FQuestProgressNotification
    {
        FName QuestID;
        FString ObjectiveID;
        int32 Count;
        int32 Required;
        ERoninQuestState State;
    };
}

void URoninQuestManager::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    // Deliberately no GetSubsystem, slot IO, or clearing: the parent's load can precede registration.
}

bool URoninQuestManager::RegisterQuestDefinition(URoninQuestAsset* QuestAsset)
{
    if (!IsInGameThread() || bDispatchingQuestEvents || !IsValid(QuestAsset) ||
        QuestAsset->QuestID.IsNone() || QuestAsset->Objectives.IsEmpty()) return false;

    if (const FRoninQuestRuntimeData* Existing = RuntimeQuests.Find(QuestAsset->QuestID))
        return EquivalentDefinition(*Existing, *QuestAsset);
    // One shared object cannot be registered again under a mutated ID.
    for (const auto& Pair : RegisteredDefinitions)
        if (Pair.Value == QuestAsset) return false;

    TSet<FString> ObjectiveIDs;
    bool bHasRequiredObjective = false;
    for (const FRoninQuestObjective& Objective : QuestAsset->Objectives)
    {
        if (Objective.ObjectiveID.TrimStartAndEnd().IsEmpty() ||
            ObjectiveIDs.Contains(Objective.ObjectiveID) || !Objective.ObjectiveTag.IsValid() ||
            Objective.RequiredCount <= 0) return false;
        ObjectiveIDs.Add(Objective.ObjectiveID);
        bHasRequiredObjective |= !Objective.bIsOptional;
    }
    if (!bHasRequiredObjective) return false;

    FRoninQuestRuntimeData Runtime;
    Runtime.QuestID = QuestAsset->QuestID;
    Runtime.QuestName = QuestAsset->QuestName;
    Runtime.QuestDescription = QuestAsset->QuestDescription;
    Runtime.TriggerRumorTag = QuestAsset->TriggerRumorTag;
    Runtime.Objectives = QuestAsset->Objectives;
    if (!RestoreQuest(Runtime, ImportedQuestStates, ImportedObjectiveCounts)) return false;
    RegisteredDefinitions.Add(Runtime.QuestID, QuestAsset);
    RuntimeQuests.Add(Runtime.QuestID, MoveTemp(Runtime));
    RebuildLegacyMirrors();
    return true;
}

bool URoninQuestManager::StartQuest(FName QuestID)
{
    if (!IsInGameThread() || bDispatchingQuestEvents) return false;
    TGuardValue<bool> Guard(bDispatchingQuestEvents, true);
    return StartQuestInternal(QuestID);
}

bool URoninQuestManager::StartQuestInternal(FName QuestID)
{
    FRoninQuestRuntimeData* Quest = RuntimeQuests.Find(QuestID);
    if (!Quest || Quest->State != ERoninQuestState::NotStarted) return false;
    Quest->State = ERoninQuestState::InProgress;
    for (FRoninQuestObjective& Objective : Quest->Objectives)
        Objective.CurrentState = ERoninQuestState::InProgress;
    RebuildLegacyMirrors();
    // Copy before delegate delivery. No shared definition is ever mutated.
    const FRoninQuestRuntimeData Snapshot = *Quest;
    BroadcastQuestUpdate(QuestID, Snapshot.State);
    for (const FRoninQuestObjective& Objective : Snapshot.Objectives)
        OnObjectiveProgressChanged.Broadcast(QuestID, Objective.ObjectiveID,
            Snapshot.ObjectiveCounts.FindRef(Objective.ObjectiveID), Objective.RequiredCount, Objective.CurrentState);
    return true;
}

void URoninQuestManager::TriggerQuestFromRumor(FGameplayTag RumorTag)
{
    if (!IsInGameThread() || bDispatchingQuestEvents || !RumorTag.IsValid()) return;
    TGuardValue<bool> Guard(bDispatchingQuestEvents, true);
    TArray<FName> IDs;
    RuntimeQuests.GetKeys(IDs);
    IDs.Sort([](const FName& A, const FName& B) { return A.ToString() < B.ToString(); });
    for (FName ID : IDs)
    {
        const FRoninQuestRuntimeData& Quest = RuntimeQuests.FindChecked(ID);
        if (Quest.TriggerRumorTag == RumorTag) StartQuestInternal(ID);
    }
}

void URoninQuestManager::UpdateObjectiveProgress(FGameplayTag ObjectiveTag, int32 Amount)
{
    // The preserved int32 API cannot represent NaN/Infinity; reject zero/negative input.
    if (!IsInGameThread() || bDispatchingQuestEvents || !ObjectiveTag.IsValid() || Amount <= 0) return;
    TGuardValue<bool> Guard(bDispatchingQuestEvents, true);
    TArray<FName> IDs;
    RuntimeQuests.GetKeys(IDs);
    IDs.Sort([](const FName& A, const FName& B) { return A.ToString() < B.ToString(); });
    TArray<FQuestProgressNotification> Notifications;
    TArray<FName> CompletedIDs;
    for (FName ID : IDs)
    {
        FRoninQuestRuntimeData& Quest = RuntimeQuests.FindChecked(ID);
        if (Quest.State != ERoninQuestState::InProgress) continue;
        for (FRoninQuestObjective& Objective : Quest.Objectives)
        {
            if (Objective.ObjectiveTag != ObjectiveTag || Objective.RequiredCount <= 0 ||
                Objective.CurrentState == ERoninQuestState::Completed) continue;
            const int32 Previous = Quest.ObjectiveCounts.FindRef(Objective.ObjectiveID);
            if (Previous < 0 || Previous > Objective.RequiredCount) continue;
            // Both operands are bounded int32 positives; addition is checked in int64 before narrowing.
            const int64 Sum = static_cast<int64>(Previous) + static_cast<int64>(Amount);
            if (Sum <= Previous || Sum > static_cast<int64>(MAX_int32) * 2) continue;
            const int32 Next = static_cast<int32>(FMath::Min(Sum, static_cast<int64>(Objective.RequiredCount)));
            if (Next == Previous) continue;
            Quest.ObjectiveCounts.Add(Objective.ObjectiveID, Next);
            Objective.CurrentState = Next == Objective.RequiredCount
                ? ERoninQuestState::Completed : ERoninQuestState::InProgress;
            FQuestProgressNotification Notice;
            Notice.QuestID = ID;
            Notice.ObjectiveID = Objective.ObjectiveID;
            Notice.Count = Next;
            Notice.Required = Objective.RequiredCount;
            Notice.State = Objective.CurrentState;
            Notifications.Add(MoveTemp(Notice));
        }
        if (HasCompletedRequiredObjectives(Quest))
        {
            CompleteQuest(RegisteredDefinitions.FindRef(ID));
            if (Quest.State == ERoninQuestState::Completed) CompletedIDs.Add(ID);
        }
    }
    RebuildLegacyMirrors();
    // Commit the entire input event before callbacks, so getters/exports see a coherent snapshot.
    for (const FQuestProgressNotification& Notice : Notifications)
        OnObjectiveProgressChanged.Broadcast(Notice.QuestID, Notice.ObjectiveID,
            Notice.Count, Notice.Required, Notice.State);
    for (FName ID : CompletedIDs) BroadcastQuestUpdate(ID, ERoninQuestState::Completed);
}

ERoninQuestState URoninQuestManager::GetQuestState(FName QuestID) const
{
    const FRoninQuestRuntimeData* Quest = RuntimeQuests.Find(QuestID);
    return Quest ? Quest->State : ERoninQuestState::NotStarted;
}

bool URoninQuestManager::GetQuestSnapshot(FName QuestID, FRoninQuestRuntimeData& OutSnapshot) const
{
    OutSnapshot = FRoninQuestRuntimeData();
    if (const FRoninQuestRuntimeData* Quest = RuntimeQuests.Find(QuestID))
    {
        OutSnapshot = *Quest;
        return true;
    }
    return false;
}

TArray<FRoninQuestRuntimeData> URoninQuestManager::GetQuestSnapshots() const
{
    TArray<FName> IDs;
    RuntimeQuests.GetKeys(IDs);
    IDs.Sort([](const FName& A, const FName& B) { return A.ToString() < B.ToString(); });
    TArray<FRoninQuestRuntimeData> Result;
    Result.Reserve(IDs.Num());
    for (FName ID : IDs) Result.Add(RuntimeQuests.FindChecked(ID));
    return Result;
}

bool URoninQuestManager::HasCompletedRequiredObjectives(const FRoninQuestRuntimeData& Quest)
{
    bool bHasRequiredObjective = false;
    for (const FRoninQuestObjective& Objective : Quest.Objectives)
    {
        if (Objective.bIsOptional) continue;
        bHasRequiredObjective = true;
        if (Objective.RequiredCount <= 0 ||
            Quest.ObjectiveCounts.FindRef(Objective.ObjectiveID) < Objective.RequiredCount) return false;
    }
    return bHasRequiredObjective;
}

void URoninQuestManager::CompleteQuest(URoninQuestAsset* QuestAsset)
{
    // Resolve by the captured registration, never by possibly edited asset fields.
    for (const auto& Pair : RegisteredDefinitions)
    {
        if (Pair.Value != QuestAsset) continue;
        FRoninQuestRuntimeData* Quest = RuntimeQuests.Find(Pair.Key);
        if (Quest && Quest->State == ERoninQuestState::InProgress && HasCompletedRequiredObjectives(*Quest))
            Quest->State = ERoninQuestState::Completed;
        return;
    }
    // No invented reward, economy/GAS access, or empty quest completion.
}

void URoninQuestManager::BroadcastQuestUpdate(FName QuestID, ERoninQuestState NewState)
{
    OnQuestStateChanged.Broadcast(QuestID, NewState);
}

void URoninQuestManager::RebuildLegacyMirrors()
{
    QuestStates.Empty();
    ActiveQuests.Empty();
    for (const auto& Pair : RuntimeQuests)
    {
        QuestStates.Add(Pair.Key, Pair.Value.State);
        if (Pair.Value.State == ERoninQuestState::InProgress)
        {
            if (URoninQuestAsset* Asset = RegisteredDefinitions.FindRef(Pair.Key)) ActiveQuests.Add(Asset);
        }
    }
}

FString URoninQuestManager::MakeObjectiveSaveKey(FName QuestID, const FString& ObjectiveID)
{
    // FName identity is case-insensitive. Canonicalize it, but preserve exact FString objective IDs.
    // Length framing prevents collisions even when names contain colons or separators.
    const FString CanonicalID = QuestID.ToString().ToLower();
    return FString::Printf(TEXT("%d:%s%d:%s"), CanonicalID.Len(), *CanonicalID, ObjectiveID.Len(), *ObjectiveID);
}

bool URoninQuestManager::RestoreQuest(FRoninQuestRuntimeData& Quest,
    const TMap<FName, uint8>& States, const TMap<FString, int32>& Counts)
{
    const uint8* SavedState = States.Find(Quest.QuestID);
    if (SavedState && !IsValidQuestState(*SavedState)) return false;
    Quest.State = SavedState ? static_cast<ERoninQuestState>(*SavedState) : ERoninQuestState::NotStarted;
    Quest.ObjectiveCounts.Empty();
    bool bHasRequired = false;
    for (FRoninQuestObjective& Objective : Quest.Objectives)
    {
        if (Objective.RequiredCount <= 0) return false;
        const int32* SavedCount = Counts.Find(MakeObjectiveSaveKey(Quest.QuestID, Objective.ObjectiveID));
        const int32 Count = SavedCount ? *SavedCount : 0;
        if (Count < 0 || Count > Objective.RequiredCount ||
            (Quest.State == ERoninQuestState::NotStarted && Count != 0)) return false;
        bHasRequired |= !Objective.bIsOptional;
        Quest.ObjectiveCounts.Add(Objective.ObjectiveID, Count);
        Objective.CurrentState = Count == Objective.RequiredCount ? ERoninQuestState::Completed :
            (Quest.State == ERoninQuestState::NotStarted ? ERoninQuestState::NotStarted :
                (Quest.State == ERoninQuestState::Failed ? ERoninQuestState::Failed : ERoninQuestState::InProgress));
    }
    if (!bHasRequired) return false;
    const bool bAllRequiredComplete = HasCompletedRequiredObjectives(Quest);
    if (Quest.State == ERoninQuestState::Completed && !bAllRequiredComplete) return false;
    if (Quest.State == ERoninQuestState::InProgress && bAllRequiredComplete)
        Quest.State = ERoninQuestState::Completed;
    return true;
}

bool URoninQuestManager::ExportSaveData(URoninSaveGame* SaveData) const
{
    if (!IsInGameThread() || !IsValid(SaveData)) return false;
    TMap<FName, uint8> States = ImportedQuestStates;
    TMap<FString, int32> Counts = ImportedObjectiveCounts;
    for (const auto& Pair : RuntimeQuests)
    {
        States.Add(Pair.Key, static_cast<uint8>(Pair.Value.State));
        for (const FRoninQuestObjective& Objective : Pair.Value.Objectives)
            Counts.Add(MakeObjectiveSaveKey(Pair.Key, Objective.ObjectiveID),
                Pair.Value.ObjectiveCounts.FindRef(Objective.ObjectiveID));
    }
    SaveData->QuestSaveVersion = RoninQuestSaveVersion;
    SaveData->SavedQuestStates = MoveTemp(States);
    SaveData->SavedQuestObjectiveCounts = MoveTemp(Counts);
    return true;
}

bool URoninQuestManager::ImportSaveData(const URoninSaveGame* SaveData)
{
    if (!IsInGameThread() || bDispatchingQuestEvents || !IsValid(SaveData) ||
        SaveData->QuestSaveVersion < 0 || SaveData->QuestSaveVersion > RoninQuestSaveVersion) return false;
    TMap<FName, uint8> States;
    TMap<FString, int32> Counts;
    if (SaveData->QuestSaveVersion == RoninQuestSaveVersion)
    {
        States = SaveData->SavedQuestStates;
        Counts = SaveData->SavedQuestObjectiveCounts;
        for (const auto& Pair : States)
            if (Pair.Key.IsNone() || !IsValidQuestState(Pair.Value)) return false;
        for (const auto& Pair : Counts)
            if (Pair.Key.IsEmpty() || Pair.Value < 0) return false;
    }
    TMap<FName, FRoninQuestRuntimeData> Restored = RuntimeQuests;
    for (auto& Pair : Restored)
        if (!RestoreQuest(Pair.Value, States, Counts)) return false;
    RuntimeQuests = MoveTemp(Restored);
    ImportedQuestStates = MoveTemp(States);
    ImportedObjectiveCounts = MoveTemp(Counts);
    RebuildLegacyMirrors();
    // Restore is silent, including completed quests. UI pulls snapshots after load/registration.
    return true;
}