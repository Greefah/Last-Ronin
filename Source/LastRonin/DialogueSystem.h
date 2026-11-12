#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "DialogueSystem.generated.h"

class UDeepSeekSubsystem;

// Existing UI delegate signature is retained.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDialogueUpdated, FString, DialogueText, const TArray<FString>&, ResponseOptions);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDialogueStart, FString, InitialContext);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnDialogueClosed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnDialogueIntelRevealed, FString, EntityID, FVector, Location);

UCLASS()
class LASTRONIN_API UDialogueSystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    UDialogueSystem();
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category = "Ronin|Dialogue")
    void StartDialogue(FString EntityID, FString Context);

    // Requires an active matching identity established by StartDialogue.
    UFUNCTION(BlueprintCallable, Category = "Ronin|Dialogue")
    void StartInterrogation(FString NPCName, FString Personality, FString PlayerInput);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Dialogue")
    void ProcessPlayerResponse(FString Response);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Dialogue")
    void EndDialogue();

    UFUNCTION(BlueprintPure, Category = "Ronin|Dialogue")
    TArray<FString> GetAvailableOptions() const { return AvailableOptions; }

    UFUNCTION(BlueprintPure, Category = "Ronin|Dialogue")
    int64 GetDialogueSessionID() const { return static_cast<int64>(SessionToken); }

    UFUNCTION(BlueprintPure, Category = "Ronin|Dialogue")
    int64 GetDialogueChoiceRevision() const { return static_cast<int64>(RequestRevision); }

    UFUNCTION(BlueprintPure, Category = "Ronin|Dialogue")
    FString GetDialogueEntityID() const { return CurrentEntityID; }

    UFUNCTION(BlueprintPure, Category = "Ronin|Dialogue")
    bool IsDialogueActive() const { return bSessionActive && !bDeinitializing; }

    UFUNCTION(BlueprintPure, Category = "Ronin|Dialogue")
    bool IsDialogueRequestPending() const { return bRequestPending; }

    // Capture both tokens when OnDialogueUpdated supplies options. Last option closes locally.
    UFUNCTION(BlueprintCallable, Category = "Ronin|Dialogue")
    bool SubmitDialogueChoice(int64 SessionID, int64 ChoiceRevision, int32 ChoiceIndex);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Dialogue")
    bool SubmitDialogueText(int64 SessionID, int64 ChoiceRevision, FString Text);

    UFUNCTION(BlueprintCallable, Category = "Ronin|Dialogue")
    bool CloseDialogueSession(int64 SessionID);

    UPROPERTY(BlueprintAssignable, Category = "Ronin|Dialogue")
    FOnDialogueStart OnDialogueStart;

    UPROPERTY(BlueprintAssignable, Category = "Ronin|Dialogue")
    FOnDialogueUpdated OnDialogueUpdated;

    UPROPERTY(BlueprintAssignable, Category = "Ronin|Dialogue")
    FOnDialogueClosed OnDialogueClosed;

    UPROPERTY(BlueprintAssignable, Category = "Ronin|Dialogue")
    FOnDialogueIntelRevealed OnIntelRevealed;

    UPROPERTY(BlueprintReadOnly, Category = "Ronin|Dialogue")
    FVector RevealedIntelLocation = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "Ronin|Dialogue")
    bool bHasRevealedIntel = false;

    UPROPERTY(BlueprintReadOnly, Category = "Ronin|Dialogue")
    bool bResolveBroken = false;

protected:
    FString CurrentEntityID = TEXT("");
    FString CurrentDialogueContext = TEXT("");
    TArray<FString> AvailableOptions;
    UDeepSeekSubsystem* GetAIBrain() const;
    void SendDialoguePrompt(FString Prompt);

    UFUNCTION()
    void ParseDialogueResponse(FString Response);

private:
    uint64 SessionToken = 0;
    uint64 RequestRevision = 0;
    bool bSessionActive = false;
    bool bRequestPending = false;
    bool bInterrogating = false;
    bool bDeinitializing = false;
    bool bHasKnownIntel = false;
    FVector KnownIntelLocation = FVector::ZeroVector;
    TMap<FString, TArray<FString>> CachedOptions;
    TMap<FString, bool> ActiveDialogues;
};