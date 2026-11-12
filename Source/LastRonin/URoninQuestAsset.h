#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"

// 1. MUST BE THE LAST INCLUDE
#include "URoninQuestAsset.generated.h"

// 2. ENUM IN GLOBAL SCOPE (Outside the classes)
UENUM(BlueprintType)
enum class ERoninQuestState : uint8
{
    NotStarted     UMETA(DisplayName = "Not Started"),
    InProgress     UMETA(DisplayName = "In Progress"),
    Completed      UMETA(DisplayName = "Completed"),
    Failed         UMETA(DisplayName = "Failed")
};

// 3. OBJECTIVE STRUCT
USTRUCT(BlueprintType)
struct LASTRONIN_API FRoninQuestObjective
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString ObjectiveID;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString Description;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bIsOptional = false;

    // Legacy definition field retained; the subsystem owns a separate runtime copy.
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    ERoninQuestState CurrentState = ERoninQuestState::NotStarted;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
    FGameplayTag ObjectiveTag;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest", meta = (ClampMin = "1", UIMin = "1"))
    int32 RequiredCount = 1;
};

// 4. THE MAIN CLASS
UCLASS()
class LASTRONIN_API URoninQuestAsset : public UDataAsset
{
    GENERATED_BODY()

public:
    URoninQuestAsset();

    // Explicit stable identity; never inferred from display text or asset name.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
    FName QuestID;

    // Invalid/empty means explicit StartQuest only. Matching is exact, not hierarchical.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
    FGameplayTag TriggerRumorTag;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
    FString QuestName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
    FString QuestDescription;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quest")
    TArray<FRoninQuestObjective> Objectives;

}; // <--- 5. THE MISSING SEMICOLON IS RIGHT HERE