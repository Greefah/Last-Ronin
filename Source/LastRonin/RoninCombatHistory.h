#pragma once

#include "CoreMinimal.h"
#include "RoninTypes.h"
#include "RoninCombatHistory.generated.h"

/** Frame-indexed commands. Consumers define button bits and apply DeterministicSeed explicitly. */
USTRUCT(BlueprintType)
struct LASTRONIN_API FRoninCombatInput
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    int64 Frame = -1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    FVector2D Move = FVector2D::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    FVector2D Look = FVector2D::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    uint8 Buttons = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    ESamuraiStance RequestedStance = ESamuraiStance::Flash;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    int32 DeterministicSeed = 0;
};

/** Selected restoration values only, NOT complete movement/GAS/animation rollback state. */
USTRUCT(BlueprintType)
struct LASTRONIN_API FRoninCombatSnapshot
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    int64 Frame = -1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    FTransform Transform = FTransform::Identity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    FVector Velocity = FVector::ZeroVector;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    ESamuraiStance Stance = ESamuraiStance::Flash;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    float Blood = 100.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    float Stamina = 100.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    float Potential = 100.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    float Posture = 0.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    int32 FreezeFrames = 0;

    // IDs and frame bounds are consumer-defined; -1 means no active identity/bound.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    int64 AttackID = -1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    int64 WindowID = -1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    int64 AttackStartFrame = -1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    int64 WindowStartFrame = -1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    int64 WindowEndFrame = -1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    bool bIsAttacking = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    bool bIsGuarding = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ronin|Combat")
    int32 DeterministicSeed = 0;
};

/**
 * Two independent bounded frame rings, owning copies of their records.
 * Each stream retains only [LatestFrame - Capacity + 1, LatestFrame].
 * Gaps are misses; in-window corrections are allowed; stale writes are rejected.
 * Single-owner native storage, with no synchronization or UObject access guarantees.
 */
class LASTRONIN_API FRoninCombatHistory
{
public:
    // Positive capacity required. Invalid configuration leaves existing history untouched.
    // Successful configuration clears history; caller is responsible for its allocation budget.
    bool Configure(int32 InCapacity)
    {
        if (InCapacity <= 0)
        {
            return false;
        }
        Capacity = InCapacity;
        Inputs.SetNum(Capacity);
        Snapshots.SetNum(Capacity);
        Reset();
        return true;
    }

    bool RecordInput(const FRoninCombatInput& Input)
    {
        if (!IsRetainedFrame(Input.Frame, LatestInputFrame))
        {
            return false;
        }
        Inputs[SlotFor(Input.Frame)] = Input;
        LatestInputFrame = FMath::Max(LatestInputFrame, Input.Frame);
        return true;
    }

    bool RecordSnapshot(const FRoninCombatSnapshot& Snapshot)
    {
        if (!IsRetainedFrame(Snapshot.Frame, LatestSnapshotFrame))
        {
            return false;
        }
        Snapshots[SlotFor(Snapshot.Frame)] = Snapshot;
        LatestSnapshotFrame = FMath::Max(LatestSnapshotFrame, Snapshot.Frame);
        return true;
    }

    // Borrowed read-only pointers; do not retain across writes, Reset, Configure or destruction.
    const FRoninCombatInput* FindInput(int64 Frame) const
    {
        if (!IsRetainedFrame(Frame, LatestInputFrame) || Frame > LatestInputFrame)
        {
            return nullptr;
        }
        const FRoninCombatInput& Entry = Inputs[SlotFor(Frame)];
        return Entry.Frame == Frame ? &Entry : nullptr;
    }

    const FRoninCombatSnapshot* FindSnapshot(int64 Frame) const
    {
        if (!IsRetainedFrame(Frame, LatestSnapshotFrame) || Frame > LatestSnapshotFrame)
        {
            return nullptr;
        }
        const FRoninCombatSnapshot& Entry = Snapshots[SlotFor(Frame)];
        return Entry.Frame == Frame ? &Entry : nullptr;
    }

    // Copy-out overloads leave the destination unchanged on a miss.
    bool FindInput(int64 Frame, FRoninCombatInput& OutInput) const
    {
        const FRoninCombatInput* Found = FindInput(Frame);
        if (!Found)
        {
            return false;
        }
        OutInput = *Found;
        return true;
    }

    bool FindSnapshot(int64 Frame, FRoninCombatSnapshot& OutSnapshot) const
    {
        const FRoninCombatSnapshot* Found = FindSnapshot(Frame);
        if (!Found)
        {
            return false;
        }
        OutSnapshot = *Found;
        return true;
    }

    // Clear records while retaining configured capacity and storage.
    void Reset()
    {
        for (FRoninCombatInput& Input : Inputs)
        {
            Input = FRoninCombatInput();
        }
        for (FRoninCombatSnapshot& Snapshot : Snapshots)
        {
            Snapshot = FRoninCombatSnapshot();
        }
        LatestInputFrame = -1;
        LatestSnapshotFrame = -1;
    }

    int32 GetCapacity() const { return Capacity; }

private:
    bool IsRetainedFrame(int64 Frame, int64 LatestFrame) const
    {
        // Subtract only when nonnegative: safe even for MAX_int64 frames.
        return Capacity > 0 && Frame >= 0 &&
            (LatestFrame < static_cast<int64>(Capacity) ||
             Frame > LatestFrame - static_cast<int64>(Capacity));
    }

    int32 SlotFor(int64 Frame) const
    {
        return static_cast<int32>(Frame % static_cast<int64>(Capacity));
    }

    int32 Capacity = 0;
    int64 LatestInputFrame = -1;
    int64 LatestSnapshotFrame = -1;
    TArray<FRoninCombatInput> Inputs;
    TArray<FRoninCombatSnapshot> Snapshots;
};
